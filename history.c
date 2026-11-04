/*
 * Command history: the numbered list behind `fc`, `history`, \! in
 * prompts and HISTFILE.  libedit keeps its own copy for arrow-key
 * recall; this file owns the canonical one so numbering and the file
 * format don't depend on libedit internals.
 *
 * HISTFILE is plain text, one entry per line (bash's format without
 * timestamps), so ~/.bash_history can be shared.  At exit only this
 * session's new entries are appended, then the file is trimmed to
 * HISTFILESIZE through a temp file + rename(): concurrent shells don't
 * overwrite each other and a crash mid-write can't truncate the file.
 * New files are created mode 0600 since history often holds secrets.
 *
 * Variables: HISTFILE (default ~/.zash_history; empty or unset after
 * startup disables saving), HISTSIZE (in memory, default 500),
 * HISTFILESIZE (default HISTSIZE), HISTCONTROL (ignorespace, ignoredups,
 * ignoreboth), FCEDIT (fc's editor; then EDITOR; then ed).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

static char **ent;              /* oldest first */
static int nent, capent;
static int base = 1;            /* event number of ent[0] */
static int unsaved;             /* entries at the end not yet in HISTFILE */
static int enabled;

static int
getnum(const char *var, int dflt)
{
	const char *v = var_get(var);
	char *e;
	long n;

	if (v == NULL || *v == '\0')
		return dflt;
	n = strtol(v, &e, 10);
	return *e == '\0' && n >= 0 && n < 1000000 ? (int)n : dflt;
}

static int
histsize(void)
{
	return getnum("HISTSIZE", 500);
}

static void
trim_to(int max)
{
	int drop = nent - max, i;

	if (drop <= 0)
		return;
	for (i = 0; i < drop; i++)
		free(ent[i]);
	memmove(ent, ent + drop, (size_t)(nent - drop) * sizeof(char *));
	nent -= drop;
	base += drop;
	if (unsaved > nent)
		unsaved = nent;
}

static void
push(const char *line, size_t n)
{
	char *s = xmalloc(n + 1);

	memcpy(s, line, n);
	s[n] = '\0';
	if (nent == capent) {
		capent = capent ? capent * 2 : 256;
		ent = xrealloc(ent, (size_t)capent * sizeof(char *));
	}
	ent[nent++] = s;
}

/* Re-sync libedit's recall list with ours. */
static void
resync(void)
{
	int i;

	le_history_clear();
	for (i = 0; i < nent; i++)
		le_history_add(ent[i]);
}

int
zhist_next_number(void)
{
	return base + nent;
}

void
zhist_add(const char *line)
{
	const char *ctl = var_get("HISTCONTROL");
	size_t n = strlen(line);

	if (!enabled)
		return;
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		n--;
	if (n == 0)
		return;
	if (ctl) {
		int both = strstr(ctl, "ignoreboth") != NULL;

		if ((both || strstr(ctl, "ignorespace")) && line[0] == ' ')
			return;
		if ((both || strstr(ctl, "ignoredups")) && nent > 0 &&
		    strlen(ent[nent - 1]) == n &&
		    memcmp(ent[nent - 1], line, n) == 0)
			return;
	}
	push(line, n);
	unsaved++;
	le_history_add(ent[nent - 1]);
	if (nent > histsize()) {
		trim_to(histsize());
		resync();
	}
}

/* Replace the newest entry (the fc line) with what fc ran. */
static void
replace_last(const char *cmd)
{
	size_t n = strlen(cmd);

	while (n > 0 && cmd[n - 1] == '\n')
		n--;
	if (nent == 0)
		return;
	free(ent[--nent]);
	push(cmd, n);
	resync();
}

/* ---- file I/O ---- */

static const char *
histfile(void)
{
	const char *f = var_get("HISTFILE");

	return f && *f ? f : NULL;
}

/* Read a whole file in one go; returns heap buffer or NULL. */
static char *
slurp(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY);
	struct sbuf b = {0};
	char tmp[65536];
	ssize_t n;

	*len = 0;
	if (fd < 0)
		return NULL;
	while ((n = read(fd, tmp, sizeof(tmp))) > 0 || (n < 0 && errno == EINTR))
		if (n > 0)
			sb_putn(&b, tmp, (size_t)n);
	close(fd);
	*len = b.len;
	return sb_cstr(&b);
}

/*
 * Offset where the last `keep` lines of buf[0..len) begin, found by
 * scanning newlines back from the end: no per-line work for lines that
 * are dropped.
 */
static size_t
tail_start(const char *buf, size_t len, int keep)
{
	size_t i = len;

	if (keep <= 0)
		return len;
	if (i > 0 && buf[i - 1] == '\n')
		i--;            /* the final newline ends the last line */
	while (i > 0) {
		if (buf[i - 1] == '\n' && --keep == 0)
			return i;
		i--;
	}
	return 0;
}

/* Atomically replace path with data (temp file + rename). */
static int
write_atomic(const char *path, const char *data, size_t len)
{
	char tmp[4096];
	int fd;
	ssize_t w;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp%ld", path,
	    (long)getpid()) >= (int)sizeof(tmp))
		return -1;
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
		return -1;
	while (len > 0) {
		if ((w = write(fd, data, len)) < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp);
			return -1;
		}
		data += w;
		len -= (size_t)w;
	}
	if (close(fd) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int
write_all(const char *path)
{
	struct sbuf b = {0};
	int i, rc;

	for (i = 0; i < nent; i++) {
		sb_puts(&b, ent[i]);
		sb_putc(&b, '\n');
	}
	rc = write_atomic(path, b.s ? b.s : "", b.len);
	sb_free(&b);
	return rc;
}

static void
trim_file(const char *path, int max)
{
	size_t len, off;
	char *buf = slurp(path, &len);

	if (buf && (off = tail_start(buf, len, max)) > 0)
		write_atomic(path, buf + off, len - off);
	free(buf);
}

static void
load_file(const char *path)
{
	size_t len, off;
	char *buf = slurp(path, &len), *p, *nl, *end;

	if (buf == NULL)
		return;
	end = buf + len;
	off = tail_start(buf, len, histsize());
	for (p = buf + off; p < end; p = nl + 1) {
		if ((nl = memchr(p, '\n', (size_t)(end - p))) == NULL)
			nl = end;
		if (nl > p)
			push(p, (size_t)(nl - p));
	}
	free(buf);
	trim_to(histsize());
	resync();
}

/* Append this session's unsaved entries, then trim the file. */
static int
append_new(const char *path)
{
	struct sbuf b = {0};
	int fd, i;
	ssize_t w;
	size_t done = 0;

	if (unsaved == 0)
		return 0;
	if ((fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600)) < 0)
		return -1;
	for (i = nent - unsaved; i < nent; i++) {
		sb_puts(&b, ent[i]);
		sb_putc(&b, '\n');
	}
	/* one write(): O_APPEND keeps concurrent shells' lines whole */
	while (done < b.len) {
		if ((w = write(fd, b.s + done, b.len - done)) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		done += (size_t)w;
	}
	sb_free(&b);
	if (close(fd) != 0 || done < b.len)
		return -1;
	unsaved = 0;
	trim_file(path, getnum("HISTFILESIZE", histsize()));
	return 0;
}

void
zhist_init(void)
{
	const char *home = var_get("HOME"), *f;

	enabled = 1;
	if (var_get("HISTFILE") == NULL && home) {
		char *p = xmalloc(strlen(home) + 16);

		sprintf(p, "%s/.zash_history", home);
		var_set("HISTFILE", p, 0);
		free(p);
	}
	if ((f = histfile()) != NULL)
		load_file(f);
}

/* At exit of the interactive top-level shell. */
void
zhist_save(void)
{
	const char *f;

	if (!enabled || sh.in_subshell || (f = histfile()) == NULL)
		return;
	if (append_new(f) < 0)
		sh_warn("history: %s: %s", f, strerror(errno));
	enabled = 0;
}

/* ---- history builtin (bash subset) ---- */

static void
list(int first, int last, int numbers, int reverse)
{
	int i;

	for (i = reverse ? last : first; reverse ? i >= first : i <= last;
	    reverse ? i-- : i++) {
		if (numbers)
			printf("%5d\t%s\n", base + i, ent[i]);
		else
			printf("\t%s\n", ent[i]);
	}
}

/*
 * history [n]   list (last n)        history -c   clear
 * history -a    append new to file   history -w   write all to file
 * history -r    read file into list
 */
int
b_history(int argc, char **argv)
{
	const char *f = histfile();
	char *e;
	long n;

	if (!enabled) {
		sh_warn("history: not enabled (non-interactive shell)");
		return 1;
	}
	if (argc > 1 && argv[1][0] == '-' && argv[1][1] && !argv[1][2]) {
		switch (argv[1][1]) {
		case 'c':
			trim_to(0);
			unsaved = 0;
			resync();
			return 0;
		case 'a':
		case 'w':
		case 'r':
			if (f == NULL) {
				sh_warn("history: HISTFILE not set");
				return 1;
			}
			if (argv[1][1] == 'a' && append_new(f) == 0)
				return 0;
			if (argv[1][1] == 'w' && write_all(f) == 0) {
				unsaved = 0;
				return 0;
			}
			if (argv[1][1] == 'r') {
				load_file(f);
				return 0;
			}
			sh_warn("history: %s: %s", f, strerror(errno));
			return 1;
		}
	}
	if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
		sh_warn("usage: history [-c|-a|-w|-r|n]");
		return 2;
	}
	n = nent;
	if (argc == 2) {
		n = strtol(argv[1], &e, 10);
		if (*e || n < 0) {
			sh_warn("history: %s: numeric argument required",
			    argv[1]);
			return 2;
		}
		if (n > nent)
			n = nent;
	}
	if (n > 0)
		list(nent - (int)n, nent - 1, 1, 0);
	return 0;
}

/* ---- fc (POSIX) ---- */

/*
 * Resolve an fc operand to an index into ent[].  `cur` is the index of
 * the fc command itself, which operands never select.  Numbers are
 * event numbers, negatives count back from the fc line, strings pick
 * the most recent entry starting with them.
 */
static int
resolve(const char *s, int cur)
{
	char *e;
	long v = strtol(s, &e, 10);
	int i;

	if (*s && *e == '\0') {
		if (v < 0)
			i = cur + (int)v;
		else
			i = (int)v - base;
		if (i < 0)
			i = 0;
		if (i >= cur)
			i = cur - 1;
		return i;
	}
	for (i = cur - 1; i >= 0; i--)
		if (strncmp(ent[i], s, strlen(s)) == 0)
			return i;
	sh_warn("fc: %s: no matching command", s);
	return -1;
}

static int
fc_reexec(const char *subst, const char *which, int cur)
{
	int i = which ? resolve(which, cur) : cur - 1;
	struct sbuf b = {0};
	const char *cmd, *eq, *hit;
	int st;

	if (i < 0)
		return 1;
	cmd = ent[i];
	if (subst && (eq = strchr(subst, '=')) != NULL && eq > subst &&
	    (hit = strstr(cmd, astrndup(subst, (size_t)(eq - subst))))) {
		sb_putn(&b, cmd, (size_t)(hit - cmd));
		sb_puts(&b, eq + 1);
		sb_puts(&b, hit + (eq - subst));
	} else
		sb_puts(&b, cmd);
	sb_cstr(&b);
	cmd = astrdup(b.s);
	sb_free(&b);
	puts(cmd);
	fflush(stdout);
	replace_last(cmd);
	st = evalstring(cmd);
	return st;
}

static int
fc_edit(const char *editor, int first, int last, int reverse)
{
	char tmpl[4096], *cmd;
	const char *tmpdir = var_get("TMPDIR");
	struct sbuf b = {0};
	FILE *f;
	int fd, i, st;
	const char *s;

	snprintf(tmpl, sizeof(tmpl), "%s/zash-fc.XXXXXX",
	    tmpdir && *tmpdir ? tmpdir : "/tmp");
	if ((fd = mkstemp(tmpl)) < 0 || (f = fdopen(fd, "w")) == NULL) {
		sh_warn("fc: %s: %s", tmpl, strerror(errno));
		if (fd >= 0)
			close(fd);
		return 1;
	}
	for (i = reverse ? last : first; reverse ? i >= first : i <= last;
	    reverse ? i-- : i++)
		fprintf(f, "%s\n", ent[i]);
	fclose(f);

	/* editor may carry options ("vim -u NONE"), so let the shell split it */
	sb_puts(&b, editor);
	sb_puts(&b, " '");
	for (s = tmpl; *s; s++)
		if (*s == '\'')
			sb_puts(&b, "'\\''");
		else
			sb_putc(&b, *s);
	sb_putc(&b, '\'');
	st = evalstring(sb_cstr(&b));
	sb_free(&b);
	if (st != 0) {
		unlink(tmpl);
		return st;
	}
	{
		size_t len;
		char *buf = slurp(tmpl, &len);

		unlink(tmpl);
		cmd = astrdup(buf ? buf : "");
		free(buf);
	}
	fputs(cmd, stdout);
	fflush(stdout);
	replace_last(cmd);
	return evalstring(cmd);
}

/*
 * fc [-r] [-e editor] [first [last]]
 * fc -l [-nr] [first [last]]
 * fc -s [old=new] [first]
 */
int
b_fc(int argc, char **argv)
{
	int list_mode = 0, nonum = 0, reverse = 0, sflag = 0, i = 1;
	int cur = nent - 1, first, last;
	const char *editor = NULL;

	if (!enabled || nent == 0) {
		sh_warn("fc: history not available");
		return 1;
	}
	for (; i < argc && argv[i][0] == '-' && argv[i][1] &&
	    !(argv[i][1] >= '0' && argv[i][1] <= '9'); i++) {
		const char *o;

		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		for (o = argv[i] + 1; *o; o++) {
			switch (*o) {
			case 'l': list_mode = 1; break;
			case 'n': nonum = 1; break;
			case 'r': reverse = 1; break;
			case 's': sflag = 1; break;
			case 'e':
				if (o[1] || i + 1 >= argc) {
					sh_warn("fc: -e needs an editor");
					return 2;
				}
				editor = argv[++i];
				goto nextarg;
			default:
				sh_warn("fc: -%c: unknown option", *o);
				return 2;
			}
		}
nextarg:;
	}
	if (editor && strcmp(editor, "-") == 0)
		sflag = 1;              /* ksh: fc -e - is fc -s */
	if (sflag) {
		const char *subst = NULL;

		if (i < argc && strchr(argv[i], '='))
			subst = argv[i++];
		return fc_reexec(subst, i < argc ? argv[i] : NULL, cur);
	}
	if (cur < 1) {
		sh_warn("fc: no previous command");
		return 1;
	}
	if (list_mode) {
		first = i < argc ? resolve(argv[i], cur) : cur - 16;
		last = i + 1 < argc ? resolve(argv[i + 1], cur) : cur - 1;
		if (first < 0 && i < argc)
			return 1;
		if (first < 0)
			first = 0;
	} else {
		first = i < argc ? resolve(argv[i], cur) : cur - 1;
		last = i + 1 < argc ? resolve(argv[i + 1], cur) : first;
	}
	if (first < 0 || last < 0)
		return 1;
	if (first > last) {
		int t = first;

		first = last;
		last = t;
		reverse = !reverse;
	}
	if (list_mode) {
		list(first, last, !nonum, reverse);
		return 0;
	}
	if (editor == NULL)
		editor = var_get("FCEDIT");
	if (editor == NULL || *editor == '\0')
		editor = var_get("EDITOR");
	if (editor == NULL || *editor == '\0')
		editor = "ed";
	return fc_edit(editor, first, last, reverse);
}
