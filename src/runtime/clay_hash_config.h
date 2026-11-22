/*
 * clay_hash_config.h — the settings Clay builds the hash library (deps/hash)
 * with, instead of its defaults.
 *
 * HT_TRACK_STATS and HT_DEFAULT_TABLE_SIZE change the HashTable layout, so
 * every file that includes hash.h must see the same values: clay.h includes
 * this header before hash.h, and the library itself is compiled through
 * clay_hash.c, which includes it before hash.c. clay_bootstrap checks that the
 * two layouts agree (clay_hash_layout_ok).
 */
#ifndef CLAY_HASH_CONFIG_H
#define CLAY_HASH_CONFIG_H

/* No collision counters: Clay never reads them, and they cost a little on
 * every probe and 24 bytes in every table */
#define HT_TRACK_STATS 0

/* Every object with slots or methods has its own table, which embeds this
 * many entries before its first grow: small, since most objects have few */
#define HT_DEFAULT_TABLE_SIZE 4

/* Grow at 3/4 full instead of 1/2, so more tables stay in the embedded
 * entries (measured: 22% less memory and ~3% faster VM self-compile together
 * with the two settings above) */
#define HT_LOAD_FACTOR_NUM 3
#define HT_LOAD_FACTOR_DEN 4

#endif /* CLAY_HASH_CONFIG_H */
