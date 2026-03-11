/*
 * graphics.c - Graphics prototype with SDL2 backend
 *
 * Phase A: Window, pixels, PPM loading, basic events, drawImage, refresh.
 * Phase B: Drawing primitives, full input, timing.
 */
#include "clay.h"
#include "graphics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CLAY_HAS_SDL2
#include <SDL.h>

/* Internal state — single window for now */
static SDL_Window   *g_window   = NULL;
static SDL_Renderer *g_renderer = NULL;
static int g_width  = 0;
static int g_height = 0;

/* Texture storage for loaded images */
#define MAX_TEXTURES 64
static SDL_Texture *g_textures[MAX_TEXTURES];
static int g_tex_widths[MAX_TEXTURES];
static int g_tex_heights[MAX_TEXTURES];
static int g_texture_count = 0;

static int g_sdl_initialized = 0;

/* Current draw color */
static Uint8 g_color_r = 255, g_color_g = 255, g_color_b = 255;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void ensure_sdl(void) {
    if (!g_sdl_initialized) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "SDL_Init failed: %s", SDL_GetError());
            clay_raise(CLAY_VAL(clay_error_new(buf, "GraphicsError")));
            return;
        }
        g_sdl_initialized = 1;
    }
}

static int extract_int(ClayValue v) {
    if (CLAY_IS_INT(v)) return (int)clay_to_int(v);
    if (CLAY_IS_TAGGED_FLOAT(v)) return (int)clay_float_untag(v);
    if (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v)) {
        ClayObject *o = CLAY_OBJ(v);
        if (o->type == CLAY_TYPE_FLOAT)
            return (int)o->as.number_float.value;
    }
    return 0;
}

static double extract_float(ClayValue v) {
    if (CLAY_IS_TAGGED_FLOAT(v)) return (double)clay_float_untag(v);
    if (CLAY_IS_INT(v)) return (double)clay_to_int(v);
    if (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v)) {
        ClayObject *o = CLAY_OBJ(v);
        if (o->type == CLAY_TYPE_FLOAT)
            return (double)o->as.number_float.value;
    }
    return 0.0;
}

/* ------------------------------------------------------------------ */
/*  Window management                                                  */
/* ------------------------------------------------------------------ */

/* Graphics window(width, height) or window(width, height, title) */
static ClayValue gfx_window(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2) {
        clay_raise(CLAY_VAL(clay_error_new(
            "Graphics window requires (width, height) or (width, height, title)",
            "ArgumentError")));
        return CLAY_NIL;
    }

    int w = extract_int(argv[0]);
    int h = extract_int(argv[1]);
    const char *title = "Clay";
    if (argc >= 3 && CLAY_IS_PTR(argv[2]) && !CLAY_IS_NIL(argv[2]))
        title = clay_string_data(CLAY_OBJ(argv[2]));

    ensure_sdl();

    if (g_window) {
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        g_renderer = NULL;
        g_window = NULL;
    }

    g_window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_SHOWN);
    if (!g_window) {
        char buf[256];
        snprintf(buf, sizeof(buf), "SDL_CreateWindow failed: %s", SDL_GetError());
        clay_raise(CLAY_VAL(clay_error_new(buf, "GraphicsError")));
        return CLAY_NIL;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        /* Fallback to software renderer */
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        clay_raise(CLAY_VAL(clay_error_new("SDL_CreateRenderer failed", "GraphicsError")));
        return CLAY_NIL;
    }

    g_width  = w;
    g_height = h;

    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);

    return CLAY_VAL(self);
}

/* Graphics close() */
static ClayValue gfx_close(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    /* Free all textures */
    for (int i = 0; i < g_texture_count; i++) {
        if (g_textures[i]) {
            SDL_DestroyTexture(g_textures[i]);
            g_textures[i] = NULL;
        }
    }
    g_texture_count = 0;

    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
    if (g_window)   { SDL_DestroyWindow(g_window);     g_window = NULL; }
    return CLAY_NIL;
}

/* Graphics width() */
static ClayValue gfx_width(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int(g_width);
}

/* Graphics height() */
static ClayValue gfx_height(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int(g_height);
}

/* Graphics title(text) */
static ClayValue gfx_title(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || CLAY_IS_NIL(argv[0])) {
        clay_raise(CLAY_VAL(clay_error_new("title requires a string argument", "ArgumentError")));
        return CLAY_NIL;
    }
    if (g_window)
        SDL_SetWindowTitle(g_window, clay_string_data(CLAY_OBJ(argv[0])));
    return CLAY_VAL(self);
}

/* Graphics resize(w, h) — resize the window without destroying textures */
static ClayValue gfx_resize(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_window || argc < 2) return CLAY_VAL(self);
    int w = extract_int(argv[0]);
    int h = extract_int(argv[1]);
    SDL_SetWindowSize(g_window, w, h);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    g_width = w;
    g_height = h;
    return CLAY_VAL(self);
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

/* Graphics clear() or clear(r, g, b) */
static ClayValue gfx_clear(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer) return CLAY_NIL;
    Uint8 r = 0, g = 0, b = 0;
    if (argc >= 3) {
        r = (Uint8)extract_int(argv[0]);
        g = (Uint8)extract_int(argv[1]);
        b = (Uint8)extract_int(argv[2]);
    }
    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    SDL_RenderClear(g_renderer);
    return CLAY_VAL(self);
}

/* Graphics setPixel(x, y, r, g, b) */
static ClayValue gfx_set_pixel(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 5) return CLAY_NIL;
    int x = extract_int(argv[0]);
    int y = extract_int(argv[1]);
    Uint8 r = (Uint8)extract_int(argv[2]);
    Uint8 g = (Uint8)extract_int(argv[3]);
    Uint8 b = (Uint8)extract_int(argv[4]);
    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    SDL_RenderDrawPoint(g_renderer, x, y);
    return CLAY_VAL(self);
}

/* Graphics color(r, g, b) — set current draw color */
static ClayValue gfx_color(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc >= 3) {
        g_color_r = (Uint8)extract_int(argv[0]);
        g_color_g = (Uint8)extract_int(argv[1]);
        g_color_b = (Uint8)extract_int(argv[2]);
    }
    return CLAY_VAL(self);
}

/* Graphics drawLine(x1, y1, x2, y2) or drawLine(x1, y1, x2, y2, r, g, b) */
static ClayValue gfx_draw_line(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 4) return CLAY_NIL;
    int x1 = extract_int(argv[0]);
    int y1 = extract_int(argv[1]);
    int x2 = extract_int(argv[2]);
    int y2 = extract_int(argv[3]);
    Uint8 r = g_color_r, g = g_color_g, b = g_color_b;
    if (argc >= 7) {
        r = (Uint8)extract_int(argv[4]);
        g = (Uint8)extract_int(argv[5]);
        b = (Uint8)extract_int(argv[6]);
    }
    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    SDL_RenderDrawLine(g_renderer, x1, y1, x2, y2);
    return CLAY_VAL(self);
}

/* Graphics drawRect(x, y, w, h) or drawRect(x, y, w, h, r, g, b) */
static ClayValue gfx_draw_rect(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 4) return CLAY_NIL;
    SDL_Rect rect;
    rect.x = extract_int(argv[0]);
    rect.y = extract_int(argv[1]);
    rect.w = extract_int(argv[2]);
    rect.h = extract_int(argv[3]);
    Uint8 r = g_color_r, g = g_color_g, b = g_color_b;
    if (argc >= 7) {
        r = (Uint8)extract_int(argv[4]);
        g = (Uint8)extract_int(argv[5]);
        b = (Uint8)extract_int(argv[6]);
    }
    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    SDL_RenderDrawRect(g_renderer, &rect);
    return CLAY_VAL(self);
}

/* Graphics fillRect(x, y, w, h) or fillRect(x, y, w, h, r, g, b) */
static ClayValue gfx_fill_rect(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 4) return CLAY_NIL;
    SDL_Rect rect;
    rect.x = extract_int(argv[0]);
    rect.y = extract_int(argv[1]);
    rect.w = extract_int(argv[2]);
    rect.h = extract_int(argv[3]);
    Uint8 r = g_color_r, g = g_color_g, b = g_color_b;
    if (argc >= 7) {
        r = (Uint8)extract_int(argv[4]);
        g = (Uint8)extract_int(argv[5]);
        b = (Uint8)extract_int(argv[6]);
    }
    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    SDL_RenderFillRect(g_renderer, &rect);
    return CLAY_VAL(self);
}

/* Midpoint circle algorithm */
static void draw_circle_points(int cx, int cy, int x, int y) {
    SDL_RenderDrawPoint(g_renderer, cx + x, cy + y);
    SDL_RenderDrawPoint(g_renderer, cx - x, cy + y);
    SDL_RenderDrawPoint(g_renderer, cx + x, cy - y);
    SDL_RenderDrawPoint(g_renderer, cx - x, cy - y);
    SDL_RenderDrawPoint(g_renderer, cx + y, cy + x);
    SDL_RenderDrawPoint(g_renderer, cx - y, cy + x);
    SDL_RenderDrawPoint(g_renderer, cx + y, cy - x);
    SDL_RenderDrawPoint(g_renderer, cx - y, cy - x);
}

/* Graphics drawCircle(cx, cy, radius) or drawCircle(cx, cy, radius, r, g, b) */
static ClayValue gfx_draw_circle(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 3) return CLAY_NIL;
    int cx = extract_int(argv[0]);
    int cy = extract_int(argv[1]);
    int radius = extract_int(argv[2]);
    Uint8 r = g_color_r, g = g_color_g, b = g_color_b;
    if (argc >= 6) {
        r = (Uint8)extract_int(argv[3]);
        g = (Uint8)extract_int(argv[4]);
        b = (Uint8)extract_int(argv[5]);
    }
    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);

    int x = 0, y = radius;
    int d = 3 - 2 * radius;
    draw_circle_points(cx, cy, x, y);
    while (y >= x) {
        x++;
        if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
        else       { d = d + 4 * x + 6; }
        draw_circle_points(cx, cy, x, y);
    }
    return CLAY_VAL(self);
}

/* Graphics fillCircle(cx, cy, radius) or fillCircle(cx, cy, radius, r, g, b) */
static ClayValue gfx_fill_circle(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 3) return CLAY_NIL;
    int cx = extract_int(argv[0]);
    int cy = extract_int(argv[1]);
    int radius = extract_int(argv[2]);
    Uint8 r = g_color_r, g = g_color_g, b = g_color_b;
    if (argc >= 6) {
        r = (Uint8)extract_int(argv[3]);
        g = (Uint8)extract_int(argv[4]);
        b = (Uint8)extract_int(argv[5]);
    }
    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);

    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)SDL_sqrt((double)(radius * radius - dy * dy));
        SDL_RenderDrawLine(g_renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
    return CLAY_VAL(self);
}

/* Graphics refresh() — present the renderer */
static ClayValue gfx_refresh(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    if (g_renderer) SDL_RenderPresent(g_renderer);
    return CLAY_VAL(self);
}

/* ------------------------------------------------------------------ */
/*  Image / PPM support                                                */
/* ------------------------------------------------------------------ */

/* Parse P3 or P6 PPM, return texture id (int) */
static ClayValue gfx_load_ppm(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || CLAY_IS_NIL(argv[0])) {
        clay_raise(CLAY_VAL(clay_error_new("loadPPM requires a path", "ArgumentError")));
        return CLAY_NIL;
    }
    if (!g_renderer) {
        clay_raise(CLAY_VAL(clay_error_new("No window — call Graphics window() first", "GraphicsError")));
        return CLAY_NIL;
    }
    if (g_texture_count >= MAX_TEXTURES) {
        clay_raise(CLAY_VAL(clay_error_new("Maximum textures reached", "GraphicsError")));
        return CLAY_NIL;
    }

    const char *path = clay_string_data(CLAY_OBJ(argv[0]));
    FILE *f = fopen(path, "rb");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot open PPM file: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }

    /* Read magic number */
    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1) {
        fclose(f);
        clay_raise(CLAY_VAL(clay_error_new("Invalid PPM: cannot read magic", "FormatError")));
        return CLAY_NIL;
    }

    int is_p6 = (strcmp(magic, "P6") == 0);
    int is_p3 = (strcmp(magic, "P3") == 0);
    if (!is_p3 && !is_p6) {
        fclose(f);
        clay_raise(CLAY_VAL(clay_error_new("Unsupported PPM format (need P3 or P6)", "FormatError")));
        return CLAY_NIL;
    }

    /* Skip comments */
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '#') { while ((ch = fgetc(f)) != EOF && ch != '\n'); }
        else if (ch > ' ') { ungetc(ch, f); break; }
    }

    int w, h, maxval;
    if (fscanf(f, "%d %d", &w, &h) != 2) {
        fclose(f);
        clay_raise(CLAY_VAL(clay_error_new("Invalid PPM: cannot read dimensions", "FormatError")));
        return CLAY_NIL;
    }

    /* Skip comments between dimensions and maxval */
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '#') { while ((ch = fgetc(f)) != EOF && ch != '\n'); }
        else if (ch > ' ') { ungetc(ch, f); break; }
    }

    if (fscanf(f, "%d", &maxval) != 1) {
        fclose(f);
        clay_raise(CLAY_VAL(clay_error_new("Invalid PPM: cannot read maxval", "FormatError")));
        return CLAY_NIL;
    }

    /* Skip single whitespace after maxval (P6 requirement) */
    fgetc(f);

    /* Read pixel data */
    Uint8 *pixels = (Uint8 *)malloc(w * h * 3);
    if (!pixels) {
        fclose(f);
        clay_raise(CLAY_VAL(clay_error_new("Out of memory loading PPM", "RuntimeError")));
        return CLAY_NIL;
    }

    if (is_p6) {
        size_t read = fread(pixels, 1, (size_t)(w * h * 3), f);
        if (read != (size_t)(w * h * 3)) {
            free(pixels);
            fclose(f);
            clay_raise(CLAY_VAL(clay_error_new("PPM P6: unexpected end of data", "FormatError")));
            return CLAY_NIL;
        }
    } else {
        /* P3: ASCII values */
        for (int i = 0; i < w * h * 3; i++) {
            int val;
            if (fscanf(f, "%d", &val) != 1) {
                free(pixels);
                fclose(f);
                clay_raise(CLAY_VAL(clay_error_new("PPM P3: unexpected end of data", "FormatError")));
                return CLAY_NIL;
            }
            pixels[i] = (Uint8)(maxval == 255 ? val : val * 255 / maxval);
        }
    }
    fclose(f);

    /* Scale if maxval != 255 (P6 path) */
    if (is_p6 && maxval != 255) {
        for (int i = 0; i < w * h * 3; i++)
            pixels[i] = (Uint8)((int)pixels[i] * 255 / maxval);
    }

    /* Create SDL texture */
    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(
        pixels, w, h, 24, w * 3,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    if (!surface) {
        free(pixels);
        clay_raise(CLAY_VAL(clay_error_new("SDL_CreateRGBSurfaceFrom failed", "GraphicsError")));
        return CLAY_NIL;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_FreeSurface(surface);
    free(pixels);

    if (!tex) {
        clay_raise(CLAY_VAL(clay_error_new("SDL_CreateTextureFromSurface failed", "GraphicsError")));
        return CLAY_NIL;
    }

    int id = g_texture_count;
    g_textures[id] = tex;
    g_tex_widths[id] = w;
    g_tex_heights[id] = h;
    g_texture_count++;

    return clay_int(id);
}

/* Graphics drawImage(textureId, x, y) or drawImage(textureId, x, y, w, h) */
static ClayValue gfx_draw_image(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 3) return CLAY_NIL;
    int id = extract_int(argv[0]);
    if (id < 0 || id >= g_texture_count || !g_textures[id]) return CLAY_NIL;

    SDL_Rect dst;
    dst.x = extract_int(argv[1]);
    dst.y = extract_int(argv[2]);
    if (argc >= 5) {
        dst.w = extract_int(argv[3]);
        dst.h = extract_int(argv[4]);
    } else {
        dst.w = g_tex_widths[id];
        dst.h = g_tex_heights[id];
    }

    SDL_RenderCopy(g_renderer, g_textures[id], NULL, &dst);
    return CLAY_VAL(self);
}

/* Graphics imageWidth(textureId) */
static ClayValue gfx_image_width(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return clay_int(0);
    int id = extract_int(argv[0]);
    if (id < 0 || id >= g_texture_count) return clay_int(0);
    return clay_int(g_tex_widths[id]);
}

/* Graphics imageHeight(textureId) */
static ClayValue gfx_image_height(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return clay_int(0);
    int id = extract_int(argv[0]);
    if (id < 0 || id >= g_texture_count) return clay_int(0);
    return clay_int(g_tex_heights[id]);
}

/* ------------------------------------------------------------------ */
/*  Events / Input                                                     */
/* ------------------------------------------------------------------ */

/* Helper to set a dict key — avoids compound literal in macro expansion */
static void dict_put(ClayObject *dict, const char *key, ClayValue val) {
    ClayValue args[2];
    args[0] = CLAY_VAL(clay_string_new(key));
    args[1] = val;
    clay_send_val(CLAY_VAL(dict), "set", 2, args);
}

/* Convert SDL event to a Clay Dict */
static ClayValue event_to_dict(SDL_Event *e) {
    ClayObject *dict = clay_clone(clay_Dict);
    const char *type_str = NULL;

    switch (e->type) {
    case SDL_QUIT:
        type_str = "quit";
        break;
    case SDL_KEYDOWN:
        type_str = "keydown";
        dict_put(dict, "key", CLAY_VAL(clay_string_new(SDL_GetKeyName(e->key.keysym.sym))));
        break;
    case SDL_KEYUP:
        type_str = "keyup";
        dict_put(dict, "key", CLAY_VAL(clay_string_new(SDL_GetKeyName(e->key.keysym.sym))));
        break;
    case SDL_MOUSEBUTTONDOWN:
        type_str = "mousedown";
        dict_put(dict, "x", clay_int(e->button.x));
        dict_put(dict, "y", clay_int(e->button.y));
        dict_put(dict, "button", clay_int(e->button.button));
        break;
    case SDL_MOUSEBUTTONUP:
        type_str = "mouseup";
        dict_put(dict, "x", clay_int(e->button.x));
        dict_put(dict, "y", clay_int(e->button.y));
        dict_put(dict, "button", clay_int(e->button.button));
        break;
    case SDL_MOUSEMOTION:
        type_str = "mousemove";
        dict_put(dict, "x", clay_int(e->motion.x));
        dict_put(dict, "y", clay_int(e->motion.y));
        break;
    default:
        clay_release(dict);
        return CLAY_NIL;
    }

    if (type_str)
        dict_put(dict, "type", CLAY_VAL(clay_string_new(type_str)));

    return CLAY_VAL(dict);
}

/* Graphics pollEvent() — returns dict or nil */
static ClayValue gfx_poll_event(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    SDL_Event e;
    if (SDL_PollEvent(&e))
        return event_to_dict(&e);
    return CLAY_NIL;
}

/* Graphics waitEvent() — blocks until event */
static ClayValue gfx_wait_event(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    SDL_Event e;
    if (SDL_WaitEvent(&e))
        return event_to_dict(&e);
    return CLAY_NIL;
}

/* Graphics isKeyDown(keyName) */
static ClayValue gfx_is_key_down(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    const char *name = clay_string_data(CLAY_OBJ(argv[0]));
    SDL_Keycode kc = SDL_GetKeyFromName(name);
    if (kc == SDLK_UNKNOWN) return CLAY_FALSE;
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
    return state[sc] ? CLAY_TRUE : CLAY_FALSE;
}

/* Graphics mouseX() */
static ClayValue gfx_mouse_x(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    int x, y;
    SDL_GetMouseState(&x, &y);
    return clay_int(x);
}

/* Graphics mouseY() */
static ClayValue gfx_mouse_y(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    int x, y;
    SDL_GetMouseState(&x, &y);
    return clay_int(y);
}

/* Graphics mouseDown() */
static ClayValue gfx_mouse_down(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(1)) ? CLAY_TRUE : CLAY_FALSE;
}

/* ------------------------------------------------------------------ */
/*  Timing                                                             */
/* ------------------------------------------------------------------ */

/* Graphics ticks() — ms since SDL init */
static ClayValue gfx_ticks(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    ensure_sdl();
    return clay_int((intptr_t)SDL_GetTicks());
}

/* Graphics delay(ms) */
static ClayValue gfx_delay(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return CLAY_NIL;
    int ms = extract_int(argv[0]);
    if (ms > 0) SDL_Delay((Uint32)ms);
    return CLAY_VAL(self);
}

/* Graphics fps(target) — delay to achieve target fps */
static ClayValue gfx_fps(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    static Uint32 last_ticks = 0;
    if (argc < 1) return CLAY_NIL;
    int target = extract_int(argv[0]);
    if (target <= 0) return CLAY_VAL(self);

    Uint32 frame_time = 1000 / (Uint32)target;
    Uint32 now = SDL_GetTicks();
    if (last_ticks != 0) {
        Uint32 elapsed = now - last_ticks;
        if (elapsed < frame_time)
            SDL_Delay(frame_time - elapsed);
    }
    last_ticks = SDL_GetTicks();
    return CLAY_VAL(self);
}

/* Graphics fullscreen() */
static ClayValue gfx_fullscreen(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    if (g_window)
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    return CLAY_VAL(self);
}

/* Graphics windowed() */
static ClayValue gfx_windowed(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    if (g_window)
        SDL_SetWindowFullscreen(g_window, 0);
    return CLAY_VAL(self);
}

/* ------------------------------------------------------------------ */
/*  Pixel buffer (for fast bulk pixel writing)                        */
/* ------------------------------------------------------------------ */

static SDL_Texture *g_pixel_tex = NULL;
static Uint32 *g_pixel_buf = NULL;
static int g_pixel_buf_w = 0, g_pixel_buf_h = 0;

/* Graphics createBuffer(w, h) — create pixel buffer for fast writes */
static ClayValue gfx_create_buffer(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 2) return CLAY_NIL;
    int w = extract_int(argv[0]);
    int h = extract_int(argv[1]);

    if (g_pixel_tex) SDL_DestroyTexture(g_pixel_tex);
    free(g_pixel_buf);

    g_pixel_buf = (Uint32 *)calloc((size_t)(w * h), sizeof(Uint32));
    g_pixel_tex = SDL_CreateTexture(g_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    g_pixel_buf_w = w;
    g_pixel_buf_h = h;

    return CLAY_VAL(self);
}

/* Graphics bufferPixel(x, y, r, g, b) — set pixel in buffer (no render) */
static ClayValue gfx_buffer_pixel(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_pixel_buf || argc < 5) return CLAY_NIL;
    int x = extract_int(argv[0]);
    int y = extract_int(argv[1]);
    if (x < 0 || x >= g_pixel_buf_w || y < 0 || y >= g_pixel_buf_h) return CLAY_VAL(self);
    Uint8 r = (Uint8)extract_int(argv[2]);
    Uint8 g = (Uint8)extract_int(argv[3]);
    Uint8 b = (Uint8)extract_int(argv[4]);
    g_pixel_buf[y * g_pixel_buf_w + x] = (Uint32)(0xFF000000 | (r << 16) | (g << 8) | b);
    return CLAY_VAL(self);
}

/* Graphics drawBuffer() — blit pixel buffer to screen */
static ClayValue gfx_draw_buffer(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    if (!g_pixel_tex || !g_pixel_buf || !g_renderer) return CLAY_NIL;
    SDL_UpdateTexture(g_pixel_tex, NULL, g_pixel_buf, g_pixel_buf_w * (int)sizeof(Uint32));
    SDL_RenderCopy(g_renderer, g_pixel_tex, NULL, NULL);
    return CLAY_VAL(self);
}

/* ------------------------------------------------------------------ */
/*  Phase C: Sprites + Transforms + Text                               */
/* ------------------------------------------------------------------ */

/* Sprite: a sub-region of a texture */
#define MAX_SPRITES 256
static struct {
    int tex_id;
    SDL_Rect src;
} g_sprites[MAX_SPRITES];
static int g_sprite_count = 0;

/* Graphics createSprite(textureId, x, y, w, h) */
static ClayValue gfx_create_sprite(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 5 || g_sprite_count >= MAX_SPRITES) return clay_int(-1);
    int id = g_sprite_count++;
    g_sprites[id].tex_id = extract_int(argv[0]);
    g_sprites[id].src.x = extract_int(argv[1]);
    g_sprites[id].src.y = extract_int(argv[2]);
    g_sprites[id].src.w = extract_int(argv[3]);
    g_sprites[id].src.h = extract_int(argv[4]);
    return clay_int(id);
}

/* Graphics drawSprite(spriteId, x, y) */
static ClayValue gfx_draw_sprite(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 3) return CLAY_NIL;
    int id = extract_int(argv[0]);
    if (id < 0 || id >= g_sprite_count) return CLAY_NIL;
    int tex = g_sprites[id].tex_id;
    if (tex < 0 || tex >= g_texture_count || !g_textures[tex]) return CLAY_NIL;
    SDL_Rect dst = { extract_int(argv[1]), extract_int(argv[2]),
                     g_sprites[id].src.w, g_sprites[id].src.h };
    SDL_RenderCopy(g_renderer, g_textures[tex], &g_sprites[id].src, &dst);
    return CLAY_VAL(self);
}

/* Graphics drawSpriteEx(spriteId, x, y, angle, scaleX, scaleY) */
static ClayValue gfx_draw_sprite_ex(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 3) return CLAY_NIL;
    int id = extract_int(argv[0]);
    if (id < 0 || id >= g_sprite_count) return CLAY_NIL;
    int tex = g_sprites[id].tex_id;
    if (tex < 0 || tex >= g_texture_count || !g_textures[tex]) return CLAY_NIL;

    int x = extract_int(argv[1]), y = extract_int(argv[2]);
    double angle = (argc >= 4) ? extract_float(argv[3]) : 0.0;
    double sx = (argc >= 5) ? extract_float(argv[4]) : 1.0;
    double sy = (argc >= 6) ? extract_float(argv[5]) : sx;

    SDL_Rect dst = { x, y, (int)(g_sprites[id].src.w * sx),
                     (int)(g_sprites[id].src.h * sy) };
    SDL_RenderCopyEx(g_renderer, g_textures[tex], &g_sprites[id].src,
                     &dst, angle, NULL, SDL_FLIP_NONE);
    return CLAY_VAL(self);
}

/* Graphics drawImageEx(texId, x, y, angle, scaleX, scaleY) */
static ClayValue gfx_draw_image_ex(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 3) return CLAY_NIL;
    int id = extract_int(argv[0]);
    if (id < 0 || id >= g_texture_count || !g_textures[id]) return CLAY_NIL;

    int x = extract_int(argv[1]), y = extract_int(argv[2]);
    double angle = (argc >= 4) ? extract_float(argv[3]) : 0.0;
    double sx = (argc >= 5) ? extract_float(argv[4]) : 1.0;
    double sy = (argc >= 6) ? extract_float(argv[5]) : sx;

    SDL_Rect dst = { x, y, (int)(g_tex_widths[id] * sx),
                     (int)(g_tex_heights[id] * sy) };
    SDL_RenderCopyEx(g_renderer, g_textures[id], NULL,
                     &dst, angle, NULL, SDL_FLIP_NONE);
    return CLAY_VAL(self);
}

/* ---- Bitmap font (built-in 5x7 glyphs for ASCII 32-126) ---- */

static const unsigned char g_font_5x7[][7] = {
    /* space */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* ! */  {0x04,0x04,0x04,0x04,0x00,0x00,0x04},
    /* " */  {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00},
    /* # */  {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    /* $ */  {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    /* % */  {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    /* & */  {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
    /* ' */  {0x0C,0x04,0x08,0x00,0x00,0x00,0x00},
    /* ( */  {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    /* ) */  {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    /* * */  {0x00,0x04,0x15,0x0E,0x15,0x04,0x00},
    /* + */  {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    /* , */  {0x00,0x00,0x00,0x00,0x0C,0x04,0x08},
    /* - */  {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* . */  {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    /* / */  {0x00,0x01,0x02,0x04,0x08,0x10,0x00},
    /* 0 */  {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* 1 */  {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* 2 */  {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    /* 3 */  {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    /* 4 */  {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* 5 */  {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* 6 */  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* 7 */  {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 8 */  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* 9 */  {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* : */  {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    /* ; */  {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},
    /* < */  {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    /* = */  {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    /* > */  {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    /* ? */  {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    /* @ */  {0x0E,0x11,0x17,0x15,0x17,0x10,0x0F},
    /* A */  {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* B */  {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* C */  {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* D */  {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    /* E */  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* F */  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* G */  {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    /* H */  {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* I */  {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* J */  {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* K */  {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* L */  {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* M */  {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    /* N */  {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    /* O */  {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* P */  {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* Q */  {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* R */  {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* S */  {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    /* T */  {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* U */  {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* V */  {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    /* W */  {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    /* X */  {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* Y */  {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},
    /* Z */  {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* [ */  {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    /* \ */  {0x00,0x10,0x08,0x04,0x02,0x01,0x00},
    /* ] */  {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    /* ^ */  {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    /* _ */  {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    /* ` */  {0x08,0x04,0x02,0x00,0x00,0x00,0x00},
    /* a */  {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    /* b */  {0x10,0x10,0x16,0x19,0x11,0x11,0x1E},
    /* c */  {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E},
    /* d */  {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F},
    /* e */  {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    /* f */  {0x06,0x09,0x08,0x1C,0x08,0x08,0x08},
    /* g */  {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E},
    /* h */  {0x10,0x10,0x16,0x19,0x11,0x11,0x11},
    /* i */  {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    /* j */  {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},
    /* k */  {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    /* l */  {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* m */  {0x00,0x00,0x1A,0x15,0x15,0x11,0x11},
    /* n */  {0x00,0x00,0x16,0x19,0x11,0x11,0x11},
    /* o */  {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    /* p */  {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
    /* q */  {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01},
    /* r */  {0x00,0x00,0x16,0x19,0x10,0x10,0x10},
    /* s */  {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E},
    /* t */  {0x08,0x08,0x1C,0x08,0x08,0x09,0x06},
    /* u */  {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},
    /* v */  {0x00,0x00,0x11,0x11,0x11,0x0A,0x04},
    /* w */  {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    /* x */  {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    /* y */  {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
    /* z */  {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
    /* { */  {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    /* | */  {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
    /* } */  {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    /* ~ */  {0x00,0x04,0x15,0x0E,0x00,0x00,0x00},
};

static int g_font_scale = 1;

/* Graphics drawText(text, x, y) */
static ClayValue gfx_draw_text(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_renderer || argc < 3) return CLAY_NIL;

    const char *text = NULL;
    if (CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0]))
        text = CLAY_OBJ(argv[0])->as.string.data;
    if (!text) return CLAY_NIL;

    int px = extract_int(argv[1]);
    int py = extract_int(argv[2]);
    int scale = g_font_scale;

    for (const char *p = text; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) ch = '?';
        const unsigned char *glyph = g_font_5x7[ch - 32];
        for (int row = 0; row < 7; row++) {
            unsigned char bits = glyph[row];
            for (int col = 0; col < 5; col++) {
                if (bits & (0x10 >> col)) {
                    if (scale == 1) {
                        SDL_RenderDrawPoint(g_renderer, px + col, py + row);
                    } else {
                        SDL_Rect r = { px + col * scale, py + row * scale, scale, scale };
                        SDL_RenderFillRect(g_renderer, &r);
                    }
                }
            }
        }
        px += 6 * scale;  /* 5 pixel width + 1 pixel gap */
    }
    return CLAY_VAL(self);
}

/* Graphics fontSize(scale) */
static ClayValue gfx_font_size(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc >= 1) {
        int s = extract_int(argv[0]);
        if (s >= 1 && s <= 16) g_font_scale = s;
    }
    return clay_int(g_font_scale);
}

/* ------------------------------------------------------------------ */
/*  Init / Shutdown                                                    */
/* ------------------------------------------------------------------ */

void clay_graphics_init(ClayObject *proto) {
    /* Window management */
    clay_set_method(proto, "window", gfx_window);
    clay_set_method(proto, "close", gfx_close);
    clay_set_method(proto, "width", gfx_width);
    clay_set_method(proto, "height", gfx_height);
    clay_set_method(proto, "title", gfx_title);
    clay_set_method(proto, "resize", gfx_resize);
    clay_set_method(proto, "fullscreen", gfx_fullscreen);
    clay_set_method(proto, "windowed", gfx_windowed);

    /* Drawing */
    clay_set_method(proto, "clear", gfx_clear);
    clay_set_method(proto, "setPixel", gfx_set_pixel);
    clay_set_method(proto, "color", gfx_color);
    clay_set_method(proto, "drawLine", gfx_draw_line);
    clay_set_method(proto, "drawRect", gfx_draw_rect);
    clay_set_method(proto, "fillRect", gfx_fill_rect);
    clay_set_method(proto, "drawCircle", gfx_draw_circle);
    clay_set_method(proto, "fillCircle", gfx_fill_circle);
    clay_set_method(proto, "refresh", gfx_refresh);

    /* Image / PPM */
    clay_set_method(proto, "loadPPM", gfx_load_ppm);
    clay_set_method(proto, "drawImage", gfx_draw_image);
    clay_set_method(proto, "imageWidth", gfx_image_width);
    clay_set_method(proto, "imageHeight", gfx_image_height);

    /* Pixel buffer (fast bulk writes) */
    clay_set_method(proto, "createBuffer", gfx_create_buffer);
    clay_set_method(proto, "bufferPixel", gfx_buffer_pixel);
    clay_set_method(proto, "drawBuffer", gfx_draw_buffer);

    /* Events / Input */
    clay_set_method(proto, "pollEvent", gfx_poll_event);
    clay_set_method(proto, "waitEvent", gfx_wait_event);
    clay_set_method(proto, "isKeyDown", gfx_is_key_down);
    clay_set_method(proto, "mouseX", gfx_mouse_x);
    clay_set_method(proto, "mouseY", gfx_mouse_y);
    clay_set_method(proto, "mouseDown", gfx_mouse_down);

    /* Timing */
    clay_set_method(proto, "ticks", gfx_ticks);
    clay_set_method(proto, "delay", gfx_delay);
    clay_set_method(proto, "fps", gfx_fps);

    /* Sprites + transforms */
    clay_set_method(proto, "createSprite", gfx_create_sprite);
    clay_set_method(proto, "drawSprite", gfx_draw_sprite);
    clay_set_method(proto, "drawSpriteEx", gfx_draw_sprite_ex);
    clay_set_method(proto, "drawImageEx", gfx_draw_image_ex);

    /* Text */
    clay_set_method(proto, "drawText", gfx_draw_text);
    clay_set_method(proto, "fontSize", gfx_font_size);
}

void clay_graphics_shutdown(void) {
    free(g_pixel_buf);
    g_pixel_buf = NULL;
    if (g_pixel_tex) { SDL_DestroyTexture(g_pixel_tex); g_pixel_tex = NULL; }

    g_sprite_count = 0;

    for (int i = 0; i < g_texture_count; i++) {
        if (g_textures[i]) { SDL_DestroyTexture(g_textures[i]); g_textures[i] = NULL; }
    }
    g_texture_count = 0;

    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
    if (g_window)   { SDL_DestroyWindow(g_window);     g_window = NULL; }
    if (g_sdl_initialized) { SDL_Quit(); g_sdl_initialized = 0; }
}

#else /* !CLAY_HAS_SDL2 */

/* Stub when SDL2 is not available */

static ClayValue gfx_not_available(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    clay_raise(CLAY_VAL(clay_error_new(
        "Graphics not available — rebuild with SDL2 (cmake -DCMAKE_TOOLCHAIN_FILE=...vcpkg...)",
        "GraphicsError")));
    return CLAY_NIL;
}

void clay_graphics_init(ClayObject *proto) {
    clay_set_method(proto, "window", gfx_not_available);
    clay_set_method(proto, "loadPPM", gfx_not_available);
}

void clay_graphics_shutdown(void) {}

#endif /* CLAY_HAS_SDL2 */
