/*
 * plugin.c — Plugin registration engine for Clay
 *
 * Implements the ClayPluginAPI vtable and plugin loading.
 * Plugins register types and methods that appear as natural Clay objects.
 */
#include "clay.h"
#include "clay_plugin.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
#include <dirent.h>
typedef void *lib_handle_t;
#define LIB_OPEN(path) dlopen(path, RTLD_NOW)
#define LIB_SYM(h, name) dlsym(h, name)
#define LIB_CLOSE(h) dlclose(h)
#define LIB_ERROR() dlerror()
#endif

/* ------------------------------------------------------------------ */
/*  Plugin global registry                                             */
/* ------------------------------------------------------------------ */

#define MAX_PLUGIN_GLOBALS 64

static struct {
    const char  *name;
    ClayObject  *proto;
} g_plugin_globals[MAX_PLUGIN_GLOBALS];
static int g_plugin_global_count = 0;

/* Loaded plugin handles for cleanup */
#define MAX_PLUGIN_HANDLES 32
static lib_handle_t g_plugin_handles[MAX_PLUGIN_HANDLES];
static int g_plugin_handle_count = 0;

int clay_plugin_is_global(const char *name) {
    for (int i = 0; i < g_plugin_global_count; i++) {
        if (strcmp(g_plugin_globals[i].name, name) == 0)
            return 1;
    }
    return 0;
}

ClayObject *clay_plugin_get_global(const char *name) {
    for (int i = 0; i < g_plugin_global_count; i++) {
        if (strcmp(g_plugin_globals[i].name, name) == 0)
            return g_plugin_globals[i].proto;
    }
    return NULL;
}

int clay_plugin_global_count(void) {
    return g_plugin_global_count;
}

const char *clay_plugin_global_name(int idx) {
    if (idx < 0 || idx >= g_plugin_global_count) return NULL;
    return g_plugin_globals[idx].name;
}

ClayObject *clay_plugin_global_object(int idx) {
    if (idx < 0 || idx >= g_plugin_global_count) return NULL;
    return g_plugin_globals[idx].proto;
}

static void register_plugin_global(const char *name, ClayObject *proto) {
    if (g_plugin_global_count >= MAX_PLUGIN_GLOBALS) return;
    g_plugin_globals[g_plugin_global_count].name = strdup(name);
    g_plugin_globals[g_plugin_global_count].proto = proto;
    g_plugin_global_count++;
}

/* ------------------------------------------------------------------ */
/*  API implementation functions                                       */
/* ------------------------------------------------------------------ */

/* Adapter: ClayPluginMethodFunc -> ClayMethodFunc
 * Plugin methods use (ClayPluginObject self, ...) which is (void* self, ...)
 * Clay methods use (ClayObject *self, ...). These are compatible since
 * ClayPluginObject is void* and ClayObject* is also a pointer. */

static ClayPluginObject api_register_type(const ClayPluginType *def) {
    ClayObject *proto = clay_object_new();
    proto->parent = clay_Object;
    proto->refcount = 1000000; /* singleton */
    clay_set_slot(proto, "_name", CLAY_VAL(clay_string_new(def->name)));

    /* Register methods */
    if (def->methods) {
        for (int i = 0; def->methods[i].name != NULL; i++) {
            clay_set_method(proto, def->methods[i].name,
                (ClayMethodFunc)def->methods[i].func);
        }
    }

    /* Store free_fn and data_size on the prototype for use by clones */
    if (def->free_fn || def->data_size > 0) {
        proto->type = CLAY_TYPE_PLUGIN;
        proto->as.plugin.data = NULL;
        proto->as.plugin.free_fn = def->free_fn;
    }

    /* Register as a global */
    register_plugin_global(def->name, proto);

    return (ClayPluginObject)proto;
}

static void api_register_methods(ClayPluginObject obj, const ClayPluginMethod *methods) {
    ClayObject *proto = (ClayObject *)obj;
    for (int i = 0; methods[i].name != NULL; i++) {
        clay_set_method(proto, methods[i].name,
            (ClayMethodFunc)methods[i].func);
    }
}

static ClayPluginValue api_make_int(intptr_t n) {
    return (ClayPluginValue)clay_int(n);
}

static ClayPluginValue api_make_float(float f) {
    return (ClayPluginValue)clay_float_tag(f);
}

static ClayPluginValue api_make_string(const char *s) {
    return (ClayPluginValue)CLAY_VAL(clay_string_new(s));
}

static ClayPluginValue api_make_bool(int truthy) {
    return truthy ? (ClayPluginValue)CLAY_TRUE : (ClayPluginValue)CLAY_FALSE;
}

static ClayPluginValue api_make_nil(void) {
    return (ClayPluginValue)CLAY_NIL;
}

static intptr_t api_to_int(ClayPluginValue v) {
    return clay_to_int((ClayValue)v);
}

static float api_to_float(ClayPluginValue v) {
    return clay_to_float((ClayValue)v);
}

static const char *api_to_string(ClayPluginValue v) {
    ClayValue cv = (ClayValue)v;
    if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv))
        return clay_string_data(CLAY_OBJ(cv));
    return "";
}

static int api_is_int(ClayPluginValue v) {
    return CLAY_IS_INT((ClayValue)v);
}

static int api_is_float(ClayPluginValue v) {
    return CLAY_IS_FLOAT((ClayValue)v);
}

static int api_is_string(ClayPluginValue v) {
    ClayValue cv = (ClayValue)v;
    return CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && CLAY_OBJ(cv)->type == CLAY_TYPE_STRING;
}

static int api_is_nil(ClayPluginValue v) {
    return CLAY_IS_NIL((ClayValue)v);
}

static ClayPluginObject api_clone(ClayPluginObject proto) {
    ClayObject *p = (ClayObject *)proto;
    ClayObject *obj = clay_clone(p);

    /* For plugin types, allocate native data if prototype specifies data_size */
    if (p->type == CLAY_TYPE_PLUGIN && p->as.plugin.free_fn) {
        obj->as.plugin.free_fn = p->as.plugin.free_fn;
    }

    return (ClayPluginObject)obj;
}

static ClayPluginValue api_send(ClayPluginValue recv, const char *sel,
                                int argc, ClayPluginValue *argv) {
    return (ClayPluginValue)clay_send_val((ClayValue)recv, sel, argc, (ClayValue *)argv);
}

static void api_set_data(ClayPluginObject obj, void *data) {
    ClayObject *o = (ClayObject *)obj;
    o->type = CLAY_TYPE_PLUGIN;
    o->as.plugin.data = data;
}

static void *api_get_data(ClayPluginObject obj) {
    ClayObject *o = (ClayObject *)obj;
    if (o->type == CLAY_TYPE_PLUGIN)
        return o->as.plugin.data;
    return NULL;
}

static void api_raise(const char *msg, const char *error_type) {
    clay_raise(CLAY_VAL(clay_error_new(msg, error_type)));
}

static void api_set_slot(ClayPluginObject obj, const char *name, ClayPluginValue val) {
    clay_set_slot((ClayObject *)obj, name, (ClayValue)val);
}

static ClayPluginValue api_get_slot(ClayPluginObject obj, const char *name) {
    return (ClayPluginValue)clay_get_slot((ClayObject *)obj, name);
}

/* The global API vtable — single instance */
static const ClayPluginAPI g_plugin_api = {
    .register_type    = api_register_type,
    .register_methods = api_register_methods,
    .make_int         = api_make_int,
    .make_float       = api_make_float,
    .make_string      = api_make_string,
    .make_bool        = api_make_bool,
    .make_nil         = api_make_nil,
    .to_int           = api_to_int,
    .to_float         = api_to_float,
    .to_string        = api_to_string,
    .is_int           = api_is_int,
    .is_float         = api_is_float,
    .is_string        = api_is_string,
    .is_nil           = api_is_nil,
    .clone            = api_clone,
    .send             = api_send,
    .set_data         = api_set_data,
    .get_data         = api_get_data,
    .raise            = api_raise,
    .set_slot         = api_set_slot,
    .get_slot         = api_get_slot,
};

/* ------------------------------------------------------------------ */
/*  Plugin loading                                                     */
/* ------------------------------------------------------------------ */

/* Load a plugin shared library and call its init function */
static int load_plugin_lib(const char *path) {
    lib_handle_t handle = LIB_OPEN(path);
    if (!handle) return 0;

    ClayPluginInitFunc init_fn = (ClayPluginInitFunc)LIB_SYM(handle, "clay_plugin_init");
    if (!init_fn) {
        LIB_CLOSE(handle);
        return 0;
    }

    /* Call plugin init — this registers types and methods */
    init_fn(&g_plugin_api);

    /* Store handle for cleanup */
    if (g_plugin_handle_count < MAX_PLUGIN_HANDLES) {
        g_plugin_handles[g_plugin_handle_count++] = handle;
    }
    return 1;
}

/* System plugin(name) — load a plugin by name from plugins/ directory */
static ClayValue system_plugin(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || CLAY_IS_NIL(argv[0])) {
        clay_raise(CLAY_VAL(clay_error_new(
            "System plugin requires a plugin name", "ArgumentError")));
        return CLAY_NIL;
    }

    const char *name = clay_string_data(CLAY_OBJ(argv[0]));

    /* Build paths to try: plugins/<name>.dll, plugins/<name>.so, etc. */
    char path[512];
    const char *exts[] = {
#ifdef _WIN32
        ".dll",
#elif defined(__APPLE__)
        ".dylib", ".so",
#else
        ".so",
#endif
        NULL
    };

    for (int i = 0; exts[i]; i++) {
        snprintf(path, sizeof(path), "plugins/%s%s", name, exts[i]);
        if (load_plugin_lib(path)) {
            /* Return the last registered prototype (the one just loaded) */
            if (g_plugin_global_count > 0)
                return CLAY_VAL(g_plugin_globals[g_plugin_global_count - 1].proto);
            return CLAY_TRUE;
        }
    }

    /* Try loading as an absolute/relative path */
    if (load_plugin_lib(name)) {
        if (g_plugin_global_count > 0)
            return CLAY_VAL(g_plugin_globals[g_plugin_global_count - 1].proto);
        return CLAY_TRUE;
    }

    char buf[512];
    snprintf(buf, sizeof(buf), "Plugin '%s' not found", name);
    clay_raise(CLAY_VAL(clay_error_new(buf, "PluginError")));
    return CLAY_NIL;
}

/* Get the plugin API (for internal use, e.g., testing) */
const ClayPluginAPI *clay_get_plugin_api(void) {
    return &g_plugin_api;
}

/* Register System plugin() method — called from bootstrap */
void clay_plugin_init_system(ClayObject *system_proto) {
    clay_set_method(system_proto, "plugin", system_plugin);
}

/* Auto-discover and load plugins from directory next to executable */
void clay_plugin_auto_load(const char *exe_path) {
    char dir[512];
    dir[0] = '\0';

    /* Extract directory from exe path */
    if (exe_path) {
        const char *last_sep = NULL;
        for (const char *p = exe_path; *p; p++) {
            if (*p == '/' || *p == '\\') last_sep = p;
        }
        if (last_sep) {
            size_t len = (size_t)(last_sep - exe_path + 1);
            if (len >= sizeof(dir)) len = sizeof(dir) - 1;
            memcpy(dir, exe_path, len);
            dir[len] = '\0';
        }
    }

    char plugins_dir[512];
    snprintf(plugins_dir, sizeof(plugins_dir), "%splugins", dir);

    const char *ext =
#ifdef _WIN32
        ".dll";
#elif defined(__APPLE__)
        ".dylib";
#else
        ".so";
#endif

    /* Try to enumerate files in plugins_dir */
#ifdef _WIN32
    {
        char pattern[512];
        snprintf(pattern, sizeof(pattern), "%s\\*%s", plugins_dir, ext);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do {
            char path[512];
            snprintf(path, sizeof(path), "%s\\%s", plugins_dir, fd.cFileName);
            load_plugin_lib(path);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#else
    {
        DIR *dp = opendir(plugins_dir);
        if (!dp) return;
        size_t ext_len = strlen(ext);
        struct dirent *ent;
        while ((ent = readdir(dp))) {
            size_t nlen = strlen(ent->d_name);
            if (nlen > ext_len && strcmp(ent->d_name + nlen - ext_len, ext) == 0) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s", plugins_dir, ent->d_name);
                load_plugin_lib(path);
            }
        }
        closedir(dp);
    }
#endif
}

/* Cleanup all loaded plugins */
void clay_plugin_shutdown(void) {
    for (int i = 0; i < g_plugin_handle_count; i++) {
        if (g_plugin_handles[i])
            LIB_CLOSE(g_plugin_handles[i]);
    }
    g_plugin_handle_count = 0;

    for (int i = 0; i < g_plugin_global_count; i++) {
        free((void *)g_plugin_globals[i].name);
    }
    g_plugin_global_count = 0;
}
