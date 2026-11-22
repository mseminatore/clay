/*
 * clay_hash.c — the hash library (deps/hash/hash.c) compiled with Clay's
 * settings (clay_hash_config.h). Build this file instead of hash.c itself,
 * so the library's HashTable layout matches the one clay.h sees.
 */
#include "clay_hash_config.h"
#include "../../deps/hash/hash.c"
