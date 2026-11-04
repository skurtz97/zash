/* POSIX builtins. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

static int b_colon(int, char **);
static int b_false(int, char **);
static int b_dot(int, char **);
static int b_break(int, char **);
static int b_cd(int, char **);
static int b_echo(int, char **);
static int b_eval(int, char **);
static int b_exec(int, char **);
static int b_exit(int, char **);
static int b_export(int, char **);
static int b_hash(int, char **);
static int b_pwd(int, char **);
static int b_read(int, char **);
static int b_readonly(int, char **);
static int b_return(int, char **);
static int b_set(int, char **);
static int b_shift(int, char **);
static int b_test(int, char **);
static int b_type(int, char **);
static int b_umask(int, char **);
static int b_unset(int, char **);
static int b_wait(int, char **);

/*
 * Not kept in source order for bsearch: collation of '.', ':', '[' and
 * letters differs between ASCII and IBM-1047, so the table is sorted
 * with strcmp() at startup in whichever charset the binary runs in.
 */
static struct builtin btab[] = {
	{ ".",        b_dot,      1 },
	{ ":",        b_colon,    1 },
	{ "[",        b_test,     0 },
	{ "break",    b_break,    1 },
	{ "cd",       b_cd,       0 },
	{ "chtag",    b_chtag,    0 },
	{ "continue", b_break,    1 },
	{ "echo",     b_echo,     0 },
	{ "eval",     b_eval,     1 },
	{ "exec",     b_exec,     1 },
	{ "exit",     b_exit,     1 },
	{ "export",   b_export,   1 },
	{ "extattr",  b_extattr,  0 },
	{ "false",    b_false,    0 },
	{ "fc",       b_fc,       0 },
	{ "hash",     b_hash,     0 },
	{ "history",  b_history,  0 },
	{ "pwd",      b_pwd,      0 },
	{ "read",     b_read,     0 },
	{ "readonly", b_readonly, 1 },
	{ "return",   b_return,   1 },
	{ "set",      b_set,      1 },
	{ "shift",    b_shift,    1 },
	{ "test",     b_test,     0 },
	{ "true",     b_colon,    0 },
	{ "tso",      b_tso,      0 },
	{ "type",     b_type,     0 },
	{ "umask",    b_umask,    0 },
	{ "unset",    b_unset,    1 },
	{ "wait",     b_wait,     0 },
};
#define NBUILTIN (sizeof(btab) / sizeof(btab[0]))

static int
bcmp_name(const void *a, const void *b)
{
	return strcmp(((const struct builtin *)a)->name,
	    ((const struct builtin *)b)->name);
}

void
builtins_init(void)
{
	qsort(btab, NBUILTIN, sizeof(btab[0]), bcmp_name);
}

const char *
builtin_name(size_t i)
{
	return i < NBUILTIN ? btab[i].name : NULL;
}

const struct builtin *
find_builtin(const char *name)
{
	struct builtin key = { name, NULL, 0 };

	return bsearch(&key, btab, NBUILTIN, sizeof(btab[0]), bcmp_name);
}

static int
getnum(const char *s, int *out)
{
	char *e;
	long v;

	errno = 0;
	v = strtol(s, &e, 10);
	if (*s == '\0' || *e != '\0' || errno || v < 0 || v > INT_MAX)
		return -1;
	*out = (int)v;
	return 0;
}

/* ---- trivial ---- */

static int
b_colon(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

static int
b_false(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 1;
}

/* ---- control flow ---- */

static int
b_break(int argc, char **argv)
{
	int n = 1;

	if (argc > 1 && (getnum(argv[1], &n) < 0 || n < 1)) {
		sh_warn("%s: %s: bad number", argv[0], argv[1]);
		return 1;
	}
	if (sh.loopdepth == 0)
		return 0;
	if (n > sh.loopdepth)
		n = sh.loopdepth;
	sh.skip = argv[0][0] == 'b' ? SKIP_BREAK : SKIP_CONT;
	sh.skipcount = n;
	return 0;
}

static int
b_return(int argc, char **argv)
{
	int n = sh.status;

	if (argc > 1 && getnum(argv[1], &n) < 0) {
		sh_warn("return: %s: bad number", argv[1]);
		n = 2;
	}
	sh.skip = SKIP_RETURN;
	sh.status = n & 0xff;
	return n & 0xff;
}

static int
b_exit(int argc, char **argv)
{
	int n = sh.status;

	if (argc > 1 && getnum(argv[1], &n) < 0) {
		sh_warn("exit: %s: bad number", argv[1]);
		n = 2;
	}
	sh_exit(n & 0xff);
}

static int
b_eval(int argc, char **argv)
{
	struct sbuf b = {0};
	int i, st;

	for (i = 1; i < argc; i++) {
		if (i > 1)
			sb_putc(&b, ' ');
		sb_puts(&b, argv[i]);
	}
	sb_cstr(&b);
	st = argc > 1 ? evalstring(b.s) : 0;
	sb_free(&b);
	return st;
}

static int
b_dot(int argc, char **argv)
{
	const char *file;
	int fd, st = 0, err;
	struct node *n;
	struct arena_mark m;

	if (argc < 2) {
		sh_warn(".: filename argument required");
		return 2;
	}
	file = argv[1];
	if (!strchr(file, '/')) {
		const char *p = path_lookup(file, &err);

		/* POSIX: search PATH for a readable file (need not be +x) */
		if (p == NULL && access(file, R_OK) == 0)
			p = file;
		if (p)
			file = p;
	}
	if ((fd = open(file, O_RDONLY)) < 0) {
		sh_error(".: %s: %s", argv[1], strerror(errno));
		return 1;
	}
	if (fd < 10) {
		int nfd = fcntl(fd, F_DUPFD, 10);

		close(fd);
		fd = nfd;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	in_push_fd(fd, 0);
	for (;;) {
		amark(&m);
		if ((n = parse_command()) == NULL)
			break;
		st = evaltree(n);
		arelease(&m);
		if (sh.skip == SKIP_RETURN && sh.funcdepth == 0) {
			sh.skip = SKIP_NONE;
			break;
		}
		if (sh.skip)
			break;
	}
	in_pop();
	close(fd);
	return st;
}

/* ---- directories ---- */

static int
b_cd(int argc, char **argv)
{
	const char *dir, *cdpath;
	char buf[PATH_MAX], *old;
	int print = 0, i = 1;

	while (i < argc && (strcmp(argv[i], "-L") == 0 ||
	    strcmp(argv[i], "-P") == 0))
		i++;            /* physical only for now */
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (i >= argc) {
		if ((dir = var_get("HOME")) == NULL || *dir == '\0') {
			sh_warn("cd: HOME not set");
			return 1;
		}
	} else if (strcmp(argv[i], "-") == 0) {
		if ((dir = var_get("OLDPWD")) == NULL) {
			sh_warn("cd: OLDPWD not set");
			return 1;
		}
		print = 1;
	} else
		dir = argv[i];

	old = getcwd(buf, sizeof(buf)) ? astrdup(buf) : NULL;
	if (dir[0] != '/' && dir[0] != '.' &&
	    (cdpath = var_get("CDPATH")) != NULL) {
		const char *p = cdpath, *c;

		for (;; p = c + 1) {
			char cand[PATH_MAX];
			size_t dl;

			c = strchr(p, ':');
			dl = c ? (size_t)(c - p) : strlen(p);
			if (dl > 0 && dl + strlen(dir) + 2 < sizeof(cand)) {
				snprintf(cand, sizeof(cand), "%.*s/%s",
				    (int)dl, p, dir);
				if (chdir(cand) == 0) {
					print = 1;
					goto done;
				}
			}
			if (c == NULL)
				break;
		}
	}
	if (chdir(dir) < 0) {
		sh_warn("cd: %s: %s", dir, strerror(errno));
		return 1;
	}
done:
	if (old)
		var_set("OLDPWD", old, 0);
	if (getcwd(buf, sizeof(buf))) {
		var_set("PWD", buf, 0);
		if (print)
			puts(buf);
	}
	return 0;
}

static int
b_pwd(int argc, char **argv)
{
	char buf[PATH_MAX];

	(void)argc;
	(void)argv;
	if (getcwd(buf, sizeof(buf)) == NULL) {
		sh_warn("pwd: %s", strerror(errno));
		return 1;
	}
	puts(buf);
	return 0;
}

/* ---- output ---- */

/* XSI echo: -n, and backslash escapes always processed. */
static int
b_echo(int argc, char **argv)
{
	int i = 1, nl = 1;

	if (i < argc && strcmp(argv[i], "-n") == 0) {
		nl = 0;
		i++;
	}
	for (; i < argc; i++) {
		const char *p;

		for (p = argv[i]; *p; p++) {
			if (*p != '\\' || p[1] == '\0') {
				putchar(*p);
				continue;
			}
			switch (*++p) {
			case 'a': putchar('\a'); break;
			case 'b': putchar('\b'); break;
			case 'c': fflush(stdout); return 0;
			case 'f': putchar('\f'); break;
			case 'n': putchar('\n'); break;
			case 'r': putchar('\r'); break;
			case 't': putchar('\t'); break;
			case 'v': putchar('\v'); break;
			case '\\': putchar('\\'); break;
			case '0': {
				int v = 0, k;

				for (k = 0; k < 3 && p[1] >= '0' && p[1] <= '7';
				    k++)
					v = v * 8 + (*++p - '0');
				putchar(v);
				break;
			}
			default:
				putchar('\\');
				putchar(*p);
			}
		}
		if (i + 1 < argc)
			putchar(' ');
	}
	if (nl)
		putchar('\n');
	return ferror(stdout) ? 1 : 0;
}

/* ---- exec ---- */

static int
b_exec(int argc, char **argv)
{
	const char *path;
	int err;

	if (argc < 2)
		return 0;       /* redirections made permanent by caller */
	path = path_lookup(argv[1], &err);
	if (path == NULL) {
		sh_warn("exec: %s: not found", argv[1]);
		sh_exit(err ? err : 127);
	}
	fflush(NULL);
	in_sync();
	le_end();
	execve(path, argv + 1, var_environ());
	sh_warn("exec: %s: %s", argv[1], strerror(errno));
	sh_exit(126);
}

/* ---- variables ---- */

static int
setattr(int argc, char **argv, int flag)
{
	int i = 1, st = 0;

	if (i < argc && strcmp(argv[i], "-p") == 0)
		i++;
	if (i >= argc) {
		var_print(flag);
		return 0;
	}
	for (; i < argc; i++) {
		char *eq = strchr(argv[i], '=');

		if (eq) {
			if (!is_name(argv[i], (size_t)(eq - argv[i]))) {
				sh_warn("%s: %s: bad variable name",
				    argv[0], argv[i]);
				st = 1;
				continue;
			}
			if (var_setkv(argv[i], flag))
				st = 1;
		} else if (is_name(argv[i], strlen(argv[i])))
			var_setflags(argv[i], flag);
		else {
			sh_warn("%s: %s: bad variable name", argv[0], argv[i]);
			st = 1;
		}
	}
	return st;
}

static int
b_export(int argc, char **argv)
{
	return setattr(argc, argv, V_EXPORT);
}

static int
b_readonly(int argc, char **argv)
{
	return setattr(argc, argv, V_READONLY);
}

static int
b_unset(int argc, char **argv)
{
	int i = 1, funcs = 0, st = 0;

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-f") == 0)
			funcs = 1;
		else if (strcmp(argv[i], "-v") == 0)
			funcs = 0;
		else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
	}
	for (; i < argc; i++)
		st |= funcs ? func_unset(argv[i]) : var_unset(argv[i]);
	return st;
}

static int
b_set(int argc, char **argv)
{
	if (argc == 1) {
		var_print(0);
		return 0;
	}
	return sh_options(argc, argv, 1);
}

static int
b_shift(int argc, char **argv)
{
	int n = 1, i;

	if (argc > 1 && getnum(argv[1], &n) < 0) {
		sh_warn("shift: %s: bad number", argv[1]);
		return 2;
	}
	if (n > sh.npos) {
		sh_warn("shift: can't shift that many");
		return 1;
	}
	for (i = 0; i < n; i++)
		free(sh.pos[i]);
	memmove(sh.pos, sh.pos + n, (size_t)(sh.npos - n + 1) * sizeof(char *));
	sh.npos -= n;
	return 0;
}

/*
 * Input for `read`.  POSIX forbids consuming bytes past the line, so on
 * pipes and terminals we read one byte per syscall.  When fd 0 is a
 * seekable regular file we read a chunk instead and lseek() back over
 * whatever follows the line (bash does the same): two syscalls per
 * line instead of one per byte.
 */
#define RCHUNK 256

struct rdin {
	char buf[RCHUNK];
	size_t pos, len;
	int seekable;
};

static int
rd_getc(struct rdin *r)
{
	ssize_t n;

	if (r->pos < r->len)
		return (unsigned char)r->buf[r->pos++];
	do
		n = read(0, r->buf, r->seekable ? RCHUNK : 1);
	while (n < 0 && errno == EINTR);
	if (n <= 0)
		return EOF;
	r->len = (size_t)n;
	r->pos = 1;
	return (unsigned char)r->buf[0];
}

/* Give back bytes read past the line. */
static void
rd_done(struct rdin *r)
{
	if (r->seekable && r->pos < r->len)
		lseek(0, -(off_t)(r->len - r->pos), SEEK_CUR);
}

/* read [-r] name... */
static int
b_read(int argc, char **argv)
{
	struct sbuf line = {0};
	const char *ifs = var_get("IFS");
	int raw = 0, i = 1, st = 0, c;
	struct rdin in;
	char *p;

	if (ifs == NULL)
		ifs = " \t\n";
	if (i < argc && strcmp(argv[i], "-r") == 0) {
		raw = 1;
		i++;
	}
	if (i >= argc) {
		sh_warn("read: variable name required");
		return 2;
	}
	in_sync();      /* script on stdin: start after this command */
	in.pos = in.len = 0;
	in.seekable = sys_input_seekable(0);
	for (;;) {
		if ((c = rd_getc(&in)) == EOF) {
			st = 1;
			break;
		}
		if (c == '\\' && !raw) {
			if ((c = rd_getc(&in)) == EOF) {
				st = 1;
				break;
			}
			if (c == '\n')
				continue;
			sb_putc(&line, '\001');   /* mark: escaped */
			sb_putc(&line, c);
			continue;
		}
		if (c == '\n')
			break;
		sb_putc(&line, c);
	}
	rd_done(&in);
	p = sb_cstr(&line);
	for (; i < argc; i++) {
		struct sbuf f = {0};

		while (*p && *p != '\001' && strchr(ifs, *p) &&
		    strchr(" \t\n", *p))
			p++;
		if (i + 1 == argc) {
			/* last var takes the rest, trailing IFS ws trimmed */
			size_t k;

			for (; *p; p++) {
				if (*p == '\001' && p[1]) {
					sb_putc(&f, *++p);
					continue;
				}
				sb_putc(&f, *p);
			}
			k = f.len;
			while (k > 0 && strchr(" \t\n", f.s[k - 1]) &&
			    strchr(ifs, f.s[k - 1]))
				k--;
			f.len = k;
		} else {
			while (*p && !(*p != '\001' && strchr(ifs, *p))) {
				if (*p == '\001' && p[1])
					p++;
				sb_putc(&f, *p++);
			}
			if (*p && strchr(ifs, *p))
				p++;
		}
		var_set(argv[i], sb_cstr(&f), 0);
		sb_free(&f);
	}
	sb_free(&line);
	return st;
}

/* ---- test ---- */

static char **targv;
static int targc, tpos;

static int t_or(void);

static int
t_unary(const char *op, const char *arg)
{
	struct stat st;
	int r;

	switch (op[1]) {
	case 'n': return *arg != '\0';
	case 'z': return *arg == '\0';
	case 't': return isatty(atoi(arg));
	case 'h':
	case 'L': return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
	case 'r': return access(arg, R_OK) == 0;
	case 'w': return access(arg, W_OK) == 0;
	case 'x': return access(arg, X_OK) == 0;
	}
	r = stat(arg, &st) == 0;
	switch (op[1]) {
	case 'e': return r;
	case 'f': return r && S_ISREG(st.st_mode);
	case 'd': return r && S_ISDIR(st.st_mode);
	case 'b': return r && S_ISBLK(st.st_mode);
	case 'c': return r && S_ISCHR(st.st_mode);
	case 'p': return r && S_ISFIFO(st.st_mode);
	case 'S': return r && S_ISSOCK(st.st_mode);
	case 's': return r && st.st_size > 0;
	case 'u': return r && (st.st_mode & S_ISUID);
	case 'g': return r && (st.st_mode & S_ISGID);
	}
	return -1;
}

static int
is_unop(const char *s)
{
	return s[0] == '-' && s[1] && !s[2] && strchr("nztLhrwxefdbcpSsug", s[1]);
}

static int
t_binary(const char *a, const char *op, const char *b, int *res)
{
	long x, y;
	char *e1, *e2;

	if (strcmp(op, "=") == 0) { *res = strcmp(a, b) == 0; return 0; }
	if (strcmp(op, "!=") == 0) { *res = strcmp(a, b) != 0; return 0; }
	if (op[0] != '-' || strlen(op) != 3)
		return -1;
	x = strtol(a, &e1, 10);
	y = strtol(b, &e2, 10);
	if (*a == '\0' || *e1 || *b == '\0' || *e2) {
		sh_warn("test: integer expression expected");
		*res = 0;
		return 0;
	}
	if (!strcmp(op, "-eq")) *res = x == y;
	else if (!strcmp(op, "-ne")) *res = x != y;
	else if (!strcmp(op, "-lt")) *res = x < y;
	else if (!strcmp(op, "-le")) *res = x <= y;
	else if (!strcmp(op, "-gt")) *res = x > y;
	else if (!strcmp(op, "-ge")) *res = x >= y;
	else return -1;
	return 0;
}

static int
t_primary(void)
{
	const char *a;
	int r;

	if (tpos >= targc)
		return 0;
	a = targv[tpos];
	if (strcmp(a, "!") == 0) {
		tpos++;
		return !t_primary();
	}
	if (strcmp(a, "(") == 0 && tpos + 1 < targc) {
		tpos++;
		r = t_or();
		if (tpos < targc && strcmp(targv[tpos], ")") == 0)
			tpos++;
		return r;
	}
	if (tpos + 2 < targc && t_binary(a, targv[tpos + 1],
	    targv[tpos + 2], &r) == 0) {
		tpos += 3;
		return r;
	}
	if (is_unop(a) && tpos + 1 < targc) {
		r = t_unary(a, targv[tpos + 1]);
		tpos += 2;
		return r > 0;
	}
	tpos++;
	return *a != '\0';
}

static int
t_and(void)
{
	int r = t_primary();

	while (tpos < targc && strcmp(targv[tpos], "-a") == 0) {
		tpos++;
		r = t_primary() && r;
	}
	return r;
}

static int
t_or(void)
{
	int r = t_and();

	while (tpos < targc && strcmp(targv[tpos], "-o") == 0) {
		tpos++;
		r = t_and() || r;
	}
	return r;
}

static int
b_test(int argc, char **argv)
{
	int r;

	if (argv[0][0] == '[') {
		if (strcmp(argv[argc - 1], "]") != 0) {
			sh_warn("[: missing ]");
			return 2;
		}
		argc--;
	}
	targv = argv + 1;
	targc = argc - 1;
	tpos = 0;
	if (targc == 0)
		return 1;
	r = t_or();
	if (tpos < targc) {
		sh_warn("test: %s: unexpected operator", targv[tpos]);
		return 2;
	}
	return !r;
}

/* ---- misc ---- */

static int
b_hash(int argc, char **argv)
{
	int i, err, st = 0;

	if (argc == 1)
		return path_hash_print();
	if (strcmp(argv[1], "-r") == 0) {
		path_hash_clear();
		return 0;
	}
	for (i = 1; i < argc; i++)
		if (path_lookup(argv[i], &err) == NULL) {
			sh_warn("hash: %s: not found", argv[i]);
			st = 1;
		}
	return st;
}

static int
b_type(int argc, char **argv)
{
	int i, err, st = 0;
	const struct builtin *b;
	const char *p;

	for (i = 1; i < argc; i++) {
		if ((b = find_builtin(argv[i])) && b->special)
			printf("%s is a special shell builtin\n", argv[i]);
		else if (func_lookup(argv[i]))
			printf("%s is a shell function\n", argv[i]);
		else if (b)
			printf("%s is a shell builtin\n", argv[i]);
		else if ((p = path_lookup(argv[i], &err)) != NULL)
			printf("%s is %s\n", argv[i], p);
		else {
			printf("%s: not found\n", argv[i]);
			st = 1;
		}
	}
	return st;
}

static int
b_umask(int argc, char **argv)
{
	mode_t m;
	char *e;
	long v;

	if (argc == 1) {
		m = umask(0);
		umask(m);
		printf("%04o\n", (unsigned)m);
		return 0;
	}
	v = strtol(argv[1], &e, 8);
	if (*e || v < 0 || v > 0777) {
		sh_warn("umask: %s: octal mode required", argv[1]);
		return 1;
	}
	umask((mode_t)v);
	return 0;
}

static int
b_wait(int argc, char **argv)
{
	int i, st = 0;

	if (argc == 1)
		return job_wait_all();
	for (i = 1; i < argc; i++)
		st = job_wait_pid((pid_t)atol(argv[i]));
	return st;
}
