/*
 * graphics.h - Graphics prototype (SDL2 backend)
 *
 * Provides window management, pixel drawing, image loading,
 * drawing primitives, and input handling for Clay programs.
 * Only available when compiled with CLAY_HAS_SDL2.
 */
#ifndef CLAY_GRAPHICS_H
#define CLAY_GRAPHICS_H

#include "clay.h"

void clay_graphics_init(ClayObject *proto);
void clay_graphics_shutdown(void);

#endif /* CLAY_GRAPHICS_H */
