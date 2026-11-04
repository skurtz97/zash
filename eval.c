/*
 * Tree evaluation.
 *
 * Performance rules, in priority order:
 *   1. External commands are started with sys_spawn() and an fd plan;
 *      the shell is never forked just to exec something.  This covers
 *      simple commands, every external stage of a pipeline, background
 *      simple commands, and $(cmd) when cmd is a single external.
 *   2. fork() is used only when shell code must run in a child: a
 *      subshell, a builtin/function/compound stage in a pipeline, or a
 *      compound background job.  On z/OS fork() copies the address space
 *      and is far more expensive than spawn(), so these paths are kept
 *      narrow.
 *   3. The last simple command of `sh -c '...'` is exec'd in place.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

#define EV_EXIT 0x01

static int cond_depth;   /* >0 while evaluating a condition (-e off) */
static int subst_status; /* status of last $(...) for bare assigns */

enum kind { K_NONE, K_FUNC, K_BUILTIN, K_EXTERNAL, K_NOTFOUND };

struct cmdinfo {
    struct strlist av;
    enum kind kind;
    const struct builtin *bi;
    struct node *fbody;
    const char *path;
    int err;
};

static int eval(struct node *n, int flags);

static void check_e(int st) {
    if (sh.opt_e && st != 0 && cond_depth == 0 && sh.skip == SKIP_NONE)
        sh_exit(st);
}

static void child_signals(int bg) {
    signal(SIGINT, bg && !sh.interactive ? SIG_IGN : SIG_DFL);
    signal(SIGQUIT, bg && !sh.interactive ? SIG_IGN : SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

static pid_t xfork(int bg) {
    pid_t pid;

    fflush(NULL);
    in_sync();
    pid = fork();
    if (pid < 0)
        sh_error("fork: %s", strerror(errno));
    if (pid == 0) {
        sh.in_subshell = 1;
        sh.interactive = 0;
        le_end();
        child_signals(bg);
    }
    return pid;
}

static _Noreturn void child_exit(int st) {
    fflush(NULL);
    _exit(st);
}

/* ---- simple commands ---- */

static void xtrace(struct node *n, struct strlist *av) {
    struct word *w;
    size_t i;
    const char *ps4 = var_get("PS4");

    if (!sh.opt_x)
        return;
    fputs(ps4 ? ps4 : "+ ", stderr);
    for (w = n->u.cmd.assigns; w; w = w->next)
        fprintf(stderr, "%s ", expand_str(w->text, EXP_TILDE));
    for (i = 0; i < av->n; i++)
        fprintf(stderr, i + 1 < av->n ? "%s " : "%s", av->v[i]);
    fputc('\n', stderr);
}

static void cmd_prepare(struct node *n, struct cmdinfo *ci) {
    const char *name;

    memset(ci, 0, sizeof(*ci));
    subst_status = 0;
    expand_words(n->u.cmd.args, EXP_FULL, &ci->av);
    xtrace(n, &ci->av);
    if (ci->av.n == 0) {
        ci->kind = K_NONE;
        return;
    }
    name = ci->av.v[0];
    if (!strchr(name, '/')) {
        /* special builtins precede functions (POSIX 2.9.1) */
        ci->bi = find_builtin(name);
        if (ci->bi && ci->bi->special) {
            ci->kind = K_BUILTIN;
            return;
        }
        if ((ci->fbody = func_lookup(name)) != NULL) {
            ci->kind = K_FUNC;
            return;
        }
        if (ci->bi) {
            ci->kind = K_BUILTIN;
            return;
        }
    }
    ci->path = path_lookup(name, &ci->err);
    ci->kind = ci->path ? K_EXTERNAL : K_NOTFOUND;
}

static char **heap_argv(char **v, int n) {
    char **r = xmalloc(((size_t)n + 1) * sizeof(char *));
    int i;

    for (i = 0; i < n; i++)
        r[i] = xstrdup(v[i]);
    r[n] = NULL;
    return r;
}

void free_argv(char **v, int n) {
    int i;

    if (v == NULL)
        return;
    for (i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

static int call_function(struct node *body, struct cmdinfo *ci) {
    char **opos = sh.pos;
    int onpos = sh.npos, st;

    sh.pos = heap_argv(ci->av.v + 1, (int)ci->av.n - 1);
    sh.npos = (int)ci->av.n - 1;
    sh.funcdepth++;
    st = eval(body, 0);
    sh.funcdepth--;
    if (sh.skip == SKIP_RETURN) {
        sh.skip = SKIP_NONE;
        st = sh.status;
    }
    free_argv(sh.pos, sh.npos);
    sh.pos = opos;
    sh.npos = onpos;
    return st;
}

static pid_t spawn_external(struct node *n, struct cmdinfo *ci,
                            struct fdplan *fp) {
    struct var_save *vs;
    pid_t pid;

    in_sync(); /* stdin script: child must start after this command */
    /* assignments first so `_TAG_REDIR_OUT=TXT cmd >f` sees the var */
    vs = var_save_and_set(n->u.cmd.assigns);
    if (fdplan_build(fp, n->redirs) < 0) {
        var_restore(vs);
        return -1;
    }
    pid = sys_spawn(ci->path, ci->av.v, var_environ(), fp);
    if (pid < 0)
        sh_warn("%s: %s", ci->av.v[0], strerror(errno));
    var_restore(vs);
    fdplan_release(fp);
    return pid;
}

static _Noreturn void exec_inplace(struct node *n, struct cmdinfo *ci) {
    struct redir_save *rs;
    struct var_save *vs;
    int err;

    rs = redir_apply(n->redirs, &err);
    if (err)
        sh_exit(1);
    redir_commit(rs);
    vs = var_save_and_set(n->u.cmd.assigns);
    (void)vs;
    fflush(NULL);
    in_sync();
    child_signals(0);
    execve(ci->path, ci->av.v, var_environ());
    sh_warn("%s: %s", ci->av.v[0], strerror(errno));
    sh_exit(errno == ENOENT ? 127 : 126);
}

static void assign_permanent(struct word *w) {
    for (; w; w = w->next)
        var_setkv(expand_str(w->text, EXP_TILDE), 0);
}

/* Run a prepared simple command in this process (externals: spawn+wait). */
static int cmd_run(struct node *n, struct cmdinfo *ci, int flags) {
    struct redir_save *rs;
    struct var_save *vs = NULL;
    struct fdplan fp;
    int err, st;
    pid_t pid;

    switch (ci->kind) {
    case K_NONE:
        assign_permanent(n->u.cmd.assigns);
        rs = redir_apply(n->redirs, &err);
        redir_restore(rs);
        return err ? 1 : subst_status;
    case K_NOTFOUND:
        rs = redir_apply(n->redirs, &err);
        if (!err)
            sh_warn("%s: %s", ci->av.v[0],
                    ci->err == 126 ? "permission denied" : "not found");
        redir_restore(rs);
        return ci->err;
    case K_EXTERNAL:
        if (flags & EV_EXIT)
            exec_inplace(n, ci);
        fdplan_init(&fp);
        pid = spawn_external(n, ci, &fp);
        if (pid < 0)
            return 126;
        return job_wait_pid(pid);
    case K_BUILTIN:
        if (ci->bi->special)
            assign_permanent(n->u.cmd.assigns);
        else
            vs = var_save_and_set(n->u.cmd.assigns);
        rs = redir_apply(n->redirs, &err);
        if (err) {
            var_restore(vs);
            return 1;
        }
        st = ci->bi->fn((int)ci->av.n, ci->av.v);
        fflush(stdout);
        if (ci->av.n == 1 && strcmp(ci->av.v[0], "exec") == 0)
            redir_commit(rs);
        else
            redir_restore(rs);
        var_restore(vs);
        return st;
    case K_FUNC:
        vs = var_save_and_set(n->u.cmd.assigns);
        rs = redir_apply(n->redirs, &err);
        if (err) {
            var_restore(vs);
            return 1;
        }
        st = call_function(ci->fbody, ci);
        redir_restore(rs);
        var_restore(vs);
        return st;
    }
    return 0;
}

/* ---- pipelines ---- */

static int pipeline(struct node *n) {
    struct node **stage;
    pid_t *pids;
    int ns = 0, i, prev = -1, st = 0;
    struct node *t;

    for (t = n; t->type == N_PIPE; t = t->u.bin.l)
        ns++;
    ns++;
    stage = aalloc((size_t)ns * sizeof(*stage));
    pids = aalloc((size_t)ns * sizeof(*pids));
    for (t = n, i = ns - 1; t->type == N_PIPE; t = t->u.bin.l)
        stage[i--] = t->u.bin.r;
    stage[0] = t;

    for (i = 0; i < ns; i++) {
        int pfd[2] = {-1, -1}, in, out;
        struct cmdinfo ci;

        if (i + 1 < ns) {
            if (sys_pipe(pfd) < 0)
                sh_error("pipe: %s", strerror(errno));
        }
        in = prev >= 0 ? prev : 0;
        out = pfd[1] >= 0 ? pfd[1] : 1;
        pids[i] = -1;

        if (stage[i]->type == N_CMD) {
            cmd_prepare(stage[i], &ci);
            if (ci.kind == K_EXTERNAL) {
                struct fdplan fp;

                fdplan_init(&fp);
                fp.map[0] = in;
                fp.map[1] = out;
                pids[i] = spawn_external(stage[i], &ci, &fp);
            }
        }
        if (pids[i] < 0 &&
            !(stage[i]->type == N_CMD && ci.kind == K_EXTERNAL)) {
            pid_t pid = xfork(0);

            if (pid == 0) {
                if (in != 0) {
                    dup2(in, 0);
                    close(in);
                }
                if (out != 1) {
                    dup2(out, 1);
                    close(out);
                }
                if (pfd[0] >= 0)
                    close(pfd[0]);
                child_exit(stage[i]->type == N_CMD ? cmd_run(stage[i], &ci, 0)
                                                   : eval(stage[i], 0));
            }
            pids[i] = pid;
        }
        if (prev >= 0)
            close(prev);
        if (pfd[1] >= 0)
            close(pfd[1]);
        prev = pfd[0];
    }
    for (i = 0; i < ns; i++) {
        int s = pids[i] > 0 ? job_wait_pid(pids[i]) : 127;

        if (i == ns - 1)
            st = s;
    }
    return st;
}

/* ---- background ---- */

static int background(struct node *n) {
    pid_t pid = -1;

    if (n->type == N_CMD) {
        struct cmdinfo ci;

        cmd_prepare(n, &ci);
        if (ci.kind == K_EXTERNAL) {
            struct fdplan fp;
            int nul = -1;

            fdplan_init(&fp);
            if (!sh.interactive && (nul = open("/dev/null", O_RDONLY)) >= 0) {
                fcntl(nul, F_SETFD, FD_CLOEXEC);
                fp.map[0] = nul;
            }
            fp.ignint = !sh.interactive;
            pid = spawn_external(n, &ci, &fp);
            if (nul >= 0)
                close(nul);
        }
    }
    if (pid < 0) {
        pid = xfork(1);
        if (pid == 0) {
            if (!sh.interactive) {
                int nul = open("/dev/null", O_RDONLY);

                if (nul >= 0 && nul != 0) {
                    dup2(nul, 0);
                    close(nul);
                }
            }
            child_exit(eval(n, 0));
        }
    }
    job_add(pid);
    sh.lastbg = pid;
    return 0;
}

/* ---- loops ---- */

/* returns 1 if the loop should stop */
static int loop_skip(void) {
    switch (sh.skip) {
    case SKIP_BREAK:
        if (--sh.skipcount <= 0)
            sh.skip = SKIP_NONE;
        return 1;
    case SKIP_CONT:
        if (--sh.skipcount <= 0) {
            sh.skip = SKIP_NONE;
            return 0;
        }
        return 1;
    case SKIP_RETURN: return 1;
    }
    return 0;
}

static int eval_for(struct node *n) {
    struct strlist sl = {0};
    size_t i;
    int st = 0;

    if (n->u.forn.has_in)
        expand_words(n->u.forn.list, EXP_FULL, &sl);
    else
        for (i = 0; i < (size_t)sh.npos; i++) {
            struct word w = {NULL, NULL};
            char *q = aalloc(strlen(sh.pos[i]) * 4 + 3), *p = q;
            const char *s;

            /* quote each param so expansion keeps it whole */
            *p++ = '\'';
            for (s = sh.pos[i]; *s; s++) {
                if (*s == '\'') {
                    memcpy(p, "'\\''", 4);
                    p += 4;
                } else
                    *p++ = *s;
            }
            *p++ = '\'';
            *p = '\0';
            w.text = q;
            expand_words(&w, 0, &sl);
        }
    sh.loopdepth++;
    for (i = 0; i < sl.n; i++) {
        struct arena_mark m;

        if (var_set(n->u.forn.var, sl.v[i], 0) != 0) {
            st = 1;
            break;
        }
        amark(&m);
        st = eval(n->u.forn.body, 0);
        arelease(&m);
        if (sh.skip && loop_skip())
            break;
    }
    sh.loopdepth--;
    return st;
}

static int eval_while(struct node *n) {
    int st = 0, c;

    sh.loopdepth++;
    for (;;) {
        struct arena_mark m;

        amark(&m);
        cond_depth++;
        c = eval(n->u.ifn.cond, 0);
        cond_depth--;
        if (!sh.skip && (c == 0) == (n->type == N_WHILE))
            st = eval(n->u.ifn.then, 0);
        arelease(&m);
        if (sh.skip) {
            if (loop_skip())
                break;
            continue;
        }
        if ((c == 0) != (n->type == N_WHILE))
            break;
    }
    sh.loopdepth--;
    return st;
}

static int eval_case(struct node *n) {
    const char *subj = expand_str(n->u.casen.subj->text, EXP_TILDE);
    struct caseitem *ci;
    struct word *p;

    for (ci = n->u.casen.items; ci; ci = ci->next)
        for (p = ci->pats; p; p = p->next)
            if (case_match(p->text, subj))
                return ci->body ? eval(ci->body, 0) : 0;
    return 0;
}

/* ---- dispatcher ---- */

static int eval_compound(struct node *n) {
    int c;

    switch (n->type) {
    case N_BRACE: return eval(n->u.grp.body, 0);
    case N_IF:
        cond_depth++;
        c = eval(n->u.ifn.cond, 0);
        cond_depth--;
        if (sh.skip)
            return c;
        if (c == 0)
            return eval(n->u.ifn.then, 0);
        return n->u.ifn.els ? eval(n->u.ifn.els, 0) : 0;
    case N_WHILE:
    case N_UNTIL: return eval_while(n);
    case N_FOR:   return eval_for(n);
    case N_CASE:  return eval_case(n);
    default:      return 0;
    }
}

static int eval(struct node *n, int flags) {
    int st = 0;

    if (n == NULL || sh.skip)
        return sh.status;
    switch (n->type) {
    case N_SEQ:
        eval(n->u.bin.l, 0);
        if (sh.skip)
            return sh.status;
        return eval(n->u.bin.r, flags);
    case N_AND:
    case N_OR:
        cond_depth++;
        st = eval(n->u.bin.l, 0);
        cond_depth--;
        if (!sh.skip && (st == 0) == (n->type == N_AND))
            st = eval(n->u.bin.r, flags);
        break;
    case N_NOT:
        cond_depth++;
        st = !eval(n->u.grp.body, 0);
        cond_depth--;
        break;
    case N_BG: st = background(n->u.bin.l); break;
    case N_PIPE:
        st = pipeline(n);
        check_e(st);
        break;
    case N_CMD: {
        struct cmdinfo ci;
        struct arena_mark m;

        /* nothing a simple command allocates outlives it */
        amark(&m);
        cmd_prepare(n, &ci);
        st = cmd_run(n, &ci, flags);
        arelease(&m);
        check_e(st);
        break;
    }
    case N_FUNC:
        func_define(n->u.func.name, n->u.func.body);
        st = 0;
        break;
    case N_SUBSHELL: {
        pid_t pid = xfork(0);

        if (pid == 0) {
            struct redir_save *rs;
            int err;

            rs = redir_apply(n->redirs, &err);
            (void)rs;
            child_exit(err ? 1 : eval(n->u.grp.body, 0));
        }
        st = job_wait_pid(pid);
        check_e(st);
        break;
    }
    default: {
        struct redir_save *rs;
        int err;

        rs = redir_apply(n->redirs, &err);
        if (err) {
            st = 1;
            break;
        }
        st = eval_compound(n);
        redir_restore(rs);
        break;
    }
    }
    sh.status = st;
    return st;
}

int evaltree(struct node *n) {
    return eval(n, 0);
}

/* Evaluate with the last simple command exec'd in place (sh -c). */
int evaltree_last(struct node *n) {
    return eval(n, EV_EXIT);
}

int evalstring(const char *s) {
    return eval(parse_string_all(s), 0);
}

/* ---- command substitution ---- */

char *cmdsubst(const char *src) {
    struct node *t = parse_string_all(src);
    struct sbuf out = {0};
    int pfd[2], st;
    pid_t pid = -1;
    char buf[4096];
    ssize_t r;
    char *res;

    if (t == NULL)
        return astrdup("");
    fflush(NULL);
    if (sys_pipe(pfd) < 0)
        sh_error("pipe: %s", strerror(errno));

    if (t->type == N_CMD) {
        struct cmdinfo ci;

        cmd_prepare(t, &ci);
        if (ci.kind == K_EXTERNAL) {
            struct fdplan fp;

            fdplan_init(&fp);
            fp.map[1] = pfd[1];
            pid = spawn_external(t, &ci, &fp);
        }
    }
    if (pid < 0) {
        pid = xfork(0);
        if (pid == 0) {
            close(pfd[0]);
            dup2(pfd[1], 1);
            close(pfd[1]);
            child_exit(eval(t, 0));
        }
    }
    close(pfd[1]);
    for (;;) {
        r = read(pfd[0], buf, sizeof(buf));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        sb_putn(&out, buf, (size_t)r);
    }
    close(pfd[0]);
    st = job_wait_pid(pid);
    subst_status = st;
    sh.status = st;
    while (out.len > 0 && out.s[out.len - 1] == '\n')
        out.len--;
    res = astrndup(out.s ? out.s : "", out.len);
    sb_free(&out);
    return res;
}
