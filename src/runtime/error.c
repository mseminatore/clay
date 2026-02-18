/*
 * error.c - Error object and try/catch mechanism
 */
#include "clay.h"

/* Global error stack */
ClayErrorStack clay_error_stack = { .depth = 0, .current_error = 0 };

/* Create an Error object with message and type slots */
ClayObject *clay_error_new(const char *message, const char *type) {
    ClayObject *err = clay_clone(clay_Error);
    clay_set_slot(err, "_message", CLAY_VAL(clay_string_new(message)));
    clay_set_slot(err, "_type", CLAY_VAL(clay_string_new(type)));
    return err;
}

/* Raise an error: longjmp to nearest try frame, or abort */
void clay_raise(ClayValue error) {
    if (clay_error_stack.depth > 0) {
        clay_error_stack.current_error = error;
        clay_error_stack.depth--;
        longjmp(clay_error_stack.frames[clay_error_stack.depth], 1);
    }
    /* No try frame — print and abort */
    if (CLAY_IS_PTR(error) && !CLAY_IS_NIL(error)) {
        ClayObject *err = CLAY_OBJ(error);
        ClayValue msg = clay_get_slot(err, "_message");
        if (!CLAY_IS_NIL(msg)) {
            fprintf(stderr, "clay: unhandled error: %s\n", clay_string_data(CLAY_OBJ(msg)));
        } else {
            fprintf(stderr, "clay: unhandled error\n");
        }
    } else {
        fprintf(stderr, "clay: unhandled error\n");
    }
    exit(1);
}

/* Error methods */
static ClayValue error_message(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_get_slot(self, "_message");
}

static ClayValue error_type(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_get_slot(self, "_type");
}

static ClayValue error_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue type = clay_get_slot(self, "_type");
    ClayValue msg = clay_get_slot(self, "_message");
    const char *t = CLAY_IS_NIL(type) ? "Error" : clay_string_data(CLAY_OBJ(type));
    const char *m = CLAY_IS_NIL(msg) ? "" : clay_string_data(CLAY_OBJ(msg));
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: %s", t, m);
    return CLAY_VAL(clay_string_new(buf));
}

void clay_error_init_methods(ClayObject *proto) {
    clay_set_method(proto, "message", error_message);
    clay_set_method(proto, "type", error_type);
    clay_set_method(proto, "toString", error_toString);
}
