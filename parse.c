/*
 * Lexer and recursive-descent parser for the POSIX shell grammar.
 *
 * Words are stored as raw source text with their quoting intact and are
 * interpreted by expand.c at run time.  Unlike ash, we do not embed
 * high-byte control markers in words: FreeBSD sh's CTLVAR (0xC1) and
 * CTLENDVAR (0xF9) are 'A' and '9' in IBM-1047, and BusyBox's CTLESC
 * (0x81) is 'a', so the marker scheme cannot work in EBCDIC mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

enum tok {
    T_EOF,
    T_NL,
    T_SEMI,
    T_AMP,
    T_PIPE,
    T_AND,
    T_OR,
    T_LP,
    T_RP,
    T_DSEMI,
    T_REDIR,
    T_WORD
};

struct hdpend {
    struct hdpend *next;
    struct redir *r;
    char *delim;
    int striptabs;
};

static struct lexstate {
    enum tok tok; /* lookahead, valid if have */
    int have;
    char *word;
    enum rtype rtype;
    int rfd;
    int striptabs;
    struct hdpend *hd; /* heredocs waiting for the next newline */
    int eof;
} lx;

static struct sbuf wb; /* word buffer, reused */

static _Noreturn void synerr(const char *msg) {
    lx.have = 0;
    lx.hd = NULL;
    sh_error("syntax error: %s", msg);
    abort(); /* not reached */
}

static int is_meta(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' ||
           c == '|' || c == '<' || c == '>' || c == '(' || c == ')';
}

/* ---- word scanning ---- */

static void scan_dquote(void);
static void scan_dollar(void);
static void scan_backq(void);

static int needc(void) {
    int c = in_getc();

    if (c == EOF)
        synerr("unexpected end of file");
    return c;
}

/* Balanced scan for $( ... ), $(( ... )), ${ ... }. */
static void scan_balanced(int open, int close) {
    int depth = 1, c;

    while (depth > 0) {
        c = needc();
        sb_putc(&wb, c);
        if (c == open)
            depth++;
        else if (c == close)
            depth--;
        else if (c == '\\')
            sb_putc(&wb, needc());
        else if (c == '\'') {
            while ((c = needc()) != '\'')
                sb_putc(&wb, c);
            sb_putc(&wb, c);
        } else if (c == '"')
            scan_dquote();
        else if (c == '`')
            scan_backq();
    }
}

static void scan_dollar(void) {
    int c = in_getc();

    if (c == '(') {
        sb_putc(&wb, c);
        scan_balanced('(', ')');
    } else if (c == '{') {
        sb_putc(&wb, c);
        scan_balanced('{', '}');
    } else
        in_ungetc(c);
}

static void scan_backq(void) {
    int c;

    while ((c = needc()) != '`') {
        sb_putc(&wb, c);
        if (c == '\\')
            sb_putc(&wb, needc());
    }
    sb_putc(&wb, c);
}

/* Called after the opening '"' has been stored. */
static void scan_dquote(void) {
    int c;

    while ((c = needc()) != '"') {
        sb_putc(&wb, c);
        if (c == '\\')
            sb_putc(&wb, needc());
        else if (c == '$')
            scan_dollar();
        else if (c == '`')
            scan_backq();
    }
    sb_putc(&wb, c);
}

static void scan_word(int c) {
    wb.len = 0;
    for (;; c = in_getc()) {
        if (c == EOF)
            break;
        if (is_meta(c)) {
            in_ungetc(c);
            break;
        }
        if (c == '\\') {
            int d = in_getc();

            if (d == '\n')
                continue; /* line continuation */
            sb_putc(&wb, c);
            if (d == EOF)
                break;
            sb_putc(&wb, d);
            continue;
        }
        sb_putc(&wb, c);
        if (c == '\'') {
            while ((c = needc()) != '\'')
                sb_putc(&wb, c);
            sb_putc(&wb, c);
        } else if (c == '"')
            scan_dquote();
        else if (c == '$')
            scan_dollar();
        else if (c == '`')
            scan_backq();
    }
}

/* ---- here-documents ---- */

static char *unquote_delim(const char *s, int *quoted) {
    struct sbuf b = {0};
    char *r;

    *quoted = 0;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            *quoted = 1;
            sb_putc(&b, *++s);
        } else if (*s == '\'' || *s == '"') {
            *quoted = 1;
        } else
            sb_putc(&b, *s);
    }
    sb_cstr(&b);
    r = astrdup(b.s);
    sb_free(&b);
    return r;
}

static void read_heredocs(void) {
    struct hdpend *h;
    struct sbuf line = {0}, body = {0};
    int c;

    /* list was built in reverse; fix order */
    struct hdpend *rev = NULL, *nx;
    for (h = lx.hd; h; h = nx) {
        nx = h->next;
        h->next = rev;
        rev = h;
    }
    lx.hd = NULL;
    for (h = rev; h; h = h->next) {
        body.len = 0;
        for (;;) {
            const char *l;

            line.len = 0;
            while ((c = in_getc()) != EOF && c != '\n')
                sb_putc(&line, c);
            l = sb_cstr(&line);
            if (h->striptabs)
                while (*l == '\t')
                    l++;
            if (strcmp(l, h->delim) == 0)
                break;
            sb_puts(&body, l);
            sb_putc(&body, '\n');
            if (c == EOF)
                break;
        }
        h->r->target = aalloc(sizeof(struct word));
        h->r->target->next = NULL;
        h->r->target->text = sb_arena(&body);
    }
    sb_free(&line);
    sb_free(&body);
}

/* ---- tokenizer ---- */

static int all_digits(const char *s, size_t n) {
    size_t i;

    if (n == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

static enum tok lex_redir(int c, int fd) {
    int d = in_getc();

    if (c == '<') {
        lx.rfd = fd < 0 ? 0 : fd;
        if (d == '<') {
            int e = in_getc();

            lx.striptabs = e == '-';
            if (!lx.striptabs)
                in_ungetc(e);
            lx.rtype = R_HEREDOC;
        } else if (d == '&')
            lx.rtype = R_DUPIN;
        else if (d == '>')
            lx.rtype = R_RDWR;
        else {
            in_ungetc(d);
            lx.rtype = R_IN;
        }
    } else {
        lx.rfd = fd < 0 ? 1 : fd;
        if (d == '>')
            lx.rtype = R_APPEND;
        else if (d == '&')
            lx.rtype = R_DUPOUT;
        else if (d == '|')
            lx.rtype = R_CLOBBER;
        else {
            in_ungetc(d);
            lx.rtype = R_OUT;
        }
    }
    return T_REDIR;
}

static enum tok lex(void) {
    int c, d;

again:
    c = in_getc();
    while (c == ' ' || c == '\t')
        c = in_getc();
    if (c == '#') {
        while ((c = in_getc()) != '\n' && c != EOF)
            ;
    }
    switch (c) {
    case EOF: return T_EOF;
    case '\n':
        if (lx.hd)
            read_heredocs();
        return T_NL;
    case '\\':
        d = in_getc();
        if (d == '\n')
            goto again;
        in_ungetc(d);
        break;
    case ';':
        d = in_getc();
        if (d == ';')
            return T_DSEMI;
        in_ungetc(d);
        return T_SEMI;
    case '&':
        d = in_getc();
        if (d == '&')
            return T_AND;
        in_ungetc(d);
        return T_AMP;
    case '|':
        d = in_getc();
        if (d == '|')
            return T_OR;
        in_ungetc(d);
        return T_PIPE;
    case '(': return T_LP;
    case ')': return T_RP;
    case '<':
    case '>': return lex_redir(c, -1);
    }
    scan_word(c);
    /* IO_NUMBER: digits immediately followed by < or > */
    if (all_digits(wb.s, wb.len)) {
        d = in_getc();
        if (d == '<' || d == '>') {
            sb_cstr(&wb);
            return lex_redir(d, atoi(wb.s));
        }
        in_ungetc(d);
    }
    lx.word = sb_arena(&wb);
    return T_WORD;
}

static enum tok peek(void) {
    if (!lx.have) {
        lx.tok = lex();
        lx.have = 1;
    }
    return lx.tok;
}

static enum tok next(void) {
    enum tok t = peek();

    lx.have = 0;
    return t;
}

static int peek_kw(const char *kw) {
    return peek() == T_WORD && strcmp(lx.word, kw) == 0;
}

static void expect_kw(const char *kw) {
    if (!peek_kw(kw)) {
        char msg[64];

        snprintf(msg, sizeof(msg), "expected '%s'", kw);
        synerr(msg);
    }
    next();
}

static void linebreak(void) {
    while (peek() == T_NL)
        next();
}

/* ---- grammar ---- */

static struct node *and_or(void);
static struct node *command(void);

static struct node *mknode(enum ntype t) {
    struct node *n = aalloc(sizeof(*n));

    memset(n, 0, sizeof(*n));
    n->type = t;
    return n;
}

static struct node *mkbin(enum ntype t, struct node *l, struct node *r) {
    struct node *n = mknode(t);

    n->u.bin.l = l;
    n->u.bin.r = r;
    return n;
}

static struct word *mkword(char *text) {
    struct word *w = aalloc(sizeof(*w));

    w->next = NULL;
    w->text = text;
    return w;
}

static const char *const terminators[] = {"then", "else", "elif", "fi", "do",
                                          "done", "esac", "}",    NULL};

static int at_terminator(void) {
    enum tok t = peek();
    int i;

    if (t == T_EOF || t == T_RP || t == T_DSEMI)
        return 1;
    if (t != T_WORD)
        return 0;
    for (i = 0; terminators[i]; i++)
        if (strcmp(lx.word, terminators[i]) == 0)
            return 1;
    return 0;
}

/* compound_list: newline-separated and_or lists up to a terminator */
static struct node *compound_list(void) {
    struct node *head = NULL, *n;
    enum tok t;

    linebreak();
    while (!at_terminator()) {
        n = and_or();
        t = peek();
        if (t == T_AMP) {
            next();
            n = mkbin(N_BG, n, NULL);
        } else if (t == T_SEMI || t == T_NL)
            next();
        linebreak();
        head = head ? mkbin(N_SEQ, head, n) : n;
    }
    if (head == NULL)
        synerr("empty command list");
    return head;
}

static struct redir *parse_redir(void) {
    struct redir *r = aalloc(sizeof(*r));

    memset(r, 0, sizeof(*r));
    r->type = lx.rtype;
    r->fd = lx.rfd;
    if (next() != T_WORD)
        synerr("missing redirection target");
    if (r->type == R_HEREDOC) {
        struct hdpend *h = aalloc(sizeof(*h));

        h->r = r;
        h->striptabs = lx.striptabs;
        h->delim = unquote_delim(lx.word, &r->hd_quoted);
        h->next = lx.hd;
        lx.hd = h;
    } else
        r->target = mkword(lx.word);
    return r;
}

static struct redir *redir_list(void) {
    struct redir *head = NULL, **tail = &head;

    while (peek() == T_REDIR) {
        next();
        *tail = parse_redir();
        tail = &(*tail)->next;
    }
    return head;
}

static int is_assignment(const char *w) {
    const char *eq = strchr(w, '=');

    return eq && eq != w && is_name(w, (size_t)(eq - w));
}

static struct node *simple_command(void) {
    struct node *n = mknode(N_CMD);
    struct word **at = &n->u.cmd.assigns, **wt = &n->u.cmd.args;
    struct redir **rt = &n->redirs;

    for (;;) {
        enum tok t = peek();

        if (t == T_REDIR) {
            next();
            *rt = parse_redir();
            rt = &(*rt)->next;
        } else if (t == T_WORD) {
            next();
            if (n->u.cmd.args == NULL && is_assignment(lx.word)) {
                *at = mkword(lx.word);
                at = &(*at)->next;
            } else {
                *wt = mkword(lx.word);
                wt = &(*wt)->next;
            }
        } else
            break;
    }
    if (!n->u.cmd.args && !n->u.cmd.assigns && !n->redirs)
        synerr("unexpected token");
    return n;
}

static struct node *if_clause(void) {
    struct node *n = mknode(N_IF);

    n->u.ifn.cond = compound_list();
    expect_kw("then");
    n->u.ifn.then = compound_list();
    if (peek_kw("elif")) {
        next();
        n->u.ifn.els = if_clause();
        return n; /* inner if_clause consumed 'fi' */
    }
    if (peek_kw("else")) {
        next();
        n->u.ifn.els = compound_list();
    }
    expect_kw("fi");
    return n;
}

static struct node *do_group(void) {
    struct node *b;

    linebreak();
    expect_kw("do");
    b = compound_list();
    expect_kw("done");
    return b;
}

static struct node *for_clause(void) {
    struct node *n = mknode(N_FOR);
    struct word **wt = &n->u.forn.list;

    if (next() != T_WORD || !is_name(lx.word, strlen(lx.word)))
        synerr("bad for loop variable");
    n->u.forn.var = lx.word;
    linebreak();
    if (peek_kw("in")) {
        next();
        n->u.forn.has_in = 1;
        while (peek() == T_WORD) {
            next();
            *wt = mkword(lx.word);
            wt = &(*wt)->next;
        }
        if (peek() == T_SEMI || peek() == T_NL)
            next();
    } else if (peek() == T_SEMI)
        next();
    n->u.forn.body = do_group();
    return n;
}

static struct node *case_clause(void) {
    struct node *n = mknode(N_CASE);
    struct caseitem **it = &n->u.casen.items;

    if (next() != T_WORD)
        synerr("bad case subject");
    n->u.casen.subj = mkword(lx.word);
    linebreak();
    expect_kw("in");
    linebreak();
    while (!peek_kw("esac")) {
        struct caseitem *ci = aalloc(sizeof(*ci));
        struct word **pt = &ci->pats;

        memset(ci, 0, sizeof(*ci));
        if (peek() == T_LP)
            next();
        for (;;) {
            if (next() != T_WORD)
                synerr("bad case pattern");
            *pt = mkword(lx.word);
            pt = &(*pt)->next;
            if (peek() != T_PIPE)
                break;
            next();
        }
        if (next() != T_RP)
            synerr("expected ')' in case");
        linebreak();
        if (peek() != T_DSEMI && !peek_kw("esac"))
            ci->body = compound_list();
        if (peek() == T_DSEMI)
            next();
        linebreak();
        *it = ci;
        it = &ci->next;
    }
    next();
    return n;
}

static struct node *command(void) {
    struct node *n;

    if (peek() == T_LP) {
        next();
        n = mknode(N_SUBSHELL);
        n->u.grp.body = compound_list();
        if (next() != T_RP)
            synerr("expected ')'");
    } else if (peek_kw("{")) {
        next();
        n = mknode(N_BRACE);
        n->u.grp.body = compound_list();
        expect_kw("}");
    } else if (peek_kw("if")) {
        next();
        n = if_clause();
    } else if (peek_kw("while") || peek_kw("until")) {
        int w = lx.word[0] == 'w';

        next();
        n = mknode(w ? N_WHILE : N_UNTIL);
        n->u.ifn.cond = compound_list();
        n->u.ifn.then = do_group();
    } else if (peek_kw("for")) {
        next();
        n = for_clause();
    } else if (peek_kw("case")) {
        next();
        n = case_clause();
    } else {
        n = simple_command();
        /* NAME () compound-command */
        if (n->u.cmd.args && !n->u.cmd.args->next && !n->u.cmd.assigns &&
            !n->redirs && peek() == T_LP) {
            const char *nm = n->u.cmd.args->text;
            struct node *f;

            if (!is_name(nm, strlen(nm)))
                synerr("bad function name");
            next();
            if (next() != T_RP)
                synerr("expected ')' after function name");
            linebreak();
            f = mknode(N_FUNC);
            f->u.func.name = n->u.cmd.args->text;
            f->u.func.body = command();
            return f;
        }
        return n;
    }
    n->redirs = redir_list();
    return n;
}

static struct node *pipeline(void) {
    int neg = 0;
    struct node *n;

    if (peek_kw("!")) {
        next();
        neg = 1;
    }
    n = command();
    while (peek() == T_PIPE) {
        next();
        linebreak();
        n = mkbin(N_PIPE, n, command());
    }
    if (neg) {
        struct node *x = mknode(N_NOT);

        x->u.grp.body = n;
        n = x;
    }
    return n;
}

static struct node *and_or(void) {
    struct node *n = pipeline();

    for (;;) {
        enum tok t = peek();

        if (t != T_AND && t != T_OR)
            return n;
        next();
        linebreak();
        n = mkbin(t == T_AND ? N_AND : N_OR, n, pipeline());
    }
}

/*
 * One complete command line: and_or lists separated by ';' or '&' up to
 * a newline.  Returns NULL at EOF.  A blank line yields an empty N_SEQ
 * with both sides NULL so the caller can tell it from EOF.
 */
struct node *parse_command(void) {
    struct node *head = NULL, *n;
    enum tok t;

    in_set_ps2(0);
    t = peek();
    if (t == T_EOF) {
        lx.have = 0; /* don't leak EOF into an outer source (.) */
        lx.eof = 1;
        return NULL;
    }
    if (t == T_NL) {
        next();
        return mkbin(N_SEQ, NULL, NULL);
    }
    in_set_ps2(1);
    for (;;) {
        n = and_or();
        t = peek();
        if (t == T_AMP) {
            next();
            n = mkbin(N_BG, n, NULL);
        } else if (t == T_SEMI)
            next();
        head = head ? mkbin(N_SEQ, head, n) : n;
        t = peek();
        if (t == T_NL || t == T_EOF) {
            if (t == T_NL)
                next();
            break;
        }
        if (t != T_WORD && t != T_REDIR && t != T_LP)
            synerr("unexpected token");
    }
    in_set_ps2(0);
    return head;
}

int parse_eof(void) {
    return lx.eof;
}

struct node *parse_string_all(const char *s) {
    struct lexstate save = lx;
    struct node *volatile head = NULL;
    struct node *n;
    jmp_buf jb, *prev = sh.errjmp;

    memset(&lx, 0, sizeof(lx));
    in_push_string(s);
    sh.errjmp = &jb;
    if (setjmp(jb)) {
        in_pop();
        lx = save;
        sh.errjmp = prev;
        longjmp(*prev, 1); /* message already printed */
    }
    while ((n = parse_command()) != NULL) {
        if (n->type == N_SEQ && !n->u.bin.l && !n->u.bin.r)
            continue;
        head = head ? mkbin(N_SEQ, head, n) : n;
    }
    in_pop();
    lx = save;
    sh.errjmp = prev;
    return head;
}

/* ---- deep copy for function bodies (arena -> heap) ---- */

static struct word *words_copy(const struct word *w) {
    struct word *head = NULL, **t = &head;

    for (; w; w = w->next) {
        *t = xmalloc(sizeof(**t));
        (*t)->text = xstrdup(w->text);
        (*t)->next = NULL;
        t = &(*t)->next;
    }
    return head;
}

static void words_free(struct word *w) {
    while (w) {
        struct word *nx = w->next;

        free(w->text);
        free(w);
        w = nx;
    }
}

static struct redir *redirs_copy(const struct redir *r) {
    struct redir *head = NULL, **t = &head;

    for (; r; r = r->next) {
        *t = xmalloc(sizeof(**t));
        **t = *r;
        (*t)->target = words_copy(r->target);
        (*t)->next = NULL;
        t = &(*t)->next;
    }
    return head;
}

static void redirs_free(struct redir *r) {
    while (r) {
        struct redir *nx = r->next;

        words_free(r->target);
        free(r);
        r = nx;
    }
}

struct node *node_copy_heap(const struct node *n) {
    struct node *c;

    if (n == NULL)
        return NULL;
    c = xmalloc(sizeof(*c));
    *c = *n;
    c->redirs = redirs_copy(n->redirs);
    switch (n->type) {
    case N_CMD:
        c->u.cmd.assigns = words_copy(n->u.cmd.assigns);
        c->u.cmd.args = words_copy(n->u.cmd.args);
        break;
    case N_PIPE:
    case N_AND:
    case N_OR:
    case N_SEQ:
    case N_BG:
        c->u.bin.l = node_copy_heap(n->u.bin.l);
        c->u.bin.r = node_copy_heap(n->u.bin.r);
        break;
    case N_NOT:
    case N_SUBSHELL:
    case N_BRACE:    c->u.grp.body = node_copy_heap(n->u.grp.body); break;
    case N_IF:
    case N_WHILE:
    case N_UNTIL:
        c->u.ifn.cond = node_copy_heap(n->u.ifn.cond);
        c->u.ifn.then = node_copy_heap(n->u.ifn.then);
        c->u.ifn.els = node_copy_heap(n->u.ifn.els);
        break;
    case N_FOR:
        c->u.forn.var = xstrdup(n->u.forn.var);
        c->u.forn.list = words_copy(n->u.forn.list);
        c->u.forn.body = node_copy_heap(n->u.forn.body);
        break;
    case N_CASE: {
        const struct caseitem *ci;
        struct caseitem **t = &c->u.casen.items;

        c->u.casen.subj = words_copy(n->u.casen.subj);
        *t = NULL;
        for (ci = n->u.casen.items; ci; ci = ci->next) {
            *t = xmalloc(sizeof(**t));
            (*t)->pats = words_copy(ci->pats);
            (*t)->body = node_copy_heap(ci->body);
            (*t)->next = NULL;
            t = &(*t)->next;
        }
        break;
    }
    case N_FUNC:
        c->u.func.name = xstrdup(n->u.func.name);
        c->u.func.body = node_copy_heap(n->u.func.body);
        break;
    }
    return c;
}

void node_free_heap(struct node *n) {
    if (n == NULL)
        return;
    redirs_free(n->redirs);
    switch (n->type) {
    case N_CMD:
        words_free(n->u.cmd.assigns);
        words_free(n->u.cmd.args);
        break;
    case N_PIPE:
    case N_AND:
    case N_OR:
    case N_SEQ:
    case N_BG:
        node_free_heap(n->u.bin.l);
        node_free_heap(n->u.bin.r);
        break;
    case N_NOT:
    case N_SUBSHELL:
    case N_BRACE:    node_free_heap(n->u.grp.body); break;
    case N_IF:
    case N_WHILE:
    case N_UNTIL:
        node_free_heap(n->u.ifn.cond);
        node_free_heap(n->u.ifn.then);
        node_free_heap(n->u.ifn.els);
        break;
    case N_FOR:
        free(n->u.forn.var);
        words_free(n->u.forn.list);
        node_free_heap(n->u.forn.body);
        break;
    case N_CASE: {
        struct caseitem *ci = n->u.casen.items, *nx;

        words_free(n->u.casen.subj);
        for (; ci; ci = nx) {
            nx = ci->next;
            words_free(ci->pats);
            node_free_heap(ci->body);
            free(ci);
        }
        break;
    }
    case N_FUNC:
        free(n->u.func.name);
        node_free_heap(n->u.func.body);
        break;
    }
    free(n);
}
