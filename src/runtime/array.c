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
        if (CLAY_IS_INT(item)) {
            snprintf(numbuf, sizeof(numbuf), "%ld", (long)clay_to_int(item));
            s = numbuf;
        } else if (CLAY_IS_NIL(item)) {
            s = "nil";
        } else {
            ClayValue sv = clay_send(CLAY_OBJ(item), "toString", 0, NULL);
            s = clay_string_data(CLAY_OBJ(sv));
        }
        size_t slen = strlen(s);
        while (len + slen + 4 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, s, slen);
        len += slen;
    }
    buf[len++] = ']';
    buf[len] = '\0';

    ClayObject *result = clay_string_new(buf);
    free(buf);
    return CLAY_VAL(result);
}

void clay_array_init_methods(ClayObject *proto) {
    clay_set_method(proto, "append", array_append);
    clay_set_method(proto, "at", array_at);
    clay_set_method(proto, "size", array_size);
    clay_set_method(proto, "each", array_each);
    clay_set_method(proto, "toString", array_toString);
}
