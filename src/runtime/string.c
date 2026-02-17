/*
 * string.c - String object implementation
 */
#include "clay.h"

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

const char *clay_string_data(ClayObject *obj) {
    if (obj && obj->type == CLAY_TYPE_STRING) {
        return obj->as.string.data;
    }
    return "";
}

/* --- String methods --- */

static ClayValue string_println(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    printf("%s\n", self->as.string.data);
    return CLAY_VAL(self);
}

static ClayValue string_print(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    printf("%s", self->as.string.data);
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
    ClayValue other_val;
    if (CLAY_IS_INT(argv[0])) {
        /* Send toString to a temporary number wrapper - for now, format directly */
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", (long)clay_to_int(argv[0]));
        ClayObject *result = clay_string_new(buf);
        size_t new_len = self->as.string.length + strlen(buf);
        char *new_data = (char *)malloc(new_len + 1);
        memcpy(new_data, self->as.string.data, self->as.string.length);
        memcpy(new_data + self->as.string.length, buf, strlen(buf));
        new_data[new_len] = '\0';
        clay_release(result);
        ClayObject *s = clay_string_new_len(new_data, new_len);
        free(new_data);
        return CLAY_VAL(s);
    }

    ClayObject *other;
    if (CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0])) {
        other_val = clay_send(CLAY_OBJ(argv[0]), "toString", 0, NULL);
        other = CLAY_OBJ(other_val);
    } else {
        other = clay_string_new("nil");
        other_val = CLAY_VAL(other);
    }

    size_t new_len = self->as.string.length + other->as.string.length;
    char *new_data = (char *)malloc(new_len + 1);
    memcpy(new_data, self->as.string.data, self->as.string.length);
    memcpy(new_data + self->as.string.length, other->as.string.data, other->as.string.length);
    new_data[new_len] = '\0';

    ClayObject *result = clay_string_new_len(new_data, new_len);
    free(new_data);
    clay_release(CLAY_OBJ(other_val));
    return CLAY_VAL(result);
}

static ClayValue string_equals(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_STRING) return CLAY_FALSE;
    return CLAY_BOOL(strcmp(self->as.string.data, other->as.string.data) == 0);
}

void clay_string_init_methods(ClayObject *proto) {
    clay_set_method(proto, "println", string_println);
    clay_set_method(proto, "print", string_print);
    clay_set_method(proto, "length", string_length);
    clay_set_method(proto, "toString", string_toString);
    clay_set_method(proto, "+", string_concat);
    clay_set_method(proto, "==", string_equals);
}
