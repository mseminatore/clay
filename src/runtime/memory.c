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
           obj == clay_File;
}

ClayObject *clay_retain(ClayObject *obj) {
    if (obj && !clay_is_singleton(obj)) {
        obj->refcount++;
    }
    return obj;
}

static void clay_dealloc(ClayObject *obj) {
    if (!obj || clay_is_singleton(obj)) return;
    clay_dealloc_count++;

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
        ht_free(obj->slots);
    }

    /* Free method keys and method structs */
    if (obj->methods) {
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        while (ht_next(obj->methods, &pos, &key, &val)) {
            free((void *)key);
            free((void *)val);
        }
        ht_free(obj->methods);
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
                    if (CLAY_IS_INT(cv) || CLAY_IS_NIL(cv) || cv == CLAY_TRUE || cv == CLAY_FALSE) {
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
        default:
            break;
    }

    free(obj);
}

void clay_release(ClayObject *obj) {
    if (!obj || clay_is_singleton(obj)) return;
    obj->refcount--;
    if (obj->refcount <= 0) {
        clay_dealloc(obj);
    }
}
