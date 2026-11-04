/*
 * zash port of NetBSD libedit: definitions shared by every vendored
 * file.  Reached through config.h (libedit sources), namespace.h
 * (vis.c, unvis.c) and nbtool_config.h (strlcpy.c, strlcat.c).  The
 * upstream files are unmodified except for ../patches/zash.patch,
 * which makes them use the character macros below.
 */
#ifndef ZASH_LIBEDIT_PORT_H
#define ZASH_LIBEDIT_PORT_H

/*
 * Character sets.  libedit is written for ASCII: map.c's 256-entry key
 * tables are indexed by ASCII code, and control characters are made
 * with arithmetic (c & 037, c | 0100, 0177, 033, meta = c | 0200).
 *
 * In an EBCDIC build (z/OS, IBM-1047) narrow literals, wide literals
 * and the C library all use native EBCDIC values ('a' == L'a' ==
 * mbrtowc("a") == 0x81, as measured by port/probe), so libedit runs on
 * native characters and only that ASCII arithmetic is translated, via
 * these macros:
 *
 *   EL_A2N(c)    ASCII code -> native char      (runtime, c < 256)
 *   EL_N2A(c)    native char -> ASCII code      (runtime, c < 256)
 *   EL_A2N_C(c)  same as EL_A2N for ASCII controls, as a constant
 *   EL_CTRL_C(c) control char for a letter or @[\]^_ literal (constant)
 *   EL_UNCTRL(c) printable form of a control char (^A -> A)
 *   EL_META(c,m) set the meta bit m on c
 *   EL_ESC, EL_DEL, EL_WESC (wide string for ESC)
 *
 * In an ASCII build each is the identity or the original expression.
 */
/* & 0xff: character constants are negative where char is signed */
#if ('A' & 0xff) == 0x41
#define LIBEDIT_EBCDIC 0
#elif ('A' & 0xff) == 0xC1 && ('a' & 0xff) == 0x81 && ('[' & 0xff) == 0xAD &&  \
    (']' & 0xff) == 0xBD &&                                                    \
    (('\n' & 0xff) == 0x15 || defined(LIBEDIT_EBCDIC_SIM))
/*
 * IBM-1047 as z/OS compilers use it ('\n' is NL, 0x15).  The test
 * build (port/test, gcc -fexec-charset=IBM1047) defines
 * LIBEDIT_EBCDIC_SIM because glibc's IBM1047 table gives '\n' = 0x25.
 */
#define LIBEDIT_EBCDIC 1
#else
#error "libedit supports ASCII and IBM-1047 EBCDIC builds only"
#endif

#if LIBEDIT_EBCDIC
extern const unsigned char libedit_a2e[256], libedit_e2a[256];
#define EL_A2N(c) ((int)libedit_a2e[(unsigned)(c) & 0xff])
#define EL_N2A(c) ((int)libedit_e2a[(unsigned)(c) & 0xff])
/* the ASCII controls whose IBM-1047 code differs (see charset.c) */
#define EL_A2N_C(c)                                                            \
    ((c) == 004    ? 0x37                                                      \
     : (c) == 005  ? 0x2d                                                      \
     : (c) == 006  ? 0x2e                                                      \
     : (c) == 007  ? 0x2f                                                      \
     : (c) == 010  ? 0x16                                                      \
     : (c) == 011  ? 0x05                                                      \
     : (c) == 012  ? 0x15                                                      \
     : (c) == 024  ? 0x3c                                                      \
     : (c) == 025  ? 0x3d                                                      \
     : (c) == 026  ? 0x32                                                      \
     : (c) == 027  ? 0x26                                                      \
     : (c) == 032  ? 0x3f                                                      \
     : (c) == 033  ? 0x27                                                      \
     : (c) == 0177 ? 0x07                                                      \
                   : (c))
/* alphabet position of an IBM-1047 letter: a-i 0x81, j-r 0x91, s-z 0xa2 */
#define EL__LPOS(c)                                                            \
    (((c) & 0xf) + (((c) >> 4 & 3) == 0 ? 0 : ((c) >> 4 & 3) == 1 ? 9 : 17))
/* IBM-1047 letters are in blocks with gaps: a-i 81-89, j-r 91-99,
 * s-z a2-a9, and the same +0x40 for A-Z ('\\' is e0, in a gap) */
#define EL__ISLET(c)                                                           \
    (((c) & 0xf) >= 1 && ((c) & 0xf) <= 9 &&                                   \
     ((((c) >> 4) & 0xb) == 0x8 || (((c) >> 4) & 0xb) == 0x9 ||                \
      ((((c) >> 4) & 0xb) == 0xa && ((c) & 0xf) >= 2)))
#define EL__CTRLA(c)                                                           \
    (EL__ISLET(c)           ? EL__LPOS(c)                                      \
     : (c) == ('[' & 0xff)  ? 033                                              \
     : (c) == ('\\' & 0xff) ? 034                                              \
     : (c) == (']' & 0xff)  ? 035                                              \
     : (c) == ('^' & 0xff)  ? 036                                              \
     : (c) == ('_' & 0xff)  ? 037                                              \
                            : 0)
#define EL_CTRL_C(c) EL_A2N_C(EL__CTRLA((c) & 0xff))
#define EL_UNCTRL(c) ((c) < 256 ? EL_A2N(EL_N2A(c) | 0100) : (c))
#define EL_META(c, m) ((c) < 256 ? EL_A2N(EL_N2A(c) | (m)) : (c))
#define EL_WESC L"\x27"
#else
#define EL_A2N(c) ((int)(c))
#define EL_N2A(c) ((int)(c))
#define EL_A2N_C(c) (c)
#define EL_CTRL_C(c) ((c) & 037)
#define EL_UNCTRL(c) ((c) | 0100)
#define EL_META(c, m) ((c) | (m))
#define EL_WESC L"\033"
#endif
#define EL_ESC EL_A2N_C(033)
#define EL_DEL EL_A2N_C(0177)

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
#undef HAVE_VIS
#define HAVE_VIS 0
#undef HAVE_SVIS
#define HAVE_SVIS 0
#undef HAVE_STRLCPY
#define HAVE_STRLCPY 0
#undef HAVE_STRLCAT
#define HAVE_STRLCAT 0
#ifndef HAVE_NBTOOL_CONFIG_H
#define HAVE_NBTOOL_CONFIG_H 1
#endif

/*
 * Every helper gets a libedit_ prefix so it can never collide with a
 * libc that has its own (glibc >= 2.38 has strlcpy; macOS has vis).
 */
#define strlcpy libedit_strlcpy
#define strlcat libedit_strlcat
#define vis libedit_vis
#define nvis libedit_nvis
#define svis libedit_svis
#define snvis libedit_snvis
#define strvis libedit_strvis
#define stravis libedit_stravis
#define strnvis libedit_strnvis
#define strsvis libedit_strsvis
#define strsnvis libedit_strsnvis
#define strvisx libedit_strvisx
#define strnvisx libedit_strnvisx
#define strenvisx libedit_strenvisx
#define strsvisx libedit_strsvisx
#define strsnvisx libedit_strsnvisx
#define strsenvisx libedit_strsenvisx
#define strunvis libedit_strunvis
#define strnunvis libedit_strnunvis
#define strunvisx libedit_strunvisx
#define strnunvisx libedit_strnunvisx
#define unvis libedit_unvis

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
#define issetugid libedit_issetugid
int libedit_issetugid(void);

#ifdef __MVS__
/*
 * z/OS: strcasecmp is only in <strings.h> (filecomplete.c expects it
 * from <string.h>), and _ALL_SOURCE gives u_char but not u_int32_t
 * (vis.c).  C11 allows repeating an identical typedef.
 */
#include <strings.h>
#include <sys/types.h>
typedef uint32_t u_int32_t;
#endif

#if defined(__MVS__) || defined(LIBEDIT_COMPAT_ALL)
#include <stdio.h>
#include <wchar.h>
#define getline libedit_getline
#define wcsdup libedit_wcsdup
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
