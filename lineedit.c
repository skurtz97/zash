/*
 * Interactive line input.  With -DWITH_LIBEDIT this uses libedit
 * (emacs bindings, history, ~/.editrc, filename completion on TAB);
 * otherwise a plain read(2) loop.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

static const char *cur_prompt = "";

/* Fallback reader: no editing, used without libedit or a terminal. */
static const char *plain_readline(const char *prompt, int *len) {
    static char *buf;
    static size_t cap;
    size_t n = 0;
    char c;
    ssize_t r;

    {
        const char *p;

        for (p = prompt; *p; p++)
            if (*p != PROMPT_IGNORE)
                (void)!write(2, p, 1);
    }
    for (;;) {
        r = read(0, &c, 1);
        if (r < 0 && errno == EINTR) {
            if (!sh.sigint)
                continue;
            sh.sigint = 0;
            (void)!write(2, "\n", 1);
            n = 0;
            c = '\n';
        } else if (r <= 0) {
            if (n == 0)
                return NULL;
            c = '\n';
        }
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 256;
            buf = xrealloc(buf, cap);
        }
        buf[n++] = c;
        if (c == '\n')
            break;
    }
    buf[n] = '\0';
    *len = (int)n;
    return buf;
}

#ifdef WITH_LIBEDIT
#include <histedit.h>

static EditLine *el;
static History *hist;
static HistEvent hev;
static const char *editor = "emacs";

static char *prompt_fn(EditLine *e) {
    (void)e;
    return (char *)cur_prompt;
}

unsigned char complete_fn(EditLine *, int);

int le_init(void) {

    el = el_init(sh.argv0, stdin, stdout, stderr);
    if (el == NULL)
        return -1;
    hist = history_init();
    /* history.c owns trimming (HISTSIZE); libedit must never drop first */
    history(hist, &hev, H_SETSIZE, 1000000);
    el_set(el, EL_HIST, history, hist);
    /* text between PROMPT_IGNORE pairs (\[ \]) has no width */
    el_set(el, EL_PROMPT_ESC, prompt_fn, PROMPT_IGNORE);
    el_set(el, EL_EDITOR, editor);
    el_set(el, EL_SIGNAL, 1);
    el_set(el, EL_ADDFN, "zash-complete", "Complete word", complete_fn);
    el_set(el, EL_BIND, "^I", "zash-complete", NULL);
    el_source(el, NULL); /* ~/.editrc */
    return 0;
}

void le_set_editor(const char *mode) {
    editor = mode;
    if (el)
        el_set(el, EL_EDITOR, mode);
}

const char *le_readline(const char *prompt, int *len) {
    const char *s;

    cur_prompt = prompt;
    if (sh.sighup)
        return NULL;
    if (el == NULL)
        return plain_readline(prompt, len);
    s = el_gets(el, len);
    if (s != NULL && *len > 0)
        return s;
    if (sh.sigint) { /* ^C: abandon line */
        sh.sigint = 0;
        el_reset(el);
        fputc('\n', stdout);
        *len = 1;
        return "\n";
    }
    return NULL; /* EOF (^D) */
}
#else
int le_init(void) {
    return 0;
}

void le_set_editor(const char *mode) {
    (void)mode;
}

const char *le_readline(const char *prompt, int *len) {
    cur_prompt = prompt;
    return plain_readline(prompt, len);
}
#endif

void le_history_clear(void) {
#ifdef WITH_LIBEDIT
    if (hist)
        history(hist, &hev, H_CLEAR);
#endif
}

void le_history_add(const char *line) {
#ifdef WITH_LIBEDIT
    if (hist)
        history(hist, &hev, H_ENTER, line);
#else
    (void)line;
#endif
}

void le_end(void) {
#ifdef WITH_LIBEDIT
    if (el)
        el_end(el);
    if (hist)
        history_end(hist);
    el = NULL;
    hist = NULL;
#endif
}
