/*
 * Platform layer for POSIX systems (Linux, BSD, macOS): posix_spawn().
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

static const char *self_path = "/bin/sh";

void sys_init(void) {
    int err;
    const char *p;

    if (sh.argv0 &&
        (p = path_lookup(sh.argv0[0] == '-' ? sh.argv0 + 1 : sh.argv0, &err)) !=
            NULL) {
        char *r = realpath(p, NULL);

        self_path = r ? r : xstrdup(p);
    }
}

/*
 * Translate an fd plan into file actions.  Sources that are themselves
 * child targets (e.g. 2>&1 1>file, or a swap) are first copied above
 * every source so later dup2()s cannot clobber them.
 */
static int build_actions(posix_spawn_file_actions_t *fa,
                         const struct fdplan *fp) {
    int i, maxfd = FDMAP_N - 1, tmp[FDMAP_N], rc = 0;

    for (i = 0; i < FDMAP_N; i++)
        if (fp->map[i] > maxfd)
            maxfd = fp->map[i];
    for (i = 0; i < FDMAP_N; i++) {
        int src = fp->map[i];

        tmp[i] = -1;
        if (src >= 0 && src != i && src < FDMAP_N) {
            tmp[i] = maxfd + 1 + i;
            rc |= posix_spawn_file_actions_adddup2(fa, src, tmp[i]);
        }
    }
    for (i = 0; i < FDMAP_N; i++) {
        int src = fp->map[i];

        if (src < 0) {
            if (fcntl(i, F_GETFD) >= 0)
                rc |= posix_spawn_file_actions_addclose(fa, i);
        } else if (src != i)
            rc |= posix_spawn_file_actions_adddup2(
                fa, tmp[i] >= 0 ? tmp[i] : src, i);
    }
    for (i = 0; i < FDMAP_N; i++)
        if (tmp[i] >= 0)
            rc |= posix_spawn_file_actions_addclose(fa, tmp[i]);
    return rc;
}

static pid_t do_spawn(const char *path, char **argv, char **envp,
                      const struct fdplan *fp) {
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    sigset_t def, none;
    struct sigaction ign, oint, oquit;
    pid_t pid = -1;
    int rc;

    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    if (build_actions(&fa, fp) != 0) {
        rc = errno ? errno : EINVAL;
        goto out;
    }
    sigemptyset(&none);
    sigemptyset(&def);
    if (!fp->ignint) {
        sigaddset(&def, SIGINT);
        sigaddset(&def, SIGQUIT);
    }
    sigaddset(&def, SIGTERM);
    sigaddset(&def, SIGTSTP);
    sigaddset(&def, SIGTTIN);
    sigaddset(&def, SIGTTOU);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setflags(&at,
                             POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

    /* posix_spawn cannot request SIG_IGN; inherit it instead */
    if (fp->ignint) {
        memset(&ign, 0, sizeof(ign));
        ign.sa_handler = SIG_IGN;
        sigaction(SIGINT, &ign, &oint);
        sigaction(SIGQUIT, &ign, &oquit);
    }
    rc = posix_spawn(&pid, path, &fa, &at, argv, envp);
    if (fp->ignint) {
        sigaction(SIGINT, &oint, NULL);
        sigaction(SIGQUIT, &oquit, NULL);
    }
out:
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return pid;
}

pid_t sys_spawn(const char *path, char **argv, char **envp,
                const struct fdplan *fp) {
    pid_t pid = do_spawn(path, argv, envp, fp);
    size_t n;
    char **nv;

    if (pid >= 0 || errno != ENOEXEC)
        return pid;
    /* script without #!: run it with this shell (POSIX 2.9.1.1) */
    for (n = 0; argv[n]; n++)
        ;
    nv = aalloc((n + 2) * sizeof(char *));
    nv[0] = (char *)self_path;
    nv[1] = (char *)path;
    memcpy(nv + 2, argv + 1, n * sizeof(char *));
    return do_spawn(self_path, nv, envp, fp);
}

/* pipe() with both ends close-on-exec (the fd plan passes them on). */
int sys_pipe(int pfd[2]) {
    if (pipe(pfd) < 0)
        return -1;
    fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

int sys_open_redir(const char *name, int oflag, int mode, int which) {
    (void)which;
    return open(name, oflag, mode);
}

void sys_tag_new_file(int fd, int which) {
    (void)fd;
    (void)which;
}

/* Safe to over-read and lseek back: regular files only (a tty can
 * report a successful lseek on Linux, so don't test with lseek). */
int sys_input_seekable(int fd) {
    struct stat st;

    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}
