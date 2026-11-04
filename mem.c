/* Arena and buffer allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

#define ABLOCK 8192
#define ALIGN                                                                  \
    (sizeof(void *) > sizeof(long double) ? sizeof(void *)                     \
                                          : sizeof(long double))

struct ablock {
    struct ablock *prev;
    size_t size, used;
    /* data follows, aligned */
};

static struct ablock *cur;
static struct ablock *spare; /* one cached block: no malloc churn */

static size_t hdr(void) {
    return (sizeof(struct ablock) + ALIGN - 1) & ~(ALIGN - 1);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);

    if (p == NULL) {
        fputs("zash: out of memory\n", stderr);
        abort();
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (p == NULL) {
        fputs("zash: out of memory\n", stderr);
        abort();
    }
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;

    return memcpy(xmalloc(n), s, n);
}

void *aalloc(size_t n) {
    struct ablock *b;
    char *p;

    n = (n + ALIGN - 1) & ~(ALIGN - 1);
    if (cur == NULL || cur->size - cur->used < n) {
        size_t sz = n > ABLOCK ? n : ABLOCK;

        if (spare && sz == ABLOCK) {
            b = spare;
            spare = NULL;
        } else
            b = xmalloc(hdr() + sz);
        b->prev = cur;
        b->size = sz;
        b->used = 0;
        cur = b;
    }
    p = (char *)cur + hdr() + cur->used;
    cur->used += n;
    return p;
}

char *astrndup(const char *s, size_t n) {
    char *p = aalloc(n + 1);

    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *astrdup(const char *s) {
    return astrndup(s, strlen(s));
}

void amark(struct arena_mark *m) {
    m->blk = cur;
    m->used = cur ? cur->used : 0;
}

void arelease(const struct arena_mark *m) {
    while (cur != m->blk) {
        struct ablock *p = cur->prev;

        if (spare == NULL && cur->size == ABLOCK)
            spare = cur;
        else
            free(cur);
        cur = p;
    }
    if (cur)
        cur->used = m->used;
}

/* ---- sbuf ---- */

void sb_grow(struct sbuf *b, size_t need) {
    size_t ncap = b->cap;

    if (b->len + need + 1 <= b->cap)
        return;
    while (b->len + need + 1 > ncap)
        ncap = ncap ? ncap * 2 : 64;
    if (b->arena) {
        char *n = aalloc(ncap);

        if (b->len)
            memcpy(n, b->s, b->len);
        b->s = n;
    } else
        b->s = xrealloc(b->s, ncap);
    b->cap = ncap;
}

void sb_putn(struct sbuf *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
}

void sb_puts(struct sbuf *b, const char *s) {
    sb_putn(b, s, strlen(s));
}

char *sb_cstr(struct sbuf *b) {
    sb_grow(b, 0);
    b->s[b->len] = '\0';
    return b->s;
}

char *sb_arena(struct sbuf *b) {
    char *p = astrndup(b->s ? b->s : "", b->len);

    b->len = 0;
    return p;
}

void sb_free(struct sbuf *b) {
    if (!b->arena)
        free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}
