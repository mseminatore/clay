/*
 * array.c - Array object implementation
 */
#include "clay.h"

#define CLAY_ARRAY_INITIAL_CAP 8

ClayObject *clay_array_new(void) {
    ClayObject *obj = clay_object_new();
    obj->type = CLAY_TYPE_ARRAY;
    obj->parent = clay_Array;
    if (clay_Array) clay_retain(clay_Array);
    obj->as.array.items = (ClayValue *)malloc(sizeof(ClayValue) * CLAY_ARRAY_INITIAL_CAP);
    obj->as.array.size = 0;
    obj->as.array.capacity = CLAY_ARRAY_INITIAL_CAP;
    return obj;
}

void clay_array_append(ClayObject *arr, ClayValue val) {
    if (arr->as.array.size >= arr->as.array.capacity) {
        arr->as.array.capacity *= 2;
        arr->as.array.items = (ClayValue *)realloc(arr->as.array.items,
            sizeof(ClayValue) * arr->as.array.capacity);
    }
    if (CLAY_IS_PTR(val) && !CLAY_IS_NIL(val) && val != CLAY_TRUE && val != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(val));
    }
    arr->as.array.items[arr->as.array.size++] = val;
}

ClayValue clay_array_at(ClayObject *arr, size_t index) {
    if (index >= arr->as.array.size) return CLAY_NIL;
    return arr->as.array.items[index];
}

size_t clay_array_size(ClayObject *arr) {
    return arr->as.array.size;
}

/* --- Array methods --- */

static ClayValue array_append(ClayObject *self, int argc, ClayValue *argv) {
    if (argc >= 1) {
        clay_array_append(self, argv[0]);
    }
    return CLAY_VAL(self);
}

static ClayValue array_at(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_INT(argv[0])) return CLAY_NIL;
    intptr_t idx = clay_to_int(argv[0]);
    if (idx < 0) return CLAY_NIL;
    return clay_array_at(self, (size_t)idx);
}

static ClayValue array_size(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_int((intptr_t)self->as.array.size);
}

static ClayValue array_each(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;

    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        clay_send(CLAY_OBJ(argv[0]), "value", 1, &item);
    }
    return CLAY_VAL(self);
}

static ClayValue array_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    /* Build "[item1, item2, ...]" */
    char *buf = (char *)malloc(256);
    size_t cap = 256, len = 0;
    buf[len++] = '[';

    for (size_t i = 0; i < self->as.array.size; i++) {
        if (i > 0) {
            buf[len++] = ',';
            buf[len++] = ' ';
        }
        ClayValue item = self->as.array.items[i];
        const char *s;
        char numbuf[32];
        ClayValue sv = CLAY_NIL;
        if (CLAY_IS_INT(item)) {
            snprintf(numbuf, sizeof(numbuf), "%ld", (long)clay_to_int(item));
            s = numbuf;
        } else if (CLAY_IS_NIL(item)) {
            s = "nil";
        } else {
            sv = clay_send(CLAY_OBJ(item), "toString", 0, NULL);
            s = clay_string_data(CLAY_OBJ(sv));
        }
        size_t slen = strlen(s);
        while (len + slen + 4 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, s, slen);
        len += slen;
        if (!CLAY_IS_NIL(sv)) clay_release(CLAY_OBJ(sv));
    }
    buf[len++] = ']';
    buf[len] = '\0';

    ClayObject *result = clay_string_new(buf);
    free(buf);
    return CLAY_VAL(result);
}

static ClayValue array_map(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        ClayValue mapped = clay_send(CLAY_OBJ(argv[0]), "value", 1, &item);
        clay_array_append(result, mapped);
    }
    return CLAY_VAL(result);
}

static ClayValue array_filter(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        ClayValue test = clay_send(CLAY_OBJ(argv[0]), "value", 1, &item);
        if (CLAY_IS_TRUTHY(test)) {
            clay_array_append(result, item);
        }
    }
    return CLAY_VAL(result);
}

static ClayValue array_reduce(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 2) return CLAY_NIL;
    ClayValue acc = argv[0];
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue args[2] = { acc, self->as.array.items[i] };
        acc = clay_send(CLAY_OBJ(argv[1]), "value", 2, args);
    }
    return acc;
}

static ClayValue array_find(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        ClayValue test = clay_send(CLAY_OBJ(argv[0]), "value", 1, &item);
        if (CLAY_IS_TRUTHY(test)) return item;
    }
    return CLAY_NIL;
}

static ClayValue array_contains(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        if (item == argv[0]) return CLAY_TRUE;
        /* Compare by value for ints and strings */
        if (CLAY_IS_INT(item) && CLAY_IS_INT(argv[0]) && item == argv[0]) return CLAY_TRUE;
        if (!CLAY_IS_INT(item) && !CLAY_IS_NIL(item) && !CLAY_IS_INT(argv[0]) && !CLAY_IS_NIL(argv[0])) {
            ClayValue eq = clay_send_val(item, "==", 1, &argv[0]);
            if (eq == CLAY_TRUE) return CLAY_TRUE;
        }
    }
    return CLAY_FALSE;
}

static ClayValue array_indexOf(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(-1);
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        if (item == argv[0]) return clay_int((intptr_t)i);
        if (!CLAY_IS_INT(item) && !CLAY_IS_NIL(item) && !CLAY_IS_INT(argv[0]) && !CLAY_IS_NIL(argv[0])) {
            ClayValue eq = clay_send_val(item, "==", 1, &argv[0]);
            if (eq == CLAY_TRUE) return clay_int((intptr_t)i);
        }
    }
    return clay_int(-1);
}

static ClayValue array_first(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    return self->as.array.items[0];
}

static ClayValue array_last(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    return self->as.array.items[self->as.array.size - 1];
}

static ClayValue array_reverse(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = clay_array_new();
    for (size_t i = self->as.array.size; i > 0; i--) {
        clay_array_append(result, self->as.array.items[i - 1]);
    }
    return CLAY_VAL(result);
}

static ClayValue array_join(ClayObject *self, int argc, ClayValue *argv) {
    const char *sep = (argc > 0 && !CLAY_IS_NIL(argv[0])) ? clay_string_data(CLAY_OBJ(argv[0])) : "";
    size_t sep_len = strlen(sep);
    char *buf = (char *)malloc(256);
    size_t cap = 256, len = 0;

    for (size_t i = 0; i < self->as.array.size; i++) {
        if (i > 0) {
            while (len + sep_len + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
            memcpy(buf + len, sep, sep_len);
            len += sep_len;
        }
        ClayValue item = self->as.array.items[i];
        const char *s;
        char numbuf[32];
        if (CLAY_IS_INT(item)) {
            snprintf(numbuf, sizeof(numbuf), "%ld", (long)clay_to_int(item));
            s = numbuf;
        } else if (CLAY_IS_NIL(item)) {
            s = "nil";
        } else {
            ClayValue sv = clay_send(CLAY_OBJ(item), "toString", 0, NULL);
            s = clay_string_data(CLAY_OBJ(sv));
            /* Copy s before releasing sv */
            size_t slen = strlen(s);
            while (len + slen + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
            memcpy(buf + len, s, slen);
            len += slen;
            clay_release(CLAY_OBJ(sv));
            continue;
        }
        size_t slen = strlen(s);
        while (len + slen + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        memcpy(buf + len, s, slen);
        len += slen;
    }
    buf[len] = '\0';
    ClayObject *result = clay_string_new(buf);
    free(buf);
    return CLAY_VAL(result);
}

static int clay_compare_values(const void *a, const void *b) {
    ClayValue va = *(const ClayValue *)a;
    ClayValue vb = *(const ClayValue *)b;
    if (CLAY_IS_INT(va) && CLAY_IS_INT(vb)) {
        intptr_t ia = clay_to_int(va), ib = clay_to_int(vb);
        return (ia > ib) - (ia < ib);
    }
    /* Fall back to string comparison */
    ClayValue sa = clay_send_val(va, "toString", 0, NULL);
    ClayValue sb = clay_send_val(vb, "toString", 0, NULL);
    int cmp = strcmp(clay_string_data(CLAY_OBJ(sa)), clay_string_data(CLAY_OBJ(sb)));
    clay_release(CLAY_OBJ(sa));
    clay_release(CLAY_OBJ(sb));
    return cmp;
}

static ClayValue array_sort(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < self->as.array.size; i++) {
        clay_array_append(result, self->as.array.items[i]);
    }
    if (result->as.array.size > 1) {
        qsort(result->as.array.items, result->as.array.size, sizeof(ClayValue), clay_compare_values);
    }
    return CLAY_VAL(result);
}

static ClayValue array_removeAt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_INT(argv[0])) return CLAY_NIL;
    intptr_t idx = clay_to_int(argv[0]);
    if (idx < 0 || (size_t)idx >= self->as.array.size) return CLAY_NIL;
    ClayValue removed = self->as.array.items[idx];
    memmove(&self->as.array.items[idx], &self->as.array.items[idx + 1],
            (self->as.array.size - (size_t)idx - 1) * sizeof(ClayValue));
    self->as.array.size--;
    return removed;
}

static ClayValue array_equals(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_ARRAY) return CLAY_FALSE;
    if (self == other) return CLAY_TRUE;
    if (self->as.array.size != other->as.array.size) return CLAY_FALSE;
    for (int i = 0; i < self->as.array.size; i++) {
        ClayValue a = self->as.array.items[i];
        ClayValue b = other->as.array.items[i];
        ClayValue eq = clay_send_val(a, "==", 1, &b);
        if (eq != CLAY_TRUE) return CLAY_FALSE;
    }
    return CLAY_TRUE;
}

static ClayValue array_neq(ClayObject *self, int argc, ClayValue *argv) {
    return array_equals(self, argc, argv) == CLAY_TRUE ? CLAY_FALSE : CLAY_TRUE;
}

void clay_array_init_methods(ClayObject *proto) {
    clay_set_method(proto, "append", array_append);
    clay_set_method(proto, "push", array_append);
    clay_set_method(proto, "at", array_at);
    clay_set_method(proto, "size", array_size);
    clay_set_method(proto, "length", array_size);
    clay_set_method(proto, "each", array_each);
    clay_set_method(proto, "map", array_map);
    clay_set_method(proto, "filter", array_filter);
    clay_set_method(proto, "reduce", array_reduce);
    clay_set_method(proto, "find", array_find);
    clay_set_method(proto, "contains", array_contains);
    clay_set_method(proto, "indexOf", array_indexOf);
    clay_set_method(proto, "first", array_first);
    clay_set_method(proto, "last", array_last);
    clay_set_method(proto, "reverse", array_reverse);
    clay_set_method(proto, "sort", array_sort);
    clay_set_method(proto, "join", array_join);
    clay_set_method(proto, "removeAt", array_removeAt);
    clay_set_method(proto, "toString", array_toString);
    clay_set_method(proto, "==", array_equals);
    clay_set_method(proto, "!=", array_neq);
}
