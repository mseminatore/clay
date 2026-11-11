/*
 * native_bricks.c - bricks with C sources (brick.json "native")
 *
 * A native brick's sources are compiled into a plugin with the compiler of
 * this clay build (clay-native.cfg) into <brick>/build/<plugin>.<ext>, and
 * rebuilt when a source is newer than it or the stamp beside it (Clay
 * version, compiler, flags) changes. Importing the brick loads the plugin;
 * `clay --compile` links the sources into the executable instead and calls
 * <plugin>_plugin_init from main().
 */
#include "native_bricks.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

int clay_plugin_load_file(const char *path);   /* runtime: plugin.c */

#define MAX_NATIVE_BRICKS 32

typedef struct {
    char dir[BRICK_MAX_PATH];
    BrickNative native;
} NativeBrick;

static NativeBrick g_bricks[MAX_NATIVE_BRICKS];
static int g_brick_count = 0;
static int g_loaded = 0;     /* bricks already loaded (the REPL loads more as imports arrive) */

void native_bricks_note(const char *brick_dir, const BrickNative *native) {
    for (int i = 0; i < g_brick_count; i++)
        if (strcmp(g_bricks[i].native.plugin, native->plugin) == 0) return;
    if (g_brick_count >= MAX_NATIVE_BRICKS) return;
    snprintf(g_bricks[g_brick_count].dir, BRICK_MAX_PATH, "%s", brick_dir);
    g_bricks[g_brick_count].native = *native;
    g_brick_count++;
}

int native_bricks_count(void) { return g_brick_count; }
const char *native_bricks_plugin(int i) { return g_bricks[i].native.plugin; }

static int mtime_of(const char *path, long long *out) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *out = (long long)st.st_mtime;
    return 0;
}

/* What the plugin was built with: rebuilt when this changes */
static void stamp_text(const NativeBrick *b, const NativeToolchain *tc, char *out, size_t sz) {
    size_t n = (size_t)snprintf(out, sz, "clay %s\ncc %s\ncflags %s\n", CLAY_VERSION, tc->cc, tc->cflags);
    for (int i = 0; i < b->native.cflag_count && n < sz; i++)
        n += (size_t)snprintf(out + n, sz - n, "%s ", b->native.cflags[i]);
    for (int i = 0; i < b->native.lib_count && n < sz; i++)
        n += (size_t)snprintf(out + n, sz - n, "%s ", b->native.libs[i]);
}

static int read_file_into(const char *path, char *buf, size_t sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, sz - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 0;
}

static int build_plugin(const NativeBrick *b, const NativeToolchain *tc,
                        const char *lib, const char *stamp_path, const char *stamp) {
    char build_dir[BRICK_MAX_PATH + 16];
    snprintf(build_dir, sizeof(build_dir), "%s/build", b->dir);
#ifdef _WIN32
    _mkdir(build_dir);
#else
    mkdir(build_dir, 0755);
#endif
    /* build to a temporary name, then rename: parallel builds never see a
     * half-written library */
    char tmp[BRICK_MAX_PATH * 2 + 32];
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", lib, (int)getpid());

    static char cmd[32768];
    size_t n;
    if (tc->msvc) {
        n = (size_t)snprintf(cmd, sizeof(cmd),
            "cmd /c \"\"%s\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x64 >nul 2>&1 && "
            "\"%s\" /nologo /LD /O2 %s ", tc->vs_path ? tc->vs_path : "", tc->cc, tc->cflags);
    } else {
        n = (size_t)snprintf(cmd, sizeof(cmd), "\"%s\" %s -O2 -fPIC -shared -w ", tc->cc, tc->cflags);
    }
    n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, tc->msvc ? "/I\"%s/src/runtime\" " : "-I\"%s/src/runtime\" ",
                          tc->clay_dir);
    for (int i = 0; i < b->native.cflag_count; i++) {
        char fl[BRICK_MAX_PATH * 2];
        brick_expand_flag(fl, sizeof(fl), b->native.cflags[i], b->dir);
        n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, "%s ", fl);
    }
    for (int i = 0; i < b->native.source_count; i++)
        n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, "\"%s/%s\" ", b->dir, b->native.sources[i]);
    if (tc->msvc) {
        n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, "/Fe:\"%s\" /Fo\"%s/\" /link", tmp, build_dir);
    } else {
        n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, "-o \"%s\"", tmp);
    }
    for (int i = 0; i < b->native.lib_count; i++) {
        char fl[BRICK_MAX_PATH * 2];
        brick_expand_flag(fl, sizeof(fl), b->native.libs[i], b->dir);
        n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, " %s", fl);
    }
    if (tc->msvc) n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, "\"");
    if (n >= sizeof(cmd)) {
        fprintf(stderr, "clay: native brick '%s': build command too long\n", b->native.plugin);
        return -1;
    }

    fprintf(stderr, "clay: building native brick '%s'...\n", b->native.plugin);
    fflush(stdout);
    fflush(stderr);
    if (system(cmd) != 0) {
        fprintf(stderr, "clay: native brick '%s' failed to build:\n  %s\n", b->native.plugin, cmd);
        remove(tmp);
        return -1;
    }
    remove(lib);                       /* Windows rename won't replace */
    if (rename(tmp, lib) != 0) {
        /* another process just finished the same build: use theirs */
        remove(tmp);
        struct stat st;
        if (stat(lib, &st) != 0) return -1;
    }
    FILE *f = fopen(stamp_path, "wb");
    if (f) { fputs(stamp, f); fclose(f); }
    return 0;
}

int native_bricks_load(const NativeToolchain *tc) {
#if defined(_WIN32)
    const char *ext = ".dll";
#elif defined(__APPLE__)
    const char *ext = ".dylib";
#else
    const char *ext = ".so";
#endif
    for (int i = g_loaded; i < g_brick_count; i++) {
        const NativeBrick *b = &g_bricks[i];
        char lib[BRICK_MAX_PATH * 2], stamp_path[BRICK_MAX_PATH * 2];
        snprintf(lib, sizeof(lib), "%s/build/%s%s", b->dir, b->native.plugin, ext);
        snprintf(stamp_path, sizeof(stamp_path), "%s/build/%s.stamp", b->dir, b->native.plugin);
        static char stamp[8192], old[8192];
        stamp_text(b, tc, stamp, sizeof(stamp));

        int stale = 0;
        long long lib_time = 0;
        if (mtime_of(lib, &lib_time) != 0) stale = 1;
        if (!stale && (read_file_into(stamp_path, old, sizeof(old)) != 0 || strcmp(old, stamp) != 0))
            stale = 1;
        for (int s = 0; !stale && s < b->native.source_count; s++) {
            char src[BRICK_MAX_PATH * 2];
            long long t;
            snprintf(src, sizeof(src), "%s/%s", b->dir, b->native.sources[s]);
            if (mtime_of(src, &t) != 0) {
                fprintf(stderr, "clay: native brick '%s': missing source %s\n", b->native.plugin, src);
                return -1;
            }
            if (t > lib_time) stale = 1;
        }
        if (stale && build_plugin(b, tc, lib, stamp_path, stamp) != 0) return -1;
        if (!clay_plugin_load_file(lib)) {
            fprintf(stderr, "clay: cannot load native brick '%s' (%s): it needs a %s_plugin_init "
                            "entry point\n", b->native.plugin, lib, b->native.plugin);
            return -1;
        }
        g_loaded = i + 1;
    }
    return 0;
}

void native_bricks_compile_args(char *buf, size_t sz, int msvc) {
    size_t n = strlen(buf);
    for (int i = 0; i < g_brick_count && n < sz; i++) {
        const NativeBrick *b = &g_bricks[i];
        for (int f = 0; f < b->native.cflag_count && n < sz; f++) {
            char fl[BRICK_MAX_PATH * 2];
            brick_expand_flag(fl, sizeof(fl), b->native.cflags[f], b->dir);
            n += (size_t)snprintf(buf + n, sz - n, " %s", fl);
        }
        for (int s = 0; s < b->native.source_count && n < sz; s++)
            n += (size_t)snprintf(buf + n, sz - n, " \"%s/%s\"", b->dir, b->native.sources[s]);
    }
    (void)msvc;
}

void native_bricks_link_args(char *buf, size_t sz, int msvc) {
    size_t n = strlen(buf);
    for (int i = 0; i < g_brick_count && n < sz; i++)
        for (int l = 0; l < g_bricks[i].native.lib_count && n < sz; l++) {
            char fl[BRICK_MAX_PATH * 2];
            brick_expand_flag(fl, sizeof(fl), g_bricks[i].native.libs[l], g_bricks[i].dir);
            /* skip one the command already has (e.g. -lm), which some
             * linkers warn about */
            char needle[BRICK_MAX_PATH * 2 + 4];
            snprintf(needle, sizeof(needle), " %s ", fl);
            char tail[BRICK_MAX_PATH * 2 + 4];
            snprintf(tail, sizeof(tail), " %s", fl);
            size_t tl = strlen(tail);
            if (strstr(buf, needle) || (n >= tl && strcmp(buf + n - tl, tail) == 0)) continue;
            n += (size_t)snprintf(buf + n, sz - n, " %s", fl);
        }
    (void)msvc;
}
