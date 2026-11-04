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
 *  - File and pipe tagging follow the build's charset: ASCII builds use
 *    the zopen (zoslib) convention (ISO8859-1 pipes, _ENCODE_FILE_NEW
 *    for new files); EBCDIC builds use the z/OS sh _TAG_REDIR_IN/OUT/ERR
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

/*
 * ASCII-mode builds (-qascii, -fzos-le-char-mode=ascii) compile 'A' as
 * 0x41.  New files follow the zopen (zoslib) convention and take the
 * tag named by _ENCODE_FILE_NEW.  Pipes are tagged IBM-1047, unlike
 * zoslib's ISO8859-1 (see sys_pipe).
 * EBCDIC builds follow z/OS /bin/sh: pipes stay untagged and new files
 * are tagged only as _TAG_REDIR_OUT/_TAG_REDIR_ERR ask.
 */
#define ASCII_BUILD ('A' == 0x41)

#define CCSID_ISO8859_1 819
#define CCSID_IBM_1047 1047
#define CCSID_BINARY 65535

static void set_tag(int fd, unsigned short ccsid, int txt, int deferred) {
    struct file_tag tag;

    memset(&tag, 0, sizeof(tag));
    tag.ft_ccsid = ccsid;
    tag.ft_txtflag = txt;
    tag.ft_deferred = deferred;
    fcntl(fd, F_SETTAG, &tag);
}

/*
 * pipe() with both ends close-on-exec.  In ASCII builds the pipe is
 * tagged IBM-1047 text, so it always carries EBCDIC: zash's ASCII
 * writes are converted on the way in and its reads on the way out,
 * while EBCDIC programs at either end pass bytes unconverted.  With
 * zoslib's ISO8859-1 tag, EBCDIC writers (sh) are converted to ASCII
 * but EBCDIC readers (od, sed) are not, so /bin pipelines broke.
 * Measured with tools/cvtprobe.c.
 */
int sys_pipe(int pfd[2]) {
    if (pipe(pfd) < 0)
        return -1;
    fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    if (ASCII_BUILD)
        set_tag(pfd[1], CCSID_IBM_1047, 1, 0);
    return 0;
}

static int tagvar(const char *name) {
    const char *v = var_get(name);

    if (v == NULL)
        return 0;
    if (strcmp(v, "TXT") == 0 || strcmp(v, "txt") == 0)
        return 'T';
    if (strcmp(v, "BIN") == 0 || strcmp(v, "bin") == 0)
        return 'B';
    return 0;
}

/*
 * zoslib's _ENCODE_FILE_NEW: ISO8859-1 (the default), IBM-1047 or
 * BINARY.  Anything else means the default.
 */
static void encode_file_new(int fd) {
    const char *v = var_get("_ENCODE_FILE_NEW");

    if (v != NULL && strcmp(v, "IBM-1047") == 0)
        set_tag(fd, CCSID_IBM_1047, 1, 0);
    else if (v != NULL && strcmp(v, "BINARY") == 0)
        set_tag(fd, CCSID_BINARY, 0, 0);
    else
        set_tag(fd, CCSID_ISO8859_1, 1, 0);
}

/*
 * Tag a file opened for writing if it is an empty, untagged regular
 * file, i.e. one this redirection created or truncated.
 *
 * EBCDIC builds: _TAG_REDIR_OUT/ERR give TXTFLAG=ON|OFF with the CCSID
 * of the program that writes first, a deferred tag (ft_ccsid 0 with
 * ft_deferred set).  ASCII builds: _ENCODE_FILE_NEW, for every fd.
 */
void sys_tag_new_file(int fd, int which) {
    struct stat st;
    int mode = 0;

    if (!ASCII_BUILD) {
        const char *var = which == 1   ? "_TAG_REDIR_OUT"
                          : which == 2 ? "_TAG_REDIR_ERR"
                                       : NULL;

        if (var == NULL || (mode = tagvar(var)) == 0)
            return;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_tag.ft_ccsid != 0 ||
        st.st_size != 0)
        return;
    if (ASCII_BUILD)
        encode_file_new(fd);
    else
        set_tag(fd, 0, mode == 'T', 1);
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

/* Single-byte code pages: converted and file offsets agree. */
static int sbcs(int ccsid) {
    switch (ccsid) {
    case 37:
    case 273:
    case 277:
    case 278:
    case 280:
    case 284:
    case 285:
    case 297:
    case 500:
    case 819:
    case 871:
    case 1047:
    case 1252: return 1;
    default:   return ccsid >= 1140 && ccsid <= 1149;
    }
}

/*
 * As on POSIX, but with auto-conversion active only when both sides
 * are single-byte: lseek offsets count file bytes, and with a multibyte
 * conversion (e.g. a UTF-8 tagged file read by an EBCDIC program) the
 * converted byte count differs, so seeking back would land mid-line.
 */
int sys_input_seekable(int fd) {
    struct stat st;
    struct f_cnvrt cv;

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    memset(&cv, 0, sizeof(cv));
    cv.cvtcmd = QUERYCVT;
    if (fcntl(fd, F_CONTROL_CVT, &cv) == 0 && cv.cvtcmd != SETCVTOFF)
        return sbcs(cv.pccsid) && sbcs(cv.fccsid);
    return 1;
}

/*
 * zoslib's UNTAGGED_READ_MODE=AUTO for files zash reads as shell input
 * (scripts, . files, HISTFILE, fc's edited file): an ASCII build guesses
 * the charset of an untagged file and, if it looks like EBCDIC, turns on
 * IBM-1047 conversion.  Newlines decide most files (0x0A vs 0x15);
 * otherwise printable ASCII is counted against EBCDIC letters, digits
 * and spaces.  Redirections are left alone: children share their
 * conversion state.
 */
void sys_input_text(int fd) {
    struct stat st;
    struct f_cnvrt cv;
    unsigned char buf[4096];
    long a = 0, e = 0;
    ssize_t n, i;

    if (!ASCII_BUILD || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_tag.ft_ccsid != 0)
        return;
    if ((n = pread(fd, buf, sizeof(buf), 0)) <= 0)
        return;
    for (i = 0; i < n; i++) {
        int c = buf[i];

        if (c == 0x0A)
            a += 8;
        else if (c == 0x15)
            e += 8;
        else if (c >= 0x81 && c <= 0xF9)
            e++; /* EBCDIC letters and digits; never ASCII text */
        else if (c == 0x40)
            e++;
        else if ((c >= 0x20 && c < 0x7F) || c == '\t')
            a++;
    }
    if (e <= a)
        return;
    memset(&cv, 0, sizeof(cv));
    cv.cvtcmd = SETCVTON;
    cv.pccsid = CCSID_ISO8859_1;
    cv.fccsid = CCSID_IBM_1047;
    fcntl(fd, F_CONTROL_CVT, &cv);
}
