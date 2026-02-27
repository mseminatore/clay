/*
 * number.c - Number methods (integers and floats)
 *
 * Integers are tagged values (low bit = 1), not ClayObject pointers.
 * Floats are ClayObject with type CLAY_TYPE_FLOAT.
 * These methods are registered on the Number prototype and
 * dispatched when clay_send detects a tagged int receiver.
 * The "self" pointer won't be a real object for ints -
 * we handle this in dispatch.c by creating a wrapper.
 */
#include "clay.h"

/* Float constructor and accessor */
ClayObject *clay_float_new(float v) {
    ClayObject *obj = clay_clone(clay_Number);
    obj->type = CLAY_TYPE_FLOAT;
    obj->as.number_float.value = v;
    return obj;
}

float clay_to_float(ClayValue v) {
    if (CLAY_IS_INT(v)) return (float)clay_to_int(v);
    if (CLAY_IS_FLOAT(v)) return CLAY_OBJ(v)->as.number_float.value;
    return 0.0f;
}

/* For tagged int dispatch, self->parent is Number prototype.
 * We store the actual int value in a slot named "_value". */

static intptr_t self_int_value(ClayObject *self) {
    if (self->type == CLAY_TYPE_FLOAT) return (intptr_t)self->as.number_float.value;
    ClayValue v = clay_get_slot(self, "_value");
    return clay_to_int(v);
}

static float self_float_value(ClayObject *self) {
    if (self->type == CLAY_TYPE_FLOAT) return self->as.number_float.value;
    ClayValue v = clay_get_slot(self, "_value");
    return (float)clay_to_int(v);
}

static int is_float_arg(ClayObject *self, int argc, ClayValue *argv) {
    if (self->type == CLAY_TYPE_FLOAT) return 1;
    if (argc > 0 && CLAY_IS_FLOAT(argv[0])) return 1;
    return 0;
}

static float arg_float_value(int argc, ClayValue *argv) {
    if (argc < 1) return 0.0f;
    return clay_to_float(argv[0]);
}

static ClayValue number_add(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(self_int_value(self));
    if (is_float_arg(self, argc, argv)) {
        return CLAY_VAL(clay_float_new(self_float_value(self) + arg_float_value(argc, argv)));
    }
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
    if (is_float_arg(self, argc, argv)) {
        return CLAY_VAL(clay_float_new(self_float_value(self) - arg_float_value(argc, argv)));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return clay_int(a - b);
}

static ClayValue number_mul(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    if (is_float_arg(self, argc, argv)) {
        return CLAY_VAL(clay_float_new(self_float_value(self) * arg_float_value(argc, argv)));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return clay_int(a * b);
}

static ClayValue number_div(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    if (is_float_arg(self, argc, argv)) {
        float b = arg_float_value(argc, argv);
        if (b == 0.0f) clay_raise(CLAY_VAL(clay_error_new("division by zero", "DivisionByZero")));
        return CLAY_VAL(clay_float_new(self_float_value(self) / b));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    if (b == 0) {
        clay_raise(CLAY_VAL(clay_error_new("division by zero", "DivisionByZero")));
    }
    return clay_int(a / b);
}

static ClayValue number_mod(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    /* modulo only works on integers */
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    if (b == 0) {
        clay_raise(CLAY_VAL(clay_error_new("modulo by zero", "DivisionByZero")));
    }
    return clay_int(a % b);
}

static ClayValue number_lt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) < arg_float_value(argc, argv));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a < b);
}

static ClayValue number_gt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) > arg_float_value(argc, argv));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a > b);
}

static ClayValue number_eq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) == arg_float_value(argc, argv));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a == b);
}

static ClayValue number_lte(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) <= arg_float_value(argc, argv));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a <= b);
}

static ClayValue number_gte(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) >= arg_float_value(argc, argv));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a >= b);
}

static ClayValue number_neq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_TRUE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) != arg_float_value(argc, argv));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : clay_to_int(clay_get_slot(CLAY_OBJ(argv[0]), "_value"));
    return CLAY_BOOL(a != b);
}

static ClayValue number_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char buf[64];
    if (self->type == CLAY_TYPE_FLOAT) {
        snprintf(buf, sizeof(buf), "%g", self->as.number_float.value);
    } else {
        snprintf(buf, sizeof(buf), "%ld", (long)self_int_value(self));
    }
    return CLAY_VAL(clay_string_new(buf));
}

static ClayValue number_println(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char buf[64];
    if (self->type == CLAY_TYPE_FLOAT) {
        snprintf(buf, sizeof(buf), "%g\n", self->as.number_float.value);
    } else {
        snprintf(buf, sizeof(buf), "%ld\n", (long)self_int_value(self));
    }
    if (clay_output_fn) {
        clay_output_fn(buf);
    } else {
        fputs(buf, stdout);
    }
    return CLAY_VAL(self);
}

static ClayValue number_toFloat(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return CLAY_VAL(clay_float_new(self_float_value(self)));
}

static ClayValue number_toInt(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_int(self_int_value(self));
}

static ClayValue number_abs(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->type == CLAY_TYPE_FLOAT) {
        float v = self->as.number_float.value;
        return CLAY_VAL(clay_float_new(v < 0 ? -v : v));
    }
    intptr_t v = self_int_value(self);
    return clay_int(v < 0 ? -v : v);
}

static ClayValue number_times(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    intptr_t n = self_int_value(self);
    ClayValue result = CLAY_NIL;
    for (intptr_t i = 0; i < n; i++) {
        ClayValue arg = clay_int(i);
        if (clay_try_push() == 0) {
            result = clay_send(CLAY_OBJ(argv[0]), "value", 1, &arg);
            clay_try_pop();
        } else {
            ClayValue err = clay_error_stack.current_error;
            if (CLAY_IS_PTR(err) && !CLAY_IS_NIL(err)) {
                ClayObject *eobj = CLAY_OBJ(err);
                ClayValue type_val = clay_get_slot(eobj, "_type");
                if (CLAY_IS_PTR(type_val) && !CLAY_IS_NIL(type_val) &&
                    CLAY_OBJ(type_val)->type == CLAY_TYPE_STRING) {
                    const char *tstr = clay_string_data(CLAY_OBJ(type_val));
                    if (strcmp(tstr, "BreakSignal") == 0) return result;
                    if (strcmp(tstr, "ContinueSignal") == 0) continue;
                }
            }
            clay_raise(err);
        }
    }
    return result;
}

void clay_number_init_methods(ClayObject *proto) {
    clay_set_method(proto, "+",  number_add);
    clay_set_method(proto, "-",  number_sub);
    clay_set_method(proto, "*",  number_mul);
    clay_set_method(proto, "/",  number_div);
    clay_set_method(proto, "%",  number_mod);
    clay_set_method(proto, "<",  number_lt);
    clay_set_method(proto, ">",  number_gt);
    clay_set_method(proto, "<=", number_lte);
    clay_set_method(proto, ">=", number_gte);
    clay_set_method(proto, "==", number_eq);
    clay_set_method(proto, "!=", number_neq);
    clay_set_method(proto, "toString", number_toString);
    clay_set_method(proto, "println",  number_println);
    clay_set_method(proto, "toFloat",  number_toFloat);
    clay_set_method(proto, "toInt",    number_toInt);
    clay_set_method(proto, "abs",      number_abs);
    clay_set_method(proto, "times",    number_times);
}
