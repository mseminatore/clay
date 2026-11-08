/*
 * native_bricks.h - bricks with C sources (brick.json "native"): built into
 * a plugin on first use, loaded when imported, linked into --compile output
 */
#ifndef CLAY_NATIVE_BRICKS_H
#define CLAY_NATIVE_BRICKS_H

#include <stddef.h>
#include "bricks.h"

/* How to compile: from clay-native.cfg, or a plain `cc` without one */
typedef struct {
    int         msvc;
    const char *cc;
    const char *cflags;     /* the cfg's flags (include paths, -std, ...) */
    const char *clay_dir;   /* the Clay checkout (src/runtime has clay_plugin.h) */
    const char *vs_path;    /* MSVC: the Visual Studio install (for vcvarsall) */
} NativeToolchain;

/* The parser records each native brick it imports (dir = the brick's) */
void native_bricks_note(const char *brick_dir, const BrickNative *native);
int  native_bricks_count(void);
const char *native_bricks_plugin(int i);

/* Build (if stale) and load every recorded brick's plugin into this
 * process, so compilers and the VM see its prototypes. 0 on success;
 * prints the problem and returns -1 otherwise. */
int native_bricks_load(const NativeToolchain *tc);

/* For `clay --compile`: append the bricks' sources and cflags (compile
 * part) and libs (link part) to a command line, quoted for the shell */
void native_bricks_compile_args(char *buf, size_t sz, int msvc);
void native_bricks_link_args(char *buf, size_t sz, int msvc);

#endif /* CLAY_NATIVE_BRICKS_H */
