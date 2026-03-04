/*
 * db.c - SQLite database prototype for Clay runtime
 *
 * Provides Db prototype with methods:
 *   Db open(path)    — open/create a SQLite database
 *   db exec(sql)     — execute SQL (INSERT, UPDATE, DELETE, CREATE TABLE)
 *   db query(sql)    — execute SELECT, returns Array of Dicts
 *   db close()       — close the database
 */
#include "clay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include SQLite amalgamation */
#include "sqlite3.h"

/* Store sqlite3* handle as an integer slot (_db_ptr) via intptr_t cast */

/* Db open(path) — open or create a database */
static ClayValue db_open(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || CLAY_IS_INT(argv[0]) || argv[0] == CLAY_NIL)
        return CLAY_NIL;

    const char *path = clay_string_data(CLAY_OBJ(argv[0]));
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Db open: %s\n", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return CLAY_NIL;
    }

    extern ClayObject *clay_Db;
    ClayObject *obj = clay_clone(clay_Db);
    clay_set_slot(obj, "_name", CLAY_VAL(clay_string_new("Db")));
    clay_set_slot(obj, "_db_ptr", clay_int((intptr_t)db));
    clay_set_slot(obj, "_path", CLAY_VAL(clay_string_new(path)));
    return CLAY_VAL(obj);
}

/* Helper: extract sqlite3* from object */
static sqlite3 *get_db(ClayObject *self) {
    ClayValue v = clay_get_slot(self, "_db_ptr");
    if (!CLAY_IS_INT(v)) return NULL;
    return (sqlite3 *)(intptr_t)clay_to_int(v);
}

/* db exec(sql) — execute non-query SQL */
static ClayValue db_exec(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || argv[0] == CLAY_NIL)
        return CLAY_NIL;

    sqlite3 *db = get_db(self);
    if (!db) {
        fprintf(stderr, "Db exec: database not open\n");
        return CLAY_NIL;
    }

    const char *sql = clay_string_data(CLAY_OBJ(argv[0]));
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Db exec: %s\n", err_msg ? err_msg : "error");
        if (err_msg) sqlite3_free(err_msg);
        return CLAY_NIL;
    }
    return CLAY_VAL(self);
}

/* db query(sql) — execute SELECT, return Array of Dicts */
static ClayValue db_query(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || argv[0] == CLAY_NIL)
        return CLAY_VAL(clay_array_new());

    sqlite3 *db = get_db(self);
    if (!db) {
        fprintf(stderr, "Db query: database not open\n");
        return CLAY_VAL(clay_array_new());
    }

    const char *sql = clay_string_data(CLAY_OBJ(argv[0]));
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Db query: %s\n", sqlite3_errmsg(db));
        return CLAY_VAL(clay_array_new());
    }

    ClayObject *results = clay_array_new();
    int col_count = sqlite3_column_count(stmt);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ClayObject *row = clay_dict_new();
        for (int i = 0; i < col_count; i++) {
            const char *col_name = sqlite3_column_name(stmt, i);
            int col_type = sqlite3_column_type(stmt, i);
            ClayValue val;

            switch (col_type) {
                case SQLITE_INTEGER:
                    val = clay_int(sqlite3_column_int64(stmt, i));
                    break;
                case SQLITE_FLOAT:
                    val = clay_float_tag((float)sqlite3_column_double(stmt, i));
                    break;
                case SQLITE_TEXT:
                    val = CLAY_VAL(clay_string_new(
                        (const char *)sqlite3_column_text(stmt, i)));
                    break;
                case SQLITE_NULL:
                default:
                    val = CLAY_NIL;
                    break;
            }
            clay_dict_set(row, col_name, val);
        }
        clay_array_append(results, CLAY_VAL(row));
    }

    sqlite3_finalize(stmt);
    return CLAY_VAL(results);
}

/* db close() — close the database */
static ClayValue db_close(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    sqlite3 *db = get_db(self);
    if (db) {
        sqlite3_close(db);
        clay_set_slot(self, "_db_ptr", clay_int(0));
    }
    return CLAY_NIL;
}

/* db lastInsertId() — return rowid of last INSERT */
static ClayValue db_last_insert_id(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    sqlite3 *db = get_db(self);
    if (!db) return CLAY_NIL;
    return clay_int((intptr_t)sqlite3_last_insert_rowid(db));
}

/* db escape(str) — escape single quotes for safe SQL embedding */
static ClayValue db_escape(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || CLAY_IS_INT(argv[0]) || argv[0] == CLAY_NIL)
        return CLAY_VAL(clay_string_new(""));

    const char *src = clay_string_data(CLAY_OBJ(argv[0]));
    size_t len = strlen(src);
    /* Worst case: every char is a quote → 2x length */
    char *buf = (char *)malloc(len * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\'') buf[j++] = '\''; /* double the quote */
        buf[j++] = src[i];
    }
    buf[j] = '\0';
    ClayValue result = CLAY_VAL(clay_string_new(buf));
    free(buf);
    return result;
}

/* Register Db methods */
void clay_db_init(ClayObject *db) {
    clay_set_method(db, "open", db_open);
    clay_set_method(db, "exec", db_exec);
    clay_set_method(db, "query", db_query);
    clay_set_method(db, "close", db_close);
    clay_set_method(db, "escape", db_escape);
    clay_set_method(db, "lastInsertId", db_last_insert_id);
}
