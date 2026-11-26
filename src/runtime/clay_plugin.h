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
 *   static ClayPluginValue my_sqrt(ClayPluginObject self, int argc, ClayPluginValue *argv) {
 *       CLAY_UNUSED(self);
 *       if (argc < 1) { api->raise("sqrt needs a number", "ArgumentError"); return CLAY_NIL; }
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
 * Type names: `x type` on a plugin object is the name of the prototype it was
 * cloned from (ClayPluginType.name, or the variable a Clay wrapper assigns it
 * to, as in `Image = ImageCore clone { ... }`).
 *
 * Ownership: make_* and the returned values of send/try_send/array_new/
 * from_floats/dict_new/make_bytes are new references. Return one from a
 * method, or release it. Arguments and get_slot/array_get/dict_get/dict_next
 * results are borrowed: retain them to keep them.
 *
 * Errors: raise, and any error inside send, to_floats or another call into
 * Clay, unwinds with longjmp straight past the plugin's C frames, so the code
 * after the call doesn't run. Don't hold memory or a lock across such a call
 * that only that code would release; use try_send (version 6) to get control
 * back first.
 *
 * Full reference: docs/plugin-api.md.
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

/* nil, e.g. `return CLAY_NIL;` after api->raise (raise does not return, but the
 * compiler needs a return value) or from a method with no result. It is the
 * same value as api->make_nil(). */
#ifndef CLAY_NIL
#define CLAY_NIL ((ClayPluginValue)0)
#endif

/* Mark a parameter a method does not use: CLAY_UNUSED(argc); */
#ifndef CLAY_UNUSED
#define CLAY_UNUSED(x) ((void)(x))
#endif

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
    void (*free_fn)(void *data);   /* Optional: runs on an object's native data when it is freed */
    size_t data_size;              /* Optional (works from API version 6): api->clone gives
                                    * each clone a zeroed buffer of this many bytes as its
                                    * native data. Clay frees the buffer after free_fn, so
                                    * free_fn must only release what the buffer points to. */
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

    /* ---- version 4 ---- */

    /* Binary data. A Clay String is a sequence of bytes of known length and may
     * hold NUL bytes, but make_string / to_string work with C strings and stop
     * at the first NUL. These carry the length:
     *   make_bytes copies len bytes into a new String (a new reference);
     *   to_bytes returns a String's bytes and sets *len to their count; the
     *     pointer is valid while v is alive (NUL-terminated, so it is also a C
     *     string when the data has no NUL). It returns NULL, with *len 0, if v
     *     is not a String.
     * Use them for anything that is not text: socket data, files, images. */
    ClayPluginValue (*make_bytes)(const void *data, size_t len);
    const char *    (*to_bytes)(ClayPluginValue v, size_t *len);

    /* ---- version 5 ---- */

    /* Dicts (string keys, kept in insertion order):
     *   dict_new returns a new, empty Dict (a new reference);
     *   dict_set stores v under key (retains v; the key is copied);
     *   dict_get returns the value under key, borrowed, or nil if missing;
     *   dict_has says whether key is present (a key can hold nil);
     *   dict_next walks the entries: start with *pos = 0, and each call
     *     sets *key and *v (both borrowed) and returns 1, or 0 at the end.
     * On a value that is not a Dict, dict_set does nothing, dict_get returns
     * nil, and dict_has, dict_size and dict_next return 0. */
    int             (*is_dict)(ClayPluginValue v);
    ClayPluginValue (*dict_new)(void);
    void            (*dict_set)(ClayPluginValue d, const char *key, ClayPluginValue v);
    ClayPluginValue (*dict_get)(ClayPluginValue d, const char *key);
    int             (*dict_has)(ClayPluginValue d, const char *key);
    size_t          (*dict_size)(ClayPluginValue d);
    int             (*dict_next)(ClayPluginValue d, size_t *pos,
                                 const char **key, ClayPluginValue *v);

    /* ---- version 6 ---- */

    /* Errors without losing control. try_send sends like send, but catches
     * what the method raises: it returns 1 and sets *out to the result, or
     * returns 0 and sets *out to the raised value (both new references).
     * After a 0, clean up, then either raise_value(*out) to let it go on
     * (it takes the reference) or release it to swallow the error. Swallow
     * only what is_error says is an error: break, continue and non-local
     * return travel the same way (a block passed along that runs `return`),
     * and swallowing one stops it halfway. */
    int             (*try_send)(ClayPluginValue recv, const char *sel, int argc,
                                ClayPluginValue *argv, ClayPluginValue *out);
    void            (*raise_value)(ClayPluginValue v);   /* does not return */
    int             (*is_error)(ClayPluginValue v);

    /* Also new in version 6: ClayPluginType.data_size allocates native data. */
} ClayPluginAPI;

#define CLAY_PLUGIN_API_VERSION 6

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
