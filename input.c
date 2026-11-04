/* Input sources: strings, files, and the interactive terminal. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

#define IBUF 4096

struct src {
	struct src *prev;
	int fd;                 /* -1 for string sources */
	int interactive;
	const char *p;          /* current read pointer */
	const char *end;
	char *buf;              /* owned buffer for fd sources */
	int unget[4];
	int nunget;
	int eof;
	int shared;             /* fd 0: commands we run read it too */
	int seekable;           /* shared and a regular file */
};

static struct src *top;
static int want_ps2;

void
in_push_string(const char *s)
{
	struct src *n = xmalloc(sizeof(*n));

	memset(n, 0, sizeof(*n));
	n->fd = -1;
	n->p = s;
	n->end = s + strlen(s);
	n->prev = top;
	top = n;
}

void
in_push_fd(int fd, int interactive)
{
	struct src *n = xmalloc(sizeof(*n));

	memset(n, 0, sizeof(*n));
	n->fd = fd;
	n->interactive = interactive;
	n->shared = fd == 0 && !interactive;
	n->seekable = n->shared && sys_input_seekable(fd);
	n->buf = xmalloc(IBUF);
	n->p = n->end = n->buf;
	n->prev = top;
	top = n;
}

void
in_pop(void)
{
	struct src *s = top;

	if (s == NULL)
		return;
	top = s->prev;
	free(s->buf);
	free(s);
}

int
in_depth(void)
{
	int n = 0;
	struct src *s;

	for (s = top; s; s = s->prev)
		n++;
	return n;
}

void
in_pop_to(int depth)
{
	while (in_depth() > depth)
		in_pop();
}

/*
 * POSIX requires a shell reading its script from stdin to leave the
 * offset just after the current command, so a `read` or child process
 * sees the following input.  Shared pipes never over-read (see refill).
 * A shared regular file is read in blocks and seeked back here, lazily:
 * eval calls this only before something that can read fd 0 (spawn,
 * fork, `read`, `exec`), so builtin-only lines cost nothing.  While a
 * command runs, the parser is parked at its end, so the buffer position
 * is the command boundary.
 */
void
in_sync(void)
{
	struct src *s;
	off_t back;

	for (s = top; s && !s->shared; s = s->prev)
		;
	if (s == NULL || !s->seekable)
		return;
	back = (off_t)(s->end - s->p) + s->nunget;
	if (back > 0 && lseek(s->fd, -back, SEEK_CUR) >= 0) {
		s->p = s->end;
		s->nunget = 0;
	}
}

int
in_interactive(void)
{
	return top && top->interactive;
}

void
in_set_ps2(int on)
{
	want_ps2 = on;
}

void
in_discard_line(void)
{
	if (top) {
		top->p = top->end;
		top->nunget = 0;
	}
	want_ps2 = 0;
}

static int
refill_interactive(struct src *s)
{
	const char *pr, *line;
	int len;

	pr = want_ps2 ? var_get("PS2") : var_get("PS1");
	pr = prompt_expand(pr ? pr : want_ps2 ? "> " : "\\$ ");
	job_reap();
	line = le_readline(pr, &len);
	if (line == NULL)
		return -1;
	if (len > IBUF - 1)
		s->buf = xrealloc(s->buf, (size_t)len + 1);
	memcpy(s->buf, line, (size_t)len);
	s->p = s->buf;
	s->end = s->buf + len;
	hist_add(line);
	return 0;
}

static int
refill(struct src *s)
{
	ssize_t n;

	if (s->fd < 0 || s->eof)
		return -1;
	if (s->interactive)
		return refill_interactive(s);
	/* shared pipe: never read past what the parser asks for */
	do
		n = read(s->fd, s->buf, s->shared && !s->seekable ? 1 : IBUF);
	while (n < 0 && errno == EINTR);
	if (n <= 0) {
		s->eof = 1;
		return -1;
	}
	s->p = s->buf;
	s->end = s->buf + n;
	return 0;
}

int
in_getc(void)
{
	struct src *s = top;
	int c;

	if (s == NULL)
		return EOF;
	if (s->nunget)
		return s->unget[--s->nunget];
	if (s->p >= s->end && refill(s) < 0)
		return EOF;
	c = (unsigned char)*s->p++;
	if (sh.opt_v && s->fd >= 0)
		(void)!write(2, s->p - 1, 1);
	return c;
}

void
in_ungetc(int c)
{
	if (top && c != EOF && top->nunget < 4)
		top->unget[top->nunget++] = c;
}
