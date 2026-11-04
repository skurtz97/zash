/*
 * Functions some target libcs lack.  See port.h for when each is used.
 *
 * Copyright (c) 2026, Sean Kurtz.  BSD 3-Clause; see ../../LICENSE.
 */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

/*
 * issetugid(): BSD.  Approximated by comparing real and effective ids,
 * which catches a running set-id program but, unlike the BSD call, not
 * one that has since dropped privileges.  libedit uses it only to
 * decide whether to read ~/.editrc.
 */
int
libedit_issetugid(void)
{
	return getuid() != geteuid() || getgid() != getegid();
}

#ifdef LIBEDIT_NEED_GETLINE
ssize_t
libedit_getline(char **lineptr, size_t *n, FILE *fp)
{
	size_t len = 0;
	int c;

	if (lineptr == NULL || n == NULL || fp == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (*lineptr == NULL || *n == 0) {
		*n = 128;
		if ((*lineptr = malloc(*n)) == NULL)
			return -1;
	}
	while ((c = getc(fp)) != EOF) {
		if (len + 2 > *n) {
			size_t nn = *n * 2;
			char *p = realloc(*lineptr, nn);

			if (p == NULL)
				return -1;
			*lineptr = p;
			*n = nn;
		}
		(*lineptr)[len++] = (char)c;
		if (c == '\n')
			break;
	}
	if (len == 0)
		return -1;
	(*lineptr)[len] = '\0';
	return (ssize_t)len;
}
#endif

#ifdef LIBEDIT_NEED_WCSDUP
wchar_t *
libedit_wcsdup(const wchar_t *s)
{
	size_t n = wcslen(s) + 1;
	wchar_t *p = malloc(n * sizeof(wchar_t));

	if (p != NULL)
		memcpy(p, s, n * sizeof(wchar_t));
	return p;
}
#endif
