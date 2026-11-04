/*
 * z/OS has no <sys/cdefs.h>; NetBSD vis.c, unvis.c, vis.h, strlcpy.c
 * and strlcat.c include it unconditionally.  This directory is on the
 * include path only for z/OS builds, so it never shadows a real one.
 */
#ifndef ZASH_ZOS_SYS_CDEFS_H
#define ZASH_ZOS_SYS_CDEFS_H

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#ifndef __RCSID
#define __RCSID(x)
#endif
#ifndef __RENAME
#define __RENAME(x)
#endif
#ifndef __restrict
#define __restrict restrict
#endif

#endif
