/*
 * cvtprobe: how z/OS autoconversion treats descriptors that an
 * ASCII-mode program shares with EBCDIC children (pipes, inherited
 * files).  It decides how an ASCII zash must set up pipes and fds.
 *
 * Build in ASCII mode, the way zash's default z/OS build is:
 *   xlc -q64 -qxplink -qascii -D_ENHANCED_ASCII_EXT=0xFFFFFFFF \
 *       -o cvtprobe tools/cvtprobe.c
 * Run from a terminal: ./cvtprobe
 *
 * "hello" is 68 65 6c 6c 6f in ASCII and 88 85 93 93 96 in EBCDIC.
 */
#define _XOPEN_SOURCE 600
#define _OPEN_SYS_FILE_EXT 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __MVS__
#error "cvtprobe is for z/OS only"
#endif

static const char *cvtname(int c) {
    switch (c) {
    case SETCVTOFF:    return "off";
    case SETCVTON:     return "on";
    case SETAUTOCVTON: return "auto";
#ifdef SETCVTALL
    case SETCVTALL: return "all";
#endif
#ifdef SETAUTOCVTALL
    case SETAUTOCVTALL: return "autoall";
#endif
    default: return "?";
    }
}

static void query(const char *what, int fd) {
    struct stat st;
    struct f_cnvrt cv;

    if (fstat(fd, &st) == -1) {
        printf("  %-26s fstat: %s\n", what, strerror(errno));
        return;
    }
    cv.cvtcmd = QUERYCVT;
    cv.pccsid = 0;
    cv.fccsid = 0;
    if (fcntl(fd, F_CONTROL_CVT, &cv) == -1)
        printf("  %-26s tag=%u txt=%u cvt: %s\n", what,
               (unsigned)st.st_tag.ft_ccsid, (unsigned)st.st_tag.ft_txtflag,
               strerror(errno));
    else
        printf("  %-26s tag=%u txt=%u cvt=%s pccsid=%d fccsid=%d\n", what,
               (unsigned)st.st_tag.ft_ccsid, (unsigned)st.st_tag.ft_txtflag,
               cvtname(cv.cvtcmd), cv.pccsid, cv.fccsid);
}

static void settag(int fd, unsigned short ccsid) {
    struct file_tag t;

    memset(&t, 0, sizeof t);
    t.ft_ccsid = ccsid;
    t.ft_txtflag = ccsid != 0;
    if (fcntl(fd, F_SETTAG, &t) == -1)
        printf("  F_SETTAG %u: %s\n", (unsigned)ccsid, strerror(errno));
}

/* Run script under /bin/sh (EBCDIC) with stdin/stdout replaced. */
static void run(const char *script, int in, int out, int closefd) {
    pid_t pid;
    int st;

    fflush(stdout);
    pid = fork();
    if (pid == -1) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        if (in >= 0)
            dup2(in, 0);
        if (out >= 0)
            dup2(out, 1);
        if (closefd >= 0)
            close(closefd);
        execl("/bin/sh", "sh", "-c", script, (char *)0);
        _exit(127);
    }
    while (waitpid(pid, &st, 0) == -1 && errno == EINTR)
        ;
}

static void hex(const char *label, const unsigned char *b, ssize_t n) {
    ssize_t i;

    printf("  %-26s", label);
    for (i = 0; i < n; i++)
        printf(" %02x", b[i]);
    if (n > 0 && b[0] == 0x68)
        printf("  ASCII");
    else if (n > 0 && b[0] == 0x88)
        printf("  EBCDIC");
    printf("\n");
}

static const char *const tagname[] = {"untagged", "tagged 819", "tagged 1047"};
static const unsigned short tagval[] = {0, 819, 1047};

int main(void) {
    char path[64];
    unsigned char buf[64];
    ssize_t n;
    int i, p[2], fd, rfd;
    struct f_cnvrt off = {SETCVTOFF, 0, 0};

    printf("environment: _BPXK_AUTOCVT=%s _CEE_RUNOPTS=%s\n",
           getenv("_BPXK_AUTOCVT") ? getenv("_BPXK_AUTOCVT") : "(unset)",
           getenv("_CEE_RUNOPTS") ? getenv("_CEE_RUNOPTS") : "(unset)");

    printf("\nA. my standard descriptors\n");
    query("fd 0", 0);
    query("fd 1", 1);
    query("fd 2", 2);

    printf("\nB. I write \"hello\" into a pipe; EBCDIC od reads it\n");
    for (i = 0; i < 3; i++) {
        if (pipe(p) == -1) {
            perror("pipe");
            return 1;
        }
        if (tagval[i])
            settag(p[1], tagval[i]);
        printf("  pipe %s:\n", tagname[i]);
        query("  read end", p[0]);
        query("  write end", p[1]);
        write(p[1], "hello\n", 6);
        close(p[1]);
        printf("  %-26s", "  od sees");
        fflush(stdout);
        run("od -An -tx1 | head -1 >/dev/tty", p[0], -1, -1);
        close(p[0]);
    }

    printf("\nC. EBCDIC echo writes \"hello\" into a pipe; I read it\n");
    for (i = 0; i < 3; i++) {
        if (pipe(p) == -1) {
            perror("pipe");
            return 1;
        }
        if (tagval[i])
            settag(p[1], tagval[i]);
        run("echo hello", -1, p[1], p[0]);
        close(p[1]);
        n = read(p[0], buf, sizeof buf);
        close(p[0]);
        hex(tagname[i], buf, n);
    }

    printf("\nD. a file I open (as zash does for >file); EBCDIC echo "
           "writes to it\n");
    for (i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "/tmp/cvtprobe.%ld.%d", (long)getpid(), i);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd == -1) {
            perror(path);
            return 1;
        }
        if (i == 1)
            write(fd, "x\n", 2);
        query(i ? "after I write first" : "fresh, before echo", fd);
        run("echo hello", -1, fd, -1);
        query("after echo", fd);
        close(fd);
        rfd = open(path, O_RDONLY);
        fcntl(rfd, F_CONTROL_CVT, &off);
        n = read(rfd, buf, sizeof buf);
        close(rfd);
        unlink(path);
        hex("raw bytes in file", buf, n);
    }
    return 0;
}
