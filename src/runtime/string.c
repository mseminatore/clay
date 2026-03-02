/*
 * string.c - String object implementation
 */
#include "clay.h"
#include "range.h"
#include <ctype.h>

ClayObject *clay_string_new(const char *str) {
    return clay_string_new_len(str, strlen(str));
}

ClayObject *clay_string_new_len(const char *str, size_t len) {
    ClayObject *obj = clay_object_new();
    obj->type = CLAY_TYPE_STRING;
    obj->parent = clay_String;
    if (clay_String) clay_retain(clay_String);
    obj->as.string.data = (char *)malloc(len + 1);
    memcpy(obj->as.string.data, str, len);
    obj->as.string.data[len] = '\0';
    obj->as.string.length = len;
    return obj;
}

/* Allocate a string object with an uninitialized buffer of given length.
 * Caller must fill data and NUL-terminate before use. */
static ClayObject *string_new_prealloc(size_t len) {
    ClayObject *obj = clay_object_new();
    obj->type = CLAY_TYPE_STRING;
    obj->parent = clay_String;
    if (clay_String) clay_retain(clay_String);
    obj->as.string.data = (char *)malloc(len + 1);
    obj->as.string.length = len;
    return obj;
}

const char *clay_string_data(ClayObject *obj) {
    if (obj && obj->type == CLAY_TYPE_STRING) {
        return obj->as.string.data;
    }
    return "";
}

/* --- String methods --- */

static ClayValue string_println(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (clay_output_fn) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s\n", self->as.string.data);
        clay_output_fn(buf);
    } else {
        printf("%s\n", self->as.string.data);
    }
    return CLAY_VAL(self);
}

static ClayValue string_print(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (clay_output_fn) {
        clay_output_fn(self->as.string.data);
    } else {
        printf("%s", self->as.string.data);
    }
    return CLAY_VAL(self);
}

static ClayValue string_length(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_int((intptr_t)self->as.string.length);
}

static ClayValue string_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return CLAY_VAL(clay_retain(self));
}

static ClayValue string_concat(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_VAL(clay_retain(self));

    /* Convert argument to string via toString */
    if (CLAY_IS_INT(argv[0])) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", (long)clay_to_int(argv[0]));
        size_t buf_len = strlen(buf);
        size_t new_len = self->as.string.length + buf_len;
        ClayObject *result = string_new_prealloc(new_len);
        memcpy(result->as.string.data, self->as.string.data, self->as.string.length);
        memcpy(result->as.string.data + self->as.string.length, buf, buf_len);
        result->as.string.data[new_len] = '\0';
        return CLAY_VAL(result);
    }

    ClayObject *other;
    ClayValue other_val;
    if (CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0])) {
        other_val = clay_send(CLAY_OBJ(argv[0]), "toString", 0, NULL);
        other = CLAY_OBJ(other_val);
    } else {
        other = clay_string_new("nil");
        other_val = CLAY_VAL(other);
    }

    size_t new_len = self->as.string.length + other->as.string.length;
    ClayObject *result = string_new_prealloc(new_len);
    memcpy(result->as.string.data, self->as.string.data, self->as.string.length);
    memcpy(result->as.string.data + self->as.string.length, other->as.string.data, other->as.string.length);
    result->as.string.data[new_len] = '\0';
    clay_release(CLAY_OBJ(other_val));
    return CLAY_VAL(result);
}

static ClayValue string_equals(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_STRING) return CLAY_FALSE;
    return CLAY_BOOL(strcmp(self->as.string.data, other->as.string.data) == 0);
}

static ClayValue string_neq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_TRUE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_STRING) return CLAY_TRUE;
    return CLAY_BOOL(strcmp(self->as.string.data, other->as.string.data) != 0);
}

static ClayValue string_at(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;

    /* Range-based slicing: "hello" at(1..3) → "el" */
    if (CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0])) {
        ClayObject *arg = CLAY_OBJ(argv[0]);
        if (arg->parent == clay_Range || arg == clay_Range) {
            ClayValue start_v = clay_get_slot(arg, "_start");
            ClayValue end_v = clay_get_slot(arg, "_end");
            intptr_t start = CLAY_IS_INT(start_v) ? clay_to_int(start_v) : 0;
            intptr_t end = CLAY_IS_INT(end_v) ? clay_to_int(end_v) : 0;
            intptr_t len = (intptr_t)self->as.string.length;
            if (start < 0) start = 0;
            if (end > len) end = len;
            if (start >= end) return CLAY_VAL(clay_string_new(""));
            return CLAY_VAL(clay_string_new_len(self->as.string.data + start, (size_t)(end - start)));
        }
    }

    /* Original integer indexing */
    intptr_t idx = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0])
                 : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    if (idx < 0 || (size_t)idx >= self->as.string.length) return CLAY_NIL;
    return CLAY_VAL(clay_string_new_len(self->as.string.data + idx, 1));
}

static ClayValue string_contains(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    ClayObject *sub = CLAY_OBJ(argv[0]);
    if (sub->type != CLAY_TYPE_STRING) return CLAY_FALSE;
    return CLAY_BOOL(strstr(self->as.string.data, sub->as.string.data) != NULL);
}

static ClayValue string_slice(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_VAL(clay_retain(self));
    intptr_t start = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : 0;
    intptr_t end = (argc > 1 && CLAY_IS_INT(argv[1])) ? clay_to_int(argv[1])
                 : (intptr_t)self->as.string.length;
    intptr_t len = (intptr_t)self->as.string.length;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return CLAY_VAL(clay_string_new(""));
    return CLAY_VAL(clay_string_new_len(self->as.string.data + start, (size_t)(end - start)));
}

static ClayValue string_split(ClayObject *self, int argc, ClayValue *argv) {
    ClayObject *arr = clay_array_new();
    const char *delim = " ";
    if (argc > 0 && CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0])) {
        ClayObject *d = CLAY_OBJ(argv[0]);
        if (d->type == CLAY_TYPE_STRING) delim = d->as.string.data;
    }
    size_t delim_len = strlen(delim);
    if (delim_len == 0) {
        /* Split into individual characters */
        for (size_t i = 0; i < self->as.string.length; i++) {
            ClayObject *ch = clay_string_new_len(self->as.string.data + i, 1);
            clay_array_append(arr, CLAY_VAL(ch));
            clay_release(ch);
        }
        return CLAY_VAL(arr);
    }
    const char *p = self->as.string.data;
    const char *end = p + self->as.string.length;
    while (p <= end) {
        const char *found = strstr(p, delim);
        if (!found) found = end;
        ClayObject *s = clay_string_new_len(p, (size_t)(found - p));
        clay_array_append(arr, CLAY_VAL(s));
        clay_release(s);
        p = found + delim_len;
        if (found == end) break;
    }
    return CLAY_VAL(arr);
}

static ClayValue string_replace(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 2 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])
                  || CLAY_IS_INT(argv[1]) || CLAY_IS_NIL(argv[1])) {
        return CLAY_VAL(clay_retain(self));
    }
    ClayObject *old_str = CLAY_OBJ(argv[0]);
    ClayObject *new_str = CLAY_OBJ(argv[1]);
    if (old_str->type != CLAY_TYPE_STRING || new_str->type != CLAY_TYPE_STRING)
        return CLAY_VAL(clay_retain(self));
    if (old_str->as.string.length == 0) return CLAY_VAL(clay_retain(self));

    /* Count occurrences to pre-allocate */
    size_t count = 0;
    const char *p = self->as.string.data;
    while ((p = strstr(p, old_str->as.string.data)) != NULL) {
        count++;
        p += old_str->as.string.length;
    }
    if (count == 0) return CLAY_VAL(clay_retain(self));

    size_t new_len = self->as.string.length
                   + count * (new_str->as.string.length - old_str->as.string.length);
    char *buf = (char *)malloc(new_len + 1);
    char *out = buf;
    p = self->as.string.data;
    const char *end = self->as.string.data + self->as.string.length;
    while (1) {
        const char *found = strstr(p, old_str->as.string.data);
        if (!found) {
            size_t tail = (size_t)(end - p);
            memcpy(out, p, tail);
            out += tail;
            break;
        }
        memcpy(out, p, (size_t)(found - p));
        out += found - p;
        memcpy(out, new_str->as.string.data, new_str->as.string.length);
        out += new_str->as.string.length;
        p = found + old_str->as.string.length;
    }

    ClayObject *result = clay_string_new_len(buf, new_len);
    free(buf);
    return CLAY_VAL(result);
}

static ClayValue string_toInt(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char *end;
    long val = strtol(self->as.string.data, &end, 10);
    if (end == self->as.string.data) {
        clay_raise(CLAY_VAL(clay_error_new("invalid integer string", "TypeError")));
    }
    return clay_int((intptr_t)val);
}

static ClayValue string_toFloat(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char *end;
    float val = (float)strtod(self->as.string.data, &end);
    if (end == self->as.string.data) {
        clay_raise(CLAY_VAL(clay_error_new("invalid float string", "TypeError")));
    }
    return CLAY_VAL(clay_float_new(val));
}

static ClayValue string_trim(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    const char *s = self->as.string.data;
    const char *start = s;
    const char *end = s + self->as.string.length;
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    return CLAY_VAL(clay_string_new_len(start, (size_t)(end - start)));
}

static ClayValue string_upper(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = string_new_prealloc(self->as.string.length);
    for (size_t i = 0; i < self->as.string.length; i++) {
        result->as.string.data[i] = (char)toupper((unsigned char)self->as.string.data[i]);
    }
    result->as.string.data[self->as.string.length] = '\0';
    return CLAY_VAL(result);
}

static ClayValue string_lower(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = string_new_prealloc(self->as.string.length);
    for (size_t i = 0; i < self->as.string.length; i++) {
        result->as.string.data[i] = (char)tolower((unsigned char)self->as.string.data[i]);
    }
    result->as.string.data[self->as.string.length] = '\0';
    return CLAY_VAL(result);
}

static ClayValue string_startsWith(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    ClayObject *prefix = CLAY_OBJ(argv[0]);
    if (prefix->as.string.length > self->as.string.length) return CLAY_FALSE;
    return CLAY_BOOL(memcmp(self->as.string.data, prefix->as.string.data, prefix->as.string.length) == 0);
}

static ClayValue string_endsWith(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    ClayObject *suffix = CLAY_OBJ(argv[0]);
    if (suffix->as.string.length > self->as.string.length) return CLAY_FALSE;
    size_t offset = self->as.string.length - suffix->as.string.length;
    return CLAY_BOOL(memcmp(self->as.string.data + offset, suffix->as.string.data, suffix->as.string.length) == 0);
}

static ClayValue string_reverse(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = string_new_prealloc(self->as.string.length);
    for (size_t i = 0; i < self->as.string.length; i++) {
        result->as.string.data[i] = self->as.string.data[self->as.string.length - 1 - i];
    }
    result->as.string.data[self->as.string.length] = '\0';
    return CLAY_VAL(result);
}

/* String match(pattern) — returns first regex match or nil */
static ClayValue string_match(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    ClayObject *pat_obj = CLAY_OBJ(argv[0]);
    if (pat_obj->type != CLAY_TYPE_STRING) return CLAY_NIL;
    int start, len;
    if (clay_regex_search(pat_obj->as.string.data, self->as.string.data, &start, &len)) {
        return CLAY_VAL(clay_string_new_len(self->as.string.data + start, (size_t)len));
    }
    return CLAY_NIL;
}

/* String test(pattern) — returns true if regex matches */
static ClayValue string_test(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    ClayObject *pat_obj = CLAY_OBJ(argv[0]);
    if (pat_obj->type != CLAY_TYPE_STRING) return CLAY_FALSE;
    int start, len;
    return CLAY_BOOL(clay_regex_search(pat_obj->as.string.data, self->as.string.data, &start, &len));
}

static ClayValue string_indexOf(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return clay_int(-1);
    ClayObject *sub = CLAY_OBJ(argv[0]);
    if (sub->type != CLAY_TYPE_STRING) return clay_int(-1);
    const char *found = strstr(self->as.string.data, sub->as.string.data);
    if (!found) return clay_int(-1);
    return clay_int((intptr_t)(found - self->as.string.data));
}

static ClayValue string_lastIndexOf(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return clay_int(-1);
    ClayObject *sub = CLAY_OBJ(argv[0]);
    if (sub->type != CLAY_TYPE_STRING) return clay_int(-1);
    if (sub->as.string.length == 0) return clay_int((intptr_t)self->as.string.length);
    const char *last = NULL;
    const char *p = self->as.string.data;
    while ((p = strstr(p, sub->as.string.data)) != NULL) {
        last = p;
        p += sub->as.string.length;
    }
    if (!last) return clay_int(-1);
    return clay_int((intptr_t)(last - self->as.string.data));
}

static ClayValue string_repeat(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_INT(argv[0])) return CLAY_VAL(clay_string_new(""));
    intptr_t n = clay_to_int(argv[0]);
    if (n <= 0) return CLAY_VAL(clay_string_new(""));
    size_t total = self->as.string.length * (size_t)n;
    ClayObject *result = string_new_prealloc(total);
    char *p = result->as.string.data;
    for (intptr_t i = 0; i < n; i++) {
        memcpy(p, self->as.string.data, self->as.string.length);
        p += self->as.string.length;
    }
    result->as.string.data[total] = '\0';
    return CLAY_VAL(result);
}

static ClayValue string_padLeft(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_INT(argv[0])) return CLAY_VAL(clay_retain(self));
    intptr_t width = clay_to_int(argv[0]);
    if ((size_t)width <= self->as.string.length) return CLAY_VAL(clay_retain(self));
    char pad = ' ';
    if (argc > 1 && !CLAY_IS_INT(argv[1]) && !CLAY_IS_NIL(argv[1])) {
        ClayObject *ps = CLAY_OBJ(argv[1]);
        if (ps->type == CLAY_TYPE_STRING && ps->as.string.length > 0)
            pad = ps->as.string.data[0];
    }
    size_t pad_count = (size_t)width - self->as.string.length;
    ClayObject *result = string_new_prealloc((size_t)width);
    memset(result->as.string.data, pad, pad_count);
    memcpy(result->as.string.data + pad_count, self->as.string.data, self->as.string.length);
    result->as.string.data[width] = '\0';
    return CLAY_VAL(result);
}

static ClayValue string_padRight(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_INT(argv[0])) return CLAY_VAL(clay_retain(self));
    intptr_t width = clay_to_int(argv[0]);
    if ((size_t)width <= self->as.string.length) return CLAY_VAL(clay_retain(self));
    char pad = ' ';
    if (argc > 1 && !CLAY_IS_INT(argv[1]) && !CLAY_IS_NIL(argv[1])) {
        ClayObject *ps = CLAY_OBJ(argv[1]);
        if (ps->type == CLAY_TYPE_STRING && ps->as.string.length > 0)
            pad = ps->as.string.data[0];
    }
    size_t pad_count = (size_t)width - self->as.string.length;
    ClayObject *result = string_new_prealloc((size_t)width);
    memcpy(result->as.string.data, self->as.string.data, self->as.string.length);
    memset(result->as.string.data + self->as.string.length, pad, pad_count);
    result->as.string.data[width] = '\0';
    return CLAY_VAL(result);
}

/* chr(code) — return a single-character string from an ASCII/Unicode code point */
static ClayValue string_chr(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return CLAY_NIL;
    intptr_t code = clay_to_int(argv[0]);
    if (code < 0 || code > 127) return CLAY_NIL;
    char buf[2] = { (char)code, '\0' };
    return CLAY_VAL(clay_string_new(buf));
}

void clay_string_init_methods(ClayObject *proto) {
    clay_set_method(proto, "println", string_println);
    clay_set_method(proto, "print", string_print);
    clay_set_method(proto, "length", string_length);
    clay_set_method(proto, "toString", string_toString);
    clay_set_method(proto, "+", string_concat);
    clay_set_method(proto, "==", string_equals);
    clay_set_method(proto, "!=", string_neq);
    clay_set_method(proto, "at", string_at);
    clay_set_method(proto, "contains", string_contains);
    clay_set_method(proto, "slice", string_slice);
    clay_set_method(proto, "split", string_split);
    clay_set_method(proto, "replace", string_replace);
    clay_set_method(proto, "toInt", string_toInt);
    clay_set_method(proto, "toFloat", string_toFloat);
    clay_set_method(proto, "trim", string_trim);
    clay_set_method(proto, "upper", string_upper);
    clay_set_method(proto, "lower", string_lower);
    clay_set_method(proto, "startsWith", string_startsWith);
    clay_set_method(proto, "endsWith", string_endsWith);
    clay_set_method(proto, "reverse", string_reverse);
    clay_set_method(proto, "match", string_match);
    clay_set_method(proto, "test", string_test);
    clay_set_method(proto, "indexOf", string_indexOf);
    clay_set_method(proto, "lastIndexOf", string_lastIndexOf);
    clay_set_method(proto, "repeat", string_repeat);
    clay_set_method(proto, "padLeft", string_padLeft);
    clay_set_method(proto, "padRight", string_padRight);
    clay_set_method(proto, "chr", string_chr);
}
