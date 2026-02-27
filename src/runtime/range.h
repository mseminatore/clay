#ifndef CLAY_RANGE_H
#define CLAY_RANGE_H
#include "clay.h"
ClayObject *clay_range_new(intptr_t start, intptr_t end, intptr_t step);
void clay_range_init_methods(ClayObject *proto);
#endif
