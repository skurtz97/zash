/*
 * zash port of NetBSD libedit: definitions shared by every vendored
 * file.  Reached through config.h (libedit sources), namespace.h
 * (vis.c, unvis.c) and nbtool_config.h (strlcpy.c, strlcat.c), so the
 * upstream files under netbsd/ compile unmodified.
 */
#ifndef ZASH_LIBEDIT_PORT_H
#define ZASH_LIBEDIT_PORT_H

/*
 * libedit's key maps (map.c) are 256-entry tables indexed by ASCII
 * codes.  An EBCDIC build would bind the wrong keys, so refuse it.
 */
#if 'A' != 0x41 || '\n' != 0x0a
#error "libedit requires an ASCII build (z/OS: -fzos-le-char-mode=ascii or -qascii)"
#endif

/* NetBSD headers pull these in implicitly (chartype.c needs uintptr_t) */
#include <stdint.h>

/* NetBSD <sys/cdefs.h> macros used by vis.h, vis.c and unvis.c */
#ifndef __arraycount
#define __arraycount(a) (sizeof(a) / sizeof(*(a)))
#endif
#ifndef __RENAME
#define __RENAME(x)
#endif
/*
 * Upstream writes `__RCSID("$NetBSD...$");` at file scope.  An empty
 * expansion leaves a stray ';' (a -pedantic warning); a forward struct
 * declaration absorbs it legally and emits nothing.
 */
#ifndef __RCSID
#define __RCSID(x) struct libedit_rcsid_unused
#endif
#ifndef _DIAGASSERT
#define _DIAGASSERT(x) ((void)0)
#endif

/*
 * Vendored libc helpers.  HAVE_* = 0 makes the NetBSD files compile
 * their own implementations; HAVE_NBTOOL_CONFIG_H selects NetBSD's
 * host-tools (non-NetBSD) code paths.
 */
#undef  HAVE_VIS
#define HAVE_VIS 0
#undef  HAVE_SVIS
#define HAVE_SVIS 0
#undef  HAVE_STRLCPY
#define HAVE_STRLCPY 0
#undef  HAVE_STRLCAT
#define HAVE_STRLCAT 0
#ifndef HAVE_NBTOOL_CONFIG_H
#define HAVE_NBTOOL_CONFIG_H 1
#endif

/*
 * Every helper gets a libedit_ prefix so it can never collide with a
 * libc that has its own (glibc >= 2.38 has strlcpy; macOS has vis).
 */
#define strlcpy     libedit_strlcpy
#define strlcat     libedit_strlcat
#define vis         libedit_vis
#define nvis        libedit_nvis
#define svis        libedit_svis
#define snvis       libedit_snvis
#define strvis      libedit_strvis
#define stravis     libedit_stravis
#define strnvis     libedit_strnvis
#define strsvis     libedit_strsvis
#define strsnvis    libedit_strsnvis
#define strvisx     libedit_strvisx
#define strnvisx    libedit_strnvisx
#define strenvisx   libedit_strenvisx
#define strsvisx    libedit_strsvisx
#define strsnvisx   libedit_strsnvisx
#define strsenvisx  libedit_strsenvisx
#define strunvis    libedit_strunvis
#define strnunvis   libedit_strnunvis
#define strunvisx   libedit_strunvisx
#define strnunvisx  libedit_strnunvisx
#define unvis       libedit_unvis

#include <stddef.h>
size_t libedit_strlcpy(char *, const char *, size_t);
size_t libedit_strlcat(char *, const char *, size_t);

/*
 * Functions not every target libc has (compat.c).  issetugid() is BSD;
 * without it el_source() refuses to read ~/.editrc.  getline() and
 * wcsdup() are POSIX.1-2008, which older z/OS releases may lack, so on
 * z/OS (and with -DLIBEDIT_COMPAT_ALL, to test them elsewhere) the
 * compat versions are used under their own names.
 */
#define issetugid   libedit_issetugid
int libedit_issetugid(void);

#if defined(__MVS__) || defined(LIBEDIT_COMPAT_ALL)
#include <stdio.h>
#include <wchar.h>
#define getline     libedit_getline
#define wcsdup      libedit_wcsdup
ssize_t libedit_getline(char **, size_t *, FILE *);
wchar_t *libedit_wcsdup(const wchar_t *);
#define LIBEDIT_NEED_GETLINE 1
#define LIBEDIT_NEED_WCSDUP 1
#endif

/* xlc has no visibility attribute; sys.h only defines it away for
 * non-GNU compilers, which covers xlc but say so explicitly. */
#if defined(__IBMC__) && !defined(__clang__)
#define __attribute__(x)
#endif

#endif /* ZASH_LIBEDIT_PORT_H */
