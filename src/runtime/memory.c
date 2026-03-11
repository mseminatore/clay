/*
 * memory.c - Reference counting memory management
 */
#include "clay.h"

/* Memory diagnostics counters */
size_t clay_alloc_count = 0;
size_t clay_dealloc_count = 0;

/* Boolean singletons - statically allocated, never freed */
ClayObject clay_true_obj  = { NULL, NULL, NULL, 1000000, CLAY_TYPE_OBJECT, {{0}} };
ClayObject clay_false_obj = { NULL, NULL, NULL, 1000000, CLAY_TYPE_OBJECT, {{0}} };

static int clay_is_singleton(ClayObject *obj) {
    return obj == &clay_true_obj ||
           obj == &clay_false_obj ||
           obj == clay_Object ||
           obj == clay_Number ||
           obj == clay_String ||
           obj == clay_Array ||
           obj == clay_Block ||
           obj == clay_Nil ||
           obj == clay_Boolean ||
           obj == clay_Dict ||
           obj == clay_Error ||
           obj == clay_Math ||
           obj == clay_Console ||
           obj == clay_File ||
           obj == clay_System ||
           obj == clay_FFI ||
           obj == clay_Date ||
           obj == clay_Http ||
           obj == clay_Regex ||
           obj == clay_Range ||
           obj == clay_Fiber ||
           obj == clay_Db ||
           obj == clay_Graphics ||
           obj == clay_BreakSignal ||
           obj == clay_ContinueSignal;
}

ClayObject *clay_retain(ClayObject *obj) {
    if (obj && !clay_is_singleton(obj)) {
        CLAY_ATOMIC_INC(&obj->refcount);
    }
    return obj;
}

static void clay_dealloc(ClayObject *obj) {
    if (!obj || clay_is_singleton(obj)) return;
    clay_dealloc_count++;

    /* Free fiber context before slots are released (ctx is stored in _ctx slot) */
    if (obj->slots) {
        ht_value_t ctx_val = ht_find(obj->slots, (ht_key_t)"_ctx");
        if (ctx_val) {
            ClayValue cv = (ClayValue)(uintptr_t)ctx_val;
            if (CLAY_IS_INT(cv)) {
                clay_fiber_context_free((intptr_t)clay_to_int(cv));
            }
        }
    }

    /* Release parent */
    if (obj->parent) {
        clay_release(obj->parent);
        obj->parent = NULL;
    }

    /* Free slot keys and release slot values */
    if (obj->slots) {
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        while (ht_next(obj->slots, &pos, &key, &val)) {
            free((void *)key);
            ClayValue cv = (ClayValue)(uintptr_t)val;
            if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE) {
                clay_release(CLAY_OBJ(cv));
            }
        }
    }

    /* Free method keys and method structs */
    if (obj->methods) {
        clay_method_cache_invalidate(obj->methods);
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        while (ht_next(obj->methods, &pos, &key, &val)) {
            free((void *)key);
            free((void *)val);
        }
        ht_free(obj->methods);
        obj->methods = NULL;
    }

    /* Free type-specific data */
    switch (obj->type) {
        case CLAY_TYPE_STRING:
            free(obj->as.string.data);
            break;
        case CLAY_TYPE_ARRAY:
            if (obj->as.array.items) {
                for (size_t i = 0; i < obj->as.array.size; i++) {
                    ClayValue cv = obj->as.array.items[i];
                    if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE) {
                        clay_release(CLAY_OBJ(cv));
                    }
                }
                free(obj->as.array.items);
            }
            break;
        case CLAY_TYPE_BLOCK:
            if (obj->as.block.env) {
                for (int i = 0; i < obj->as.block.env_size; i++) {
                    ClayValue cv = obj->as.block.env[i];
                    if (CLAY_IS_INT(cv) || CLAY_IS_TAGGED_FLOAT(cv) || CLAY_IS_NIL(cv) || cv == CLAY_TRUE || cv == CLAY_FALSE) {
                        continue; /* tagged int, nil, or bool — nothing to free */
                    }
                    if (obj->as.block.env_are_cells) {
                        clay_cell_release((ClayCell *)(uintptr_t)cv);
                    } else {
                        clay_release(CLAY_OBJ(cv));
                    }
                }
                free(obj->as.block.env);
            }
            break;
        case CLAY_TYPE_DICT:
            if (obj->as.dict.map) {
                size_t dpos = 0;
                ht_key_t dkey;
                ht_value_t dval;
                while (ht_next(obj->as.dict.map, &dpos, &dkey, &dval)) {
                    free((void *)dkey);
                    ClayValue cv = (ClayValue)(uintptr_t)dval;
                    if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE) {
                        clay_release(CLAY_OBJ(cv));
                    }
                }
                ht_free(obj->as.dict.map);
            }
            break;
        case CLAY_TYPE_PLUGIN:
            if (obj->as.plugin.data && obj->as.plugin.free_fn)
                obj->as.plugin.free_fn(obj->as.plugin.data);
            break;
        default:
            break;
    }

    /* Pool simple objects for reuse; free complex ones */
    if (obj->type == CLAY_TYPE_OBJECT && obj->slots) {
        ht_clear(obj->slots);
        clay_object_pool_return(obj);
    } else {
        if (obj->slots) ht_free(obj->slots);
        free(obj);
    }
}

void clay_release(ClayObject *obj) {
    if (!obj || clay_is_singleton(obj)) return;
    if (CLAY_ATOMIC_DEC(&obj->refcount) <= 0) {
        clay_dealloc(obj);
    }
}
