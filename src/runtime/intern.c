/*
 * intern.c - String interning for selector strings
 *
 * Provides clay_intern() which returns a canonical pointer for each
 * unique string. Interned strings can be compared by pointer equality
 * instead of strcmp, enabling fast method cache lookups.
 */
#include "clay.h"

static HashTable *intern_table = NULL;

static int intern_str_compare(ht_key_t a, ht_key_t b) {
    return strcmp((const char *)a, (const char *)b) == 0;
}

const char *clay_intern(const char *name) {
    if (!name) return NULL;

    if (!intern_table) {
        intern_table = ht_create();
        ht_set_hash_func(intern_table, HT_HASH_STRING);
        ht_set_compare_func(intern_table, intern_str_compare);
    }

    /* Check if already interned */
    ht_value_t val;
    if (ht_lookup(intern_table, name, &val)) {
        return (const char *)val;
    }

    /* Not found — intern it */
    char *copy = strdup(name);
    ht_insert(intern_table, copy, (ht_value_t)copy);
    return copy;
}
