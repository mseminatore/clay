/* semver.c -- npm-style version ranges for brick.json's "clay" field.
 *
 * A port of stdlib/semver's satisfies (lib/stdlib/semver/src/main.clay);
 * selfhost/clay-bricks calls that brick, so the two must agree. */
#include "semver.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define SV_PRE   64
#define SV_COMPS 32     /* comparators in one alternative */
#define SV_TOKS  32

typedef struct {
    long major, minor, patch;
    char pre[SV_PRE];
} SemVer;

typedef struct {
    char   op[3];       /* "<", "<=", ">", ">=", "=" */
    SemVer v;
} SvComp;

/* A version number: digits, no leading zero. -1 if invalid. */
static long sv_num(const char *s, size_t n) {
    if (n == 0 || n > 15) return -1;
    if (n > 1 && s[0] == '0') return -1;
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static int sv_is_wild(const char *s, size_t n) {
    return n == 1 && (s[0] == '*' || s[0] == 'x' || s[0] == 'X');
}

/* "1.2.3[-pre][+build]" -> v. 0 on success. */
static int sv_parse(const char *text, SemVer *v) {
    char s[128];
    snprintf(s, sizeof(s), "%s", text);
    char *plus = strchr(s, '+');
    if (plus) {
        if (plus[1] == '\0') return -1;
        *plus = '\0';
    }
    v->pre[0] = '\0';
    char *dash = strchr(s, '-');
    if (dash) {
        if (dash[1] == '\0') return -1;
        snprintf(v->pre, sizeof(v->pre), "%s", dash + 1);
        *dash = '\0';
    }
    long parts[3];
    int n = 0;
    const char *p = s;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (n >= 3) return -1;
        parts[n] = sv_num(p, len);
        if (parts[n] < 0) return -1;
        n++;
        if (!dot) break;
        p = dot + 1;
    }
    if (n != 3) return -1;
    v->major = parts[0];
    v->minor = parts[1];
    v->patch = parts[2];
    return 0;
}

static int sv_cmp_int(long a, long b) { return a < b ? -1 : (a > b ? 1 : 0); }

static int sv_numeric(const char *s, size_t n) {
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* Prerelease precedence: none > any; dot-separated identifiers, numbers
 * below words, numbers numerically, words in ASCII order */
static int sv_cmp_pre(const char *a, const char *b) {
    if (strcmp(a, b) == 0) return 0;
    if (!a[0]) return 1;
    if (!b[0]) return -1;
    for (;;) {
        size_t la = strcspn(a, "."), lb = strcspn(b, ".");
        int na = sv_numeric(a, la), nb = sv_numeric(b, lb);
        int c;
        if (na && nb) {
            long x = 0, y = 0;
            for (size_t i = 0; i < la; i++) x = x * 10 + (a[i] - '0');
            for (size_t i = 0; i < lb; i++) y = y * 10 + (b[i] - '0');
            c = sv_cmp_int(x, y);
        } else if (na) {
            c = -1;
        } else if (nb) {
            c = 1;
        } else {
            size_t m = la < lb ? la : lb;
            c = 0;
            for (size_t i = 0; i < m && c == 0; i++)
                c = sv_cmp_int((unsigned char)a[i], (unsigned char)b[i]);
            if (c == 0) c = sv_cmp_int((long)la, (long)lb);
        }
        if (c != 0) return c;
        int ea = a[la] == '\0', eb = b[lb] == '\0';
        if (ea || eb) return ea && eb ? 0 : (ea ? -1 : 1);
        a += la + 1;
        b += lb + 1;
    }
}

static int sv_compare(const SemVer *a, const SemVer *b) {
    int c = sv_cmp_int(a->major, b->major);
    if (!c) c = sv_cmp_int(a->minor, b->minor);
    if (!c) c = sv_cmp_int(a->patch, b->patch);
    if (!c) c = sv_cmp_pre(a->pre, b->pre);
    return c;
}

static SemVer sv_make(long maj, long min, long pat, const char *pre) {
    SemVer v;
    v.major = maj;
    v.minor = min;
    v.patch = pat;
    snprintf(v.pre, sizeof(v.pre), "%s", pre);
    return v;
}

/* A partial version "1", "1.2", "1.2.3", "1.x", "*": -1 for a wildcard or
 * missing part. 0 on success. */
static int sv_partial(const char *text, long out[3], char *pre, size_t pre_sz) {
    char s[128];
    const char *t = text;
    if (*t == 'v' || *t == 'V') t++;
    snprintf(s, sizeof(s), "%s", t);
    char *plus = strchr(s, '+');
    if (plus) *plus = '\0';
    pre[0] = '\0';
    char *dash = strchr(s, '-');
    if (dash) {
        snprintf(pre, pre_sz, "%s", dash + 1);
        *dash = '\0';
    }
    out[0] = out[1] = out[2] = -1;
    if (s[0] == '\0' || sv_is_wild(s, strlen(s))) return 0;
    int i = 0, stop = 0;
    const char *p = s;
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (i >= 3) return -1;
        if (sv_is_wild(p, len) || stop) {
            stop = 1;
        } else {
            out[i] = sv_num(p, len);
            if (out[i] < 0) return -1;
        }
        i++;
        if (!dot) break;
        p = dot + 1;
    }
    return 0;
}

static int sv_push(SvComp *set, int *n, const char *op, SemVer v) {
    if (*n >= SV_COMPS) return -1;
    snprintf(set[*n].op, sizeof(set[*n].op), "%s", op);
    set[*n].v = v;
    (*n)++;
    return 0;
}

/* One comparator token ("^1.2", ">=1.0.0", "1.x") -> comparators. 0 on success. */
static int sv_comparators(const char *tok, SvComp *set, int *n) {
    static const char *ops[] = { "<=", ">=", "<", ">", "=", "^", "~", NULL };
    char op[3] = "";
    for (int i = 0; ops[i]; i++) {
        if (strncmp(tok, ops[i], strlen(ops[i])) == 0) {
            snprintf(op, sizeof(op), "%s", ops[i]);
            break;
        }
    }
    const char *rest = tok + strlen(op);
    if (strcmp(op, "~") == 0 && *rest == '>') rest++;     /* ~> is ~ */
    while (*rest == ' ') rest++;
    long p[3];
    char pre[SV_PRE];
    if (sv_partial(rest, p, pre, sizeof(pre)) != 0) return -1;
    long maj = p[0], min = p[1], pat = p[2];

    if (maj < 0) {
        if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0)
            return sv_push(set, n, "<", sv_make(0, 0, 0, "0"));
        return 0;                                           /* anything */
    }
    if (strcmp(op, "^") == 0) {
        SemVer lo = sv_make(maj, min < 0 ? 0 : min, pat < 0 ? 0 : pat, pre), hi;
        if (maj > 0) hi = sv_make(maj + 1, 0, 0, "");
        else if (min < 0) hi = sv_make(1, 0, 0, "");
        else if (min > 0 || pat < 0) hi = sv_make(0, min + 1, 0, "");
        else hi = sv_make(0, 0, pat + 1, "");
        if (sv_push(set, n, ">=", lo) != 0) return -1;
        return sv_push(set, n, "<", hi);
    }
    int plain = op[0] == '\0' || strcmp(op, "=") == 0;
    if (pat >= 0 && plain) return sv_push(set, n, "=", sv_make(maj, min, pat, pre));
    if (plain || strcmp(op, "~") == 0) {
        SemVer lo = sv_make(maj, min < 0 ? 0 : min, pat < 0 ? 0 : pat, pre);
        SemVer hi = min < 0 ? sv_make(maj + 1, 0, 0, "") : sv_make(maj, min + 1, 0, "");
        if (sv_push(set, n, ">=", lo) != 0) return -1;
        return sv_push(set, n, "<", hi);
    }
    /* < <= > >= with a partial: fill with zeros, or bump past it */
    SemVer full = sv_make(maj, min < 0 ? 0 : min, pat < 0 ? 0 : pat, pre);
    if (pat >= 0) return sv_push(set, n, op, full);
    SemVer next = min < 0 ? sv_make(maj + 1, 0, 0, "") : sv_make(maj, min + 1, 0, "");
    if (strcmp(op, ">=") == 0) return sv_push(set, n, ">=", full);
    if (strcmp(op, "<") == 0) return sv_push(set, n, "<", full);
    if (strcmp(op, ">") == 0) return sv_push(set, n, ">=", next);
    return sv_push(set, n, "<", next);                      /* <= */
}

static int sv_test(const char *op, const SemVer *v, const SemVer *t) {
    int c = sv_compare(v, t);
    if (strcmp(op, "=") == 0) return c == 0;
    if (strcmp(op, "<") == 0) return c < 0;
    if (strcmp(op, "<=") == 0) return c <= 0;
    if (strcmp(op, ">") == 0) return c > 0;
    return c >= 0;
}

static int sv_satisfies_set(const SemVer *v, const SvComp *set, int n) {
    for (int i = 0; i < n; i++)
        if (!sv_test(set[i].op, v, &set[i].v)) return 0;
    if (!v->pre[0]) return 1;
    /* a prerelease must be named by a comparator with the same x.y.z */
    for (int i = 0; i < n; i++) {
        const SemVer *t = &set[i].v;
        if (t->pre[0] && strcmp(t->pre, "0") != 0 && t->major == v->major &&
            t->minor == v->minor && t->patch == v->patch)
            return 1;
    }
    return 0;
}

static void sv_trim(char *s) {
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static int sv_is_op(const char *t) {
    return !strcmp(t, "<") || !strcmp(t, "<=") || !strcmp(t, ">") || !strcmp(t, ">=") ||
           !strcmp(t, "=") || !strcmp(t, "^") || !strcmp(t, "~");
}

/* One alternative (no "||") -> comparators. 0 on success. */
static int sv_parse_set(char *alt, SvComp *set, int *n) {
    sv_trim(alt);
    char *hy = strstr(alt, " - ");
    if (hy) {
        char a[128], b[128], ta[132], tb[132];
        *hy = '\0';
        snprintf(a, sizeof(a), "%s", alt);
        snprintf(b, sizeof(b), "%s", hy + 3);
        sv_trim(a);
        sv_trim(b);
        snprintf(ta, sizeof(ta), ">=%s", a);
        snprintf(tb, sizeof(tb), "<=%s", b);
        if (sv_comparators(ta, set, n) != 0) return -1;
        return sv_comparators(tb, set, n);
    }
    /* "> = 1.2" / ">= 1.2": join an operator to its version */
    char pending[16] = "";
    char *next = alt;
    while (*next) {
        while (*next == ' ') next++;
        if (!*next) break;
        char *t = next;
        while (*next && *next != ' ') next++;
        if (*next) *next++ = '\0';
        if (sv_is_op(t)) {
            if (strlen(pending) + strlen(t) >= sizeof(pending)) return -1;
            strcat(pending, t);
            continue;
        }
        char tok[160];
        snprintf(tok, sizeof(tok), "%s%s", pending, t);
        pending[0] = '\0';
        if (sv_comparators(tok, set, n) != 0) return -1;
    }
    return 0;
}

int semver_satisfies(const char *version, const char *range) {
    SemVer v;
    if (!version || !range || sv_parse(version, &v) != 0) return -1;
    char buf[512];
    if (strlen(range) >= sizeof(buf)) return -1;
    snprintf(buf, sizeof(buf), "%s", range);
    int hit = 0;
    char *alt = buf;
    for (;;) {
        char *bar = strstr(alt, "||");
        if (bar) *bar = '\0';
        SvComp set[SV_COMPS];
        int n = 0;
        if (sv_parse_set(alt, set, &n) != 0) return -1;
        if (sv_satisfies_set(&v, set, n)) hit = 1;
        if (!bar) break;
        alt = bar + 2;
    }
    return hit;
}
