/*
 * dispatch.c - Message sending and method dispatch
 */
#include "clay.h"

/* Global method context pointer — set before calling a method function.
 * Used by the BC compiler's generic dispatch to retrieve per-method data. */
void *clay_method_context = NULL;

/* When non-NULL, block trampolines bind this as 'self' (slot-as-method dispatch) */
ClayObject *clay_slot_method_self = NULL;

/* --- Global method cache --- */
#define METHOD_CACHE_SIZE 1024  /* must be power of 2 */
#define METHOD_CACHE_MASK (METHOD_CACHE_SIZE - 1)

typedef struct {
    HashTable      *methods;   /* methods table pointer (cache key part 1) */
    const char     *selector;  /* selector string (cache key part 2) */
    ClayMethod     *method;    /* cached method struct */
} MethodCacheEntry;

static MethodCacheEntry method_cache[METHOD_CACHE_SIZE];

void clay_method_cache_clear(void) {
    memset(method_cache, 0, sizeof(method_cache));
}

/* Lookup returning full ClayMethod* (walks prototype chain) */
static ClayMethod *lookup_method_full(ClayObject *obj, const char *name) {
    ClayObject *cur = obj;
    while (cur) {
        if (cur->methods) {
            ht_value_t val;
            if (ht_lookup(cur->methods, name, &val)) {
                return (ClayMethod *)val;
            }
        }
        cur = cur->parent;
    }
    return NULL;
}

/* Cached method lookup — checks cache first, falls back to chain walk */
static ClayMethod *cached_lookup_method(ClayObject *obj, const char *selector) {
    unsigned int idx = ((unsigned int)(uintptr_t)obj->methods ^
                        (unsigned int)(uintptr_t)selector) & METHOD_CACHE_MASK;
    MethodCacheEntry *entry = &method_cache[idx];

    if (entry->methods == obj->methods && entry->selector == selector) {
        return entry->method;
    }

    ClayMethod *m = lookup_method_full(obj, selector);
    if (m) {
        entry->methods  = obj->methods;
        entry->selector = selector;
        entry->method   = m;
    }
    return m;
}

/* Helper: invoke method, setting context before call */
static inline ClayValue invoke_method(ClayMethod *m, ClayObject *obj, int argc, ClayValue *argv) {
    clay_method_context = m->context;
    return m->func(obj, argc, argv);
}

/* Public API: cached method lookup (returns full ClayMethod*) */
ClayMethod *clay_cached_lookup(ClayObject *obj, const char *selector) {
    return cached_lookup_method(obj, selector);
}

/* Public API: invoke method with context setup */
ClayValue clay_invoke(ClayMethod *m, ClayObject *obj, int argc, ClayValue *argv) {
    return invoke_method(m, obj, argc, argv);
}

ClayValue clay_send_val(ClayValue receiver, const char *selector, int argc, ClayValue *argv) {
    /* Push trace frame for stack traces */
    clay_trace_push(clay_source_file, clay_source_line, selector);

    /* Handle tagged integer receivers — skip clone, lookup directly on Number */
    if (CLAY_IS_INT(receiver)) {
        ClayMethod *m = cached_lookup_method(clay_Number, selector);
        if (m) {
            /* Create wrapper only for the call (method needs self with _value) */
            ClayObject *wrapper = clay_clone(clay_Number);
            clay_set_slot(wrapper, "_value", receiver);
            ClayValue result = invoke_method(m, wrapper, argc, argv);
            /* If method returned self (the wrapper), return the original integer */
            if (result == CLAY_VAL(wrapper)) {
                clay_release(wrapper);
                clay_trace_pop();
                return receiver;
            }
            /* Retain result before releasing wrapper (result may reference wrapper) */
            if (!CLAY_IS_NIL(result) && !CLAY_IS_INT(result) &&
                result != CLAY_TRUE && result != CLAY_FALSE) {
                clay_retain(CLAY_OBJ(result));
                clay_release(wrapper);
                clay_release(CLAY_OBJ(result));
            } else {
                clay_release(wrapper);
            }
            clay_trace_pop();
            return result;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "integer does not understand '%s'", selector);
        clay_raise(CLAY_VAL(clay_error_new(buf, "MethodNotFound")));
        clay_trace_pop();
        return CLAY_NIL;
    }

    /* Handle nil */
    if (CLAY_IS_NIL(receiver)) {
        if (clay_Nil) {
            ClayMethod *m = cached_lookup_method(clay_Nil, selector);
            if (m) {
                ClayValue result = invoke_method(m, clay_Nil, argc, argv);
                clay_trace_pop();
                return result;
            }
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "message '%s' sent to nil", selector);
        clay_raise(CLAY_VAL(clay_error_new(buf, "NilError")));
        clay_trace_pop();
        return CLAY_NIL;
    }

    /* Handle boolean singletons and regular objects */
    ClayObject *obj = CLAY_OBJ(receiver);

    ClayMethod *m = cached_lookup_method(obj, selector);
    if (m) {
        ClayValue result = invoke_method(m, obj, argc, argv);
        clay_trace_pop();
        return result;
    }

    /* Slot access fallback: if a slot exists with this name, check its value.
     * If it's a Block, invoke it (Io-style: block slots act as methods).
     * Use getSlot("name") to retrieve a block without invoking it.
     * Otherwise return the slot value (only for zero-arg sends). */
    if (clay_has_slot(obj, selector)) {
        ClayValue slot_val = clay_get_slot(obj, selector);
        if (!CLAY_IS_NIL(slot_val) && !CLAY_IS_INT(slot_val) &&
            slot_val != CLAY_TRUE && slot_val != CLAY_FALSE) {
            ClayObject *slot_obj = CLAY_OBJ(slot_val);
            if (slot_obj && slot_obj->type == CLAY_TYPE_BLOCK && slot_obj->as.block.func) {
                clay_slot_method_self = obj;
                ClayValue result = slot_obj->as.block.func(slot_obj, argc, argv);
                clay_slot_method_self = NULL;
                clay_trace_pop();
                return result;
            }
        }
        if (argc == 0) {
            clay_trace_pop();
            return slot_val;
        }
    }

    /* Try doesNotUnderstand */
    ClayMethod *dnu = lookup_method_full(obj, "doesNotUnderstand");
    if (dnu) {
        ClayObject *sel_str = clay_string_new(selector);
        ClayObject *args_arr = clay_array_new();
        for (int i = 0; i < argc; i++) {
            clay_array_append(args_arr, argv[i]);
        }
        ClayValue dnu_argv[2] = { CLAY_VAL(sel_str), CLAY_VAL(args_arr) };
        ClayValue result = invoke_method(dnu, obj, 2, dnu_argv);
        clay_release(sel_str);
        clay_release(args_arr);
        clay_trace_pop();
        return result;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "object does not understand '%s'", selector);
    clay_raise(CLAY_VAL(clay_error_new(buf, "MethodNotFound")));
    clay_trace_pop();
    return CLAY_NIL;
}

ClayValue clay_send_super(ClayValue receiver, ClayObject *defining_obj,
                          const char *selector, int argc, ClayValue *argv) {
    clay_trace_push(clay_source_file, clay_source_line, selector);

    /* Start method lookup from the parent of the defining prototype */
    ClayObject *start = defining_obj ? defining_obj->parent : NULL;
    if (start) {
        ClayMethod *m = lookup_method_full(start, selector);
        if (m) {
            ClayObject *self = CLAY_OBJ(receiver);
            ClayValue result = invoke_method(m, self, argc, argv);
            clay_trace_pop();
            return result;
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "super does not understand '%s'", selector);
    clay_raise(CLAY_VAL(clay_error_new(buf, "MethodNotFound")));
    clay_trace_pop();
    return CLAY_NIL;
}
