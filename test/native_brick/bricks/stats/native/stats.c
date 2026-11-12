/* stats.c - a native brick test fixture: exercises plugin API version 2
 * (arrays and float buffers in and out, native data with a free function,
 * errors) and a brick cflag (STATS_SCALE). */
#include "clay_plugin.h"
#include <stdlib.h>
#include <string.h>
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

/* An accumulator object holding a C struct */
typedef struct { double total; long count; } Acc;

static void acc_free(void *data) { free(data); }

static ClayPluginValue acc_new(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;
    ClayPluginObject obj = api->clone(self);
    api->set_data(obj, calloc(1, sizeof(Acc)));
    return (ClayPluginValue)obj;
}

static ClayPluginValue acc_add(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    Acc *a = (Acc *)api->get_data(self);
    if (!a) { api->raise("use StatsAcc new", "RuntimeError"); return 0; }
    for (int i = 0; i < argc; i++) { a->total += api->to_float(argv[i]); a->count++; }
    return (ClayPluginValue)self;
}

static ClayPluginValue acc_mean(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)argc; (void)argv;
    Acc *a = (Acc *)api->get_data(self);
    return api->make_float(a && a->count ? (float)(a->total / (double)a->count) : 0.0f);
}

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
    { "reverseBytes", stats_reverse_bytes }, { "byteCount", stats_byte_count }, { "counting", stats_counting }, { NULL, NULL }
};
static ClayPluginMethod acc_methods[] = {
    { "new", acc_new }, { "add", acc_add }, { "mean", acc_mean }, { NULL, NULL }
};

CLAY_PLUGIN_EXPORT void stats_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType t1 = { "StatsNative", stats_methods, NULL, 0 };
    api->register_type(&t1);
    ClayPluginType t2 = { "StatsAcc", acc_methods, acc_free, sizeof(Acc) };
    api->register_type(&t2);
}
