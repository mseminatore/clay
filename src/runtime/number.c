/*
 * number.c - Number methods (operate on tagged integers)
 *
 * Numbers are tagged values, not ClayObject pointers.
 * These methods are registered on the Number prototype and
 * dispatched when clay_send detects a tagged int receiver.
 * The "self" pointer won't be a real object for ints -
 * we handle this in dispatch.c by creating a wrapper.
 */
#include "clay.h"

/* For tagged int dispatch, self->parent is Number prototype.
 * We store the actual int value in a slot named "_value". */

static intptr_t self_int_value(ClayObject *self) {
    ClayValue v = clay_get_slot(self, "_value");
    return clay_to_int(v);
}

static ClayValue number_add(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(self_int_value(self));
    intptr_t a = self_int_value(self);
    intptr_t b;
    if (CLAY_IS_INT(argv[0])) {
        b = clay_to_int(argv[0]);
    } else {
        b = clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    }
    return clay_int(a + b);
}

static ClayValue number_sub(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(-self_int_value(self));
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return clay_int(a - b);
}

static ClayValue number_mul(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return clay_int(a * b);
}

static ClayValue number_div(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    if (b == 0) {
        clay_raise(CLAY_VAL(clay_error_new("division by zero", "DivisionByZero")));
    }
    return clay_int(a / b);
}

static ClayValue number_mod(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    if (b == 0) {
        clay_raise(CLAY_VAL(clay_error_new("modulo by zero", "DivisionByZero")));
    }
    return clay_int(a % b);
}

static ClayValue number_lt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a < b);
}

static ClayValue number_gt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a > b);
}

static ClayValue number_eq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a == b);
}

static ClayValue number_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", (long)self_int_value(self));
    return CLAY_VAL(clay_string_new(buf));
}

static ClayValue number_println(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    printf("%ld\n", (long)self_int_value(self));
    return CLAY_VAL(self);
}

void clay_number_init_methods(ClayObject *proto) {
    clay_set_method(proto, "+",  number_add);
    clay_set_method(proto, "-",  number_sub);
    clay_set_method(proto, "*",  number_mul);
    clay_set_method(proto, "/",  number_div);
    clay_set_method(proto, "%",  number_mod);
    clay_set_method(proto, "<",  number_lt);
    clay_set_method(proto, ">",  number_gt);
    clay_set_method(proto, "==", number_eq);
    clay_set_method(proto, "toString", number_toString);
    clay_set_method(proto, "println",  number_println);
}
