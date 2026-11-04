/*
 * Replaces NetBSD's lib/libedit/config.h for the zash port.  Picks the
 * feature set by platform instead of running configure.
 */
#ifndef ZASH_LIBEDIT_CONFIG_H
#define ZASH_LIBEDIT_CONFIG_H

#include "port.h"

#define HAVE_GETPW_R_POSIX 1
#define HAVE_ISSETUGID 1        /* compat.c */
/* struct dirent has no d_namlen on Linux or z/OS */

#if defined(__MVS__)
/*
 * z/OS: X/Open curses in /usr/lib (or zopen ncurses).  No <termcap.h>
 * or <sys/cdefs.h>; the termcap entry points come from <term.h>.
 * VERIFY on z/OS.
 */
#define HAVE_CURSES_H 1
#define HAVE_TERM_H 1
#else
/* Linux, BSD, macOS: ncurses provides <termcap.h> */
#define HAVE_SYS_CDEFS_H 1
#define HAVE_CURSES_H 1
#define HAVE_TERMCAP_H 1
#endif

#include "sys.h"

#endif /* ZASH_LIBEDIT_CONFIG_H */
