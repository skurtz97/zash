/*
 * Platform layer for z/OS UNIX System Services.
 *
 * NOT YET COMPILED ON z/OS.  Everything here is written against the
 * documented LE C runtime and BPX services but must be verified with
 * IBM Open XL C/C++ on a real system.  Items marked VERIFY are the ones
 * most likely to need adjustment.
 *
 *  - Process creation uses spawn() (BPX1SPN) with an fd_map, never
 *    fork()+exec().  _BPX_SHAREAS and STEPLIB are honored by spawn from
 *    the environment we pass, so `export _BPX_SHAREAS=YES` gives local
 *    (same address space) spawns with no shell code involved.
 *  - _TAG_REDIR_IN/OUT/ERR implement the z/OS sh redirection tagging
 *    rules (z/OS UNIX Command Reference, sh, "Shell variables for
 *    automatic conversion").
 *  - Redirection to MVS data sets ("//'HLQ.DS'", "//DD:NAME") goes
 *    through a pump process, since open() cannot open data sets.
 */
#ifndef __MVS__
#error "sys_zos.c is for z/OS; build with SYSOBJ=sys_posix.o elsewhere"
#endif

#define _OPEN_SYS_FILE_EXT 1 /* struct file_tag, F_SETTAG, F_CONTROL_CVT */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h> /* z/OS spawn(), struct inheritance */
#include <stdio.h>
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
 * spawn() takes the child fd table directly: fd_map[i] is the parent fd
 * that becomes child fd i, or SPAWN_FDCLOSED.  No dup2 ordering issues
 * as with posix_spawn file actions.  Fds >= fd_count are not inherited.
 * VERIFY: SPAWN_FDCLOSED semantics, and that fds >= fd_count are closed
 * (we rely on that instead of FD_CLOEXEC for shell-private fds).
 */
static pid_t do_spawn(const char *path, char **argv, char **envp,
                      const struct fdplan *fp) {
    struct inheritance inh;
    int map[FDMAP_N], i;

    for (i = 0; i < FDMAP_N; i++) {
        int src = fp->map[i];

        map[i] = src >= 0 && fcntl(src, F_GETFD) >= 0 ? src : SPAWN_FDCLOSED;
    }
    memset(&inh, 0, sizeof(inh));
    inh.flags = SPAWN_SETSIGMASK | SPAWN_SETSIGDEF;
    sigemptyset(&inh.sigmask);
    sigemptyset(&inh.sigdefault);
    if (!fp->ignint) {
        sigaddset(&inh.sigdefault, SIGINT);
        sigaddset(&inh.sigdefault, SIGQUIT);
    }
    sigaddset(&inh.sigdefault, SIGTERM);
    sigaddset(&inh.sigdefault, SIGTSTP);
    sigaddset(&inh.sigdefault, SIGTTIN);
    sigaddset(&inh.sigdefault, SIGTTOU);
    /*
	 * VERIFY: in -fzos-le-char-mode=ascii builds, confirm spawn()
	 * resolves to the ASCII entry point and converts path/argv/envp.
	 */
    return spawn(path, FDMAP_N, map, &inh, (const char **)argv,
                 (const char **)envp);
}

pid_t sys_spawn(const char *path, char **argv, char **envp,
                const struct fdplan *fp) {
    pid_t pid = do_spawn(path, argv, envp, fp);
    size_t n;
    char **nv;

    /*
	 * With _BPX_SPAWN_SCRIPT=YES spawn handles #! and plain scripts
	 * itself; otherwise ENOEXEC comes back and we run the script with
	 * this shell, matching POSIX 2.9.1.1.
	 */
    if (pid >= 0 || errno != ENOEXEC)
        return pid;
    for (n = 0; argv[n]; n++)
        ;
    nv = aalloc((n + 2) * sizeof(char *));
    nv[0] = (char *)self_path;
    nv[1] = (char *)path;
    memcpy(nv + 2, argv + 1, n * sizeof(char *));
    return do_spawn(self_path, nv, envp, fp);
}

/* ---- file tagging ---- */

static int tagvar(const char *name) {
    const char *v = var_get(name);

    if (v == NULL)
        return 0;
    if (strcmp(v, "TXT") == 0)
        return 'T';
    if (strcmp(v, "BIN") == 0)
        return 'B';
    return 0;
}

/*
 * _TAG_REDIR_OUT/ERR: tag the target, if not already tagged, as
 * TXTFLAG=ON|OFF with CCSID = program CCSID at first write.  That is a
 * deferred tag: ft_ccsid 0 with ft_deferred set.  VERIFY field names.
 */
void sys_tag_new_file(int fd, int which) {
    const char *var = which == 1   ? "_TAG_REDIR_OUT"
                      : which == 2 ? "_TAG_REDIR_ERR"
                                   : NULL;
    struct stat st;
    struct file_tag tag;
    int mode;

    if (var == NULL || (mode = tagvar(var)) == 0)
        return;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_tag.ft_ccsid != 0)
        return; /* only untagged regular files */
    memset(&tag, 0, sizeof(tag));
    tag.ft_ccsid = 0;
    tag.ft_txtflag = mode == 'T';
    tag.ft_deferred = 1;
    fcntl(fd, F_SETTAG, &tag);
}

/*
 * _TAG_REDIR_IN=TXT: treat input as TXTFLAG=ON with its existing CCSID,
 * i.e. turn auto-conversion on for this open file; BIN turns it off.
 * VERIFY: struct f_cnvrt / SETCVTON / SETCVTOFF usage.
 */
static void tag_redir_in(int fd) {
    struct stat st;
    struct f_cnvrt cv;
    int mode = tagvar("_TAG_REDIR_IN");

    if (mode == 0 || fstat(fd, &st) < 0)
        return;
    memset(&cv, 0, sizeof(cv));
    if (mode == 'T') {
        if (st.st_tag.ft_ccsid == 0)
            return; /* "no effect if CCSID = 0" */
        cv.cvtcmd = SETCVTON;
        cv.pccsid = 0; /* program CCSID */
        cv.fccsid = st.st_tag.ft_ccsid;
    } else
        cv.cvtcmd = SETCVTOFF;
    fcntl(fd, F_CONTROL_CVT, &cv);
}

/* ---- MVS data sets ---- */

static int is_dataset(const char *name) {
    return name[0] == '/' && name[1] == '/' &&
           (name[2] == '\'' || strncmp(name + 2, "DD:", 3) == 0);
}

/*
 * A pump child copies between a pipe and the data set opened with
 * fopen() in record (text) mode.  This costs a fork; a future version
 * could spawn a small helper or use a thread instead.  Limitation: the
 * shell does not wait for an output pump before the next command, so
 * `cmd >//'DS'; cat "//'DS'"` can race.
 */
static int open_dataset(const char *name, int oflag) {
    int pfd[2], writing = (oflag & O_ACCMODE) != O_RDONLY;
    const char *mode = !writing ? "r" : (oflag & O_APPEND) ? "a" : "w";
    FILE *f;
    pid_t pid;

    if ((f = fopen(name, mode)) == NULL)
        return -1;
    if (pipe(pfd) < 0) {
        fclose(f);
        return -1;
    }
    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        fclose(f);
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        char buf[4096];
        ssize_t n;
        size_t r;

        if (writing) {
            close(pfd[1]);
            while ((n = read(pfd[0], buf, sizeof(buf))) > 0)
                fwrite(buf, 1, (size_t)n, f);
        } else {
            close(pfd[0]);
            while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
                if (write(pfd[1], buf, r) < 0)
                    break;
        }
        _exit(fclose(f) == 0 ? 0 : 1);
    }
    fclose(f);
    job_orphan(pid);
    if (writing) {
        close(pfd[0]);
        return pfd[1];
    }
    close(pfd[1]);
    return pfd[0];
}

int sys_open_redir(const char *name, int oflag, int mode, int which) {
    int fd;

    if (is_dataset(name))
        return open_dataset(name, oflag);
    fd = open(name, oflag, mode);
    if (fd < 0)
        return fd;
    if (which == 0)
        tag_redir_in(fd);
    else if (oflag & (O_WRONLY | O_RDWR))
        sys_tag_new_file(fd, which);
    return fd;
}

/*
 * As on POSIX, but also refuse when auto-conversion is active on the
 * fd: lseek offsets count file bytes, and with a multibyte conversion
 * (e.g. a UTF-8 tagged file read by an EBCDIC program) the converted
 * byte count differs, so seeking back would land mid-line.
 * VERIFY: QUERYCVT usage and the "conversion off" result value.
 */
int sys_input_seekable(int fd) {
    struct stat st;
    struct f_cnvrt cv;

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    memset(&cv, 0, sizeof(cv));
    cv.cvtcmd = QUERYCVT;
    if (fcntl(fd, F_CONTROL_CVT, &cv) == 0 && cv.cvtcmd != SETCVTOFF)
        return 0;
    return 1;
}
