/*
 * zash - an Almquist-family shell for z/OS UNIX and POSIX systems.
 * Shared declarations.
 */
#ifndef ZASH_SHELL_H
#define ZASH_SHELL_H

#include <setjmp.h>
#include <stddef.h>
#include <sys/types.h>

/* ---- arena allocator (mem.c) ------------------------------------------ */

/*
 * Parse trees and expansion temporaries come from a bump arena that is
 * reset after every top-level command.  This is the ash "stack string"
 * idea: one pointer bump per allocation, one reset per command, no
 * per-node free().
 */
struct arena_mark {
    struct ablock *blk;
    size_t used;
};

void *aalloc(size_t n);
char *astrdup(const char *s);
char *astrndup(const char *s, size_t n);
void amark(struct arena_mark *m);
void arelease(const struct arena_mark *m);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* growable byte buffer, heap-backed (used by lexer/expander) */
struct sbuf {
    char *s;
    size_t len, cap;
    int arena; /* storage from aalloc(); sb_free is a no-op */
};
void sb_grow(struct sbuf *b, size_t need);
static inline void sb_putc(struct sbuf *b, int c) {
    if (b->len + 2 > b->cap)
        sb_grow(b, 1);
    b->s[b->len++] = (char)c;
}
void sb_puts(struct sbuf *b, const char *s);
void sb_putn(struct sbuf *b, const char *s, size_t n);
char *sb_cstr(struct sbuf *b);  /* NUL-terminate, return b->s */
char *sb_arena(struct sbuf *b); /* copy into arena, reset buffer */
void sb_free(struct sbuf *b);

/* ---- parse tree (zparse.c) --------------------------------------------- */

struct word {
    struct word *next;
    char *text; /* raw source text, quoting intact */
};

enum rtype {
    R_IN,      /* <   */
    R_OUT,     /* >   */
    R_CLOBBER, /* >|  */
    R_APPEND,  /* >>  */
    R_RDWR,    /* <>  */
    R_DUPIN,   /* <&  */
    R_DUPOUT,  /* >&  */
    R_HEREDOC  /* <<  (body expanded at run time) */
};

struct redir {
    struct redir *next;
    enum rtype type;
    int fd;
    struct word *target; /* file, fd number, '-', or heredoc body */
    int hd_quoted;       /* heredoc delimiter was quoted: no expansion */
};

enum ntype {
    N_CMD,
    N_PIPE,
    N_AND,
    N_OR,
    N_SEQ,
    N_BG,
    N_NOT,
    N_SUBSHELL,
    N_BRACE,
    N_IF,
    N_WHILE,
    N_UNTIL,
    N_FOR,
    N_CASE,
    N_FUNC
};

struct caseitem {
    struct caseitem *next;
    struct word *pats;
    struct node *body;
};

struct node {
    enum ntype type;
    struct redir *redirs; /* for compound commands and N_CMD */
    union {
        struct {
            struct word *assigns, *args;
        } cmd;
        struct {
            struct node *l, *r;
        } bin;
        struct {
            struct node *body;
        } grp;
        struct {
            struct node *cond, *then, *els;
        } ifn; /* while/until use cond + then */
        struct {
            char *var;
            struct word *list;
            int has_in;
            struct node *body;
        } forn;
        struct {
            struct word *subj;
            struct caseitem *items;
        } casen;
        struct {
            char *name;
            struct node *body;
        } func;
    } u;
};

/* returns NULL at EOF; longjmps on syntax error */
struct node *parse_command(void);
struct node *parse_string_all(const char *s); /* whole string as list */
struct node *node_copy_heap(const struct node *n);
void node_free_heap(struct node *n);
int parse_eof(void);

/* ---- input (input.c) -------------------------------------------------- */

void in_push_string(const char *s);
void in_push_fd(int fd, int interactive);
void in_pop(void);
int in_depth(void);
void in_pop_to(int depth);
int in_getc(void);
void in_ungetc(int c);
void in_set_ps2(int on);
void in_discard_line(void);
void in_sync(void);
int in_interactive(void);

/* ---- line editing (lineedit.c) ---------------------------------------- */

int le_init(void);
const char *le_readline(const char *prompt, int *len);
void le_history_add(const char *line);
void le_end(void);
void le_set_editor(const char *mode);
void le_history_clear(void);

/* zhistory.c */
void zhist_init(void);
void zhist_add(const char *line);
void zhist_save(void);
int zhist_next_number(void);
int b_fc(int argc, char **argv);
int b_history(int argc, char **argv);

/* zprompt.c, complete.c */
#define PROMPT_IGNORE '\001' /* brackets \[ \] text for EL_PROMPT_ESC */
const char *prompt_expand(const char *ps);
void complete_add(void *cands, const char *name);

/* ---- variables (var.c) ------------------------------------------------ */

#define V_EXPORT 0x01
#define V_READONLY 0x02
#define V_UNSET 0x04

void var_init(char **envp);
const char *var_get(const char *name);
int var_set(const char *name, const char *val, int flags);
int var_setkv(const char *assign, int flags); /* "NAME=value" */
int var_unset(const char *name);
int var_flags(const char *name);
void var_setflags(const char *name, int flags);
char **var_environ(void); /* cached */
void var_print(int flags);
void var_foreach_prefix(const char *pre, void *cands);
int is_name(const char *s, size_t n);

/* temporary assignments for regular builtins/functions */
struct var_save;
struct var_save *var_save_and_set(struct word *assigns);
void var_restore(struct var_save *s);

/* ---- expansion (expand.c) --------------------------------------------- */

#define EXP_SPLIT 0x01
#define EXP_GLOB 0x02
#define EXP_TILDE 0x04
#define EXP_FULL (EXP_SPLIT | EXP_GLOB | EXP_TILDE)

struct strlist {
    char **v;
    size_t n, cap;
};

void expand_words(struct word *w, int flags, struct strlist *out);
char *expand_str(const char *text, int flags); /* single field, arena */
char *expand_heredoc(const char *text);
long arith_eval(const char *expr);
int case_match(const char *pat_text, const char *subj);

/* ---- execution (exec.c, eval.c) --------------------------------------- */

int evaltree(struct node *n);
int evaltree_last(struct node *n);
void free_argv(char **v, int n);
int evalstring(const char *s);
char *cmdsubst(const char *src); /* captured output, arena */

typedef int (*builtin_fn)(int argc, char **argv);
struct builtin {
    const char *name;
    builtin_fn fn;
    int special; /* POSIX special builtin */
};
void builtins_init(void);
const struct builtin *find_builtin(const char *name);
const char *builtin_name(size_t i);
const char *path_lookup(const char *name, int *err);
void path_hash_clear(void);
int path_hash_print(void);

struct node *func_lookup(const char *name);
void func_define(const char *name, struct node *body);
int func_unset(const char *name);
void func_foreach_prefix(const char *pre, void *cands);

/* fd plan for spawning without touching the parent's fds */
#define FDMAP_N 10
struct fdplan {
    int map[FDMAP_N]; /* child fd i <- parent fd map[i]; -1 closed */
    int opened[FDMAP_N * 2];
    int nopened;
    int ignint; /* child ignores SIGINT/SIGQUIT (async) */
};
void fdplan_init(struct fdplan *p);
int fdplan_build(struct fdplan *p, struct redir *r);
void fdplan_release(struct fdplan *p);

/* in-process redirection with save/restore (builtins, compounds) */
struct redir_save;
struct redir_save *redir_apply(struct redir *r, int *err);
void redir_restore(struct redir_save *s);
void redir_commit(struct redir_save *s);

/* jobs (jobs.c) */
void job_add(pid_t pid);
void job_orphan(pid_t pid);
int job_wait_pid(pid_t pid);
int job_wait_all(void);
void job_reap(void);
int wait_status(int st);
int job_count(void);

/* ---- platform (sys_*.c) ----------------------------------------------- */

void sys_init(void);
pid_t sys_spawn(const char *path, char **argv, char **envp,
                const struct fdplan *fp);
int sys_pipe(int pfd[2]);
int sys_open_redir(const char *name, int oflag, int mode, int which);
void sys_tag_new_file(int fd, int which);
int sys_input_seekable(int fd);
void sys_input_text(int fd);

/* z/OS builtins (zos_builtins.c) */
int b_tso(int argc, char **argv);
int b_extattr(int argc, char **argv);
int b_chtag(int argc, char **argv);

/* ---- global shell state (main.c) -------------------------------------- */

enum { SKIP_NONE, SKIP_BREAK, SKIP_CONT, SKIP_RETURN };

struct shell {
    const char *argv0;
    char **pos; /* positional parameters $1.. */
    int npos;
    int status;   /* $? */
    pid_t lastbg; /* $! */
    pid_t pid;    /* $$ */
    int interactive;
    int opt_e, opt_u, opt_x, opt_f, opt_C, opt_v, opt_n;
    int skip, skipcount;
    int loopdepth, funcdepth;
    int in_subshell;
    int cmdno; /* commands read, for \# in prompts */
    int sigint;
    volatile int sighup; /* terminal hung up: save history, exit */
    jmp_buf *errjmp;
};
extern struct shell sh;

_Noreturn void sh_exit(int st);
void sh_error(const char *fmt, ...); /* longjmps */
void sh_warn(const char *fmt, ...);
int sh_options(int argc, char **argv, int setcmd);
char *sh_optstring(void);

#endif
