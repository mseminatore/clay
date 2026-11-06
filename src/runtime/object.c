/*
 * object.c - Core object model: create, clone, slots, methods
 */
#include "clay.h"

/* String comparison function for hash tables */
static int clay_str_compare(ht_key_t a, ht_key_t b) {
    return strcmp((const char *)a, (const char *)b) == 0;
}

/* Create a new HashTable configured for string keys */
static HashTable *clay_ht_create(void) {
    HashTable *ht = ht_create();
    ht_set_hash_func(ht, HT_HASH_STRING);
    ht_set_compare_func(ht, clay_str_compare);
    return ht;
}

/* --- Object free-list pool ---
 * Recycles ClayObject structs with their slots hash table to avoid
 * repeated calloc/free + ht_create/ht_free cycles. */
#define OBJECT_POOL_MAX 256
static ClayObject *object_pool[OBJECT_POOL_MAX];
static int object_pool_count = 0;

#ifdef CLAY_THREAD_SAFE
  #ifdef _WIN32
    /* Minimal Windows API declarations to avoid including windows.h */
    typedef struct { void *DebugInfo; long LockCount; long RecursionCount;
                     void *OwningThread; void *LockSemaphore; uintptr_t SpinCount; } CLAY_CS;
    __declspec(dllimport) void __stdcall InitializeCriticalSection(CLAY_CS *);
    __declspec(dllimport) void __stdcall EnterCriticalSection(CLAY_CS *);
    __declspec(dllimport) void __stdcall LeaveCriticalSection(CLAY_CS *);
    static CLAY_CS pool_cs;
  #else
    #include <pthread.h>
    static pthread_mutex_t pool_cs = PTHREAD_MUTEX_INITIALIZER;
  #endif
  static int pool_cs_inited = 0;
  static void pool_lock(void) {
      #ifdef _WIN32
      if (!pool_cs_inited) { InitializeCriticalSection(&pool_cs); pool_cs_inited = 1; }
      EnterCriticalSection(&pool_cs);
      #else
      pthread_mutex_lock(&pool_cs);
      #endif
  }
  static void pool_unlock(void) {
      #ifdef _WIN32
      LeaveCriticalSection(&pool_cs);
      #else
      pthread_mutex_unlock(&pool_cs);
      #endif
  }
#else
  static void pool_lock(void) {}
  static void pool_unlock(void) {}
#endif

/* --- Slots that shadow inherited methods ---
 * Lookup rule: walking the prototype chain from the receiver, the nearest
 * definition wins -- a slot on an object hides a method inherited from
 * further up (on the same object a method wins). Checking slots on every
 * send would slow all dispatch, so only "shadowed" selectors -- names some
 * object has as a slot while a method of that name exists above it -- take
 * the careful walk (clay_lookup_nearest); everything else uses the method
 * caches as before.
 *
 * Two small Bloom filters of slot and method names pre-filter the checks
 * (false positives only cost speed). Shadowed selectors are an append-only
 * list read without locks; the count is published after the entry. */
#define NAME_BLOOM_BITS 8192
static volatile unsigned char g_slot_bloom[NAME_BLOOM_BITS / 8];
static volatile unsigned char g_method_bloom[NAME_BLOOM_BITS / 8];

#define SHADOW_MAX 128
static const char *g_shadowed[SHADOW_MAX];
volatile long clay_shadow_count = 0;
static volatile int g_shadow_overflow = 0;   /* too many: treat every selector as shadowed */

static void name_hashes(const char *s, unsigned *h1, unsigned *h2) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    *h1 = h & (NAME_BLOOM_BITS - 1);
    *h2 = (h >> 13) & (NAME_BLOOM_BITS - 1);
}

static int bloom_has(volatile unsigned char *b, const char *name) {
    unsigned h1, h2;
    name_hashes(name, &h1, &h2);
    return (b[h1 >> 3] & (1u << (h1 & 7))) && (b[h2 >> 3] & (1u << (h2 & 7)));
}

static void bloom_add(volatile unsigned char *b, const char *name) {
    if (bloom_has(b, name)) return;
    unsigned h1, h2;
    name_hashes(name, &h1, &h2);
    pool_lock();
    b[h1 >> 3] |= (unsigned char)(1u << (h1 & 7));
    b[h2 >> 3] |= (unsigned char)(1u << (h2 & 7));
    pool_unlock();
}

static volatile unsigned char g_shadow_bloom[NAME_BLOOM_BITS / 8];   /* names in g_shadowed */

int clay_selector_shadowed_slow(const char *selector) {
    if (g_shadow_overflow) return 1;
    if (!bloom_has(g_shadow_bloom, selector)) return 0;
    long n = clay_shadow_count;
    for (long i = 0; i < n; i++)
        if (strcmp(g_shadowed[i], selector) == 0) return 1;
    return 0;
}

static void shadow_add(const char *name) {
    if (clay_selector_shadowed_slow(name)) return;
    pool_lock();
    if (!clay_selector_shadowed_slow(name)) {
        if (clay_shadow_count < SHADOW_MAX) {
            g_shadowed[clay_shadow_count] = strdup(name);
            unsigned h1, h2;
            name_hashes(name, &h1, &h2);
            g_shadow_bloom[h1 >> 3] |= (unsigned char)(1u << (h1 & 7));
            g_shadow_bloom[h2 >> 3] |= (unsigned char)(1u << (h2 & 7));
            CLAY_ATOMIC_INC(&clay_shadow_count);
        } else {
            g_shadow_overflow = 1;
            CLAY_ATOMIC_INC(&clay_shadow_count);   /* keep the fast check true */
        }
    }
    pool_unlock();
}

/* A new slot `name` on obj: does it hide a method inherited by obj? */
static void note_new_slot(ClayObject *obj, const char *name) {
    bloom_add(g_slot_bloom, name);
    if (!bloom_has(g_method_bloom, name)) return;
    if (obj->methods) {
        ht_value_t v;
        if (ht_lookup(obj->methods, name, &v)) return;   /* own method wins */
    }
    if (obj->parent && clay_lookup_method(obj->parent, name)) shadow_add(name);
}

/* A new method `name`: an existing slot of that name below it may now hide it */
static void note_new_method(const char *name) {
    bloom_add(g_method_bloom, name);
    if (bloom_has(g_slot_bloom, name)) shadow_add(name);
}

/* Method lookup by the nearest-definition rule (see above): NULL when a slot
 * named `name` is found first, or nothing is found */
ClayMethod *clay_lookup_nearest(ClayObject *obj, const char *name) {
    for (ClayObject *cur = obj; cur; cur = cur->parent) {
        ht_value_t val;
        if (cur->methods && ht_lookup(cur->methods, name, &val)) return (ClayMethod *)val;
        if (cur->slots && ht_lookup(cur->slots, name, &val)) return NULL;
    }
    return NULL;
}

/* Return an object to the pool (called from clay_dealloc after cleanup).
 * The object's slots hash table is already cleared; methods is NULL. */
void clay_object_pool_return(ClayObject *obj) {
    pool_lock();
    if (object_pool_count < OBJECT_POOL_MAX && obj->slots) {
        /* Keep the slots hash table for reuse */
        obj->parent = NULL;
        obj->methods = NULL;
        obj->refcount = 0;
        obj->type = CLAY_TYPE_OBJECT;
        memset(&obj->as, 0, sizeof(obj->as));
        object_pool[object_pool_count++] = obj;
        pool_unlock();
    } else {
        pool_unlock();
        if (obj->slots) ht_free(obj->slots);
        free(obj);
    }
}

ClayObject *clay_object_new(void) {
    ClayObject *obj;

    pool_lock();
    if (object_pool_count > 0) {
        obj = object_pool[--object_pool_count];
        pool_unlock();
        /* slots hash table is already allocated and cleared */
    } else {
        pool_unlock();
        obj = (ClayObject *)calloc(1, sizeof(ClayObject));
        if (!obj) {
            fprintf(stderr, "clay: out of memory\n");
            exit(1);
        }
        obj->slots = clay_ht_create();
    }

    obj->parent   = NULL;
    obj->methods  = NULL; /* lazy: created on demand by clay_set_method_ctx */
    obj->refcount = 1;
    obj->type     = CLAY_TYPE_OBJECT;
    CLAY_ATOMIC_INC(&clay_alloc_count);
    return obj;
}

ClayObject *clay_clone(ClayObject *proto) {
    ClayObject *obj = clay_object_new();
    obj->parent = proto;
    if (proto) {
        clay_retain(proto);
        obj->type = proto->type;
    }

    /* Type-specific init for clones */
    if (proto && proto->type == CLAY_TYPE_DICT) {
        obj->as.dict.map = ht_create();
        ht_set_hash_func(obj->as.dict.map, HT_HASH_STRING);
        extern int dict_str_compare(ht_key_t a, ht_key_t b);
        ht_set_compare_func(obj->as.dict.map, dict_str_compare);
        /* Copy entries from prototype dict */
        if (proto->as.dict.map) {
            size_t pos = 0;
            ht_key_t key;
            ht_value_t val;
            while (ht_next(proto->as.dict.map, &pos, &key, &val)) {
                char *key_copy = strdup((const char *)key);
                ClayValue cv = (ClayValue)(uintptr_t)val;
                if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE)
                    clay_retain(CLAY_OBJ(cv));
                ht_insert(obj->as.dict.map, key_copy, val);
            }
        }
    } else if (proto && proto->type == CLAY_TYPE_ARRAY) {
        int cap = proto->as.array.capacity > 8 ? proto->as.array.capacity : 8;
        obj->as.array.items = (ClayValue *)malloc(sizeof(ClayValue) * (size_t)cap);
        obj->as.array.size = proto->as.array.size;
        obj->as.array.capacity = cap;
        for (size_t i = 0; i < proto->as.array.size; i++) {
            obj->as.array.items[i] = proto->as.array.items[i];
            ClayValue v = proto->as.array.items[i];
            if (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v) && v != CLAY_TRUE && v != CLAY_FALSE)
                clay_retain(CLAY_OBJ(v));
        }
    } else if (proto && proto->type == CLAY_TYPE_PLUGIN) {
        /* Inherit free_fn from prototype; data starts NULL */
        obj->as.plugin.data = NULL;
        obj->as.plugin.free_fn = proto->as.plugin.free_fn;
    } else if (proto && proto->type == CLAY_TYPE_VEC3) {
        obj->as.vec3.x = proto->as.vec3.x;
        obj->as.vec3.y = proto->as.vec3.y;
        obj->as.vec3.z = proto->as.vec3.z;
    }

    /* Copy slots from prototype — DISABLED for copy-on-write optimization.
     * clay_get_slot() walks the parent chain, so reads still work.
     * clay_set_slot() writes to local slots, creating them on first write. */
#if 0
    if (proto && proto->slots) {
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        while (ht_next(proto->slots, &pos, &key, &val)) {
            char *key_copy = strdup((const char *)key);
            ClayValue cv = (ClayValue)(uintptr_t)val;
            /* Retain object-valued slots */
            if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE) {
                clay_retain(CLAY_OBJ(cv));
            }
            ht_insert(obj->slots, key_copy, val);
        }
    }
#endif

    return obj;
}

void clay_set_slot(ClayObject *obj, const char *name, ClayValue value) {
    /* `x foo = v` on nil or a number: raise instead of dereferencing */
    if (!obj) {
        char msg[192];
        snprintf(msg, sizeof(msg), "cannot set slot '%s' on nil", name);
        clay_raise(CLAY_VAL(clay_error_new(msg, "NilError")));
        return;
    }
    if (((uintptr_t)obj & 3) != 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "cannot set slot '%s' on a number", name);
        clay_raise(CLAY_VAL(clay_error_new(msg, "TypeError")));
        return;
    }
    /* Check if key already exists — update in-place to avoid remove+insert+strdup */
    ht_value_t old;
    if (ht_lookup(obj->slots, (ht_key_t)name, &old)) {
        ClayValue ov = (ClayValue)(uintptr_t)old;
        if (CLAY_IS_PTR(ov) && !CLAY_IS_NIL(ov) && ov != CLAY_TRUE && ov != CLAY_FALSE) {
            clay_release(CLAY_OBJ(ov));
        }
        /* Retain new value if it's an object */
        if (CLAY_IS_PTR(value) && !CLAY_IS_NIL(value) && value != CLAY_TRUE && value != CLAY_FALSE) {
            clay_retain(CLAY_OBJ(value));
        }
        /* Update value in-place (ht_add replaces existing) */
        ht_add(obj->slots, (ht_key_t)name, (ht_value_t)(uintptr_t)value);
        return;
    }

    /* Retain new value if it's an object */
    if (CLAY_IS_PTR(value) && !CLAY_IS_NIL(value) && value != CLAY_TRUE && value != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(value));
    }

    char *key_copy = strdup(name);
    ht_insert(obj->slots, key_copy, (ht_value_t)(uintptr_t)value);
    note_new_slot(obj, name);
}

void clay_set_slot_take(ClayObject *obj, const char *name, ClayValue value) {
    clay_set_slot(obj, name, value);
    clay_release_val(value);
}

ClayValue clay_get_slot(ClayObject *obj, const char *name) {
    /* Look up in this object first, then walk parent chain */
    ClayObject *cur = obj;
    while (cur) {
        if (cur->slots) {
            ht_value_t val;
            if (ht_lookup(cur->slots, name, &val)) {
                return (ClayValue)(uintptr_t)val;
            }
        }
        cur = cur->parent;
    }
    return CLAY_NIL;
}

int clay_has_slot(ClayObject *obj, const char *name) {
    ClayObject *cur = obj;
    while (cur) {
        if (cur->slots) {
            ht_value_t val;
            if (ht_lookup(cur->slots, name, &val)) {
                return 1;
            }
        }
        cur = cur->parent;
    }
    return 0;
}

void clay_set_method(ClayObject *obj, const char *name, ClayMethodFunc func) {
    clay_set_method_ctx(obj, name, func, NULL);
}

void clay_set_method_ctx(ClayObject *obj, const char *name, ClayMethodFunc func, void *context) {
    /* Lazy-create methods hash table on first use */
    if (!obj->methods) {
        obj->methods = clay_ht_create();
    }

    /* Allocate a ClayMethod struct to store the function pointer */
    ClayMethod *m = (ClayMethod *)malloc(sizeof(ClayMethod));
    m->func = func;
    m->context = context;

    /* Remove old if exists */
    ht_value_t old;
    if (ht_lookup(obj->methods, name, &old)) {
        free((void *)old);
        ht_remove(obj->methods, name);
    }

    char *key_copy = strdup(name);
    ht_insert(obj->methods, key_copy, (ht_value_t)m);
    note_new_method(name);

    /* Invalidate method cache — methods table changed */
    clay_method_cache_clear();
}

ClayMethodFunc clay_lookup_method(ClayObject *obj, const char *name) {
    ClayObject *cur = obj;
    while (cur) {
        if (cur->methods) {
            ht_value_t val;
            if (ht_lookup(cur->methods, name, &val)) {
                ClayMethod *m = (ClayMethod *)val;
                return m->func;
            }
        }
        cur = cur->parent;
    }
    return NULL;
}
