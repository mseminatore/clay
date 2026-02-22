/*
 * dict.c - Dict (HashMap) object implementation
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

ClayObject *clay_dict_new(void) {
    ClayObject *obj = clay_object_new();
    obj->type = CLAY_TYPE_DICT;
    obj->parent = clay_Dict;
    if (clay_Dict) clay_retain(clay_Dict);
    obj->as.dict.map = dict_ht_create();
    return obj;
}

void clay_dict_set(ClayObject *dict, const char *key, ClayValue val) {
    /* Release old value if key already exists */
    ht_value_t old = ht_find(dict->as.dict.map, (ht_key_t)key);
    if (old) {
        ClayValue cv = (ClayValue)(uintptr_t)old;
        if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE) {
            clay_release(CLAY_OBJ(cv));
        }
        /* ht_add replaces value, keeps existing key */
        ht_add(dict->as.dict.map, (ht_key_t)key, (ht_value_t)(uintptr_t)val);
    } else {
        char *key_copy = strdup(key);
        ht_add(dict->as.dict.map, (ht_key_t)key_copy, (ht_value_t)(uintptr_t)val);
    }

    /* Retain new value */
    if (CLAY_IS_PTR(val) && !CLAY_IS_NIL(val) && val != CLAY_TRUE && val != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(val));
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
    ht_value_t old = ht_find(dict->as.dict.map, (ht_key_t)key);
    if (old) {
        ClayValue cv = (ClayValue)(uintptr_t)old;
        if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE) {
            clay_release(CLAY_OBJ(cv));
        }
    }
    ht_remove(dict->as.dict.map, (ht_key_t)key);
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
    return clay_dict_get(self, key);
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
    ht_key_t key;
    ht_value_t val;
    while (ht_next(self->as.dict.map, &pos, &key, &val)) {
        clay_array_append(arr, CLAY_VAL(clay_string_new((const char *)key)));
    }
    return CLAY_VAL(arr);
}

static ClayValue dict_values(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *arr = clay_array_new();
    size_t pos = 0;
    ht_key_t key;
    ht_value_t val;
    while (ht_next(self->as.dict.map, &pos, &key, &val)) {
        clay_array_append(arr, (ClayValue)(uintptr_t)val);
    }
    return CLAY_VAL(arr);
}

static ClayValue dict_each(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    size_t pos = 0;
    ht_key_t key;
    ht_value_t val;
    while (ht_next(self->as.dict.map, &pos, &key, &val)) {
        ClayValue args[2] = {
            CLAY_VAL(clay_string_new((const char *)key)),
            (ClayValue)(uintptr_t)val
        };
        clay_send(CLAY_OBJ(argv[0]), "value", 2, args);
    }
    return CLAY_VAL(self);
}

static ClayValue dict_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char *buf = (char *)malloc(256);
    size_t cap = 256, len = 0;
    buf[len++] = '{';

    size_t pos = 0;
    ht_key_t key;
    ht_value_t val;
    int first = 1;
    while (ht_next(self->as.dict.map, &pos, &key, &val)) {
        if (!first) { buf[len++] = ','; buf[len++] = ' '; }
        first = 0;

        const char *ks = (const char *)key;
        size_t klen = strlen(ks);

        /* Value to string */
        ClayValue cv = (ClayValue)(uintptr_t)val;
        const char *vs;
        char numbuf[32];
        ClayValue sv = CLAY_NIL;
        if (CLAY_IS_INT(cv)) {
            snprintf(numbuf, sizeof(numbuf), "%ld", (long)clay_to_int(cv));
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
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_DICT) return CLAY_FALSE;
    if (self == other) return CLAY_TRUE;
    int self_size = self->as.dict.map ? (int)ht_size(self->as.dict.map) : 0;
    int other_size = other->as.dict.map ? (int)ht_size(other->as.dict.map) : 0;
    if (self_size != other_size) return CLAY_FALSE;
    if (self_size == 0) return CLAY_TRUE;
    size_t pos = 0;
    ht_key_t key;
    ht_value_t val;
    while (ht_next(self->as.dict.map, &pos, &key, &val)) {
        ht_value_t oval = ht_find(other->as.dict.map, key);
        if (!oval) return CLAY_FALSE;
        ClayValue a = (ClayValue)(uintptr_t)val;
        ClayValue b = (ClayValue)(uintptr_t)oval;
        ClayValue eq = clay_send_val(a, "==", 1, &b);
        if (eq != CLAY_TRUE) return CLAY_FALSE;
    }
    return CLAY_TRUE;
}

static ClayValue dict_neq(ClayObject *self, int argc, ClayValue *argv) {
    return dict_equals(self, argc, argv) == CLAY_TRUE ? CLAY_FALSE : CLAY_TRUE;
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
}
