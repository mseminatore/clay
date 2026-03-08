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

ClayObject *clay_object_new(void) {
    ClayObject *obj = (ClayObject *)calloc(1, sizeof(ClayObject));
    if (!obj) {
        fprintf(stderr, "clay: out of memory\n");
        exit(1);
    }
    obj->parent   = NULL;
    obj->slots    = clay_ht_create();
    obj->methods  = NULL; /* lazy: created on demand by clay_set_method_ctx */
    obj->refcount = 1;
    obj->type     = CLAY_TYPE_OBJECT;
    clay_alloc_count++;
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
        for (int i = 0; i < proto->as.array.size; i++) {
            obj->as.array.items[i] = proto->as.array.items[i];
            ClayValue v = proto->as.array.items[i];
            if (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v) && v != CLAY_TRUE && v != CLAY_FALSE)
                clay_retain(CLAY_OBJ(v));
        }
    }

    /* Copy slots from prototype */
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

    return obj;
}

void clay_set_slot(ClayObject *obj, const char *name, ClayValue value) {
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
