/*
 * Stand-in for <sys/param.h> on z/OS (z/OS-only include path).  The
 * vendored sources need only NBBY from it (vis.c); el.c includes it
 * but uses nothing.
 */
#ifndef ZASH_ZOS_SYS_PARAM_H
#define ZASH_ZOS_SYS_PARAM_H

#include <limits.h>
#include <sys/types.h>

#ifndef NBBY
#define NBBY CHAR_BIT
#endif

#endif
