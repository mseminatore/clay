/*
 * file.c - File I/O object for Clay
 */
#include "clay.h"

#ifdef _MSC_VER
#define strdup _strdup
#endif

/* File read(path) - read entire file contents as String */
static ClayValue file_read(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) {
        clay_raise(CLAY_VAL(clay_error_new("File read requires a path string", "ArgumentError")));
        return CLAY_NIL;
    }
    const char *path = clay_string_data(CLAY_OBJ(argv[0]));
    FILE *f = fopen(path, "rb");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot open file: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (char *)malloc(size + 1);
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);
    ClayObject *result = clay_string_new(data);
    free(data);
    return CLAY_VAL(result);
}

/* File readLines(path) - read file as Array of line Strings */
static ClayValue file_readLines(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) {
        clay_raise(CLAY_VAL(clay_error_new("File readLines requires a path string", "ArgumentError")));
        return CLAY_NIL;
    }
    const char *path = clay_string_data(CLAY_OBJ(argv[0]));
    FILE *f = fopen(path, "r");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot open file: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    ClayObject *arr = clay_array_new();
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        clay_array_append(arr, CLAY_VAL(clay_string_new(line)));
    }
    fclose(f);
    return CLAY_VAL(arr);
}

/* File write(path, content) - write string to file (overwrite) */
static ClayValue file_write(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2 || !CLAY_IS_PTR(argv[0]) || !CLAY_IS_PTR(argv[1])) {
        clay_raise(CLAY_VAL(clay_error_new("File write requires path and content strings", "ArgumentError")));
        return CLAY_NIL;
    }
    const char *path = clay_string_data(CLAY_OBJ(argv[0]));
    const char *content = clay_string_data(CLAY_OBJ(argv[1]));
    FILE *f = fopen(path, "w");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot write file: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    fputs(content, f);
    fclose(f);
    return CLAY_TRUE;
}

/* File append(path, content) - append string to file */
static ClayValue file_append(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 2 || !CLAY_IS_PTR(argv[0]) || !CLAY_IS_PTR(argv[1])) {
        clay_raise(CLAY_VAL(clay_error_new("File append requires path and content strings", "ArgumentError")));
        return CLAY_NIL;
    }
    const char *path = clay_string_data(CLAY_OBJ(argv[0]));
    const char *content = clay_string_data(CLAY_OBJ(argv[1]));
    FILE *f = fopen(path, "a");
    if (!f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot append to file: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    fputs(content, f);
    fclose(f);
    return CLAY_TRUE;
}

/* File exists(path) - check if file exists */
static ClayValue file_exists(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) {
        return CLAY_FALSE;
    }
    const char *path = clay_string_data(CLAY_OBJ(argv[0]));
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return CLAY_TRUE;
    }
    return CLAY_FALSE;
}

void clay_file_init(ClayObject *proto) {
    clay_set_method(proto, "read", file_read);
    clay_set_method(proto, "readLines", file_readLines);
    clay_set_method(proto, "write", file_write);
    clay_set_method(proto, "append", file_append);
    clay_set_method(proto, "exists", file_exists);
}
