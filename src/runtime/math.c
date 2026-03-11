/*
 * math.c - Math object with common math functions
 */
#include "clay.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

static float get_float_arg(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return 0.0f;
    return clay_to_float(argv[0]);
}

static ClayValue math_abs(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return clay_int(0);
    if (CLAY_IS_INT(argv[0])) {
        intptr_t v = clay_to_int(argv[0]);
        return clay_int(v < 0 ? -v : v);
    }
    return clay_float_tag(fabsf(clay_to_float(argv[0])));
}

static ClayValue math_sqrt(ClayObject *self, int argc, ClayValue *argv) {
    return clay_float_tag(sqrtf(get_float_arg(self, argc, argv)));
}

static ClayValue math_pow(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) return clay_int(0);
    float base = clay_to_float(argv[0]);
    float exp = clay_to_float(argv[1]);
    float result = powf(base, exp);
    /* Return int if both args were int and result is integral */
    if (CLAY_IS_INT(argv[0]) && CLAY_IS_INT(argv[1]) && result == (float)(intptr_t)result) {
        return clay_int((intptr_t)result);
    }
    return clay_float_tag(result);
}

static ClayValue math_min(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
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
    if (argc < 2) return clay_int(0);
    if (CLAY_IS_INT(argv[0]) && CLAY_IS_INT(argv[1])) {
        intptr_t a = clay_to_int(argv[0]), b = clay_to_int(argv[1]);
        return clay_int(a > b ? a : b);
    }
    float a = clay_to_float(argv[0]), b = clay_to_float(argv[1]);
    return clay_float_tag(a > b ? a : b);
}

static ClayValue math_floor(ClayObject *self, int argc, ClayValue *argv) {
    return clay_int((intptr_t)floorf(get_float_arg(self, argc, argv)));
}

static ClayValue math_ceil(ClayObject *self, int argc, ClayValue *argv) {
    return clay_int((intptr_t)ceilf(get_float_arg(self, argc, argv)));
}

static ClayValue math_round(ClayObject *self, int argc, ClayValue *argv) {
    return clay_int((intptr_t)roundf(get_float_arg(self, argc, argv)));
}

static ClayValue math_sin(ClayObject *self, int argc, ClayValue *argv) {
    return clay_float_tag(sinf(get_float_arg(self, argc, argv)));
}

static ClayValue math_cos(ClayObject *self, int argc, ClayValue *argv) {
    return clay_float_tag(cosf(get_float_arg(self, argc, argv)));
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

void clay_math_init(ClayObject *proto) {
    clay_set_method(proto, "abs",    math_abs);
    clay_set_method(proto, "sqrt",   math_sqrt);
    clay_set_method(proto, "pow",    math_pow);
    clay_set_method(proto, "min",    math_min);
    clay_set_method(proto, "max",    math_max);
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
