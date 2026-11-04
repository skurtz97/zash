/* zash: startup, option parsing, error handling, top-level loop. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

extern char **environ;

struct shell sh;

static int opt_i, opt_s, opt_c;

_Noreturn void sh_exit(int st) {
    if (sh.interactive && !sh.in_subshell)
        zhist_save();
    fflush(NULL);
    le_end();
    exit(st);
}

static void vwarn(const char *fmt, va_list ap) {
    fflush(stdout);
    fputs("zash: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void sh_warn(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vwarn(fmt, ap);
    va_end(ap);
}

/* Fatal to the current command: non-interactive shells exit 2. */
void sh_error(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vwarn(fmt, ap);
    va_end(ap);
    sh.status = 2;
    if (!sh.interactive || sh.errjmp == NULL)
        sh_exit(2);
    longjmp(*sh.errjmp, 1);
}

/* ---- options ---- */

static const struct {
    char letter;
    const char *name;
    int *flag;
} opts[] = {
    {'C', "noclobber", &sh.opt_C}, {'e', "errexit", &sh.opt_e},
    {'f', "noglob", &sh.opt_f},    {'n', "noexec", &sh.opt_n},
    {'u', "nounset", &sh.opt_u},   {'v', "verbose", &sh.opt_v},
    {'x', "xtrace", &sh.opt_x},
};
#define NOPTS (sizeof(opts) / sizeof(opts[0]))

char *sh_optstring(void) {
    static char buf[NOPTS + 4];
    size_t i, n = 0;

    for (i = 0; i < NOPTS; i++)
        if (*opts[i].flag)
            buf[n++] = opts[i].letter;
    if (sh.interactive)
        buf[n++] = 'i';
    buf[n] = '\0';
    return buf;
}

static int set_long(const char *name, int on) {
    size_t i;

    if (strcmp(name, "vi") == 0 || strcmp(name, "emacs") == 0) {
        le_set_editor(on ? name : "emacs");
        return 0;
    }
    for (i = 0; i < NOPTS; i++)
        if (strcmp(opts[i].name, name) == 0) {
            *opts[i].flag = on;
            return 0;
        }
    sh_warn("set: %s: unknown option", name);
    return 1;
}

static void print_opts(void) {
    size_t i;

    for (i = 0; i < NOPTS; i++)
        printf("%-12s%s\n", opts[i].name, *opts[i].flag ? "on" : "off");
}

/* Parse leading option words; returns index of first operand or -1. */
static int parse_opts(int argc, char **argv, int startup) {
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int on;

        if (strcmp(a, "--") == 0)
            return i + 1;
        if ((a[0] != '-' && a[0] != '+') || a[1] == '\0')
            break;
        on = a[0] == '-';
        for (a++; *a; a++) {
            size_t k;

            if (*a == 'o') {
                if (i + 1 >= argc) {
                    if (!startup) {
                        print_opts();
                        return argc;
                    }
                    return -1;
                }
                if (set_long(argv[++i], on))
                    return -1;
                continue;
            }
            if (startup && *a == 'c') {
                opt_c = 1;
                continue;
            }
            if (startup && *a == 's') {
                opt_s = 1;
                continue;
            }
            if (startup && *a == 'i') {
                opt_i = 1;
                continue;
            }
            for (k = 0; k < NOPTS; k++)
                if (opts[k].letter == *a) {
                    *opts[k].flag = on;
                    break;
                }
            if (k == NOPTS) {
                sh_warn("-%c: unknown option", *a);
                return -1;
            }
        }
    }
    return i;
}

static void set_positional(char **v, int n) {
    free_argv(sh.pos, sh.npos);
    sh.pos = xmalloc(((size_t)n + 1) * sizeof(char *));
    for (sh.npos = 0; sh.npos < n; sh.npos++)
        sh.pos[sh.npos] = xstrdup(v[sh.npos]);
    sh.pos[n] = NULL;
}

int sh_options(int argc, char **argv, int setcmd) {
    int i = parse_opts(argc, argv, !setcmd);

    (void)setcmd;
    if (i < 0)
        return 2;
    if (i < argc || (i > 1 && strcmp(argv[i - 1], "--") == 0))
        set_positional(argv + i, argc - i);
    return 0;
}

/* ---- signals ---- */

static void on_sigint(int sig) {
    (void)sig;
    sh.sigint = 1;
}

static void on_sighup(int sig) {
    (void)sig;
    sh.sighup = 1; /* the read loop sees EOF and exits via sh_exit */
}

static void setup_signals(void) {
    struct sigaction sa;

    if (!sh.interactive)
        return;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint; /* no SA_RESTART: interrupt reads */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_sighup;
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
}

/* ---- top level ---- */

static int run_loop(int fd, int interactive) {
    jmp_buf jb;
    struct arena_mark m;
    struct node *n;
    int depth;

    in_push_fd(fd, interactive);
    depth = in_depth();
    sh.errjmp = &jb;
    for (;;) {
        amark(&m);
        if (setjmp(jb)) {
            arelease(&m);
            in_pop_to(depth);
            in_discard_line();
            sh.skip = SKIP_NONE;
            sh.loopdepth = sh.funcdepth = 0;
            continue;
        }
        if (sh.sigint) {
            sh.sigint = 0;
            in_discard_line();
        }
        n = parse_command();
        if (n == NULL)
            break;
        if (!(n->type == N_SEQ && !n->u.bin.l && !n->u.bin.r))
            sh.cmdno++;
        if (!sh.opt_n)
            evaltree(n);
        arelease(&m);
        sh.skip = SKIP_NONE;
    }
    if (interactive && !sh.sighup)
        fputc('\n', stderr);
    in_pop();
    return sh.sighup ? 128 + SIGHUP : sh.status;
}

static void source_if(const char *path) {
    char *argv[3] = {".", NULL, NULL};
    const struct builtin *dot = find_builtin(".");

    jmp_buf jb, *prev = sh.errjmp;
    int depth = in_depth();

    if (path == NULL || access(path, R_OK) != 0)
        return;
    argv[1] = (char *)path;
    sh.errjmp = &jb;
    if (setjmp(jb) == 0)
        dot->fn(2, argv);
    else
        in_pop_to(depth); /* error in profile: keep going */
    sh.errjmp = prev;
    sh.skip = SKIP_NONE;
}

/*
 * An inherited PWD is kept only if it is absolute and names the current
 * directory (so symlinked paths survive); otherwise use getcwd().
 */
static void fix_pwd(void) {
    const char *pwd = var_get("PWD");
    struct stat a, b;
    char cwd[4096];

    if (pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino)
        return;
    if (getcwd(cwd, sizeof(cwd)))
        var_set("PWD", cwd, V_EXPORT);
}

static int move_high(int fd) {
    int nfd = fcntl(fd, F_DUPFD, 10);

    if (nfd >= 0) {
        close(fd);
        fd = nfd;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int main(int argc, char **argv) {
    int i, fd = 0, login;
    const char *cmd = NULL, *script = NULL;
    char buf[32];

    sh.pid = getpid();
    sh.argv0 = argv[0];
    login = argv[0][0] == '-';
    builtins_init();
    var_init(environ);
    sys_init();
    snprintf(buf, sizeof(buf), "%ld", (long)getppid());
    var_set("PPID", buf, 0);
    fix_pwd();

    i = parse_opts(argc, argv, 1);
    if (i < 0) {
        fputs("usage: zash [-CefnuvxIs] [-o option] "
              "[-c string [name [arg...]] | script [arg...]]\n",
              stderr);
        return 2;
    }
    if (opt_c) {
        if (i >= argc) {
            sh_warn("-c requires an argument");
            return 2;
        }
        cmd = argv[i++];
        if (i < argc)
            sh.argv0 = argv[i++];
    } else if (!opt_s && i < argc) {
        script = argv[i++];
        sh.argv0 = script;
        if ((fd = open(script, O_RDONLY)) < 0) {
            sh_warn("%s: %s", script, strerror(errno));
            return errno == ENOENT ? 127 : 126;
        }
        fd = move_high(fd);
    }
    set_positional(argv + i, argc - i);

    sh.interactive =
        opt_i || (cmd == NULL && script == NULL && isatty(0) && isatty(2));
    setup_signals();

    if (login) {
        const char *home = var_get("HOME");

        source_if("/etc/profile");
        if (home) {
            char *p = xmalloc(strlen(home) + 10);

            sprintf(p, "%s/.profile", home);
            source_if(p);
            free(p);
        }
    }
    if (sh.interactive) {
        const char *env = var_get("ENV");

        if (env)
            source_if(expand_str(env, EXP_TILDE));
        le_init();
        zhist_init();
    }
    if (cmd)
        sh_exit(evaltree_last(parse_string_all(cmd)));
    sh_exit(run_loop(fd, sh.interactive && fd == 0));
}
