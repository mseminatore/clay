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
 *   CLAY_PLUGIN_EXPORT void clay_plugin_init(const ClayPluginAPI *clay_api) {
 *       api = clay_api;
 *       ClayPluginType def = { "MyMath", methods, NULL, 0 };
 *       api->register_type(&def);
 *   }
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
} ClayPluginAPI;

/* Plugin entry point signature */
typedef void (*ClayPluginInitFunc)(const ClayPluginAPI *api);

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
