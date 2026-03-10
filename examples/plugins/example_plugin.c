/*
 * example_plugin.c — Example Clay plugin demonstrating the plugin API
 *
 * This plugin registers a "MathEx" prototype with extended math functions.
 *
 * Build (Windows):
 *   cl /LD /I src\runtime examples\plugins\example_plugin.c /Fe:plugins\mathex.dll
 *
 * Build (Unix):
 *   cc -shared -fPIC -I src/runtime examples/plugins/example_plugin.c -o plugins/mathex.so -lm
 *
 * Usage in Clay:
 *   System plugin("mathex")
 *   MathEx sqrt(16) println()    // 4.0
 *   MathEx pow(2, 10) println()  // 1024.0
 */
#include "clay_plugin.h"
#include <math.h>

static const ClayPluginAPI *api;

static ClayPluginValue mathex_sqrt(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("sqrt requires 1 argument", "ArgumentError"); return 0; }
    float val = api->to_float(argv[0]);
    return api->make_float(sqrtf(val));
}

static ClayPluginValue mathex_pow(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 2) { api->raise("pow requires 2 arguments", "ArgumentError"); return 0; }
    float base = api->to_float(argv[0]);
    float exp = api->to_float(argv[1]);
    return api->make_float(powf(base, exp));
}

static ClayPluginValue mathex_sin(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("sin requires 1 argument", "ArgumentError"); return 0; }
    return api->make_float(sinf(api->to_float(argv[0])));
}

static ClayPluginValue mathex_cos(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("cos requires 1 argument", "ArgumentError"); return 0; }
    return api->make_float(cosf(api->to_float(argv[0])));
}

static ClayPluginValue mathex_atan2(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 2) { api->raise("atan2 requires 2 arguments", "ArgumentError"); return 0; }
    return api->make_float(atan2f(api->to_float(argv[0]), api->to_float(argv[1])));
}

static ClayPluginValue mathex_lerp(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 3) { api->raise("lerp requires 3 arguments (a, b, t)", "ArgumentError"); return 0; }
    float a = api->to_float(argv[0]);
    float b = api->to_float(argv[1]);
    float t = api->to_float(argv[2]);
    return api->make_float(a + (b - a) * t);
}

static ClayPluginValue mathex_clamp(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 3) { api->raise("clamp requires 3 arguments (val, min, max)", "ArgumentError"); return 0; }
    float val = api->to_float(argv[0]);
    float lo  = api->to_float(argv[1]);
    float hi  = api->to_float(argv[2]);
    if (val < lo) val = lo;
    if (val > hi) val = hi;
    return api->make_float(val);
}

static ClayPluginMethod methods[] = {
    { "sqrt",  mathex_sqrt },
    { "pow",   mathex_pow },
    { "sin",   mathex_sin },
    { "cos",   mathex_cos },
    { "atan2", mathex_atan2 },
    { "lerp",  mathex_lerp },
    { "clamp", mathex_clamp },
    { NULL, NULL }
};

CLAY_PLUGIN_EXPORT void clay_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType def = { "MathEx", methods, NULL, 0 };
    api->register_type(&def);
}
