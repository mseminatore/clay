/*
 * tagged.c - Tagged value encoding for small integers and floats
 */
#include "clay.h"

ClayValue clay_int(intptr_t n) {
    return (ClayValue)((n << 2) | CLAY_TAG_INT);
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
