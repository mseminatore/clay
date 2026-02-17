/*
 * tagged.c - Tagged value encoding for small integers
 */
#include "clay.h"

ClayValue clay_int(intptr_t n) {
    return (ClayValue)((n << 1) | CLAY_TAG_INT);
}

intptr_t clay_to_int(ClayValue v) {
    return ((intptr_t)v) >> 1;
}

int clay_is_int(ClayValue v) {
    return CLAY_IS_INT(v);
}
