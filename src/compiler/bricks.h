/*
 * bricks.h - Clay package manager (Bricks)
 */
#ifndef BRICKS_H
#define BRICKS_H

#define BRICK_MAX_DEPS 64
#define BRICK_MAX_NAME 128
#define BRICK_MAX_PATH 1024

typedef struct {
    char name[BRICK_MAX_NAME];
    char url[BRICK_MAX_PATH];   /* e.g. "github.com/user/lib@v1.0" */
} BrickDep;

#define BRICK_MAX_NATIVE 32     /* sources, cflags or libs of a native brick */
#define BRICK_MAX_FLAG   128

/* "native": { "plugin": "ann", "sources": [...], "cflags": [...], "libs": [...] }
 * C sources (relative to the brick) built into a plugin: a shared library
 * for the VM, linked statically into `clay --compile` executables. */
typedef struct {
    char plugin[BRICK_MAX_NAME];        /* "" = not a native brick */
    char sources[BRICK_MAX_NATIVE][BRICK_MAX_PATH];
    int  source_count;
    char cflags[BRICK_MAX_NATIVE][BRICK_MAX_FLAG];
    int  cflag_count;
    char libs[BRICK_MAX_NATIVE][BRICK_MAX_FLAG];
    int  lib_count;
} BrickNative;

typedef struct {
    char name[BRICK_MAX_NAME];
    char version[64];
    char description[256];
    char main_file[BRICK_MAX_PATH];
    BrickDep deps[BRICK_MAX_DEPS];
    int dep_count;
    BrickNative native;
} BrickManifest;

/* A compiler flag a brick may ask for: starts with '-' and holds only
 * letters, digits and . _ = + , / - (no spaces, quotes or shell syntax) */
int brick_safe_flag(const char *flag);

/* Parse a brick.json file. Returns 0 on success, -1 on error. */
int brick_parse(const char *path, BrickManifest *out);

/* Write a brick.json file. Returns 0 on success. */
int brick_write(const char *path, const BrickManifest *manifest);

/* Run a bricks subcommand. Returns 0 on success. */
int brick_command(int argc, char **argv);

#endif
