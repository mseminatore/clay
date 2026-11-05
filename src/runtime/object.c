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
