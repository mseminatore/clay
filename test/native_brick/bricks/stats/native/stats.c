/* stats.c - a native brick test fixture: exercises plugin API versions 2-5
 * (arrays and float buffers in and out, native data with a free function,
 * errors, binary strings, dicts) and a brick cflag (STATS_SCALE). */
#include "clay_plugin.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stats_extra.h>

#ifndef STATS_OS
#error "STATS_OS comes from the host platform's cflags-<platform> in brick.json"
#endif

#ifndef STATS_SCALE
#error "STATS_SCALE comes from brick.json cflags"
#endif

static const ClayPluginAPI *api;

/* StatsNative sum(array) / mean(array): numbers (nested arrays flatten) */
static ClayPluginValue stats_sum(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1 || !api->is_array(argv[0])) { api->raise("sum needs an Array", "ArgumentError"); return 0; }
    size_t n = api->to_floats(argv[0], NULL, 0);
    float *buf = (float *)malloc((n ? n : 1) * sizeof(float));
    api->to_floats(argv[0], buf, n);
    float total = 0;
    for (size_t i = 0; i < n; i++) total += buf[i];
    free(buf);
    return api->make_float(total);
}

/* StatsNative scale(array, cols): each number times STATS_SCALE, as rows of cols */
static ClayPluginValue stats_scale(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    size_t n = api->to_floats(argv[0], NULL, 0);
    float *buf = (float *)malloc((n ? n : 1) * sizeof(float));
    api->to_floats(argv[0], buf, n);
    for (size_t i = 0; i < n; i++) buf[i] *= STATS_SCALE;
    size_t cols = argc > 1 ? (size_t)api->to_int(argv[1]) : 0;
    ClayPluginValue out = api->from_floats(buf, n, cols);
    free(buf);
    return out;
}

/* StatsNative reverseBytes(s): the bytes backwards (API version 4: a string
 * with NUL bytes keeps its length) */
static ClayPluginValue stats_reverse_bytes(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    size_t n = 0;
    const char *in = argc > 0 ? api->to_bytes(argv[0], &n) : NULL;
    if (!in) { api->raise("reverseBytes needs a String", "ArgumentError"); return 0; }
    char *out = (char *)malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) out[i] = in[n - 1 - i];
    ClayPluginValue result = api->make_bytes(out, n);
    free(out);
    return result;
}

/* StatsNative byteCount(s): how many bytes the String has, NULs included */
static ClayPluginValue stats_byte_count(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    size_t n = 0;
    if (argc < 1 || !api->to_bytes(argv[0], &n)) { api->raise("byteCount needs a String", "ArgumentError"); return 0; }
    return api->make_int((intptr_t)n);
}

/* StatsNative counting(n): the bytes 0, 1, ... n-1 */
static ClayPluginValue stats_counting(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    size_t n = argc > 0 ? (size_t)api->to_int(argv[0]) : 0;
    unsigned char *out = (unsigned char *)malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) out[i] = (unsigned char)i;
    ClayPluginValue result = api->make_bytes(out, n);
    free(out);
    return result;
}

/* StatsNative summary(array): a new Dict {count, sum, name} (API version 5) */
static ClayPluginValue stats_summary(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);
    if (argc < 1 || !api->is_array(argv[0])) { api->raise("summary needs an Array", "ArgumentError"); return CLAY_NIL; }
    size_t n = api->to_floats(argv[0], NULL, 0);
    float *buf = (float *)malloc((n ? n : 1) * sizeof(float));
    api->to_floats(argv[0], buf, n);
    float total = 0;
    for (size_t i = 0; i < n; i++) total += buf[i];
    free(buf);
    ClayPluginValue d = api->dict_new();
    api->dict_set(d, "count", api->make_int((intptr_t)n));
    api->dict_set(d, "sum", api->make_float(total));
    ClayPluginValue name = api->make_string("stats");
    api->dict_set(d, "name", name);   /* dict_set retains, so drop ours */
    api->release(name);
    api->dict_set(d, "count", api->make_int((intptr_t)n * 10));  /* replace keeps its place */
    return d;
}

/* StatsNative dictKeys(d): the keys in order, read with dict_next */
static ClayPluginValue stats_dict_keys(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self);
    if (argc < 1 || !api->is_dict(argv[0])) { api->raise("dictKeys needs a Dict", "ArgumentError"); return CLAY_NIL; }
    ClayPluginValue keys = api->array_new();
    size_t pos = 0;
    const char *key;
    ClayPluginValue v;
    while (api->dict_next(argv[0], &pos, &key, &v)) {
        ClayPluginValue k = api->make_string(key);
        api->array_push(keys, k);
        api->release(k);
    }
    return keys;
}

/* StatsNative dictLookup(d, key): [size, has, value] -- also on a non-Dict */
static ClayPluginValue stats_dict_lookup(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self); CLAY_UNUSED(argc);
    const char *key = api->to_string(argv[1]);
    ClayPluginValue out = api->array_new();
    api->array_push(out, api->make_int((intptr_t)api->dict_size(argv[0])));
    api->array_push(out, api->make_bool(api->dict_has(argv[0], key)));
    api->array_push(out, api->dict_get(argv[0], key));   /* borrowed: push retains */
    api->dict_set(argv[0], "seen", api->make_bool(1));    /* no-op on a non-Dict */
    return out;
}

/* An accumulator object holding a C struct */
typedef struct { double total; long count; } Acc;

static void acc_free(void *data) { free(data); }

static ClayPluginValue acc_new(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);
    ClayPluginObject obj = api->clone(self);
    api->set_data(obj, calloc(1, sizeof(Acc)));
    return (ClayPluginValue)obj;
}

static ClayPluginValue acc_add(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Acc *a = (Acc *)api->get_data(self);
    if (!a) { api->raise("use StatsAcc new", "RuntimeError"); return CLAY_NIL; }
    for (int i = 0; i < argc; i++) { a->total += api->to_float(argv[i]); a->count++; }
    return (ClayPluginValue)self;
}

static ClayPluginValue acc_mean(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;
    Acc *a = (Acc *)api->get_data(self);
    return api->make_float(a && a->count ? (float)(a->total / (double)a->count) : 0.0f);
}

/* API version 6: a type with data_size -- Clay allocates (and frees) the
 * struct; free_fn releases only what the struct points to */
typedef struct { char *label; long hits; } Box;

static int g_box_freed = 0;   /* how many free_fn calls ran (StatsBox freed) */

static void box_free(void *data) {
    Box *b = (Box *)data;
    free(b->label);
    g_box_freed++;
}

static ClayPluginValue box_new(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    ClayPluginObject obj = api->clone(self);
    Box *b = (Box *)api->get_data(obj);          /* zeroed, owned by Clay */
    const char *s = argc > 0 ? api->to_string(argv[0]) : "";
    b->label = (char *)malloc(strlen(s) + 1);
    strcpy(b->label, s);
    return (ClayPluginValue)obj;
}

static ClayPluginValue box_hit(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);
    Box *b = (Box *)api->get_data(self);
    if (!b) { api->raise("use StatsBox new", "RuntimeError"); return CLAY_NIL; }
    b->hits++;
    return (ClayPluginValue)self;
}

static ClayPluginValue box_info(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);
    Box *b = (Box *)api->get_data(self);
    if (!b) return CLAY_NIL;
    ClayPluginValue out = api->array_new();
    ClayPluginValue l = api->make_string(b->label ? b->label : "");
    api->array_push(out, l);
    api->release(l);
    api->array_push(out, api->make_int(b->hits));
    return out;
}

static ClayPluginValue box_freed(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self); CLAY_UNUSED(argc); CLAY_UNUSED(argv);
    return api->make_int(g_box_freed);
}

/* StatsNative guarded(recv, sel): try_send with a buffer to clean up either
 * way; [true, result] or [false, the error's message] */
static int g_guard_freed = 0;
static ClayPluginValue stats_guarded(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self); CLAY_UNUSED(argc);
    char *scratch = (char *)malloc(64);
    ClayPluginValue r;
    int ok = api->try_send(argv[0], api->to_string(argv[1]), 0, NULL, &r);
    free(scratch);
    g_guard_freed++;
    if (!ok) {
        if (!api->is_error(r)) api->raise_value(r);   /* break / return: let it go on */
        ClayPluginValue msg = api->send(r, "message", 0, NULL);
        api->release(r);
        r = msg;
    }
    ClayPluginValue out = api->array_new();
    api->array_push(out, api->make_bool(ok));
    api->array_push(out, r);
    api->release(r);
    return out;
}

/* StatsNative rethrow(block): run block value, clean up, raise again */
static ClayPluginValue stats_rethrow(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self); CLAY_UNUSED(argc);
    char *scratch = (char *)malloc(64);
    ClayPluginValue r;
    int ok = api->try_send(argv[0], "value", 0, NULL, &r);
    free(scratch);
    g_guard_freed++;
    if (!ok) api->raise_value(r);
    return r;
}

static ClayPluginValue stats_guard_freed(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self); CLAY_UNUSED(argc); CLAY_UNUSED(argv);
    return api->make_int(g_guard_freed);
}

/* StatsNative toInt(v): api->to_int on any value */
static ClayPluginValue stats_to_int(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(self); CLAY_UNUSED(argc);
    return api->make_int(api->to_int(argv[0]));
}

/* StatsMany<i> index -> i: many registered types (there used to be room for 64) */
static ClayPluginValue many_index(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    CLAY_UNUSED(argc); CLAY_UNUSED(argv);
    return api->get_slot(self, "_index");
}
static ClayPluginMethod many_methods[] = { { "index", many_index }, { NULL, NULL } };

static ClayPluginValue stats_version(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self; (void)argc; (void)argv;
    return api->make_int(api->api_version);
}

/* the cflags-<platform> entry of the host (1 macos, 2 linux, 3 windows) and the -I@/ header */
static ClayPluginValue stats_os(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self; (void)argc; (void)argv;
    return api->make_int(STATS_OS);
}
static ClayPluginValue stats_extra(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self; (void)argc; (void)argv;
    return api->make_int(STATS_EXTRA);
}

static ClayPluginMethod stats_methods[] = {
    { "os", stats_os }, { "extra", stats_extra }, { "sum", stats_sum }, { "scale", stats_scale }, { "apiVersion", stats_version },
    { "reverseBytes", stats_reverse_bytes }, { "byteCount", stats_byte_count }, { "counting", stats_counting },
    { "summary", stats_summary }, { "dictKeys", stats_dict_keys }, { "dictLookup", stats_dict_lookup },
    { "guarded", stats_guarded }, { "rethrow", stats_rethrow }, { "guardFreed", stats_guard_freed },
    { "toInt", stats_to_int }, { NULL, NULL }
};
static ClayPluginMethod box_methods[] = {
    { "new", box_new }, { "hit", box_hit }, { "info", box_info }, { "freed", box_freed }, { NULL, NULL }
};
static ClayPluginMethod acc_methods[] = {
    { "new", acc_new }, { "add", acc_add }, { "mean", acc_mean }, { NULL, NULL }
};

CLAY_PLUGIN_EXPORT void stats_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType t1 = { "StatsNative", stats_methods, NULL, 0 };
    api->register_type(&t1);
    /* written before data_size worked: its own struct, set right after clone */
    ClayPluginType t2 = { "StatsAcc", acc_methods, acc_free, sizeof(Acc) };
    api->register_type(&t2);
    ClayPluginType t3 = { "StatsBox", box_methods, box_free, sizeof(Box) };
    api->register_type(&t3);
    for (int i = 0; i < 70; i++) {
        char name[32];
        snprintf(name, sizeof(name), "StatsMany%d", i);
        ClayPluginType t = { name, many_methods, NULL, 0 };
        ClayPluginObject proto = api->register_type(&t);
        api->set_slot(proto, "_index", api->make_int(i));
    }
}
