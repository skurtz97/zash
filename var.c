/*
 * Shell variables.  Each variable is stored as a single "NAME=value"
 * string so the exported environment can be built as an array of
 * pointers into the table with no copying; the array is cached and only
 * rebuilt after an exported variable changes.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

#define VBUCKETS 256 /* power of two */

struct var {
    struct var *next;
    char *text; /* "NAME=value" */
    size_t nlen;
    int flags;
};

static struct var *tab[VBUCKETS];
static char **envcache;
static size_t envcap;
static int env_dirty = 1;

int is_name(const char *s, size_t n) {
    size_t i;

    if (n == 0 || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (i = 1; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_'))
            return 0;
    return 1;
}

static unsigned hash(const char *s, size_t n) {
    unsigned h = 2166136261u;

    while (n--)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h & (VBUCKETS - 1);
}

static struct var **lookup(const char *name, size_t n) {
    struct var **vp = &tab[hash(name, n)];

    for (; *vp; vp = &(*vp)->next)
        if ((*vp)->nlen == n && memcmp((*vp)->text, name, n) == 0)
            break;
    return vp;
}

static void special_hook(const char *name, size_t n) {
    if (n == 4 && memcmp(name, "PATH", 4) == 0)
        path_hash_clear();
}

static int setn(const char *name, size_t n, const char *val, int flags) {
    struct var **vp = lookup(name, n), *v = *vp;
    size_t vl = val ? strlen(val) : 0;

    if (v && (v->flags & V_READONLY)) {
        sh_warn("%.*s: is read only", (int)n, name);
        return 1;
    }
    if (v == NULL) {
        v = xmalloc(sizeof(*v));
        v->next = NULL;
        v->text = NULL;
        v->nlen = n;
        v->flags = 0;
        *vp = v;
    }
    free(v->text);
    v->text = xmalloc(n + vl + 2);
    memcpy(v->text, name, n);
    v->text[n] = '=';
    if (val)
        memcpy(v->text + n + 1, val, vl);
    v->text[n + 1 + vl] = '\0';
    v->flags = (v->flags & ~V_UNSET) | flags;
    if (val == NULL)
        v->flags |= V_UNSET;
    if (v->flags & V_EXPORT)
        env_dirty = 1;
    special_hook(name, n);
    return 0;
}

int var_set(const char *name, const char *val, int flags) {
    return setn(name, strlen(name), val, flags);
}

int var_setkv(const char *assign, int flags) {
    const char *eq = strchr(assign, '=');

    if (eq == NULL)
        return 1;
    return setn(assign, (size_t)(eq - assign), eq + 1, flags);
}

const char *var_get(const char *name) {
    struct var *v = *lookup(name, strlen(name));

    if (v == NULL || (v->flags & V_UNSET))
        return NULL;
    return v->text + v->nlen + 1;
}

int var_flags(const char *name) {
    struct var *v = *lookup(name, strlen(name));

    return v ? v->flags : V_UNSET;
}

void var_setflags(const char *name, int flags) {
    size_t n = strlen(name);
    struct var *v = *lookup(name, n);

    if (v == NULL) {
        setn(name, n, NULL, flags);
        return;
    }
    v->flags |= flags;
    if (flags & V_EXPORT)
        env_dirty = 1;
}

int var_unset(const char *name) {
    size_t n = strlen(name);
    struct var **vp = lookup(name, n), *v = *vp;

    if (v == NULL)
        return 0;
    if (v->flags & V_READONLY) {
        sh_warn("%s: is read only", name);
        return 1;
    }
    if (v->flags & V_EXPORT)
        env_dirty = 1;
    *vp = v->next;
    free(v->text);
    free(v);
    special_hook(name, n);
    return 0;
}

void var_init(char **envp) {
    char buf[32];

    for (; envp && *envp; envp++)
        if (strchr(*envp, '='))
            var_setkv(*envp, V_EXPORT);
    if (var_get("PS1") == NULL)
        var_set("PS1", "\\$ ", 0);
    if (var_get("PS2") == NULL)
        var_set("PS2", "> ", 0);
    if (var_get("PS4") == NULL)
        var_set("PS4", "+ ", 0);
    if (var_get("IFS") == NULL)
        var_set("IFS", " \t\n", 0);
    snprintf(buf, sizeof(buf), "%ld", (long)sh.pid);
    var_set("PPID", buf, 0); /* overwritten by main */
}

char **var_environ(void) {
    size_t i, n = 0;
    struct var *v;

    if (!env_dirty)
        return envcache;
    for (i = 0; i < VBUCKETS; i++)
        for (v = tab[i]; v; v = v->next)
            if ((v->flags & (V_EXPORT | V_UNSET)) == V_EXPORT)
                n++;
    if (n + 1 > envcap) {
        envcap = n + 16;
        envcache = xrealloc(envcache, envcap * sizeof(char *));
    }
    n = 0;
    for (i = 0; i < VBUCKETS; i++)
        for (v = tab[i]; v; v = v->next)
            if ((v->flags & (V_EXPORT | V_UNSET)) == V_EXPORT)
                envcache[n++] = v->text;
    envcache[n] = NULL;
    env_dirty = 0;
    return envcache;
}

static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void var_print(int flags) {
    size_t i, n = 0, k;
    struct var *v;
    char **list;
    const char *pfx;

    for (i = 0; i < VBUCKETS; i++)
        for (v = tab[i]; v; v = v->next)
            n++;
    list = xmalloc((n + 1) * sizeof(char *));
    n = 0;
    for (i = 0; i < VBUCKETS; i++)
        for (v = tab[i]; v; v = v->next)
            if (flags == 0 || (v->flags & flags))
                list[n++] = v->text;
    qsort(list, n, sizeof(char *), cmpstr);
    pfx = flags & V_EXPORT ? "export " : flags & V_READONLY ? "readonly " : "";
    for (k = 0; k < n; k++) {
        const char *eq = strchr(list[k], '='), *p;
        struct var *vv = *lookup(list[k], (size_t)(eq - list[k]));

        fputs(pfx, stdout);
        fwrite(list[k], 1, (size_t)(eq - list[k]), stdout);
        if (vv->flags & V_UNSET) {
            putchar('\n');
            continue;
        }
        fputs("='", stdout);
        for (p = eq + 1; *p; p++)
            if (*p == '\'')
                fputs("'\\''", stdout);
            else
                putchar(*p);
        fputs("'\n", stdout);
    }
    free(list);
}

void var_foreach_prefix(const char *pre, void *cands) {
    size_t pl = strlen(pre), i;
    struct var *v;
    char name[256];

    for (i = 0; i < VBUCKETS; i++)
        for (v = tab[i]; v; v = v->next)
            if (v->nlen >= pl && v->nlen < sizeof(name) &&
                !(v->flags & V_UNSET) && memcmp(v->text, pre, pl) == 0) {
                memcpy(name, v->text, v->nlen);
                name[v->nlen] = '\0';
                complete_add(cands, name);
            }
}

/* ---- temporary assignments ---- */

struct var_save {
    struct var_save *next;
    char *name;
    char *oldval; /* NULL if previously unset */
    int oldflags;
    int existed;
};

struct var_save *var_save_and_set(struct word *assigns) {
    struct var_save *head = NULL;

    for (; assigns; assigns = assigns->next) {
        char *val = expand_str(assigns->text, EXP_TILDE);
        char *eq = strchr(val, '=');
        struct var_save *s = xmalloc(sizeof(*s));
        const char *old;

        *eq = '\0';
        s->name = xstrdup(val);
        old = var_get(val);
        s->existed = (var_flags(val) & V_UNSET) == 0 || old;
        s->oldval = old ? xstrdup(old) : NULL;
        s->oldflags = var_flags(val);
        s->next = head;
        head = s;
        var_set(val, eq + 1, V_EXPORT);
    }
    return head;
}

void var_restore(struct var_save *s) {
    while (s) {
        struct var_save *nx = s->next;
        size_t n = strlen(s->name);
        struct var *v = *lookup(s->name, n);

        if (s->oldval) {
            if (v)
                v->flags &= ~V_EXPORT;
            setn(s->name, n, s->oldval, s->oldflags & ~V_UNSET);
            if (!(s->oldflags & V_EXPORT) && (v = *lookup(s->name, n)))
                v->flags &= ~V_EXPORT;
        } else
            var_unset(s->name);
        env_dirty = 1;
        free(s->name);
        free(s->oldval);
        free(s);
        s = nx;
    }
}
