/*
 * file.c - File I/O object for Clay
 */
/* mkdtemp is POSIX.1-2008; macOS also needs its own extensions switched on */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
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
#include <errno.h>
#endif
#include <stdlib.h>
#include <string.h>

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

static void raise_file_error(const char *fmt, const char *path) {
    char buf[1200];
    snprintf(buf, sizeof(buf), fmt, path);
    clay_raise(CLAY_VAL(clay_error_new(buf, "FileError")));
}

/* The whole file, NUL-terminated, with its length; raises FileError */
static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        raise_file_error("Cannot open file: %s", path);
        return NULL;
    }
    size_t cap = 65536, n = 0;
    char *data = (char *)malloc(cap);
    size_t got;
    while (data && (got = fread(data + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (n + 1 >= cap) {
            char *bigger = (char *)realloc(data, cap * 2);
            if (!bigger) { free(data); data = NULL; break; }
            data = bigger;
            cap *= 2;
        }
    }
    fclose(f);
    if (!data) {
        raise_file_error("Out of memory reading: %s", path);
        return NULL;
    }
    data[n] = '\0';
    *len = n;
    return data;
}

/* File read(path) - the whole file as a String (any bytes, including NUL) */
static ClayValue file_read(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "read");
    if (!path) return CLAY_NIL;
    size_t len;
    char *data = read_all(path, &len);
    ClayObject *result = clay_string_new_len(data, len);
    free(data);
    return CLAY_VAL(result);
}

/* File readLines(path) - Array of lines, without "\n" / "\r\n" endings */
static ClayValue file_readLines(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "readLines");
    if (!path) return CLAY_NIL;
    size_t len;
    char *data = read_all(path, &len);
    ClayObject *arr = clay_array_new();
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i < len && data[i] != '\n') continue;
        if (i == len && start == len) break;        /* no empty last line */
        size_t end = i;
        if (end > start && data[end - 1] == '\r') end--;
        ClayObject *line = clay_string_new_len(data + start, end - start);
        clay_array_append(arr, CLAY_VAL(line));
        clay_release(line);
        start = i + 1;
    }
    free(data);
    return CLAY_VAL(arr);
}

/* File write(path, content) - write string to file (overwrite) */
static ClayValue file_write(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "write");
    if (!path) return CLAY_NIL;
    if (argc < 2 || !CLAY_IS_TYPE(argv[1], CLAY_TYPE_STRING)) {
        clay_raise(CLAY_VAL(clay_error_new("File write requires path and content strings", "ArgumentError")));
        return CLAY_NIL;
    }
    ClayObject *content = CLAY_OBJ(argv[1]);
    FILE *f = fopen(path, "wb");
    if (!f) {
        raise_file_error("Cannot write file: %s", path);
        return CLAY_NIL;
    }
    size_t n = content->as.string.length;
    size_t wrote = fwrite(clay_string_data(content), 1, n, f);
    int closed = fclose(f);
    if (wrote != n || closed != 0) raise_file_error("Error writing file: %s", path);
    return CLAY_TRUE;
}

/* File append(path, content) - append string to file */
static ClayValue file_append(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "append");
    if (!path) return CLAY_NIL;
    if (argc < 2 || !CLAY_IS_TYPE(argv[1], CLAY_TYPE_STRING)) {
        clay_raise(CLAY_VAL(clay_error_new("File append requires path and content strings", "ArgumentError")));
        return CLAY_NIL;
    }
    ClayObject *content = CLAY_OBJ(argv[1]);
    FILE *f = fopen(path, "ab");
    if (!f) {
        raise_file_error("Cannot append to file: %s", path);
        return CLAY_NIL;
    }
    size_t n = content->as.string.length;
    size_t wrote = fwrite(clay_string_data(content), 1, n, f);
    int closed = fclose(f);
    if (wrote != n || closed != 0) raise_file_error("Error writing file: %s", path);
    return CLAY_TRUE;
}

/* File exists(path) - check if file exists */
static ClayValue file_exists(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_TYPE(argv[0], CLAY_TYPE_STRING)) return CLAY_FALSE;
    struct stat st;
    return CLAY_BOOL(stat(clay_string_data(CLAY_OBJ(argv[0])), &st) == 0);
}

/* File delete(path) - remove a file */
static ClayValue file_delete(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "delete");
    if (!path) return CLAY_NIL;
    struct stat st;
    int is_dir = stat(path, &st) == 0 &&
#ifdef _WIN32
        (st.st_mode & _S_IFDIR);
#else
        S_ISDIR(st.st_mode);
#endif
#ifdef _WIN32
    int rc = is_dir ? _rmdir(path) : remove(path);
#else
    int rc = is_dir ? rmdir(path) : remove(path);
#endif
    if (rc != 0) {
        char buf[1200];
        snprintf(buf, sizeof(buf), is_dir ? "Cannot delete directory (is it empty?): %s"
                                          : "Cannot delete: %s", path);
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

/* --- listDir --- */

typedef struct { char **items; size_t n, cap; } NameList;

static void names_add(NameList *l, const char *name) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = (char **)realloc(l->items, l->cap * sizeof(char *));
    }
    l->items[l->n++] = strdup(name);
}

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* File listDir(path) - the names in a directory (not "." or ".."), sorted */
static ClayValue file_listDir(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "listDir");
    if (!path) return CLAY_NIL;
    NameList names = {0};
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        raise_file_error("Cannot list directory: %s", path);
        return CLAY_NIL;
    }
    do {
        if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0)
            names_add(&names, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) {
        raise_file_error("Cannot list directory: %s", path);
        return CLAY_NIL;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            names_add(&names, entry->d_name);
    closedir(d);
#endif
    if (names.n > 1) qsort(names.items, names.n, sizeof(char *), name_cmp);
    ClayObject *arr = clay_array_new();
    for (size_t i = 0; i < names.n; i++) {
        ClayObject *str = clay_string_new(names.items[i]);
        clay_array_append(arr, CLAY_VAL(str));
        clay_release(str);
        free(names.items[i]);
    }
    free(names.items);
    return CLAY_VAL(arr);
}

/* File copy(from, to) - copy a file's bytes (and, on Unix, its permissions);
 * `to` is replaced if it exists */
static ClayValue file_copy(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *from = get_path_arg(argc, argv, 0, "copy");
    if (!from) return CLAY_NIL;
    const char *to = get_path_arg(argc, argv, 1, "copy");
    if (!to) return CLAY_NIL;
    FILE *in = fopen(from, "rb");
    if (!in) {
        raise_file_error("Cannot open file: %s", from);
        return CLAY_NIL;
    }
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        raise_file_error("Cannot write file: %s", to);
        return CLAY_NIL;
    }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) {
        raise_file_error("Error copying to: %s", to);
        return CLAY_NIL;
    }
#ifndef _WIN32
    struct stat st;
    if (stat(from, &st) == 0) chmod(to, st.st_mode & 07777);
#endif
    return CLAY_TRUE;
}

/* File modified(path) - last modification time, in seconds since the epoch
 * (use with Date fromTimestamp) */
static ClayValue file_modified(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "modified");
    if (!path) return CLAY_NIL;
    struct stat st;
    if (stat(path, &st) != 0) {
        raise_file_error("Cannot stat: %s", path);
        return CLAY_NIL;
    }
    return clay_int((intptr_t)st.st_mtime);
}

/* File cwd - the current working directory */
static ClayValue file_cwd(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    char buf[4096];
#ifdef _WIN32
    if (!_getcwd(buf, sizeof(buf))) return CLAY_NIL;
#else
    if (!getcwd(buf, sizeof(buf))) return CLAY_NIL;
#endif
    return CLAY_VAL(clay_string_new(buf));
}

/* File chdir(path) - change the current working directory */
static ClayValue file_chdir(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *path = get_path_arg(argc, argv, 0, "chdir");
    if (!path) return CLAY_NIL;
#ifdef _WIN32
    int rc = _chdir(path);
#else
    int rc = chdir(path);
#endif
    if (rc != 0) {
        raise_file_error("Cannot change directory to: %s", path);
        return CLAY_NIL;
    }
    return CLAY_TRUE;
}

/* File tempDir - create a new, empty, uniquely named directory in the
 * system's temporary directory and return its path */
static ClayValue file_tempDir(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    char path[1200];
#ifdef _WIN32
    char base[MAX_PATH + 1];
    DWORD n = GetTempPathA(sizeof(base), base);
    if (n == 0 || n > sizeof(base)) snprintf(base, sizeof(base), ".\\");
    static unsigned counter = 0;
    for (int tries = 0; tries < 1000; tries++) {
        snprintf(path, sizeof(path), "%sclay-%lu-%lu-%u", base,
                 (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(),
                 counter++);
        if (_mkdir(path) == 0) return CLAY_VAL(clay_string_new(path));
    }
#else
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    size_t bl = strlen(base);
    snprintf(path, sizeof(path), "%s%sclay-XXXXXX", base,
             (bl > 0 && base[bl - 1] == '/') ? "" : "/");
    if (mkdtemp(path)) return CLAY_VAL(clay_string_new(path));
#endif
    clay_raise(CLAY_VAL(clay_error_new("Cannot create a temporary directory", "FileError")));
    return CLAY_NIL;
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
    clay_set_method(proto, "copy", file_copy);
    clay_set_method(proto, "modified", file_modified);
    clay_set_method(proto, "cwd", file_cwd);
    clay_set_method(proto, "chdir", file_chdir);
    clay_set_method(proto, "tempDir", file_tempDir);
}
