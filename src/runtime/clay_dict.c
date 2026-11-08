/*
 * dict.c - Dict (HashMap) object implementation. Entries iterate in the
 * order their keys were first added (each, keys, values, toString, ...).
 */
#include "clay.h"

/* String comparison for dict keys (non-static, used by object.c clone) */
int dict_str_compare(ht_key_t a, ht_key_t b) {
    return strcmp((const char *)a, (const char *)b) == 0;
}

static HashTable *dict_ht_create(void) {
    HashTable *ht = ht_create();
    ht_set_hash_func(ht, HT_HASH_STRING);
    ht_set_compare_func(ht, dict_str_compare);
    return ht;
}

/* --- Insertion order ---
 * The hash table maps key -> value; `order` lists the same key strings (the
 * table owns them) in the order they were first added. A removed key leaves
 * a NULL hole, compacted when holes outnumber live keys. */
typedef struct ClayDictOrder {
    const char **keys;
    size_t len, cap, dead;
} ClayDictOrder;

static ClayDictOrder *order_of(ClayObject *d) {
    if (!d->as.dict.order)
        d->as.dict.order = (ClayDictOrder *)calloc(1, sizeof(ClayDictOrder));
    return d->as.dict.order;
}

static void order_compact(ClayDictOrder *o) {
    size_t j = 0;
    for (size_t i = 0; i < o->len; i++)
        if (o->keys[i]) o->keys[j++] = o->keys[i];
    o->len = j;
    o->dead = 0;
}

static void order_push(ClayObject *d, const char *key) {
    ClayDictOrder *o = order_of(d);
    if (o->dead > 0 && o->dead * 2 >= o->len) order_compact(o);
    if (o->len == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 8;
        o->keys = (const char **)realloc(o->keys, o->cap * sizeof(char *));
    }
    o->keys[o->len++] = key;
}

/* Forget `key` in the order; returns the stored key string (to free) */
static const char *order_remove(ClayObject *d, const char *key) {
    ClayDictOrder *o = d->as.dict.order;
    if (!o) return NULL;
    for (size_t i = 0; i < o->len; i++) {
        if (o->keys[i] && strcmp(o->keys[i], key) == 0) {
            const char *stored = o->keys[i];
            o->keys[i] = NULL;
            o->dead++;
            return stored;
        }
    }
    return NULL;
}

int clay_dict_next(ClayObject *d, size_t *pos, const char **key, ClayValue *val) {
    ClayDictOrder *o = d->as.dict.order;
    if (!o) return 0;
    while (*pos < o->len) {
        const char *k = o->keys[(*pos)++];
        if (!k) continue;
        *key = k;
        *val = (ClayValue)(uintptr_t)ht_find(d->as.dict.map, (ht_key_t)k);
        return 1;
    }
    return 0;
}

static void release_value(ClayValue cv) {
    if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE)
        clay_release(CLAY_OBJ(cv));
}

static void retain_value(ClayValue cv) {
    if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE)
        clay_retain(CLAY_OBJ(cv));
}

ClayObject *clay_dict_new(void) {
    ClayObject *obj = clay_object_new();
    obj->type = CLAY_TYPE_DICT;
    obj->parent = clay_Dict;
    if (clay_Dict) clay_retain(clay_Dict);
    obj->as.dict.map = dict_ht_create();
    obj->as.dict.order = NULL;
    return obj;
}

/* A clone of a Dict starts with a copy of its entries, in the same order */
void clay_dict_copy_into(ClayObject *dst, ClayObject *src) {
    dst->as.dict.map = dict_ht_create();
    dst->as.dict.order = NULL;
    if (!src->as.dict.map) return;
    size_t pos = 0;
    const char *key;
    ClayValue val;
    while (clay_dict_next(src, &pos, &key, &val)) clay_dict_set(dst, key, val);
}

/* Release every value and key, and the table and order themselves */
void clay_dict_free_contents(ClayObject *d) {
    size_t pos = 0;
    const char *key;
    ClayValue val;
    while (clay_dict_next(d, &pos, &key, &val)) {
        release_value(val);
        free((void *)key);
    }
    if (d->as.dict.map) ht_free(d->as.dict.map);
    if (d->as.dict.order) {
        free(d->as.dict.order->keys);
        free(d->as.dict.order);
    }
    d->as.dict.map = NULL;
    d->as.dict.order = NULL;
}

void clay_dict_set(ClayObject *dict, const char *key, ClayValue val) {
    retain_value(val);   /* first, in case val is the one being replaced */
    if (ht_contains(dict->as.dict.map, (ht_key_t)key)) {
        release_value((ClayValue)(uintptr_t)ht_find(dict->as.dict.map, (ht_key_t)key));
        /* ht_add replaces the value and keeps the existing key (and its place) */
        ht_add(dict->as.dict.map, (ht_key_t)key, (ht_value_t)(uintptr_t)val);
    } else {
        char *key_copy = strdup(key);
        ht_add(dict->as.dict.map, (ht_key_t)key_copy, (ht_value_t)(uintptr_t)val);
        order_push(dict, key_copy);
    }
}

ClayValue clay_dict_get(ClayObject *dict, const char *key) {
    ht_value_t val = ht_find(dict->as.dict.map, (ht_key_t)key);
    if (!val) return CLAY_NIL;
    return (ClayValue)(uintptr_t)val;
}

int clay_dict_has(ClayObject *dict, const char *key) {
    return ht_contains(dict->as.dict.map, (ht_key_t)key);
}

void clay_dict_remove(ClayObject *dict, const char *key) {
    if (!ht_contains(dict->as.dict.map, (ht_key_t)key)) return;
    release_value((ClayValue)(uintptr_t)ht_find(dict->as.dict.map, (ht_key_t)key));
    const char *stored = order_remove(dict, key);
    ht_remove(dict->as.dict.map, (ht_key_t)key);
    free((void *)stored);          /* the table's copy of the key */
}

size_t clay_dict_size(ClayObject *dict) {
    return ht_size(dict->as.dict.map);
}

/* --- Dict methods --- */

static ClayValue dict_set(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 2) return CLAY_VAL(self);
    if (!CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_VAL(self);
    const char *key = clay_string_data(CLAY_OBJ(argv[0]));
    clay_dict_set(self, key, argv[1]);
    return CLAY_VAL(self);
}

static ClayValue dict_get(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    const char *key = clay_string_data(CLAY_OBJ(argv[0]));
    ClayValue result = clay_dict_get(self, key);
    clay_retain_val(result);
    return result;
}

static ClayValue dict_has(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    const char *key = clay_string_data(CLAY_OBJ(argv[0]));
    return clay_dict_has(self, key) ? CLAY_TRUE : CLAY_FALSE;
}

static ClayValue dict_remove(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_VAL(self);
    const char *key = clay_string_data(CLAY_OBJ(argv[0]));
    clay_dict_remove(self, key);
    return CLAY_VAL(self);
}

static ClayValue dict_size(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_int((intptr_t)clay_dict_size(self));
}

static ClayValue dict_keys(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *arr = clay_array_new();
    size_t pos = 0;
    const char *key;
    ClayValue val;
    while (clay_dict_next(self, &pos, &key, &val)) {
        ClayObject *ks = clay_string_new(key);
        clay_array_append(arr, CLAY_VAL(ks));
        clay_release(ks);
    }
    return CLAY_VAL(arr);
}

static ClayValue dict_values(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *arr = clay_array_new();
    size_t pos = 0;
    const char *key;
    ClayValue val;
    while (clay_dict_next(self, &pos, &key, &val)) clay_array_append(arr, val);
    return CLAY_VAL(arr);
}

static ClayValue dict_each(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    size_t pos = 0;
    const char *key;
    ClayValue val;
    while (clay_dict_next(self, &pos, &key, &val)) {
        ClayObject *key_str = clay_string_new(key);
        ClayValue args[2] = { CLAY_VAL(key_str), val };
        ClayValue ret = clay_send(CLAY_OBJ(argv[0]), "value", 2, args);
        clay_release_val(ret);
        clay_release(key_str);
    }
    return CLAY_VAL(self);
}

static ClayValue dict_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char *buf = (char *)malloc(256);
    size_t cap = 256, len = 0;
    buf[len++] = '{';

    size_t pos = 0;
    const char *key;
    ClayValue val;
    int first = 1;
    while (clay_dict_next(self, &pos, &key, &val)) {
        if (!first) { buf[len++] = ','; buf[len++] = ' '; }
        first = 0;

        const char *ks = key;
        size_t klen = strlen(ks);

        /* Value to string */
        ClayValue cv = val;
        const char *vs;
        char numbuf[32];
        ClayValue sv = CLAY_NIL;
        if (CLAY_IS_INT(cv)) {
            snprintf(numbuf, sizeof(numbuf), "%lld", (long long)clay_to_int(cv));
            vs = numbuf;
        } else if (CLAY_IS_NIL(cv)) {
            vs = "nil";
        } else {
            sv = clay_send(CLAY_OBJ(cv), "toString", 0, NULL);
            vs = clay_string_data(CLAY_OBJ(sv));
        }
        size_t vlen = strlen(vs);

        /* Grow buffer: key + ": " + value + ", " + safety */
        while (len + klen + vlen + 8 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }

        memcpy(buf + len, ks, klen);
        len += klen;
        buf[len++] = ':';
        buf[len++] = ' ';
        memcpy(buf + len, vs, vlen);
        len += vlen;
        if (!CLAY_IS_NIL(sv)) clay_release(CLAY_OBJ(sv));
    }

    if (len + 2 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
    buf[len++] = '}';
    buf[len] = '\0';

    ClayObject *result = clay_string_new(buf);
    free(buf);
    return CLAY_VAL(result);
}

static ClayValue dict_equals(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_OBJ(argv[0])) return CLAY_FALSE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_DICT) return CLAY_FALSE;
    if (self == other) return CLAY_TRUE;
    int self_size = self->as.dict.map ? (int)ht_size(self->as.dict.map) : 0;
    int other_size = other->as.dict.map ? (int)ht_size(other->as.dict.map) : 0;
    if (self_size != other_size) return CLAY_FALSE;
    if (self_size == 0) return CLAY_TRUE;
    size_t pos = 0;
    const char *key;
    ClayValue val;
    while (clay_dict_next(self, &pos, &key, &val)) {
        if (!ht_contains(other->as.dict.map, (ht_key_t)key)) return CLAY_FALSE;
        ClayValue a = val;
        ClayValue b = (ClayValue)(uintptr_t)ht_find(other->as.dict.map, (ht_key_t)key);
        ClayValue eq = clay_send_val(a, "==", 1, &b);
        if (eq != CLAY_TRUE) return CLAY_FALSE;
    }
    return CLAY_TRUE;
}

static ClayValue dict_neq(ClayObject *self, int argc, ClayValue *argv) {
    return dict_equals(self, argc, argv) == CLAY_TRUE ? CLAY_FALSE : CLAY_TRUE;
}

static ClayValue dict_merge(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_OBJ(argv[0])) return CLAY_VAL(self);
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_DICT) return CLAY_VAL(self);
    size_t pos = 0;
    const char *key;
    ClayValue val;
    while (clay_dict_next(other, &pos, &key, &val)) clay_dict_set(self, key, val);
    return CLAY_VAL(self);
}

static ClayValue dict_entries(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *arr = clay_array_new();
    size_t pos = 0;
    const char *key;
    ClayValue val;
    while (clay_dict_next(self, &pos, &key, &val)) {
        ClayObject *pair = clay_array_new();
        ClayObject *ks = clay_string_new(key);
        clay_array_append(pair, CLAY_VAL(ks));
        clay_release(ks);
        clay_array_append(pair, val);
        clay_array_append(arr, CLAY_VAL(pair));
        clay_release(pair);
    }
    return CLAY_VAL(arr);
}

static ClayValue dict_clear(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    clay_dict_free_contents(self);
    self->as.dict.map = dict_ht_create();
    return CLAY_VAL(self);
}

void clay_dict_init_methods(ClayObject *proto) {
    clay_set_method(proto, "set", dict_set);
    clay_set_method(proto, "get", dict_get);
    clay_set_method(proto, "has", dict_has);
    clay_set_method(proto, "remove", dict_remove);
    clay_set_method(proto, "size", dict_size);
    clay_set_method(proto, "keys", dict_keys);
    clay_set_method(proto, "values", dict_values);
    clay_set_method(proto, "each", dict_each);
    clay_set_method(proto, "toString", dict_toString);
    clay_set_method(proto, "==", dict_equals);
    clay_set_method(proto, "!=", dict_neq);
    clay_set_method(proto, "merge", dict_merge);
    clay_set_method(proto, "entries", dict_entries);
    clay_set_method(proto, "clear", dict_clear);
}
