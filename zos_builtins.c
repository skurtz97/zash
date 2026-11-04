/*
 * z/OS builtins: tso, extattr, chtag.
 *
 * Each is a builtin so common cases avoid a spawn; any form not handled
 * in-process is deferred to the system utility of the same name.
 * Off z/OS they report that they are unavailable.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

#ifdef __MVS__
#include <_Ccsid.h> /* __toCSName(); VERIFY header name */

/* Run argv as an external command, bypassing the builtin of that name. */
static int defer(const char *util, char **argv) {
    struct fdplan fp;
    const char *path;
    char *save = argv[0];
    int err, st;
    pid_t pid;

    if ((path = path_lookup(util, &err)) == NULL) {
        sh_warn("%s: not found", util);
        return err;
    }
    fdplan_init(&fp);
    fflush(stdout);
    in_sync();
    argv[0] = (char *)util;
    pid = sys_spawn(path, argv, var_environ(), &fp);
    argv[0] = save;
    if (pid < 0) {
        sh_warn("%s: %s", util, strerror(errno));
        return 126;
    }
    st = job_wait_pid(pid);
    return st;
}

/*
 * tso: run a TSO/E command.  Defers to tsocmd(1) for now.
 * TODO: in-process TSO via the TSO/E environment service (IKJTSOEV) to
 * avoid a spawn per command; needs care with ASCII mode and
 * authorization.
 */
int b_tso(int argc, char **argv) {
    if (argc < 2) {
        sh_warn("usage: tso command");
        return 2;
    }
    return defer("tsocmd", argv);
}

/*
 * extattr: TODO in-process chattr() for the common +a/-a, +p/-p, +s/-s,
 * +l/-l forms; defers to /bin/extattr until then.
 */
int b_extattr(int argc, char **argv) {
    (void)argc;
    return defer("/bin/extattr", argv);
}

static void print_tag(const char *file) {
    struct stat st;
    char name[64];

    if (stat(file, &st) < 0) {
        sh_warn("chtag: %s: %s", file, strerror(errno));
        return;
    }
    if (st.st_tag.ft_ccsid == 0) {
        printf("- untagged    T=off %s\n", file);
        return;
    }
    if (st.st_tag.ft_ccsid == 65535) {
        printf("b binary      T=off %s\n", file);
        return;
    }
    if (__toCSName(st.st_tag.ft_ccsid, name) != 0)
        snprintf(name, sizeof(name), "%u", (unsigned)st.st_tag.ft_ccsid);
    printf("%c %-11s T=%s %s\n", st.st_tag.ft_txtflag ? 't' : 'm', name,
           st.st_tag.ft_txtflag ? "on " : "off", file);
}

/* chtag: native fast path for `chtag -p file...`; all else deferred. */
int b_chtag(int argc, char **argv) {
    int i;

    if (argc >= 3 && strcmp(argv[1], "-p") == 0) {
        for (i = 2; i < argc; i++)
            if (argv[i][0] == '-')
                return defer("/bin/chtag", argv);
        for (i = 2; i < argc; i++)
            print_tag(argv[i]);
        return 0;
    }
    return defer("/bin/chtag", argv);
}

#else /* !__MVS__ */

static int unavailable(char **argv) {
    sh_warn("%s: only available on z/OS", argv[0]);
    return 2;
}

int b_tso(int argc, char **argv) {
    (void)argc;
    return unavailable(argv);
}
int b_extattr(int argc, char **argv) {
    (void)argc;
    return unavailable(argv);
}
int b_chtag(int argc, char **argv) {
    (void)argc;
    return unavailable(argv);
}

#endif
