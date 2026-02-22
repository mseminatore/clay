/*
 * ffi.c - Foreign Function Interface for Clay
 *
 * Loads shared libraries and calls exported C functions that follow
 * the Clay method signature: ClayValue fn(ClayObject *self, int argc, ClayValue *argv)
 *
 * This "plugin" approach avoids complex type marshalling — plugin authors
 * use the Clay C API directly to work with ClayValues.
 */
#include "clay.h"
#include "ffi.h"
#include <string.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef HMODULE lib_handle_t;
#define LIB_OPEN(path) LoadLibraryA(path)
#define LIB_SYM(h, name) ((void *)GetProcAddress(h, name))
#define LIB_CLOSE(h) FreeLibrary(h)
#define LIB_ERROR() "LoadLibrary failed"
#else
#include <dlfcn.h>
typedef void *lib_handle_t;
#define LIB_OPEN(path) dlopen(path, RTLD_NOW)
#define LIB_SYM(h, name) dlsym(h, name)
#define LIB_CLOSE(h) dlclose(h)
#define LIB_ERROR() dlerror()
#endif

/*
 * FFI load(path) — load a shared library, return a handle object.
 * The handle stores the library pointer in a slot for later use.
 */
static ClayValue ffi_load(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) {
        clay_raise(CLAY_VAL(clay_error_new(
            "FFI load requires a library path string", "ArgumentError")));
        return CLAY_NIL;
    }
    const char *path = clay_string_data(CLAY_OBJ(argv[0]));
    lib_handle_t handle = LIB_OPEN(path);
    if (!handle) {
        char buf[512];
        snprintf(buf, sizeof(buf), "FFI: cannot load library '%s': %s",
                 path, LIB_ERROR());
        clay_raise(CLAY_VAL(clay_error_new(buf, "FFIError")));
        return CLAY_NIL;
    }

    /* Create a handle object that stores the native pointer */
    ClayObject *obj = clay_clone(self);
    /* Store handle as tagged integer (platform pointer fits in uintptr_t) */
    clay_set_slot(obj, "_handle", (ClayValue)(uintptr_t)handle);
    clay_set_slot(obj, "_path", CLAY_VAL(clay_string_new(path)));
    return CLAY_VAL(obj);
}

/*
 * handle call(funcName, args...) — look up and call a ClayMethodFunc from the library.
 * The function must have signature: ClayValue fn(ClayObject *self, int argc, ClayValue *argv)
 */
static ClayValue ffi_call(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) {
        clay_raise(CLAY_VAL(clay_error_new(
            "FFI call requires a function name string", "ArgumentError")));
        return CLAY_NIL;
    }

    /* Get the library handle from self */
    ClayValue handle_val = clay_get_slot(self, "_handle");
    if (CLAY_IS_NIL(handle_val)) {
        clay_raise(CLAY_VAL(clay_error_new(
            "FFI call: not a loaded library handle", "FFIError")));
        return CLAY_NIL;
    }
    lib_handle_t handle = (lib_handle_t)(uintptr_t)handle_val;

    const char *func_name = clay_string_data(CLAY_OBJ(argv[0]));
    void *sym = LIB_SYM(handle, func_name);
    if (!sym) {
        char buf[256];
        snprintf(buf, sizeof(buf), "FFI: symbol '%s' not found", func_name);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FFIError")));
        return CLAY_NIL;
    }

    /* Cast to ClayMethodFunc and call with remaining args */
    ClayMethodFunc fn = (ClayMethodFunc)sym;
    int fn_argc = argc - 1;
    ClayValue *fn_argv = fn_argc > 0 ? &argv[1] : NULL;
    return fn(self, fn_argc, fn_argv);
}

/*
 * handle close() — close a loaded library.
 */
static ClayValue ffi_close(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue handle_val = clay_get_slot(self, "_handle");
    if (!CLAY_IS_NIL(handle_val)) {
        lib_handle_t handle = (lib_handle_t)(uintptr_t)handle_val;
        LIB_CLOSE(handle);
        clay_set_slot(self, "_handle", CLAY_NIL);
    }
    return CLAY_TRUE;
}

void clay_ffi_init(ClayObject *proto) {
    clay_set_method(proto, "load", ffi_load);
    clay_set_method(proto, "call", ffi_call);
    clay_set_method(proto, "close", ffi_close);
}
