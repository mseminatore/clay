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

void clay_method_cache_invalidate(void *methods_ptr) {
    HashTable *target = (HashTable *)methods_ptr;
    for (int i = 0; i < METHOD_CACHE_SIZE; i++) {
        if (method_cache[i].methods == target) {
            method_cache[i].methods = NULL;
        }
    }
}

/* Lookup returning full ClayMethod* (walks prototype chain).
   If 'defining_out' is non-NULL, stores the prototype where the method was found. */
static ClayMethod *lookup_method_full(ClayObject *obj, const char *name,
                                      HashTable **defining_out) {
    ClayObject *cur = obj;
    while (cur) {
        if (cur->methods) {
            ht_value_t val;
            if (ht_lookup(cur->methods, name, &val)) {
                if (defining_out) *defining_out = cur->methods;
                return (ClayMethod *)val;
            }
        }
        cur = cur->parent;
    }
    if (defining_out) *defining_out = NULL;
    return NULL;
}

/* Hash helper — uses object identity (methods ptr or obj ptr) for cache index */
static inline unsigned int cache_hash(uintptr_t key1, uintptr_t key2) {
    uintptr_t h = (key1 >> 3) ^ (key2 >> 1);
    h *= 0x9E3779B9u;
    return (unsigned int)(h >> 16) & METHOD_CACHE_MASK;
}

/* Cached method lookup — checks cache first, falls back to chain walk.
   Two-level cache: first checks receiver's own methods table, then
   checks the defining prototype's table (shared across siblings). */
static ClayMethod *cached_lookup_method(ClayObject *obj, const char *selector) {
    /* Use obj pointer itself when methods is NULL to avoid hash collisions */
    uintptr_t recv_key = obj->methods ? (uintptr_t)obj->methods : (uintptr_t)obj;
    unsigned int idx = cache_hash(recv_key, (uintptr_t)selector);
    MethodCacheEntry *entry = &method_cache[idx];

    if (entry->methods == (HashTable *)recv_key && entry->selector == selector) {
        return entry->method;
    }

    /* Shortcut: check parent's cache before full chain walk.
       Only safe when receiver has no local methods (can't override parent). */
    if (!obj->methods && obj->parent) {
        uintptr_t parent_key = obj->parent->methods
            ? (uintptr_t)obj->parent->methods : (uintptr_t)obj->parent;
        unsigned int pidx = cache_hash(parent_key, (uintptr_t)selector);
        MethodCacheEntry *pentry = &method_cache[pidx];
        if (pentry->methods == (HashTable *)parent_key &&
            pentry->selector == selector && pentry->method) {
            /* Found in parent's cache — also populate receiver's entry */
            entry->methods  = (HashTable *)recv_key;
            entry->selector = selector;
            entry->method   = pentry->method;
            return pentry->method;
        }
    }

    /* Full prototype chain walk */
    HashTable *defining = NULL;
    ClayMethod *m = lookup_method_full(obj, selector, &defining);
    if (m) {
        /* Cache under receiver's key */
        entry->methods  = (HashTable *)recv_key;
        entry->selector = selector;
        entry->method   = m;

        /* Also cache under defining prototype's key (shared by siblings) */
        if (defining && defining != obj->methods) {
            unsigned int didx = cache_hash((uintptr_t)defining, (uintptr_t)selector);
            MethodCacheEntry *dentry = &method_cache[didx];
            dentry->methods  = defining;
            dentry->selector = selector;
            dentry->method   = m;
        }
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
            if (!CLAY_IS_NIL(result) && CLAY_IS_PTR(result) &&
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

    /* Handle tagged float receivers — same wrapper approach as ints */
    if (CLAY_IS_TAGGED_FLOAT(receiver)) {
        ClayMethod *m = cached_lookup_method(clay_Number, selector);
        if (m) {
            ClayObject *wrapper = clay_clone(clay_Number);
            clay_set_slot(wrapper, "_value", receiver);
            ClayValue result = invoke_method(m, wrapper, argc, argv);
            if (result == CLAY_VAL(wrapper)) {
                clay_release(wrapper);
                clay_trace_pop();
                return receiver;
            }
            if (!CLAY_IS_NIL(result) && CLAY_IS_PTR(result) &&
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
        snprintf(buf, sizeof(buf), "float does not understand '%s'", selector);
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
        if (!CLAY_IS_NIL(slot_val) && CLAY_IS_PTR(slot_val) &&
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
    ClayMethod *dnu = lookup_method_full(obj, "doesNotUnderstand", NULL);
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
        ClayMethod *m = lookup_method_full(start, selector, NULL);
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
