/*
 * math.c - Math object with common math functions
 */
#include "clay.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

/* First argument as a float; raises for missing / non-number (who: "Math sqrt") */
#define get_float_arg(who) clay_arg_float(argc, argv, 0, who)

static ClayValue math_abs(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    clay_arg_float(argc, argv, 0, "Math abs");   /* validate: raises if not a Number */
    if (argc < 1) return clay_int(0);
    if (CLAY_IS_INT(argv[0])) {
        intptr_t v = clay_to_int(argv[0]);
        return clay_int(v < 0 ? -v : v);
    }
    return clay_float_tag(fabsf(clay_to_float(argv[0])));
}

static ClayValue math_sqrt(ClayObject *self, int argc, ClayValue *argv) {
    return clay_float_tag(sqrtf(get_float_arg("Math sqrt")));
}

static ClayValue math_pow(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    clay_arg_float(argc, argv, 0, "Math pow");   /* validate: raises if not a Number */
    clay_arg_float(argc, argv, 1, "Math pow");   /* validate: raises if not a Number */
    if (argc < 2) return clay_int(0);
    float base = clay_to_float(argv[0]);
    float exp = clay_to_float(argv[1]);
    float result = powf(base, exp);
    /* Return int if both args were int and result is integral */
    if (CLAY_IS_INT(argv[0]) && CLAY_IS_INT(argv[1]) &&
        result >= (float)CLAY_INT_MIN && result <= (float)CLAY_INT_MAX &&   /* cast is UB outside */
        result == (float)(intptr_t)result) {
        return clay_int((intptr_t)result);
    }
    return clay_float_tag(result);
}

static ClayValue math_min(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    clay_arg_float(argc, argv, 0, "Math min");   /* validate: raises if not a Number */
    clay_arg_float(argc, argv, 1, "Math min");   /* validate: raises if not a Number */
    if (argc < 2) return clay_int(0);
    if (CLAY_IS_INT(argv[0]) && CLAY_IS_INT(argv[1])) {
        intptr_t a = clay_to_int(argv[0]), b = clay_to_int(argv[1]);
        return clay_int(a < b ? a : b);
    }
    float a = clay_to_float(argv[0]), b = clay_to_float(argv[1]);
    return clay_float_tag(a < b ? a : b);
}

static ClayValue math_max(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    clay_arg_float(argc, argv, 0, "Math max");   /* validate: raises if not a Number */
    clay_arg_float(argc, argv, 1, "Math max");   /* validate: raises if not a Number */
    if (argc < 2) return clay_int(0);
    if (CLAY_IS_INT(argv[0]) && CLAY_IS_INT(argv[1])) {
        intptr_t a = clay_to_int(argv[0]), b = clay_to_int(argv[1]);
        return clay_int(a > b ? a : b);
    }
    float a = clay_to_float(argv[0]), b = clay_to_float(argv[1]);
    return clay_float_tag(a > b ? a : b);
}

static ClayValue math_floor(ClayObject *self, int argc, ClayValue *argv) {
    return clay_int(clay_float_to_int(floorf(get_float_arg("Math floor"))));
}

static ClayValue math_ceil(ClayObject *self, int argc, ClayValue *argv) {
    return clay_int(clay_float_to_int(ceilf(get_float_arg("Math ceil"))));
}

static ClayValue math_round(ClayObject *self, int argc, ClayValue *argv) {
    return clay_int(clay_float_to_int(roundf(get_float_arg("Math round"))));
}

static ClayValue math_sin(ClayObject *self, int argc, ClayValue *argv) {
    return clay_float_tag(sinf(get_float_arg("Math sin")));
}

static ClayValue math_cos(ClayObject *self, int argc, ClayValue *argv) {
    return clay_float_tag(cosf(get_float_arg("Math cos")));
}

/* Per-thread xorshift32 PRNG — thread-safe, no shared state */
static CLAY_THREAD_LOCAL uint32_t g_random_state = 0;

static uint32_t xorshift32(void) {
    uint32_t x = g_random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_random_state = x;
    return x;
}

static ClayValue math_random(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    if (g_random_state == 0) {
        g_random_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)&self;
        if (g_random_state == 0) g_random_state = 1;
    }
    return clay_float_tag((float)xorshift32() / (float)UINT32_MAX);
}

/* --- clamp / lerp / sign: shared by Math and Number (number.c) --- */

static int num_less(ClayValue a, ClayValue b) {
    if (CLAY_IS_INT(a) && CLAY_IS_INT(b)) return clay_to_int(a) < clay_to_int(b);
    return clay_to_float(a) < clay_to_float(b);
}

/* n limited to [lo, hi]; returns whichever of the three is chosen, so integer
 * arguments give an integer. Raises ArgumentError if lo > hi. */
ClayValue clay_math_clamp(ClayValue n, ClayValue lo, ClayValue hi, const char *who) {
    if (num_less(hi, lo)) clay_raise_argument_error(who, "lower bound is greater than upper bound");
    if (num_less(n, lo)) return lo;
    if (num_less(hi, n)) return hi;
    return n;
}

/* a + (b - a) * t: an integer when all three are integers, else a float */
ClayValue clay_math_lerp(ClayValue a, ClayValue b, ClayValue t) {
    if (CLAY_IS_INT(a) && CLAY_IS_INT(b) && CLAY_IS_INT(t)) {
        intptr_t ia = clay_to_int(a), ib = clay_to_int(b);
        return clay_int(ia + clay_int_mul(ib - ia, clay_to_int(t)));
    }
    float fa = clay_to_float(a), fb = clay_to_float(b);
    return clay_float_tag(fa + (fb - fa) * clay_to_float(t));
}

/* -1, 0 or 1 (always an integer) */
ClayValue clay_math_sign(ClayValue n) {
    if (CLAY_IS_INT(n)) {
        intptr_t v = clay_to_int(n);
        return clay_int(v > 0 ? 1 : (v < 0 ? -1 : 0));
    }
    float f = clay_to_float(n);
    return clay_int(f > 0 ? 1 : (f < 0 ? -1 : 0));
}

static ClayValue math_clamp(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    for (int i = 0; i < 3; i++) clay_arg_float(argc, argv, i, "Math clamp");
    return clay_math_clamp(argv[0], argv[1], argv[2], "Math clamp");
}

static ClayValue math_lerp(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    for (int i = 0; i < 3; i++) clay_arg_float(argc, argv, i, "Math lerp");
    return clay_math_lerp(argv[0], argv[1], argv[2]);
}

static ClayValue math_sign(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    clay_arg_float(argc, argv, 0, "Math sign");
    return clay_math_sign(argv[0]);
}

void clay_math_init(ClayObject *proto) {
    clay_set_method(proto, "abs",    math_abs);
    clay_set_method(proto, "sqrt",   math_sqrt);
    clay_set_method(proto, "pow",    math_pow);
    clay_set_method(proto, "min",    math_min);
    clay_set_method(proto, "max",    math_max);
    clay_set_method(proto, "clamp",  math_clamp);
    clay_set_method(proto, "lerp",   math_lerp);
    clay_set_method(proto, "sign",   math_sign);
    clay_set_method(proto, "floor",  math_floor);
    clay_set_method(proto, "ceil",   math_ceil);
    clay_set_method(proto, "round",  math_round);
    clay_set_method(proto, "sin",    math_sin);
    clay_set_method(proto, "cos",    math_cos);
    clay_set_method(proto, "random", math_random);

    /* Constants as slots */
    clay_set_slot(proto, "pi", clay_float_tag(3.14159265f));
    clay_set_slot(proto, "e",  clay_float_tag(2.71828183f));
}
