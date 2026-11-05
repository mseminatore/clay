/*
 * tagged.c - Tagged value encoding for small integers and floats
 */
#include "clay.h"

/* Raise OverflowError for an integer result outside the tagged range */
void clay_raise_overflow(void) {
    clay_raise(CLAY_VAL(clay_error_new(
        "integer overflow (integers are 62-bit; use toFloat for larger values)",
        "OverflowError")));
}

ClayValue clay_int(intptr_t n) {
    if (n > CLAY_INT_MAX || n < CLAY_INT_MIN) clay_raise_overflow();
    /* shift as unsigned: left-shifting a negative value is undefined in C */
    return (ClayValue)(((uintptr_t)n << 2) | CLAY_TAG_INT);
}

intptr_t clay_int_mul(intptr_t a, intptr_t b) {
    /* Operands are tagged-range ints, so |a|, |b| < 2^61: no INTPTR_MIN edge */
    if (a != 0 && b != 0) {
        intptr_t aa = a < 0 ? -a : a, ab = b < 0 ? -b : b;
        if (aa > CLAY_INT_MAX / ab) clay_raise_overflow();
    }
    return a * b;   /* caller passes the result through clay_int() */
}

intptr_t clay_float_to_int(float f) {
    /* Out-of-range (or NaN) float → int conversion is undefined in C */
    if (!(f >= (float)CLAY_INT_MIN && f <= (float)CLAY_INT_MAX)) clay_raise_overflow();
    return (intptr_t)f;
}

intptr_t clay_to_int(ClayValue v) {
    return ((intptr_t)v) >> 2;
}

int clay_is_int(ClayValue v) {
    return CLAY_IS_INT(v);
}

ClayValue clay_float_tag(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return (ClayValue)((uintptr_t)bits << 2) | CLAY_TAG_FLOAT;
}

float clay_float_untag(ClayValue v) {
    uint32_t bits = (uint32_t)(v >> 2);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
