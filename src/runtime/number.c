/*
 * number.c - Number methods (integers and floats)
 *
 * Integers are tagged values: (v & 3) == 1, value in upper bits.
 * Floats are tagged values: (v & 3) == 3, IEEE 754 bits in upper word.
 * These methods are registered on the Number prototype and
 * dispatched when clay_send detects a tagged int/float receiver.
 * The "self" pointer is a wrapper clone with _value slot for tagged dispatch.
 */
#include "clay.h"
#include <math.h>

/* Float constructor — returns tagged float (no heap allocation) */
ClayObject *clay_float_new(float v) {
    /* Legacy API returns ClayObject*, but we cast the tagged value.
     * Callers typically wrap with CLAY_VAL() anyway. */
    return (ClayObject *)(uintptr_t)clay_float_tag(v);
}

/* Format a float as the shortest decimal that reads back as the same 32-bit
 * value (at most 9 significant digits), in %g style: 0.1 → "0.1",
 * 1000000.125 → "1000000.1", 16777216 → "16777216", 3.0 → "3". */
void clay_format_float(float f, char *buf, size_t sz) {
    if (f != f) { snprintf(buf, sz, "nan"); return; }
    if (f > 3.4028235e38f) { snprintf(buf, sz, "inf"); return; }
    if (f < -3.4028235e38f) { snprintf(buf, sz, "-inf"); return; }
    int prec = 1;
    for (; prec <= 9; prec++) {
        snprintf(buf, sz, "%.*g", prec, (double)f);
        if (strtof(buf, NULL) == f) break;
    }
    if (prec > 9) prec = 9;
    /* %g switches to exponent form once the decimal exponent reaches the
     * precision (50 at precision 1 is "5e+01"); for values below 1e9 widen the
     * precision so they print in plain notation instead ("50", "1000000"). */
    if (f != 0.0f) {
        float a = f < 0 ? -f : f;
        int exp10 = (int)floor(log10((double)a));
        if (exp10 >= prec && exp10 < 9) {
            prec = exp10 + 1;
            snprintf(buf, sz, "%.*g", prec, (double)f);
        }
    }
}

float clay_to_float(ClayValue v) {
    if (CLAY_IS_TAGGED_FLOAT(v)) return clay_float_untag(v);
    if (CLAY_IS_INT(v)) return (float)clay_to_int(v);
    if (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v) && CLAY_OBJ(v)->type == CLAY_TYPE_FLOAT)
        return CLAY_OBJ(v)->as.number_float.value;
    return 0.0f;
}

/* For tagged int dispatch, self->parent is Number prototype.
 * We store the actual int value in a slot named "_value". */

static intptr_t self_int_value(ClayObject *self) {
    if (self->type == CLAY_TYPE_FLOAT) return clay_float_to_int(self->as.number_float.value);
    ClayValue v = clay_get_slot(self, "_value");
    if (CLAY_IS_TAGGED_FLOAT(v)) return clay_float_to_int(clay_float_untag(v));
    return clay_to_int(v);
}

static float self_float_value(ClayObject *self) {
    if (self->type == CLAY_TYPE_FLOAT) return self->as.number_float.value;
    ClayValue v = clay_get_slot(self, "_value");
    if (CLAY_IS_TAGGED_FLOAT(v)) return clay_float_untag(v);
    return (float)clay_to_int(v);
}

static int is_float_arg(ClayObject *self, int argc, ClayValue *argv) {
    if (self->type == CLAY_TYPE_FLOAT) return 1;
    /* Check _value slot for tagged float */
    ClayValue sv = clay_get_slot(self, "_value");
    if (CLAY_IS_TAGGED_FLOAT(sv)) return 1;
    if (argc > 0 && CLAY_IS_FLOAT(argv[0])) return 1;
    return 0;
}

static float arg_float_value(int argc, ClayValue *argv) {
    if (argc < 1) return 0.0f;
    return clay_to_float(argv[0]);
}

/* Unwrap a numeric argument (tagged int/float, boxed float, or a tagged-dispatch
 * wrapper carrying _value) to an integer. Returns 0 if v is not a number. */
static int arg_int(ClayValue v, intptr_t *out) {
    if (CLAY_IS_INT(v)) { *out = clay_to_int(v); return 1; }
    if (CLAY_IS_TAGGED_FLOAT(v)) { *out = clay_float_to_int(clay_float_untag(v)); return 1; }
    if (!CLAY_IS_OBJ(v)) return 0;
    ClayObject *o = CLAY_OBJ(v);
    if (o->type == CLAY_TYPE_FLOAT) { *out = clay_float_to_int(o->as.number_float.value); return 1; }
    if (o->parent != clay_Number && o != clay_Number) return 0;
    ClayValue inner = clay_get_slot(o, "_value");
    if (CLAY_IS_INT(inner)) { *out = clay_to_int(inner); return 1; }
    if (CLAY_IS_TAGGED_FLOAT(inner)) { *out = clay_float_to_int(clay_float_untag(inner)); return 1; }
    return 0;
}

/* Float argument for arithmetic/ordering: raises TypeError for non-numbers */
static float arg_float_or_raise(ClayValue v, const char *op) {
    intptr_t ignored;
    if (!arg_int(v, &ignored)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Number %s: argument is not a Number", op);
        clay_raise(CLAY_VAL(clay_error_new(msg, "TypeError")));
    }
    return clay_to_float(v);
}

/* Integer argument for arithmetic/ordering: raises TypeError for non-numbers */
static intptr_t arg_int_or_raise(ClayValue v, const char *op) {
    intptr_t n = 0;
    if (!arg_int(v, &n)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Number %s: argument is not a Number", op);
        clay_raise(CLAY_VAL(clay_error_new(msg, "TypeError")));
    }
    return n;
}

static ClayValue number_add(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(self_int_value(self));
    if (is_float_arg(self, argc, argv)) {
        return clay_float_tag(self_float_value(self) + arg_float_or_raise(argv[0], "+"));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], "+");
    return clay_int(a + b);
}

static ClayValue number_sub(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(-self_int_value(self));
    if (is_float_arg(self, argc, argv)) {
        return clay_float_tag(self_float_value(self) - arg_float_or_raise(argv[0], "-"));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], "-");
    return clay_int(a - b);
}

static ClayValue number_mul(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    if (is_float_arg(self, argc, argv)) {
        return clay_float_tag(self_float_value(self) * arg_float_or_raise(argv[0], "*"));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], "*");
    return clay_int(clay_int_mul(a, b));
}

static ClayValue number_div(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    if (is_float_arg(self, argc, argv)) {
        float b = arg_float_or_raise(argv[0], "/");
        /* Float division follows IEEE 754 (x / 0.0 is inf or nan); only
         * integer division by zero raises DivisionByZero */
        return clay_float_tag(self_float_value(self) / b);
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], "/");
    if (b == 0) {
        clay_raise(CLAY_VAL(clay_error_new("division by zero", "DivisionByZero")));
    }
    return clay_int(a / b);
}

static ClayValue number_mod(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(0);
    if (is_float_arg(self, argc, argv)) {
        float b = arg_float_or_raise(argv[0], "%");
        /* IEEE 754, like float division: fmodf(x, 0) is nan */
        return clay_float_tag(fmodf(self_float_value(self), b));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], "%");
    if (b == 0) {
        clay_raise(CLAY_VAL(clay_error_new("modulo by zero", "DivisionByZero")));
    }
    return clay_int(a % b);
}

static ClayValue number_lt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) < arg_float_or_raise(argv[0], "<"));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], "<");
    return CLAY_BOOL(a < b);
}

static ClayValue number_gt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) > arg_float_or_raise(argv[0], ">"));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], ">");
    return CLAY_BOOL(a > b);
}

static ClayValue number_eq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    intptr_t b;
    if (!arg_int(argv[0], &b)) return CLAY_FALSE;  /* not a number: never equal */
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) == arg_float_value(argc, argv));
    }
    intptr_t a = self_int_value(self);
    return CLAY_BOOL(a == b);
}

static ClayValue number_lte(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) <= arg_float_or_raise(argv[0], "<="));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], "<=");
    return CLAY_BOOL(a <= b);
}

static ClayValue number_gte(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) >= arg_float_or_raise(argv[0], ">="));
    }
    intptr_t a = self_int_value(self);
    intptr_t b = arg_int_or_raise(argv[0], ">=");
    return CLAY_BOOL(a >= b);
}

static ClayValue number_neq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_TRUE;
    intptr_t b;
    if (!arg_int(argv[0], &b)) return CLAY_TRUE;  /* not a number: never equal */
    if (is_float_arg(self, argc, argv)) {
        return CLAY_BOOL(self_float_value(self) != arg_float_value(argc, argv));
    }
    intptr_t a = self_int_value(self);
    return CLAY_BOOL(a != b);
}

static ClayValue number_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char buf[64];
    if (is_float_arg(self, 0, NULL)) {
        clay_format_float(self_float_value(self), buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "%lld", (long long)self_int_value(self));
    }
    return CLAY_VAL(clay_string_new(buf));
}

static ClayValue number_println(ClayObject *self, int argc, ClayValue *argv) {
    (void)argv;
    if (argc > 0) clay_raise_argument_error("println",
        "takes no arguments — it prints the receiver (to print a value, use Console println(value))");

    char buf[64];
    if (is_float_arg(self, 0, NULL)) {
        clay_format_float(self_float_value(self), buf, sizeof(buf) - 1);
        strcat(buf, "\n");
    } else {
        snprintf(buf, sizeof(buf), "%lld\n", (long long)self_int_value(self));
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
    return clay_float_tag(self_float_value(self));
}

static ClayValue number_toInt(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_int(self_int_value(self));
}

static ClayValue number_abs(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (is_float_arg(self, 0, NULL)) {
        float v = self_float_value(self);
        return clay_float_tag(v < 0 ? -v : v);
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
        ClayTryFrame _tf;
        if (clay_try_push(&_tf) == 0) {
            ClayValue new_result = clay_send(CLAY_OBJ(argv[0]), "value", 1, &arg);
            clay_release_val(result);
            result = new_result;
            clay_try_pop();
        } else {
            ClayValue err = clay_error_stack.current_error;
            int sig = clay_loop_signal(err);
            if (sig == CLAY_SIGNAL_BREAK) return result;
            if (sig == CLAY_SIGNAL_CONTINUE) continue;
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
