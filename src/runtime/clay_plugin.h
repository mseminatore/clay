/*
 * clay_plugin.h — Public API for Clay language plugins
 *
 * Include this header in your plugin's C source. Compile as a shared library
 * (.dll / .so / .dylib) and place in the plugins/ directory next to the
 * clay executable, or load explicitly with System plugin("name").
 *
 * Example:
 *   #include "clay_plugin.h"
 *   #include <math.h>
 *
 *   static const ClayPluginAPI *api;
 *
 *   static ClayPluginValue my_sqrt(ClayPluginValue self, int argc, ClayPluginValue *argv) {
 *       (void)self;
 *       float val = api->to_float(argv[0]);
 *       return api->make_float(sqrtf(val));
 *   }
 *
 *   static ClayPluginMethod methods[] = {
 *       { "sqrt", my_sqrt },
 *       { NULL, NULL }
 *   };
 *
 *   CLAY_PLUGIN_EXPORT void mymath_plugin_init(const ClayPluginAPI *clay_api) {
 *       api = clay_api;
 *       ClayPluginType def = { "MyMath", methods, NULL, 0 };
 *       api->register_type(&def);
 *   }
 *
 * Entry point: name it <plugin>_plugin_init, where <plugin> is the library's
 * file name without its extension (mymath.so -> mymath_plugin_init), so that
 * several plugins can be linked into one native executable (native bricks).
 * clay_plugin_init is still accepted for plugins loaded from a shared library.
 *
 * API versions: fields are only ever added at the end of ClayPluginAPI, and
 * api_version says which are present. Check it before using version 2
 * fields: if (api->api_version >= 2) ... (version 1 had no api_version
 * field; Clay always passes a table of the current version).
 *
 * Ownership: make_* and the returned values of send/array_new/from_floats
 * are new references. Return one from a method, or release it. Arguments
 * and array_get results are borrowed: retain them to keep them.
 */
#ifndef CLAY_PLUGIN_H
#define CLAY_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle types — plugins don't need Clay internals */
typedef uintptr_t ClayPluginValue;
typedef void*     ClayPluginObject;

/* Method function signature */
typedef ClayPluginValue (*ClayPluginMethodFunc)(
    ClayPluginObject self, int argc, ClayPluginValue *argv);

/* Method descriptor */
typedef struct {
    const char          *name;
    ClayPluginMethodFunc func;
} ClayPluginMethod;

/* Type descriptor — register a new prototype */
typedef struct {
    const char        *name;       /* Prototype name ("Canvas", "MyMath") */
    ClayPluginMethod  *methods;    /* NULL-terminated method array */
    void (*free_fn)(void *data);   /* Optional: cleanup for native data */
    size_t data_size;              /* Optional: bytes for per-instance native data */
} ClayPluginType;

/* Plugin API vtable — passed to clay_plugin_init */
typedef struct ClayPluginAPI {
    /* Register a new prototype (becomes a global like File, Http, etc.) */
    ClayPluginObject (*register_type)(const ClayPluginType *def);

    /* Register additional methods on an existing prototype */
    void (*register_methods)(ClayPluginObject proto, const ClayPluginMethod *methods);

    /* Create Clay values from C types */
    ClayPluginValue (*make_int)(intptr_t n);
    ClayPluginValue (*make_float)(float f);
    ClayPluginValue (*make_string)(const char *s);
    ClayPluginValue (*make_bool)(int truthy);
    ClayPluginValue (*make_nil)(void);

    /* Extract C values from Clay values */
    intptr_t    (*to_int)(ClayPluginValue v);
    float       (*to_float)(ClayPluginValue v);
    const char* (*to_string)(ClayPluginValue v);
    int         (*is_int)(ClayPluginValue v);
    int         (*is_float)(ClayPluginValue v);
    int         (*is_string)(ClayPluginValue v);
    int         (*is_nil)(ClayPluginValue v);

    /* Object operations */
    ClayPluginObject (*clone)(ClayPluginObject proto);
    ClayPluginValue  (*send)(ClayPluginValue recv, const char *sel,
                             int argc, ClayPluginValue *argv);

    /* Per-object native data (for wrapping C structs) */
    void  (*set_data)(ClayPluginObject obj, void *data);
    void* (*get_data)(ClayPluginObject obj);

    /* Error handling */
    void (*raise)(const char *msg, const char *error_type);

    /* Slot access */
    void            (*set_slot)(ClayPluginObject obj, const char *name, ClayPluginValue val);
    ClayPluginValue (*get_slot)(ClayPluginObject obj, const char *name);

    /* ---- version 2 ---- */
    int api_version;                       /* CLAY_PLUGIN_API_VERSION */

    /* Keep a value beyond the call it was passed to (e.g. in native data) */
    void (*retain)(ClayPluginValue v);
    void (*release)(ClayPluginValue v);

    /* Arrays */
    int             (*is_array)(ClayPluginValue v);
    ClayPluginValue (*array_new)(void);
    size_t          (*array_size)(ClayPluginValue arr);
    ClayPluginValue (*array_get)(ClayPluginValue arr, size_t i);   /* borrowed; nil past the end */
    void            (*array_push)(ClayPluginValue arr, ClayPluginValue v);  /* retains v */

    /* Numbers in bulk: an Array of numbers <-> a float buffer.
     * to_floats flattens nested arrays (rows of a matrix) in order, writes at
     * most max values and returns how many numbers there are (so call it with
     * max = 0 to size the buffer); raises TypeError on a non-number.
     * from_floats makes an Array of n numbers, or of n / cols rows of cols
     * numbers when cols > 0. */
    size_t          (*to_floats)(ClayPluginValue arr, float *out, size_t max);
    ClayPluginValue (*from_floats)(const float *data, size_t n, size_t cols);

    /* ---- version 3 ---- */

    /* Write text where Clay's own output goes: stdout, or the debugger's
     * Debug Console under --dap. Use it instead of printf/fputs(stdout) for
     * output the user should see (progress, logs): C stdout is the debug
     * protocol channel when Clay runs as a debug adapter. */
    void            (*write_output)(const char *text);
} ClayPluginAPI;

#define CLAY_PLUGIN_API_VERSION 3

/* Plugin entry point signature */
typedef void (*ClayPluginInitFunc)(const ClayPluginAPI *api);

/* Runtime side (not for plugins): register a plugin linked into the
 * executable, as `clay --compile` does for native bricks */
void clay_plugin_register_static(ClayPluginInitFunc init_fn);

/* Export macro for cross-platform shared library exports */
#ifdef _WIN32
#   define CLAY_PLUGIN_EXPORT __declspec(dllexport)
#else
#   define CLAY_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif /* CLAY_PLUGIN_H */
