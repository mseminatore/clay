/*
 * file.c - File I/O object for Clay
 */
#include "clay.h"
#include <sys/stat.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

#ifdef _WIN32
#include <direct.h>
#include <io.h>
/* Windows directory listing */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <dirent.h>
#endif

/* Helper: extract path string from argv[index], or raise on bad arg */
static const char *get_path_arg(int argc, ClayValue *argv, int index,
                                 const char *method_name) {
    if (argc <= index || !CLAY_IS_PTR(argv[index]) || CLAY_IS_NIL(argv[index])) {
        char buf[128];
        snprintf(buf, sizeof(buf), "File %s requires a path string", method_name);
        clay_raise(CLAY_VAL(clay_error_new(buf, "ArgumentError")));
        return NULL;
    }
    return clay_string_data(CLAY_OBJ(argv[index]));
}

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

/* File delete(path) - remove a file */
static ClayValue file_delete(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "delete");
    if (!path) return CLAY_NIL;
    if (remove(path) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot delete: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    return CLAY_TRUE;
}

/* File rename(oldPath, newPath) - rename/move a file */
static ClayValue file_rename(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *old_path = get_path_arg(argc, argv, 0, "rename");
    if (!old_path) return CLAY_NIL;
    const char *new_path = get_path_arg(argc, argv, 1, "rename");
    if (!new_path) return CLAY_NIL;
    if (rename(old_path, new_path) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot rename: %s -> %s", old_path, new_path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    return CLAY_TRUE;
}

/* File size(path) - return file size in bytes */
static ClayValue file_size(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "size");
    if (!path) return CLAY_NIL;
    struct stat st;
    if (stat(path, &st) != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot stat: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    return clay_int((intptr_t)st.st_size);
}

/* File isDir(path) - true if path is a directory */
static ClayValue file_isDir(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "isDir");
    if (!path) return CLAY_NIL;
    struct stat st;
    if (stat(path, &st) != 0) return CLAY_FALSE;
#ifdef _WIN32
    return CLAY_BOOL(st.st_mode & _S_IFDIR);
#else
    return CLAY_BOOL(S_ISDIR(st.st_mode));
#endif
}

/* File isFile(path) - true if path is a regular file */
static ClayValue file_isFile(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "isFile");
    if (!path) return CLAY_NIL;
    struct stat st;
    if (stat(path, &st) != 0) return CLAY_FALSE;
#ifdef _WIN32
    return CLAY_BOOL(st.st_mode & _S_IFREG);
#else
    return CLAY_BOOL(S_ISREG(st.st_mode));
#endif
}

/* File mkdir(path) - create a directory (no error if already exists) */
static ClayValue file_mkdir(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "mkdir");
    if (!path) return CLAY_NIL;
    /* Check if already exists */
    struct stat st;
    if (stat(path, &st) == 0) return CLAY_TRUE; /* already exists, OK */
    int rc;
#ifdef _WIN32
    rc = _mkdir(path);
#else
    rc = mkdir(path, 0755);
#endif
    if (rc != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot create directory: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    return CLAY_TRUE;
}

/* File listDir(path) - return Array of filenames in directory */
static ClayValue file_listDir(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "listDir");
    if (!path) return CLAY_NIL;
    ClayObject *arr = clay_array_new();
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot list directory: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    do {
        if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
            clay_array_append(arr, CLAY_VAL(clay_string_new(fd.cFileName)));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot list directory: %s", path);
        clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
        return CLAY_NIL;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            clay_array_append(arr, CLAY_VAL(clay_string_new(entry->d_name)));
        }
    }
    closedir(d);
#endif
    return CLAY_VAL(arr);
}

void clay_file_init(ClayObject *proto) {
    clay_set_method(proto, "read", file_read);
    clay_set_method(proto, "readLines", file_readLines);
    clay_set_method(proto, "write", file_write);
    clay_set_method(proto, "append", file_append);
    clay_set_method(proto, "exists", file_exists);
    clay_set_method(proto, "delete", file_delete);
    clay_set_method(proto, "rename", file_rename);
    clay_set_method(proto, "size", file_size);
    clay_set_method(proto, "isDir", file_isDir);
    clay_set_method(proto, "isFile", file_isFile);
    clay_set_method(proto, "mkdir", file_mkdir);
    clay_set_method(proto, "listDir", file_listDir);
}
