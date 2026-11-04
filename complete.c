/*
 * TAB completion for libedit.
 *
 *   command position, no '/'  -> keywords, builtins, functions, PATH
 *   word starting with '$'    -> variable names
 *   anything else             -> files (dirs get '/', specials escaped)
 *
 * PATH executables are cached per directory and rescanned only when a
 * directory's mtime changes, so TAB costs one stat() per PATH entry in
 * the common case.  Multiple matches are inserted up to the longest
 * common prefix, then listed in columns (asking first past 100).
 */
#ifdef WITH_LIBEDIT
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <histedit.h>

#include "shell.h"

struct cand {
	char *s;
	int isdir;
};

struct cands {
	struct cand *v;
	size_t n, cap;
};

static void
cand_add(struct cands *c, const char *s, int isdir)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 64;
		c->v = xrealloc(c->v, c->cap * sizeof(*c->v));
	}
	c->v[c->n].s = xstrdup(s);
	c->v[c->n++].isdir = isdir;
}

static void
cand_free(struct cands *c)
{
	size_t i;

	for (i = 0; i < c->n; i++)
		free(c->v[i].s);
	free(c->v);
	memset(c, 0, sizeof(*c));
}

static int
cmpcand(const void *a, const void *b)
{
	return strcmp(((const struct cand *)a)->s, ((const struct cand *)b)->s);
}

/* sort, then drop duplicates (same name in two PATH dirs, builtin+file) */
static void
cand_uniq(struct cands *c)
{
	size_t i, j = 0;

	if (c->n < 2)
		return;
	qsort(c->v, c->n, sizeof(*c->v), cmpcand);
	for (i = 1; i < c->n; i++) {
		if (strcmp(c->v[i].s, c->v[j].s) == 0) {
			free(c->v[i].s);
			continue;
		}
		c->v[++j] = c->v[i];
	}
	c->n = j + 1;
}

/* ---- PATH command cache ---- */

struct pdir {
	char *path;
	time_t mtime;
	char **names;
	size_t n;
};

static struct pdir *pdirs;
static size_t npdirs;
static char *cached_path;

static void
pdir_scan(struct pdir *d)
{
	DIR *dp;
	struct dirent *de;
	size_t cap = 0, i;
	char full[4096];

	for (i = 0; i < d->n; i++)
		free(d->names[i]);
	free(d->names);
	d->names = NULL;
	d->n = 0;
	if ((dp = opendir(d->path)) == NULL)
		return;
	while ((de = readdir(dp)) != NULL) {
		struct stat st;

		if (de->d_name[0] == '.')
			continue;
		snprintf(full, sizeof(full), "%s/%s", d->path, de->d_name);
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) ||
		    access(full, X_OK) != 0)
			continue;
		if (d->n == cap) {
			cap = cap ? cap * 2 : 128;
			d->names = xrealloc(d->names, cap * sizeof(char *));
		}
		d->names[d->n++] = xstrdup(de->d_name);
	}
	closedir(dp);
}

static void
path_refresh(void)
{
	const char *path = var_get("PATH");
	const char *p, *c;
	size_t i;

	if (path == NULL)
		path = "/bin:/usr/bin";
	if (cached_path == NULL || strcmp(cached_path, path) != 0) {
		for (i = 0; i < npdirs; i++) {
			size_t k;

			for (k = 0; k < pdirs[i].n; k++)
				free(pdirs[i].names[k]);
			free(pdirs[i].names);
			free(pdirs[i].path);
		}
		free(pdirs);
		pdirs = NULL;
		npdirs = 0;
		free(cached_path);
		cached_path = xstrdup(path);
		for (p = path;; p = c + 1) {
			size_t n;

			c = strchr(p, ':');
			n = c ? (size_t)(c - p) : strlen(p);
			pdirs = xrealloc(pdirs, (npdirs + 1) * sizeof(*pdirs));
			memset(&pdirs[npdirs], 0, sizeof(*pdirs));
			pdirs[npdirs].path = xstrdup(n ? astrndup(p, n) : ".");
			pdirs[npdirs].mtime = (time_t)-1;
			npdirs++;
			if (c == NULL)
				break;
		}
	}
	for (i = 0; i < npdirs; i++) {
		struct stat st;

		if (stat(pdirs[i].path, &st) != 0) {
			st.st_mtime = 0;
		}
		if (st.st_mtime != pdirs[i].mtime) {
			pdirs[i].mtime = st.st_mtime;
			pdir_scan(&pdirs[i]);
		}
	}
}

static const char *const keywords[] = {
	"case", "do", "done", "elif", "else", "esac", "fi", "for", "if",
	"in", "then", "until", "while", NULL
};

static void
complete_command(const char *pre, struct cands *c)
{
	size_t pl = strlen(pre), i, k;
	const char *name;

	for (i = 0; keywords[i]; i++)
		if (strncmp(keywords[i], pre, pl) == 0)
			cand_add(c, keywords[i], 0);
	for (i = 0; (name = builtin_name(i)) != NULL; i++)
		if (strncmp(name, pre, pl) == 0)
			cand_add(c, name, 0);
	func_foreach_prefix(pre, c);
	path_refresh();
	for (i = 0; i < npdirs; i++)
		for (k = 0; k < pdirs[i].n; k++)
			if (strncmp(pdirs[i].names[k], pre, pl) == 0)
				cand_add(c, pdirs[i].names[k], 0);
}

/* ---- files ---- */

/* Undo backslash/quote escaping in what the user typed. */
static char *
unescape(const char *s, size_t n)
{
	struct sbuf b = {0};
	size_t i;
	int q = 0;

	for (i = 0; i < n; i++) {
		if (q) {
			if (s[i] == q)
				q = 0;
			else
				sb_putc(&b, s[i]);
		} else if (s[i] == '\'' || s[i] == '"')
			q = s[i];
		else if (s[i] == '\\' && i + 1 < n)
			sb_putc(&b, s[++i]);
		else
			sb_putc(&b, s[i]);
	}
	sb_cstr(&b);
	return b.s;
}

static void
complete_file(const char *word, struct cands *c, int dirs_and_exec)
{
	const char *slash = strrchr(word, '/');
	char dir[4096], full[4096];
	const char *base, *home;
	DIR *dp;
	struct dirent *de;
	size_t bl;

	if (slash) {
		size_t dl = (size_t)(slash - word);

		if (word[0] == '~' && (word[1] == '/' || word + 1 == slash) &&
		    (home = var_get("HOME")) != NULL)
			snprintf(dir, sizeof(dir), "%s%.*s", home,
			    (int)(dl - 1), word + 1);
		else if (dl == 0)
			strcpy(dir, "/");
		else
			snprintf(dir, sizeof(dir), "%.*s", (int)dl, word);
		base = slash + 1;
	} else {
		strcpy(dir, ".");
		base = word;
	}
	bl = strlen(base);
	if ((dp = opendir(dir)) == NULL)
		return;
	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		int isdir;

		if (strncmp(de->d_name, base, bl) != 0)
			continue;
		if (de->d_name[0] == '.' && (base[0] != '.' ||
		    strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0))
			continue;
		snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
		if (dirs_and_exec && !isdir && access(full, X_OK) != 0)
			continue;
		cand_add(c, de->d_name, isdir);
	}
	closedir(dp);
}

/* ---- variables ---- */

static void
complete_var(const char *pre, struct cands *c)
{
	var_foreach_prefix(pre, c);
}

void
complete_add(void *c, const char *name)
{
	cand_add(c, name, 0);
}

/* ---- context ---- */

static int
is_cmd_sep(int ch)
{
	return ch == ';' || ch == '|' || ch == '&' || ch == '(' ||
	    ch == '`' || ch == '{' || ch == '\n';
}

/*
 * Is the word starting at `start` in command position?  Walk back over
 * blanks and earlier words: a separator, line start, a keyword that
 * introduces a command, or only NAME=value assignments before it.
 */
static int
command_position(const char *buf, const char *start)
{
	const char *p = start;

	for (;;) {
		const char *we;

		while (p > buf && (p[-1] == ' ' || p[-1] == '\t'))
			p--;
		if (p == buf || is_cmd_sep((unsigned char)p[-1]))
			return 1;
		if (p - buf >= 2 && p[-1] == '(' && p[-2] == '$')
			return 1;
		we = p;
		while (p > buf && !strchr(" \t;|&(`{\n", p[-1]))
			p--;
		{
			size_t n = (size_t)(we - p);
			static const char *const lead[] = {
				"then", "do", "else", "elif", "if", "while",
				"until", "!", "exec", "command", "time",
				"nohup", "sudo", "env", NULL
			};
			int i;
			const char *eq = memchr(p, '=', n);

			for (i = 0; lead[i]; i++)
				if (strlen(lead[i]) == n &&
				    strncmp(p, lead[i], n) == 0)
					return 1;
			if (eq && eq > p && is_name(p, (size_t)(eq - p)))
				continue;       /* assignment: keep looking */
			return 0;
		}
	}
}

/* ---- insertion and listing ---- */

/*
 * Insert completed text, quoted for its context: mode 0 raw (variable
 * names), 1 unquoted (backslash all specials), '"' inside double quotes
 * (only $ ` " \ need it), '\'' inside single quotes (none can; a quote
 * in the name is closed, escaped and reopened).
 */
static void
insert_escaped(EditLine *e, const char *s, int mode)
{
	char buf[2] = { 0, 0 };

	for (; *s; s++) {
		if (mode == '\'' && *s == '\'') {
			el_insertstr(e, "'\\''");
			continue;
		}
		if ((mode == 1 && strchr(" \t\n\\'\"$`*?[]()&;|<>#!{}", *s)) ||
		    (mode == '"' && strchr("$`\"\\", *s)))
			el_insertstr(e, "\\");
		buf[0] = *s;
		el_insertstr(e, buf);
	}
}

static int
term_width(void)
{
#ifdef TIOCGWINSZ
	struct winsize ws;

	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
#endif
	{
		const char *c = var_get("COLUMNS");

		return c && atoi(c) > 0 ? atoi(c) : 80;
	}
}

static int
list_cands(EditLine *e, struct cands *c)
{
	size_t i, w = 0, cols, rows, r, k;
	int width = term_width();
	char ch;

	if (c->n > 100) {
		printf("\nDisplay all %zu possibilities? (y or n) ", c->n);
		fflush(stdout);
		if (el_getc(e, &ch) != 1 || (ch != 'y' && ch != 'Y')) {
			putchar('\n');
			return CC_REDISPLAY;
		}
	}
	for (i = 0; i < c->n; i++) {
		size_t l = strlen(c->v[i].s) + (c->v[i].isdir ? 1 : 0);

		if (l > w)
			w = l;
	}
	w += 2;
	cols = (size_t)width / w;
	if (cols == 0)
		cols = 1;
	rows = (c->n + cols - 1) / cols;
	putchar('\n');
	for (r = 0; r < rows; r++) {
		for (k = 0; k < cols; k++) {
			size_t idx = k * rows + r;

			if (idx >= c->n)
				break;
			printf("%s%s", c->v[idx].s, c->v[idx].isdir ? "/" : "");
			if (k + 1 < cols && idx + rows < c->n)
				printf("%*s", (int)(w - strlen(c->v[idx].s) -
				    (c->v[idx].isdir ? 1 : 0)), "");
		}
		putchar('\n');
	}
	return CC_REDISPLAY;
}

unsigned char
complete_fn(EditLine *e, int ch)
{
	const LineInfo *li = el_line(e);
	const char *start = li->cursor, *p;
	struct cands c = {0};
	char *word;
	size_t wl, typed, pre, i;
	int cmdpos, isvar = 0, isfile = 0, q = 0;
	unsigned char ret = CC_REFRESH;

	(void)ch;
	/*
	 * Word start: scan forward from the line start so quotes are
	 * honoured; a separator only counts outside quotes.  q is the open
	 * quote at the cursor (0, '\'' or '"').
	 */
	start = li->buffer;
	for (p = li->buffer; p < li->cursor; p++) {
		if (q) {
			if (*p == q)
				q = 0;
			else if (q == '"' && *p == '\\' && p + 1 < li->cursor)
				p++;
		} else if (*p == '\\' && p + 1 < li->cursor)
			p++;
		else if (*p == '\'' || *p == '"')
			q = *p;
		else if (strchr(" \t;|&<>()`{\n", *p))
			start = p + 1;
		else if (*p == '$' && p + 1 < li->cursor && p[1] == '(')
			start = p + 2, p++;
	}
	typed = (size_t)(li->cursor - start);
	word = unescape(start, typed);
	wl = strlen(word);
	cmdpos = command_position(li->buffer, start);

	if (word[0] == '$' && !strchr(word, '/')) {
		isvar = 1;
		complete_var(word + 1, &c);
	} else if (cmdpos && !strchr(word, '/') && word[0] != '~' &&
	    word[0] != '.') {
		complete_command(word, &c);
	} else {
		isfile = 1;
		complete_file(word, &c, cmdpos);
	}
	cand_uniq(&c);
	if (c.n == 0) {
		ret = CC_ERROR;
		goto out;
	}

	/* the part of the word the candidates replace */
	if (isvar)
		p = word + 1;
	else if (isfile && strrchr(word, '/'))
		p = strrchr(word, '/') + 1;
	else
		p = word;
	pre = strlen(c.v[0].s);
	for (i = 1; i < c.n; i++) {
		size_t j;

		for (j = 0; j < pre && c.v[i].s[j] == c.v[0].s[j]; j++)
			;
		pre = j;
	}
	if (pre > strlen(p)) {
		char *ins = astrndup(c.v[0].s + strlen(p), pre - strlen(p));

		insert_escaped(e, ins, isvar ? 0 : q ? q : 1);
	}
	if (c.n == 1) {
		if (isfile && c.v[0].isdir)
			el_insertstr(e, "/");
		else if (!isvar || li->cursor == li->lastchar) {
			if (q)
				el_insertstr(e, q == '"' ? "\"" : "'");
			el_insertstr(e, " ");
		}
	} else if (pre <= strlen(p)) {
		ret = list_cands(e, &c);
	}
out:
	(void)wl;
	free(word);
	cand_free(&c);
	return ret;
}
#else /* !WITH_LIBEDIT */

#include "shell.h"

void
complete_add(void *cands, const char *name)
{
	(void)cands;
	(void)name;
}
#endif
