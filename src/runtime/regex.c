/*
 * regex.c - Regex prototype for Clay runtime
 *
 * Cross-platform regex support:
 * - Unix/macOS: POSIX <regex.h> with ERE (Extended Regular Expressions)
 * - Windows: Built-in minimal regex engine
 */
#include "clay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ===== Platform regex abstraction ===== */

/*
 * re_search(pat, text, &match_start, &match_len) -> 1 if found, 0 if not
 * Finds first match of ERE pattern in text.
 */

#ifdef _WIN32

/* ---------- Minimal ERE engine for Windows ----------
 * Supports: . ^ $ * + ? [...] [^...] \d \w \s \D \W \S | (...) \\
 * Recursive backtracking with depth limit.
 */

/* Match one pattern element vs one char. Returns 1 if match, 0 otherwise.
 * Advances *pat_end past the element. */
static int re_match_one(const char *pat, char ch, const char **pat_end) {
    if (ch == '\0') return 0;
    if (*pat == '.') { *pat_end = pat + 1; return 1; }
    if (*pat == '\\' && pat[1]) {
        *pat_end = pat + 2;
        switch (pat[1]) {
            case 'd': return isdigit((unsigned char)ch) != 0;
            case 'D': return isdigit((unsigned char)ch) == 0;
            case 'w': return (isalnum((unsigned char)ch) || ch == '_');
            case 'W': return !(isalnum((unsigned char)ch) || ch == '_');
            case 's': return isspace((unsigned char)ch) != 0;
            case 'S': return isspace((unsigned char)ch) == 0;
            default:  return ch == pat[1];
        }
    }
    if (*pat == '[') {
        const char *p = pat + 1;
        int negate = 0;
        if (*p == '^') { negate = 1; p++; }
        int matched = 0;
        while (*p && *p != ']') {
            if (p[1] == '-' && p[2] && p[2] != ']') {
                if ((unsigned char)ch >= (unsigned char)p[0] &&
                    (unsigned char)ch <= (unsigned char)p[2]) matched = 1;
                p += 3;
            } else if (*p == '\\' && p[1]) {
                const char *dummy;
                char esc_pat[3] = {'\\', p[1], '\0'};
                if (re_match_one(esc_pat, ch, &dummy)) matched = 1;
                p += 2;
            } else {
                if (ch == *p) matched = 1;
                p++;
            }
        }
        if (*p == ']') p++;
        *pat_end = p;
        return negate ? !matched : matched;
    }
    *pat_end = pat + 1;
    return ch == *pat;
}

/* Skip past one pattern element (for measuring element boundaries) */
static const char *re_skip_elem(const char *pat) {
    if (*pat == '[') {
        const char *p = pat + 1;
        if (*p == '^') p++;
        while (*p && *p != ']') {
            if (*p == '\\' && p[1]) p += 2; else p++;
        }
        return *p == ']' ? p + 1 : p;
    }
    if (*pat == '\\' && pat[1]) return pat + 2;
    return pat + 1;
}

/* Find matching ')' */
static const char *re_find_close(const char *pat) {
    int d = 1;
    const char *p = pat + 1;
    while (*p && d > 0) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '(') d++;
        if (*p == ')') { d--; if (d == 0) return p; }
        p++;
    }
    return NULL;
}

/* Core recursive matcher. Returns pointer past matched text, or NULL. */
static const char *re_try(const char *pat, const char *text, int depth) {
    if (depth > 500) return NULL;

    while (*pat) {
        if (*pat == '$' && pat[1] == '\0')
            return (*text == '\0') ? text : NULL;

        /* Group with alternation */
        if (*pat == '(') {
            const char *close = re_find_close(pat);
            if (!close) return NULL;
            const char *after_group = close + 1;
            char group_quant = *after_group;
            const char *after_quant = after_group;
            if (group_quant == '*' || group_quant == '+' || group_quant == '?')
                after_quant = after_group + 1;
            else
                group_quant = 0;

            /* Extract alternatives */
            if (group_quant == 0) {
                /* No quantifier: try each | alternative */
                const char *alt = pat + 1;
                while (alt <= close) {
                    const char *aend = alt;
                    int pd = 0;
                    while (aend < close) {
                        if (*aend == '\\' && aend[1]) { aend += 2; continue; }
                        if (*aend == '(') pd++;
                        else if (*aend == ')') pd--;
                        else if (*aend == '|' && pd == 0) break;
                        aend++;
                    }
                    /* Try: alt_text + rest */
                    size_t alen = (size_t)(aend - alt);
                    size_t rlen = strlen(after_group);
                    char *sub = (char *)malloc(alen + rlen + 1);
                    memcpy(sub, alt, alen);
                    memcpy(sub + alen, after_group, rlen + 1);
                    const char *r = re_try(sub, text, depth + 1);
                    free(sub);
                    if (r) return r;
                    alt = aend < close ? aend + 1 : close + 1;
                }
                return NULL;
            }

            /* Group with quantifier */
            /* Try matching the group contents repeatedly */
            if (group_quant == '?' || group_quant == '*') {
                /* Try zero matches first for ? , or last for * */
                if (group_quant == '?') {
                    /* Try 1 match, then 0 */
                    const char *alt = pat + 1;
                    while (alt < close) {
                        const char *aend = alt;
                        int pd = 0;
                        while (aend < close) {
                            if (*aend == '\\' && aend[1]) { aend += 2; continue; }
                            if (*aend == '(') pd++;
                            else if (*aend == ')') pd--;
                            else if (*aend == '|' && pd == 0) break;
                            aend++;
                        }
                        size_t alen = (size_t)(aend - alt);
                        char *sub = (char *)malloc(alen + 1);
                        memcpy(sub, alt, alen);
                        sub[alen] = '\0';
                        const char *r = re_try(sub, text, depth + 1);
                        free(sub);
                        if (r) {
                            const char *rest = re_try(after_quant, r, depth + 1);
                            if (rest) return rest;
                        }
                        alt = aend < close ? aend + 1 : close + 1;
                    }
                    return re_try(after_quant, text, depth + 1);
                }
                /* * : greedy, try max matches then backtrack */
                /* Collect match positions */
                const char *positions[256];
                int npos = 0;
                positions[npos++] = text;
                const char *cur = text;
                while (npos < 255) {
                    int found = 0;
                    const char *alt = pat + 1;
                    while (alt < close) {
                        const char *aend = alt;
                        int pd = 0;
                        while (aend < close) {
                            if (*aend == '\\' && aend[1]) { aend += 2; continue; }
                            if (*aend == '(') pd++;
                            else if (*aend == ')') pd--;
                            else if (*aend == '|' && pd == 0) break;
                            aend++;
                        }
                        size_t alen = (size_t)(aend - alt);
                        char *sub = (char *)malloc(alen + 1);
                        memcpy(sub, alt, alen);
                        sub[alen] = '\0';
                        const char *r = re_try(sub, cur, depth + 1);
                        free(sub);
                        if (r && r > cur) { cur = r; positions[npos++] = cur; found = 1; break; }
                        alt = aend < close ? aend + 1 : close + 1;
                    }
                    if (!found) break;
                }
                for (int i = npos - 1; i >= 0; i--) {
                    const char *rest = re_try(after_quant, positions[i], depth + 1);
                    if (rest) return rest;
                }
                return NULL;
            }
            if (group_quant == '+') {
                /* Like * but must match at least once */
                const char *positions[256];
                int npos = 0;
                const char *cur = text;
                while (npos < 255) {
                    int found = 0;
                    const char *alt = pat + 1;
                    while (alt < close) {
                        const char *aend = alt;
                        int pd = 0;
                        while (aend < close) {
                            if (*aend == '\\' && aend[1]) { aend += 2; continue; }
                            if (*aend == '(') pd++;
                            else if (*aend == ')') pd--;
                            else if (*aend == '|' && pd == 0) break;
                            aend++;
                        }
                        size_t alen = (size_t)(aend - alt);
                        char *sub = (char *)malloc(alen + 1);
                        memcpy(sub, alt, alen);
                        sub[alen] = '\0';
                        const char *r = re_try(sub, cur, depth + 1);
                        free(sub);
                        if (r && r > cur) { cur = r; positions[npos++] = cur; found = 1; break; }
                        alt = aend < close ? aend + 1 : close + 1;
                    }
                    if (!found) break;
                }
                if (npos == 0) return NULL;
                for (int i = npos - 1; i >= 0; i--) {
                    const char *rest = re_try(after_quant, positions[i], depth + 1);
                    if (rest) return rest;
                }
                return NULL;
            }
        }

        /* Top-level alternation */
        {
            const char *p = pat;
            int pd = 0;
            while (*p) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == '(') pd++;
                else if (*p == ')') pd--;
                else if (*p == '|' && pd == 0) {
                    size_t llen = (size_t)(p - pat);
                    char *left = (char *)malloc(llen + 1);
                    memcpy(left, pat, llen);
                    left[llen] = '\0';
                    const char *r = re_try(left, text, depth + 1);
                    free(left);
                    if (r) return r;
                    return re_try(p + 1, text, depth + 1);
                }
                p++;
            }
        }

        /* Single element + optional quantifier */
        const char *elem_end = re_skip_elem(pat);
        char quant = *elem_end;
        if (quant == '*' || quant == '+' || quant == '?') {
            const char *rest = elem_end + 1;
            if (quant == '?') {
                const char *pe;
                if (re_match_one(pat, *text, &pe)) {
                    const char *r = re_try(rest, text + 1, depth + 1);
                    if (r) return r;
                }
                return re_try(rest, text, depth + 1);
            }
            /* Greedy * or + */
            const char *t = text;
            int count = 0;
            const char *pe;
            while (*t && re_match_one(pat, *t, &pe)) { t++; count++; }
            int min = (quant == '+') ? 1 : 0;
            for (int i = count; i >= min; i--) {
                const char *r = re_try(rest, text + i, depth + 1);
                if (r) return r;
            }
            return NULL;
        }

        /* Plain element */
        const char *pe;
        if (!re_match_one(pat, *text, &pe)) return NULL;
        text++;
        pat = elem_end;
    }
    return text;
}

static int re_search(const char *pat, const char *text, int *match_start, int *match_len) {
    int anchored = 0;
    if (*pat == '^') { anchored = 1; pat++; }

    const char *start = anchored ? text : text;
    const char *end = text + strlen(text);

    for (const char *t = start; t <= end; t++) {
        const char *matched_end = re_try(pat, t, 0);
        if (matched_end) {
            *match_start = (int)(t - text);
            *match_len = (int)(matched_end - t);
            return 1;
        }
        if (anchored || *t == '\0') break;
    }
    return 0;
}

#else
/* ---------- Unix/macOS: POSIX regex ---------- */
#include <regex.h>

static int re_search(const char *pat, const char *text, int *match_start, int *match_len) {
    regex_t re;
    regmatch_t pmatch[1];
    if (regcomp(&re, pat, REG_EXTENDED) != 0) return 0;
    int result = regexec(&re, text, 1, pmatch, 0);
    if (result == 0) {
        *match_start = (int)pmatch[0].rm_so;
        *match_len = (int)(pmatch[0].rm_eo - pmatch[0].rm_so);
        regfree(&re);
        return 1;
    }
    regfree(&re);
    return 0;
}

#endif /* _WIN32 */

/* ===== Regex prototype methods ===== */

/* Regex match(str, pattern) -> first match string or nil */
static ClayValue regex_match(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) return CLAY_NIL;
    if (!CLAY_IS_OBJ(argv[0])) return CLAY_NIL;
    if (!CLAY_IS_OBJ(argv[1])) return CLAY_NIL;

    const char *text = clay_string_data(CLAY_OBJ(argv[0]));
    const char *pat = clay_string_data(CLAY_OBJ(argv[1]));

    int start, len;
    if (re_search(pat, text, &start, &len)) {
        return CLAY_VAL(clay_string_new_len(text + start, (size_t)len));
    }
    return CLAY_NIL;
}

/* Regex test(str, pattern) -> true/false */
static ClayValue regex_test(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) return CLAY_FALSE;
    if (!CLAY_IS_OBJ(argv[0])) return CLAY_FALSE;
    if (!CLAY_IS_OBJ(argv[1])) return CLAY_FALSE;

    const char *text = clay_string_data(CLAY_OBJ(argv[0]));
    const char *pat = clay_string_data(CLAY_OBJ(argv[1]));

    int start, len;
    return CLAY_BOOL(re_search(pat, text, &start, &len));
}

/* Regex replace(str, pattern, replacement) -> new string */
static ClayValue regex_replace(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 3) return argc > 0 ? argv[0] : CLAY_NIL;
    if (!CLAY_IS_OBJ(argv[0])) return argv[0];
    if (!CLAY_IS_OBJ(argv[1])) return argv[0];
    if (!CLAY_IS_OBJ(argv[2])) return argv[0];

    const char *text = clay_string_data(CLAY_OBJ(argv[0]));
    const char *pat = clay_string_data(CLAY_OBJ(argv[1]));
    const char *rep = clay_string_data(CLAY_OBJ(argv[2]));
    size_t rep_len = strlen(rep);

    /* Build result by replacing all matches */
    size_t result_cap = strlen(text) + 64;
    char *result = (char *)malloc(result_cap);
    size_t result_len = 0;
    const char *cursor = text;

    while (*cursor) {
        int start, len;
        if (re_search(pat, cursor, &start, &len) && len > 0) {
            /* Copy text before match */
            size_t needed = result_len + (size_t)start + rep_len + 1;
            if (needed > result_cap) {
                result_cap = needed * 2;
                result = (char *)realloc(result, result_cap);
            }
            memcpy(result + result_len, cursor, (size_t)start);
            result_len += (size_t)start;
            /* Copy replacement */
            memcpy(result + result_len, rep, rep_len);
            result_len += rep_len;
            cursor += start + len;
        } else {
            /* No more matches — copy rest */
            size_t rest = strlen(cursor);
            size_t needed = result_len + rest + 1;
            if (needed > result_cap) {
                result_cap = needed * 2;
                result = (char *)realloc(result, result_cap);
            }
            memcpy(result + result_len, cursor, rest);
            result_len += rest;
            break;
        }
    }
    result[result_len] = '\0';

    ClayValue ret = CLAY_VAL(clay_string_new_len(result, result_len));
    free(result);
    return ret;
}

/* Regex find(str, pattern) -> array of all match strings */
static ClayValue regex_find(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) return CLAY_VAL(clay_array_new());
    if (!CLAY_IS_OBJ(argv[0])) return CLAY_VAL(clay_array_new());
    if (!CLAY_IS_OBJ(argv[1])) return CLAY_VAL(clay_array_new());

    const char *text = clay_string_data(CLAY_OBJ(argv[0]));
    const char *pat = clay_string_data(CLAY_OBJ(argv[1]));

    ClayObject *arr = clay_array_new();
    const char *cursor = text;

    while (*cursor) {
        int start, len;
        if (re_search(pat, cursor, &start, &len) && len > 0) {
            ClayObject *s = clay_string_new_len(cursor + start, (size_t)len);
            clay_array_append(arr, CLAY_VAL(s));
            cursor += start + len;
        } else {
            break;
        }
    }

    return CLAY_VAL(arr);
}

/* Public API for String convenience methods */
int clay_regex_search(const char *pat, const char *text, int *match_start, int *match_len) {
    return re_search(pat, text, match_start, match_len);
}

/* Register Regex methods */
void clay_regex_init_methods(ClayObject *proto) {
    clay_set_method(proto, "match", regex_match);
    clay_set_method(proto, "test", regex_test);
    clay_set_method(proto, "replace", regex_replace);
    clay_set_method(proto, "find", regex_find);
}
