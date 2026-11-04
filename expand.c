/*
 * Word expansion.  A raw word is walked once, producing the expanded
 * bytes plus a parallel array of marks recording where each byte came
 * from.  Field splitting and pathname expansion then consult the marks,
 * so no in-band escape bytes are needed (see the EBCDIC note in parse.c).
 */
#include <ctype.h>
#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

enum {
    Q_LIT,    /* unquoted literal text */
    Q_QUOTED, /* quoted text: never split or globbed */
    Q_EXP,    /* unquoted expansion result: split + glob */
    Q_BREAK,  /* field boundary from unquoted $@ / $* */
    Q_BREAKQ  /* boundary from "$@": empty fields kept */
};

struct xbuf {
    struct sbuf s, q;
    int had_quote;
};

static void exp_text(const char *p, const char *end, int inq, int hd,
                     struct xbuf *xb, int flags);

static void xput(struct xbuf *xb, int c, int mark) {
    sb_putc(&xb->s, c);
    sb_putc(&xb->q, mark);
}

static void xputs(struct xbuf *xb, const char *s, int mark) {
    for (; *s; s++)
        xput(xb, *s, mark);
}

/* Expansion buffers live in the arena: growth is a bump + copy and the
 * command's arena reset frees them; no malloc/free per word. */
static void xinit(struct xbuf *xb) {
    memset(xb, 0, sizeof(*xb));
    xb->s.arena = xb->q.arena = 1;
}

static void xfree(struct xbuf *xb) {
    (void)xb;
}

/* ---- scanning helpers over raw (already validated) text ---- */

static const char *skip_dq(const char *p);

static const char *
skip_bq(const char *p) /* p after opening '`'; returns after '`' */
{
    while (*p && *p != '`') {
        if (*p == '\\' && p[1])
            p++;
        p++;
    }
    return *p ? p + 1 : p;
}

static const char *skip_bal(const char *p, int open,
                            int close) /* p after open char */
{
    int depth = 1;

    while (*p && depth > 0) {
        if (*p == open)
            depth++;
        else if (*p == close)
            depth--;
        else if (*p == '\\' && p[1])
            p++;
        else if (*p == '\'') {
            while (*++p && *p != '\'')
                ;
        } else if (*p == '"') {
            p = skip_dq(p + 1);
            continue;
        } else if (*p == '`') {
            p = skip_bq(p + 1);
            continue;
        }
        if (*p)
            p++;
    }
    return p;
}

static const char *
skip_dq(const char *p) /* p after opening '"'; returns after '"' */
{
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p++;
        else if (*p == '$' && p[1] == '(') {
            p = skip_bal(p + 2, '(', ')');
            continue;
        } else if (*p == '$' && p[1] == '{') {
            p = skip_bal(p + 2, '{', '}');
            continue;
        } else if (*p == '`') {
            p = skip_bq(p + 1);
            continue;
        }
        p++;
    }
    return *p ? p + 1 : p;
}

/* ---- parameters ---- */

static char numbuf[3][32];

static const char *special_param(int c, int *is_at) {
    static int nb;
    char *b = numbuf[nb++ % 3];

    *is_at = 0;
    switch (c) {
    case '?': snprintf(b, 32, "%d", sh.status); return b;
    case '$': snprintf(b, 32, "%ld", (long)sh.pid); return b;
    case '!':
        if (sh.lastbg == 0)
            return NULL;
        snprintf(b, 32, "%ld", (long)sh.lastbg);
        return b;
    case '#': snprintf(b, 32, "%d", sh.npos); return b;
    case '-': return sh_optstring();
    case '0': return sh.argv0;
    case '@':
    case '*': *is_at = c; return sh.npos ? sh.pos[0] : NULL;
    }
    if (c >= '1' && c <= '9')
        return c - '0' <= sh.npos ? sh.pos[c - '1'] : NULL;
    return NULL;
}

static const char *param_value(const char *name, size_t n, int *is_at) {
    char tmp[256];

    *is_at = 0;
    if (n == 1 && !is_name(name, 1))
        return special_param(name[0], is_at);
    if (n > 0 && isdigit((unsigned char)name[0])) {
        int i = atoi(name);

        return i >= 1 && i <= sh.npos ? sh.pos[i - 1] : NULL;
    }
    if (n >= sizeof(tmp))
        return NULL;
    memcpy(tmp, name, n);
    tmp[n] = '\0';
    return var_get(tmp);
}

/* Append $@ / $* respecting quoting. */
static void put_params(struct xbuf *xb, int which, int inq) {
    int i;
    const char *ifs = var_get("IFS");
    char sep = ifs ? *ifs : ' ';

    for (i = 0; i < sh.npos; i++) {
        if (i > 0) {
            if (!inq)
                xput(xb, ' ', Q_BREAK);
            else if (which == '@')
                xput(xb, ' ', Q_BREAKQ);
            else if (sep)
                xput(xb, sep, Q_QUOTED);
        }
        xputs(xb, sh.pos[i], inq ? Q_QUOTED : Q_EXP);
    }
}

static void unset_check(const char *name, size_t n) {
    if (sh.opt_u)
        sh_error("%.*s: parameter not set", (int)n, name);
}

/* Build an fnmatch pattern from an xbuf: quoted bytes get escaped. */
static char *xb_pattern(struct xbuf *xb) {
    struct sbuf b = {NULL, 0, 0, 1};
    size_t i;

    for (i = 0; i < xb->s.len; i++) {
        int c = xb->s.s[i];

        if ((xb->q.s[i] == Q_QUOTED && strchr("*?[]\\", c)) ||
            (xb->q.s[i] == Q_EXP && c == '\\'))
            sb_putc(&b, '\\');
        sb_putc(&b, c);
    }
    return sb_cstr(&b);
}

static char *pattern_of(const char *w, const char *end) {
    struct xbuf t;
    char *r;

    xinit(&t);
    exp_text(w, end, 0, 0, &t, 0);
    r = xb_pattern(&t);
    xfree(&t);
    return r;
}

static int prefix_match(char *tmp, size_t i, const char *pat) {
    char save = tmp[i];
    int m;

    tmp[i] = '\0';
    m = fnmatch(pat, tmp, 0) == 0;
    tmp[i] = save;
    return m;
}

/* ${v#p} ${v##p} ${v%p} ${v%%p} */
static const char *trim(const char *val, const char *pat, int op, int longest) {
    size_t len = strlen(val), i;
    char *tmp = astrdup(val);

    if (op == '#') {
        if (!longest) {
            for (i = 0; i <= len; i++)
                if (prefix_match(tmp, i, pat))
                    return val + i;
        } else {
            for (i = len + 1; i-- > 0;)
                if (prefix_match(tmp, i, pat))
                    return val + i;
        }
        return val;
    }
    if (!longest) {
        for (i = len + 1; i-- > 0;)
            if (fnmatch(pat, val + i, 0) == 0)
                goto cut;
    } else {
        for (i = 0; i <= len; i++)
            if (fnmatch(pat, val + i, 0) == 0)
                goto cut;
    }
    return val;
cut:
    tmp[i] = '\0';
    return tmp;
}

/* ${...}; p points just after '{', end at the matching '}' */
static void exp_brace(const char *p, const char *end, int inq,
                      struct xbuf *xb) {
    const char *name = p, *op, *word;
    size_t n;
    int len_op = 0, colon = 0, is_at, set;
    const char *val;
    char b[32];

    if (*p == '#' && p + 1 < end) {
        len_op = 1;
        name = ++p;
    }
    if (isdigit((unsigned char)*p)) {
        while (p < end && isdigit((unsigned char)*p))
            p++;
    } else if (is_name(p, 1)) {
        while (p < end && (isalnum((unsigned char)*p) || *p == '_'))
            p++;
    } else if (p < end)
        p++;
    n = (size_t)(p - name);
    if (n == 0)
        sh_error("bad substitution");
    val = param_value(name, n, &is_at);

    if (len_op) {
        if (p != end)
            sh_error("bad substitution");
        if (val == NULL)
            unset_check(name, n);
        snprintf(b, sizeof(b), "%zu", val ? strlen(val) : (size_t)0);
        xputs(xb, b, inq ? Q_QUOTED : Q_EXP);
        return;
    }
    op = p;
    if (op == end) {
        if (is_at) {
            put_params(xb, is_at, inq);
            return;
        }
        if (val == NULL)
            unset_check(name, n);
        else
            xputs(xb, val, inq ? Q_QUOTED : Q_EXP);
        return;
    }
    if (*op == ':') {
        colon = 1;
        op++;
    }
    word = op + 1;
    set = val != NULL && (!colon || *val);
    switch (*op) {
    case '-':
        if (set)
            goto use_val;
        exp_text(word, end, inq, 0, xb, EXP_TILDE);
        return;
    case '+':
        if (set)
            exp_text(word, end, inq, 0, xb, EXP_TILDE);
        return;
    case '=':
        if (set)
            goto use_val;
        if (!is_name(name, n))
            sh_error("%.*s: cannot assign", (int)n, name);
        {
            char *nm = astrndup(name, n);
            char *w =
                expand_str(astrndup(word, (size_t)(end - word)), EXP_TILDE);

            var_set(nm, w, 0);
            xputs(xb, w, inq ? Q_QUOTED : Q_EXP);
        }
        return;
    case '?':
        if (set)
            goto use_val;
        if (word < end)
            sh_error("%.*s: %s", (int)n, name,
                     expand_str(astrndup(word, (size_t)(end - word)), 0));
        sh_error("%.*s: parameter null or not set", (int)n, name);
        return;
    case '#':
    case '%':
        if (colon)
            break;
        {
            int longest = op[1] == op[0];
            const char *pw = op + 1 + longest;

            if (val == NULL) {
                unset_check(name, n);
                val = "";
            }
            val = trim(val, pattern_of(pw, end), *op, longest);
            xputs(xb, val, inq ? Q_QUOTED : Q_EXP);
        }
        return;
    }
    sh_error("bad substitution");
use_val:
    if (is_at)
        put_params(xb, is_at, inq);
    else
        xputs(xb, val, inq ? Q_QUOTED : Q_EXP);
}

static void put_subst(struct xbuf *xb, const char *s, int inq) {
    xputs(xb, s, inq ? Q_QUOTED : Q_EXP);
}

/* p points at '$'; returns pointer past the expansion */
static const char *exp_dollar(const char *p, int inq, struct xbuf *xb) {
    const char *q, *v;
    int is_at;

    p++;
    if (*p == '(' && p[1] == '(') {
        char *inner, b[32];

        /* scanning from the 2nd '(' with depth 1 ends after "))" */
        q = skip_bal(p + 1, '(', ')');
        inner = astrndup(p + 2, (size_t)(q - 2 - (p + 2)));
        snprintf(b, sizeof(b), "%ld", arith_eval(expand_str(inner, 0)));
        put_subst(xb, b, inq);
        return q;
    }
    if (*p == '(') {
        q = skip_bal(p + 1, '(', ')');
        put_subst(xb, cmdsubst(astrndup(p + 1, (size_t)(q - 1 - (p + 1)))),
                  inq);
        return q;
    }
    if (*p == '{') {
        q = skip_bal(p + 1, '{', '}');
        exp_brace(p + 1, q - 1, inq, xb);
        return q;
    }
    if (is_name(p, 1)) {
        q = p;
        while (isalnum((unsigned char)*q) || *q == '_')
            q++;
        v = param_value(p, (size_t)(q - p), &is_at);
        if (v == NULL)
            unset_check(p, (size_t)(q - p));
        else
            xputs(xb, v, inq ? Q_QUOTED : Q_EXP);
        return q;
    }
    if (*p && strchr("@*#?-$!0123456789", *p)) {
        v = special_param(*p, &is_at);
        if (is_at)
            put_params(xb, is_at, inq);
        else if (v == NULL && *p != '@' && *p != '*')
            unset_check(p, 1);
        else if (v)
            xputs(xb, v, inq ? Q_QUOTED : Q_EXP);
        return p + 1;
    }
    xput(xb, '$', inq ? Q_QUOTED : Q_LIT);
    return p;
}

static const char *exp_tilde(const char *p, const char *end, struct xbuf *xb) {
    const char *q = p + 1, *home = NULL;
    char user[256];
    size_t n;

    while (q < end && *q != '/')
        q++;
    n = (size_t)(q - p - 1);
    if (n == 0)
        home = var_get("HOME");
    else if (n < sizeof(user)) {
        struct passwd *pw;

        memcpy(user, p + 1, n);
        user[n] = '\0';
        if (strpbrk(user, "\"'\\$`") == NULL && (pw = getpwnam(user)) != NULL)
            home = pw->pw_dir;
    }
    if (home == NULL) {
        xput(xb, '~', Q_LIT);
        return p + 1;
    }
    xputs(xb, home, Q_QUOTED);
    return q;
}

static void exp_text(const char *p, const char *end, int inq, int hd,
                     struct xbuf *xb, int flags) {
    const char *start = p;

    while (p < end) {
        int c = *p;

        if (c == '\'' && !inq && !hd) {
            xb->had_quote = 1;
            while (++p < end && *p != '\'')
                xput(xb, *p, Q_QUOTED);
            p++;
        } else if (c == '"' && !inq && !hd) {
            const char *q = skip_dq(p + 1);

            if (!(q - p == 4 && p[1] == '$' && p[2] == '@' && sh.npos == 0))
                xb->had_quote = 1;
            exp_text(p + 1, q - 1, 1, 0, xb, 0);
            p = q;
        } else if (c == '\\' && p + 1 < end) {
            int d = p[1];

            if (!inq && !hd) {
                xput(xb, d, Q_QUOTED);
                p += 2;
            } else if (d == '$' || d == '`' || d == '\\' || (d == '"' && !hd)) {
                xput(xb, d, Q_QUOTED);
                p += 2;
            } else if (d == '\n') {
                p += 2;
            } else {
                xput(xb, '\\', Q_QUOTED);
                p++;
            }
        } else if (c == '$') {
            p = exp_dollar(p, inq || hd, xb);
        } else if (c == '`') {
            const char *q = skip_bq(p + 1);
            struct sbuf b = {0};
            const char *s;

            for (s = p + 1; s < q - 1; s++) {
                if (*s == '\\' && strchr("$`\\", s[1]))
                    s++;
                sb_putc(&b, *s);
            }
            sb_cstr(&b);
            put_subst(xb, cmdsubst(b.s), inq || hd);
            sb_free(&b);
            p = q;
        } else if (c == '~' && p == start && !inq && !hd &&
                   (flags & EXP_TILDE)) {
            p = exp_tilde(p, end, xb);
        } else {
            xput(xb, c, inq || hd ? Q_QUOTED : Q_LIT);
            p++;
        }
    }
}

/* ---- field splitting and globbing ---- */

static void sl_add(struct strlist *sl, char *s) {
    if (sl->n + 1 >= sl->cap) {
        size_t nc = sl->cap ? sl->cap * 2 : 16;
        char **nv = aalloc(nc * sizeof(char *));

        if (sl->n)
            memcpy(nv, sl->v, sl->n * sizeof(char *));
        sl->v = nv;
        sl->cap = nc;
    }
    sl->v[sl->n++] = s;
    sl->v[sl->n] = NULL;
}

/*
 * True if the field contains an unquoted * or ?, or an unquoted '['
 * with a later unquoted ']'.  A lone '[' (the test command) is not a
 * pattern, so `[ ... ]` never touches the filesystem.
 */
static int has_glob(const struct xbuf *f) {
    size_t i, j;

    for (i = 0; i < f->s.len; i++) {
        int c = f->s.s[i];

        if (f->q.s[i] == Q_QUOTED)
            continue;
        if (c == '*' || c == '?')
            return 1;
        if (c == '[')
            for (j = i + 1; j < f->s.len; j++)
                if (f->s.s[j] == ']' && f->q.s[j] != Q_QUOTED)
                    return 1;
    }
    return 0;
}

static void emit(struct xbuf *f, int flags, struct strlist *out) {
    if ((flags & EXP_GLOB) && !sh.opt_f && has_glob(f)) {
        glob_t g;
        char *pat = xb_pattern(f);

        if (glob(pat, 0, NULL, &g) == 0) {
            size_t i;

            for (i = 0; i < g.gl_pathc; i++)
                sl_add(out, astrdup(g.gl_pathv[i]));
            globfree(&g);
            goto done;
        }
        globfree(&g);
    }
    sl_add(out, astrndup(f->s.s ? f->s.s : "", f->s.len));
done:
    f->s.len = f->q.len = 0;
}

static void split(struct xbuf *xb, int flags, struct strlist *out) {
    const char *ifs = var_get("IFS");
    struct xbuf f;
    size_t i, start_n = out->n;
    int have = 0, pending = 0;

    xinit(&f);
    if (ifs == NULL)
        ifs = " \t\n";
    for (i = 0; i < xb->s.len; i++) {
        int c = xb->s.s[i], m = xb->q.s[i];

        if (m == Q_BREAKQ) {
            emit(&f, flags, out);
            have = 0;
            pending = 1; /* next field exists even if empty */
        } else if (m == Q_BREAK) {
            if (have || pending)
                emit(&f, flags, out);
            have = pending = 0;
        } else if (m == Q_EXP && *ifs && strchr(ifs, c)) {
            int ws = c == ' ' || c == '\t' || c == '\n';

            if (have || pending || !ws)
                emit(&f, flags, out);
            have = pending = 0;
            while (i + 1 < xb->s.len && xb->q.s[i + 1] == Q_EXP &&
                   strchr(ifs, xb->s.s[i + 1]) &&
                   strchr(" \t\n", xb->s.s[i + 1]))
                i++;
        } else {
            xput(&f, c, m);
            have = 1;
        }
    }
    if (have || pending || (xb->had_quote && out->n == start_n))
        emit(&f, flags, out);
    xfree(&f);
}

void expand_words(struct word *w, int flags, struct strlist *out) {
    struct xbuf xb;

    xinit(&xb);
    for (; w; w = w->next) {
        const char *t = w->text;

        xb.s.len = xb.q.len = 0;
        xb.had_quote = 0;
        exp_text(t, t + strlen(t), 0, 0, &xb, flags);
        if (flags & EXP_SPLIT)
            split(&xb, flags, out);
        else {
            size_t i;

            for (i = 0; i < xb.s.len; i++)
                if (xb.q.s[i] >= Q_BREAK)
                    xb.s.s[i] = ' ';
            emit(&xb, flags & ~EXP_GLOB, out);
        }
    }
    xfree(&xb);
}

char *expand_str(const char *text, int flags) {
    struct xbuf xb;
    size_t i;
    char *r;

    xinit(&xb);
    exp_text(text, text + strlen(text), 0, 0, &xb, flags);
    for (i = 0; i < xb.s.len; i++)
        if (xb.q.s[i] >= Q_BREAK)
            xb.s.s[i] = ' ';
    r = astrndup(xb.s.s ? xb.s.s : "", xb.s.len);
    xfree(&xb);
    return r;
}

char *expand_heredoc(const char *text) {
    struct xbuf xb;
    char *r;

    xinit(&xb);
    exp_text(text, text + strlen(text), 0, 1, &xb, 0);
    r = astrndup(xb.s.s ? xb.s.s : "", xb.s.len);
    xfree(&xb);
    return r;
}

int case_match(const char *pat_text, const char *subj) {
    return fnmatch(pattern_of(pat_text, pat_text + strlen(pat_text)), subj,
                   0) == 0;
}

/* ---- arithmetic ---- */

static const char *ap;

static long a_or(void);

static void a_ws(void) {
    while (*ap == ' ' || *ap == '\t' || *ap == '\n')
        ap++;
}

static long a_primary(void) {
    long v;

    a_ws();
    if (*ap == '(') {
        ap++;
        v = a_or();
        a_ws();
        if (*ap++ != ')')
            sh_error("arithmetic: missing ')'");
        return v;
    }
    if (*ap == '-') {
        ap++;
        return -a_primary();
    }
    if (*ap == '+') {
        ap++;
        return a_primary();
    }
    if (*ap == '!') {
        ap++;
        return !a_primary();
    }
    if (*ap == '~') {
        ap++;
        return ~a_primary();
    }
    if (isdigit((unsigned char)*ap)) {
        char *e;

        v = strtol(ap, &e, 0);
        ap = e;
        return v;
    }
    if (is_name(ap, 1)) {
        const char *s = ap;
        char nm[256];
        const char *val;
        size_t n;

        while (isalnum((unsigned char)*ap) || *ap == '_')
            ap++;
        n = (size_t)(ap - s);
        if (n >= sizeof(nm))
            sh_error("arithmetic: name too long");
        memcpy(nm, s, n);
        nm[n] = '\0';
        val = var_get(nm);
        if (val == NULL || *val == '\0') {
            if (sh.opt_u && val == NULL)
                sh_error("%s: parameter not set", nm);
            return 0;
        }
        return strtol(val, NULL, 0);
    }
    sh_error("arithmetic: syntax error near '%s'", ap);
    return 0;
}

static long a_mul(void) {
    long v = a_primary(), r;

    for (;;) {
        a_ws();
        if (*ap != '*' && *ap != '/' && *ap != '%')
            return v;
        {
            int op = *ap++;

            r = a_primary();
            if (op != '*' && r == 0)
                sh_error("arithmetic: division by zero");
            v = op == '*' ? v * r : op == '/' ? v / r : v % r;
        }
    }
}

static long a_add(void) {
    long v = a_mul();

    for (;;) {
        a_ws();
        if (*ap == '+')
            ap++, v += a_mul();
        else if (*ap == '-')
            ap++, v -= a_mul();
        else
            return v;
    }
}

static long a_shift(void) {
    long v = a_add();

    for (;;) {
        a_ws();
        if (ap[0] == '<' && ap[1] == '<')
            ap += 2, v <<= a_add();
        else if (ap[0] == '>' && ap[1] == '>')
            ap += 2, v >>= a_add();
        else
            return v;
    }
}

static long a_rel(void) {
    long v = a_shift();

    for (;;) {
        a_ws();
        if (ap[0] == '<' && ap[1] == '=')
            ap += 2, v = v <= a_shift();
        else if (ap[0] == '>' && ap[1] == '=')
            ap += 2, v = v >= a_shift();
        else if (ap[0] == '<' && ap[1] != '<')
            ap++, v = v < a_shift();
        else if (ap[0] == '>' && ap[1] != '>')
            ap++, v = v > a_shift();
        else
            return v;
    }
}

static long a_eq(void) {
    long v = a_rel();

    for (;;) {
        a_ws();
        if (ap[0] == '=' && ap[1] == '=')
            ap += 2, v = v == a_rel();
        else if (ap[0] == '!' && ap[1] == '=')
            ap += 2, v = v != a_rel();
        else
            return v;
    }
}

static long a_bits(void) {
    long v = a_eq();

    for (;;) {
        a_ws();
        if (ap[0] == '&' && ap[1] != '&')
            ap++, v &= a_eq();
        else if (ap[0] == '^')
            ap++, v ^= a_eq();
        else if (ap[0] == '|' && ap[1] != '|')
            ap++, v |= a_eq();
        else
            return v;
    }
}

static long a_and(void) {
    long v = a_bits();

    for (;;) {
        a_ws();
        if (ap[0] != '&' || ap[1] != '&')
            return v;
        ap += 2;
        v = a_bits() && v;
    }
}

static long a_or(void) {
    long v = a_and();

    for (;;) {
        a_ws();
        if (ap[0] != '|' || ap[1] != '|')
            return v;
        ap += 2;
        v = a_and() || v;
    }
}

long arith_eval(const char *expr) {
    long v;

    ap = expr;
    v = a_or();
    a_ws();
    if (*ap)
        sh_error("arithmetic: syntax error near '%s'", ap);
    return v;
}
