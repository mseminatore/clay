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

typedef struct {
    char name[BRICK_MAX_NAME];
    char version[64];
    char description[256];
    char main_file[BRICK_MAX_PATH];
    BrickDep deps[BRICK_MAX_DEPS];
    int dep_count;
} BrickManifest;

/* Parse a brick.json file. Returns 0 on success, -1 on error. */
int brick_parse(const char *path, BrickManifest *out);

/* Write a brick.json file. Returns 0 on success. */
int brick_write(const char *path, const BrickManifest *manifest);

/* Run a bricks subcommand. Returns 0 on success. */
int brick_command(int argc, char **argv);

#endif
