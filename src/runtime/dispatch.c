/*
 * dispatch.c - Message sending and method dispatch
 */
#include "clay.h"

/* --- Global method cache --- */
#define METHOD_CACHE_SIZE 1024  /* must be power of 2 */
#define METHOD_CACHE_MASK (METHOD_CACHE_SIZE - 1)

typedef struct {
    HashTable      *methods;   /* methods table pointer (cache key part 1) */
    const char     *selector;  /* selector string (cache key part 2) */
    ClayMethodFunc  func;      /* cached method function pointer */
} MethodCacheEntry;

static MethodCacheEntry method_cache[METHOD_CACHE_SIZE];

void clay_method_cache_clear(void) {
    memset(method_cache, 0, sizeof(method_cache));
}

/* Cached method lookup — checks cache first, falls back to chain walk */
static ClayMethodFunc cached_lookup(ClayObject *obj, const char *selector) {
    unsigned int idx = ((unsigned int)(uintptr_t)obj->methods ^
                        (unsigned int)(uintptr_t)selector) & METHOD_CACHE_MASK;
    MethodCacheEntry *entry = &method_cache[idx];

    /* Fast pointer comparison — works when selector pointers are stable
       (same TU string literals, or interned strings) */
    if (entry->methods == obj->methods && entry->selector == selector) {
        return entry->func;
    }

    /* Cache miss — do full chain walk */
    ClayMethodFunc func = clay_lookup_method(obj, selector);
    if (func) {
        entry->methods  = obj->methods;
        entry->selector = selector;
        entry->func     = func;
    }
    return func;
}

ClayValue clay_send_val(ClayValue receiver, const char *selector, int argc, ClayValue *argv) {
    /* Handle tagged integer receivers — skip clone, lookup directly on Number */
    if (CLAY_IS_INT(receiver)) {
        ClayMethodFunc func = cached_lookup(clay_Number, selector);
        if (func) {
            /* Create wrapper only for the call (method needs self with _value) */
            ClayObject *wrapper = clay_clone(clay_Number);
            clay_set_slot(wrapper, "_value", receiver);
            ClayValue result = func(wrapper, argc, argv);
            clay_release(wrapper);
            return result;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "integer does not understand '%s'", selector);
        clay_raise(CLAY_VAL(clay_error_new(buf, "MethodNotFound")));
        return CLAY_NIL;
    }

    /* Handle nil */
    if (CLAY_IS_NIL(receiver)) {
        if (clay_Nil) {
            ClayMethodFunc func = cached_lookup(clay_Nil, selector);
            if (func) {
                return func(clay_Nil, argc, argv);
            }
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "message '%s' sent to nil", selector);
        clay_raise(CLAY_VAL(clay_error_new(buf, "NilError")));
        return CLAY_NIL;
    }

    /* Handle boolean singletons and regular objects */
    ClayObject *obj = CLAY_OBJ(receiver);

    ClayMethodFunc func = cached_lookup(obj, selector);
    if (func) {
        return func(obj, argc, argv);
    }

    /* Slot access fallback: if a slot exists with this name, return its value */
    if (argc == 0 && clay_has_slot(obj, selector)) {
        return clay_get_slot(obj, selector);
    }

    /* Try doesNotUnderstand */
    ClayMethodFunc dnu = clay_lookup_method(obj, "doesNotUnderstand");
    if (dnu) {
        ClayObject *sel_str = clay_string_new(selector);
        ClayObject *args_arr = clay_array_new();
        for (int i = 0; i < argc; i++) {
            clay_array_append(args_arr, argv[i]);
        }
        ClayValue dnu_argv[2] = { CLAY_VAL(sel_str), CLAY_VAL(args_arr) };
        ClayValue result = dnu(obj, 2, dnu_argv);
        clay_release(sel_str);
        clay_release(args_arr);
        return result;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "object does not understand '%s'", selector);
    clay_raise(CLAY_VAL(clay_error_new(buf, "MethodNotFound")));
    return CLAY_NIL;
}

ClayValue clay_send_super(ClayValue receiver, ClayObject *defining_obj,
                          const char *selector, int argc, ClayValue *argv) {
    /* Start method lookup from the parent of the defining prototype */
    ClayObject *start = defining_obj ? defining_obj->parent : NULL;
    if (start) {
        ClayMethodFunc func = clay_lookup_method(start, selector);
        if (func) {
            ClayObject *self = CLAY_OBJ(receiver);
            return func(self, argc, argv);
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "super does not understand '%s'", selector);
    clay_raise(CLAY_VAL(clay_error_new(buf, "MethodNotFound")));
    return CLAY_NIL;
}
