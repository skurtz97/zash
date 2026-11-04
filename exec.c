/*
 * Command lookup (functions, builtins, hashed PATH) and redirection.
 *
 * Redirections for spawned commands are compiled into an fd plan: the
 * parent opens targets close-on-exec and records which parent fd each
 * child fd 0-9 should receive.  The platform layer turns the plan into
 * posix_spawn file actions or a z/OS spawn() fd_map, so the parent never
 * dup2()s its own descriptors for an external command.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shell.h"

/* POSIX lets <limits.h> omit PIPE_BUF (z/OS does); 512 is the minimum
 * any system guarantees, so a write that small is still atomic. */
#ifndef PIPE_BUF
#ifdef _POSIX_PIPE_BUF
#define PIPE_BUF _POSIX_PIPE_BUF
#else
#define PIPE_BUF 512
#endif
#endif

/* ---- PATH hash ---- */

#define HBUCKETS 128

struct hent {
    struct hent *next;
    char *name;
    char *path;
    unsigned hits;
};

static struct hent *htab[HBUCKETS];

static unsigned hhash(const char *s) {
    unsigned h = 5381;

    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h & (HBUCKETS - 1);
}

void path_hash_clear(void) {
    int i;

    for (i = 0; i < HBUCKETS; i++) {
        struct hent *e = htab[i], *nx;

        for (; e; e = nx) {
            nx = e->next;
            free(e->name);
            free(e->path);
            free(e);
        }
        htab[i] = NULL;
    }
}

int path_hash_print(void) {
    int i;
    struct hent *e;

    for (i = 0; i < HBUCKETS; i++)
        for (e = htab[i]; e; e = e->next)
            printf("%u\t%s\n", e->hits, e->path);
    return 0;
}

static int is_exec(const char *p) {
    struct stat st;

    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0;
}

/*
 * Resolve a command name.  *err is set to 127 (not found) or 126 (found
 * but not executable) on failure.  Results are cached until PATH changes
 * or `hash -r`.
 */
const char *path_lookup(const char *name, int *err) {
    unsigned h;
    struct hent *e;
    const char *path, *p, *colon;
    char buf[PATH_MAX];
    int seen = 0;

    *err = 0;
    if (strchr(name, '/'))
        return name;
    h = hhash(name);
    for (e = htab[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) {
            e->hits++;
            return e->path;
        }
    if ((path = var_get("PATH")) == NULL)
        path = "/bin:/usr/bin";
    for (p = path;; p = colon + 1) {
        size_t dl, nl = strlen(name);

        colon = strchr(p, ':');
        dl = colon ? (size_t)(colon - p) : strlen(p);
        if (dl + nl + 2 <= sizeof(buf)) {
            if (dl == 0) {
                memcpy(buf, name, nl + 1);
            } else {
                memcpy(buf, p, dl);
                buf[dl] = '/';
                memcpy(buf + dl + 1, name, nl + 1);
            }
            if (is_exec(buf)) {
                e = xmalloc(sizeof(*e));
                e->name = xstrdup(name);
                e->path = xstrdup(buf);
                e->hits = 1;
                e->next = htab[h];
                htab[h] = e;
                return e->path;
            }
            if (access(buf, F_OK) == 0)
                seen = 1;
        }
        if (colon == NULL)
            break;
    }
    *err = seen ? 126 : 127;
    return NULL;
}

/* ---- functions ---- */

#define FBUCKETS 64

struct func {
    struct func *next;
    char *name;
    struct node *body;
};

static struct func *ftab[FBUCKETS];

static struct func **flookup(const char *name) {
    struct func **fp = &ftab[hhash(name) & (FBUCKETS - 1)];

    for (; *fp; fp = &(*fp)->next)
        if (strcmp((*fp)->name, name) == 0)
            break;
    return fp;
}

struct node *func_lookup(const char *name) {
    struct func *f = *flookup(name);

    return f ? f->body : NULL;
}

void func_define(const char *name, struct node *body) {
    struct func **fp = flookup(name), *f = *fp;

    if (f == NULL) {
        f = xmalloc(sizeof(*f));
        f->name = xstrdup(name);
        f->next = NULL;
        *fp = f;
    } else if (sh.funcdepth == 0) {
        node_free_heap(f->body);
    }
    /* else: may be executing; leak the old body rather than free it */
    f->body = node_copy_heap(body);
}

void func_foreach_prefix(const char *pre, void *cands) {
    size_t pl = strlen(pre);
    int i;
    struct func *f;

    for (i = 0; i < FBUCKETS; i++)
        for (f = ftab[i]; f; f = f->next)
            if (strncmp(f->name, pre, pl) == 0)
                complete_add(cands, f->name);
}

int func_unset(const char *name) {
    struct func **fp = flookup(name), *f = *fp;

    if (f == NULL)
        return 0;
    *fp = f->next;
    if (sh.funcdepth == 0)
        node_free_heap(f->body);
    free(f->name);
    free(f);
    return 0;
}

/* ---- redirection sources ---- */

static void cloexec(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int which_stream(int fd) {
    return fd >= 0 && fd <= 2 ? fd : -1;
}

/* Heredoc body to a pipe; large bodies are written by a child. */
static int heredoc_fd(struct redir *r) {
    const char *body = r->target ? r->target->text : "";
    size_t len;
    int pfd[2];

    if (!r->hd_quoted)
        body = expand_heredoc(body);
    len = strlen(body);
    if (sys_pipe(pfd) < 0)
        return -1;
    if (len <= PIPE_BUF) {
        if (len && write(pfd[1], body, len) < 0) {
            close(pfd[0]);
            close(pfd[1]);
            return -1;
        }
    } else {
        pid_t pid = fork();

        if (pid == 0) {
            close(pfd[0]);
            while (len > 0) {
                ssize_t w = write(pfd[1], body, len);

                if (w <= 0)
                    _exit(1);
                body += w;
                len -= (size_t)w;
            }
            _exit(0);
        }
        if (pid > 0)
            job_orphan(pid); /* reaped at the next prompt */
    }
    close(pfd[1]);
    return pfd[0];
}

/*
 * Resolve one redirection to a source fd.  Returns the fd, -2 for
 * "close the target", or -1 on error (message printed).  *owned is set
 * when the caller must close the returned fd.
 */
static int redir_source(struct redir *r, int *owned, const int *map) {
    const char *name;
    int fd, oflag = 0;

    *owned = 0;
    if (r->type == R_HEREDOC) {
        fd = heredoc_fd(r);
        if (fd < 0)
            sh_warn("heredoc: %s", strerror(errno));
        *owned = fd >= 0;
        return fd;
    }
    name = expand_str(r->target->text, EXP_TILDE);
    if (r->type == R_DUPIN || r->type == R_DUPOUT) {
        char *e;
        long n;

        if (strcmp(name, "-") == 0)
            return -2;
        n = strtol(name, &e, 10);
        if (*name == '\0' || *e != '\0' || n < 0 || n >= FDMAP_N) {
            sh_warn("%s: bad file descriptor", name);
            return -1;
        }
        fd = map ? map[n] : (int)n;
        if (fd < 0 || fcntl(fd, F_GETFD) < 0) {
            sh_warn("%ld: bad file descriptor", n);
            return -1;
        }
        return fd;
    }
    switch (r->type) {
    case R_IN: oflag = O_RDONLY; break;
    case R_OUT:
        oflag = O_WRONLY | O_CREAT | O_TRUNC | (sh.opt_C ? O_EXCL : 0);
        break;
    case R_CLOBBER: oflag = O_WRONLY | O_CREAT | O_TRUNC; break;
    case R_APPEND:  oflag = O_WRONLY | O_CREAT | O_APPEND; break;
    case R_RDWR:    oflag = O_RDWR | O_CREAT; break;
    default:        break;
    }
    fd = sys_open_redir(name, oflag, 0666, which_stream(r->fd));
    if (fd < 0) {
        sh_warn("%s: %s", name, strerror(errno));
        return -1;
    }
    cloexec(fd);
    *owned = 1;
    return fd;
}

/* ---- fd plans (for spawn) ---- */

int fdplan_build(struct fdplan *p, struct redir *r) {
    /* caller has run fdplan_init() and may have seeded pipe fds */
    for (; r; r = r->next) {
        int owned, fd;

        if (r->fd >= FDMAP_N) {
            sh_warn("%d: file descriptor out of range", r->fd);
            fdplan_release(p);
            return -1;
        }
        fd = redir_source(r, &owned, p->map);
        if (fd == -1) {
            fdplan_release(p);
            return -1;
        }
        if (owned)
            p->opened[p->nopened++] = fd;
        p->map[r->fd] = fd == -2 ? -1 : fd;
    }
    return 0;
}

void fdplan_init(struct fdplan *p) {
    int i;

    for (i = 0; i < FDMAP_N; i++)
        p->map[i] = i;
    p->nopened = 0;
    p->ignint = 0;
}

void fdplan_release(struct fdplan *p) {
    while (p->nopened > 0)
        close(p->opened[--p->nopened]);
}

/* ---- in-process redirection ---- */

struct redir_save {
    struct redir_save *next;
    int fd;
    int saved; /* -1: fd was closed before */
};

struct redir_save *redir_apply(struct redir *r, int *err) {
    struct redir_save *head = NULL;

    *err = 0;
    if (r == NULL)
        return NULL;
    fflush(stdout);
    fflush(stderr);
    for (; r; r = r->next) {
        int owned, fd;
        struct redir_save *s;

        /* settle a stdin script's offset while fd 0 is still it */
        if (r->fd == 0)
            in_sync();
        fd = redir_source(r, &owned, NULL);

        if (fd == -1) {
            *err = 1;
            break;
        }
        s = xmalloc(sizeof(*s));
        s->fd = r->fd;
        s->saved = fcntl(r->fd, F_DUPFD, 10);
        if (s->saved >= 0)
            cloexec(s->saved);
        s->next = head;
        head = s;
        if (fd == -2)
            close(r->fd);
        else if (fd != r->fd) {
            dup2(fd, r->fd);
            if (owned)
                close(fd);
        }
    }
    if (*err) {
        redir_restore(head);
        return NULL;
    }
    return head;
}

void redir_restore(struct redir_save *s) {
    if (s == NULL)
        return;
    fflush(stdout);
    fflush(stderr);
    while (s) {
        struct redir_save *nx = s->next;

        if (s->saved >= 0) {
            dup2(s->saved, s->fd);
            close(s->saved);
        } else
            close(s->fd);
        free(s);
        s = nx;
    }
}

/* Make in-process redirections permanent (`exec >file`). */
void redir_commit(struct redir_save *s) {
    while (s) {
        struct redir_save *nx = s->next;

        if (s->saved >= 0)
            close(s->saved);
        free(s);
        s = nx;
    }
}
