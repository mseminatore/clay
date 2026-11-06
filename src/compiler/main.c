/*
 * main.c - Clay compiler driver
 *
 * Usage: clay [options] [source.clay]
 *   --compile     Compile to native binary via C codegen
 *   -o <output>   Output binary name (implies --compile)
 *   -c            Emit C code only (implies --compile)
 *   --ast         Print AST and exit
 *   --tokens      Print the lexer's tokens and exit
 *   --bytecode    Print the compiled bytecode and exit
 *   --repl        Interactive REPL (C compilation)
 *   --vm-repl     Interactive REPL (bytecode VM, faster)
 *
 * With no arguments, launches the VM REPL.
 * With a source file, runs it in the bytecode VM.
 */
/* glibc only declares realpath() with _DEFAULT_SOURCE (or _XOPEN_SOURCE);
 * the build defines just _POSIX_C_SOURCE, which would leave it implicitly
 * declared as returning int — truncating the returned pointer. */
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
/* Minimal Win32 console API declarations to avoid windows.h name conflicts */
typedef void *HANDLE;
typedef unsigned long DWORD;
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) int __stdcall GetConsoleMode(HANDLE, DWORD *);
__declspec(dllimport) int __stdcall SetConsoleMode(HANDLE, DWORD);
__declspec(dllimport) DWORD __stdcall GetModuleFileNameA(void *, char *, DWORD);
#include <process.h>   /* _getpid */
#else
#include <unistd.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#include "crossline.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "bc_compiler.h"
#include "bricks.h"
#include "ast.h"
#include "dap.h"

/* Plugin auto-load */
extern void clay_plugin_auto_load(const char *exe_path);
static const char *g_exe_path = NULL;

#ifdef _MSC_VER
#define strdup _strdup
#endif

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "clay: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Replace .clay extension with new extension */
static char *replace_ext(const char *path, const char *new_ext) {
    /* the extension is a dot in the last path component only */
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    const char *dot = strrchr(base, '.');
    size_t base_len = dot ? (size_t)(dot - path) : strlen(path);
    char *result = (char *)malloc(base_len + strlen(new_ext) + 1);
    memcpy(result, path, base_len);
    strcpy(result + base_len, new_ext);
    return result;
}

/* Absolute path of the running clay executable and its directory (with a
 * trailing separator). Resolved from the OS rather than argv[0], so it also
 * works when clay is found via PATH. Empty if unknown. */
static char g_exe_path_buf[1024] = "";
static char g_exe_dir[1024] = "";

static void resolve_exe_path(const char *argv0) {
    char path[1024] = "";
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (n == 0 || n >= sizeof(path)) {
        path[0] = '\0';
        if (argv0) _fullpath(path, argv0, sizeof(path));
    }
#else
    char raw[1024] = "";
#if defined(__APPLE__)
    uint32_t size = (uint32_t)sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) raw[0] = '\0';   /* may be relative */
#else
    ssize_t len = readlink("/proc/self/exe", raw, sizeof(raw) - 1);
    if (len > 0) {
        raw[len] = '\0';
        snprintf(path, sizeof(path), "%s", raw);   /* already canonical and absolute */
    } else {
        raw[0] = '\0';
    }
#endif
    if (!path[0]) {
        if (!raw[0] && argv0) snprintf(raw, sizeof(raw), "%s", argv0);
        char *rp = raw[0] ? realpath(raw, NULL) : NULL;
        snprintf(path, sizeof(path), "%s", rp ? rp : raw);
        free(rp);
    }
#endif
    snprintf(g_exe_path_buf, sizeof(g_exe_path_buf), "%s", path);
    snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", path);
    char *sep = strrchr(g_exe_dir, '/');
#ifdef _WIN32
    char *bsep = strrchr(g_exe_dir, '\\');
    if (bsep && (!sep || bsep > sep)) sep = bsep;
#endif
    if (sep) sep[1] = '\0'; else g_exe_dir[0] = '\0';
}

/* Pointer to the last path separator in `p` ('/' or, on Windows, '\') */
static char *last_sep(char *p) {
    char *sep = strrchr(p, '/');
#ifdef _WIN32
    char *bsep = strrchr(p, '\\');
    if (bsep && (!sep || bsep > sep)) sep = bsep;
#endif
    return sep;
}

/* Resolve the Clay source tree root: the nearest directory, starting at the
 * binary's own, that holds src/runtime/clay.h. The binary is usually
 * build/clay or build/<Config>/clay (CMake), or clay at the root (Makefile). */
static void resolve_clay_dir(const char *argv0, char *clay_dir, size_t sz) {
    (void)argv0;
    snprintf(clay_dir, sz, ".");
    if (!g_exe_dir[0]) return;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", g_exe_dir);
    size_t len = strlen(dir);
    if (len > 0) dir[len - 1] = '\0';                 /* drop trailing separator */
    for (int up = 0; up < 4; up++) {
        char probe[1100];
        snprintf(probe, sizeof(probe), "%s/src/runtime/clay.h", dir);
        FILE *f = fopen(probe, "r");
        if (f) {
            fclose(f);
            snprintf(clay_dir, sz, "%s", dir);
            return;
        }
        char *sep = last_sep(dir);
        if (!sep) return;
        *sep = '\0';
    }
}

/* --- Native compile configuration (clay-native.cfg next to the binary) ---
 * Written by CMake: the C compiler, flags and prebuilt runtime libraries of
 * this build, so --compile links instead of recompiling the runtime. */
typedef struct {
    int  loaded;
    char kind[16];      /* "cc" or "msvc" */
    char cc[1024];
    char cflags[4096];
    char libs[4096];
} NativeCfg;

static int load_native_cfg(NativeCfg *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    if (!g_exe_dir[0]) return 0;
    char path[1200];
    snprintf(path, sizeof(path), "%sclay-native.cfg", g_exe_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[4200];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (line[0] == '#' || !line[0]) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *val = eq + 1;
        if (strcmp(line, "kind") == 0)        snprintf(cfg->kind, sizeof(cfg->kind), "%s", val);
        else if (strcmp(line, "cc") == 0)     snprintf(cfg->cc, sizeof(cfg->cc), "%s", val);
        else if (strcmp(line, "cflags") == 0) snprintf(cfg->cflags, sizeof(cfg->cflags), "%s", val);
        else if (strcmp(line, "libs") == 0)   snprintf(cfg->libs, sizeof(cfg->libs), "%s", val);
    }
    fclose(f);
    cfg->loaded = cfg->cc[0] && cfg->libs[0];
    return cfg->loaded;
}

/* Runtime sources compiled into every native binary. Keep in sync with
 * RUNTIME_SOURCES in CMakeLists.txt (and the Makefile). Paths are relative to
 * the Clay source tree and use '/' — converted for Windows below. */
static const char *const clay_native_sources[] = {
    "src/runtime/object.c",   "src/runtime/memory.c",   "src/runtime/dispatch.c",
    "src/runtime/tagged.c",   "src/runtime/string.c",   "src/runtime/number.c",
    "src/runtime/block.c",    "src/runtime/array.c",    "src/runtime/error.c",
    "src/runtime/clay_dict.c","src/runtime/range.c",    "src/runtime/math.c",
    "src/runtime/file.c",     "src/runtime/ffi.c",      "src/runtime/date.c",
    "src/runtime/http.c",     "src/runtime/regex.c",    "src/runtime/db.c",
    "src/runtime/bootstrap.c","src/runtime/intern.c",   "src/runtime/graphics.c",
    "src/runtime/vec3.c",     "src/runtime/thread.c",   "src/runtime/plugin.c",
    "src/runtime/fiber.c",    "src/runtime/vm_bridge.c",
    "src/vm/generated/vm.c",  "src/vm/generated/native.c", "src/vm/generated/disasm.c",
    "deps/crossline/crossline.c",
    "src/runtime/fiber_ctx.c",
#ifndef _WIN32
#if defined(__x86_64__) || defined(_M_X64)
    "src/runtime/fiber_amd64.S",
#elif defined(__aarch64__)
    "src/runtime/fiber_arm64.S",
#endif
#endif
    "deps/hash/hash.c",
    "deps/sqlite/sqlite3.c",
    NULL
};

/* Append `"<clay_dir>/<rel>" ` to cmd (with '\' separators on Windows) */
static int append_source(char *cmd, size_t sz, const char *clay_dir, const char *rel) {
    size_t len = strlen(cmd);
    int n = snprintf(cmd + len, sz - len, "\"%s/%s\" ", clay_dir, rel);
    if (n < 0 || (size_t)n >= sz - len) return -1;
#ifdef _WIN32
    for (char *c = cmd + len; *c; c++) if (*c == '/') *c = '\\';
#endif
    return 0;
}

#ifdef _WIN32
/* Locate the newest Visual Studio installation (for vcvarsall.bat) */
static int find_vs_path(char *vs_path, size_t sz) {
    vs_path[0] = '\0';
    const char *pf86 = getenv("ProgramFiles(x86)");
    if (!pf86) pf86 = "C:\\Program Files (x86)";
    char vswhere_cmd[512];
    snprintf(vswhere_cmd, sizeof(vswhere_cmd),
        "\"%s\\Microsoft Visual Studio\\Installer\\vswhere.exe\" "
        "-latest -property installationPath", pf86);
    FILE *fp = _popen(vswhere_cmd, "r");
    if (fp) {
        if (fgets(vs_path, (int)sz, fp)) {
            size_t len = strlen(vs_path);
            while (len > 0 && (vs_path[len-1] == '\n' || vs_path[len-1] == '\r'))
                vs_path[--len] = '\0';
        }
        _pclose(fp);
    }
    if (vs_path[0] == '\0') {
        fprintf(stderr, "clay: cannot find Visual Studio installation\n");
        return -1;
    }
    return 0;
}
#endif

/* Compile a C file to an executable. Returns 0 on success.
 * Uses clay-native.cfg (written by the CMake build) to link the prebuilt
 * runtime; without it, falls back to compiling the runtime from source. */
static int compile_c_file(const char *c_file, const char *output_name,
                          const char *clay_dir, int quiet) {
    char cmd[16384];
    NativeCfg cfg;
    if (load_native_cfg(&cfg)) {
#ifdef _WIN32
        if (strcmp(cfg.kind, "msvc") == 0) {
            char vs_path[512];
            if (find_vs_path(vs_path, sizeof(vs_path)) != 0) return 1;
            snprintf(cmd, sizeof(cmd),
                "cmd /c \"\"%s\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x64 >nul 2>&1 && "
                "\"%s\" %s %s /Fo\"%%TEMP%%\\\\\" \"%s\" /Fe:\"%s\" %s %s\"",
                vs_path, cfg.cc, cfg.cflags, quiet ? "/W0" : "/W3",
                c_file, output_name, cfg.libs, quiet ? ">nul 2>&1" : "");
            fflush(stdout);
            return system(cmd);
        }
#endif
        snprintf(cmd, sizeof(cmd), "\"%s\" %s %s \"%s\" %s -o \"%s\"",
                 cfg.cc, cfg.cflags, quiet ? "-w" : "-Wall", c_file, cfg.libs, output_name);
        fflush(stdout);
        return system(cmd);
    }

#ifdef _WIN32
    char vs_path[512];
    if (find_vs_path(vs_path, sizeof(vs_path)) != 0) return 1;
    snprintf(cmd, sizeof(cmd),
        "cmd /c \"\"%s\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x64 >nul 2>&1 && "
        "cl /nologo %s /O2 /GF "
        "/I\"%s\\src\\runtime\" /I\"%s\\deps\\hash\" /I\"%s\\deps\\sqlite\" "
        "/I\"%s\\src\\vm\\generated\" /I\"%s\\deps\\crossline\" "
        "/D_CRT_SECURE_NO_WARNINGS "
        "/Fo\"%%TEMP%%\\\\\" "
        "\"%s\" ",
        vs_path, quiet ? "/W0" : "/W3",
        clay_dir, clay_dir, clay_dir, clay_dir, clay_dir, c_file);
    for (int i = 0; clay_native_sources[i]; i++)
        if (append_source(cmd, sizeof(cmd), clay_dir, clay_native_sources[i]) != 0)
            return 1;
    size_t len = strlen(cmd);
    snprintf(cmd + len, sizeof(cmd) - len,
        "/Fe:\"%s\" winhttp.lib ws2_32.lib %s\"",
        output_name, quiet ? ">nul 2>&1" : "");
#else
    snprintf(cmd, sizeof(cmd),
        "cc -std=c99 -O2 %s "
#ifdef __linux__
        "-D_DEFAULT_SOURCE "   /* POSIX + BSD declarations under -std=c99 */
#endif
        "-I\"%s/src/runtime\" -I\"%s/deps/hash\" -I\"%s/deps/sqlite\" "
        "-I\"%s/src/vm/generated\" -I\"%s/deps/crossline\" "
        "\"%s\" ",
        quiet ? "-w" : "-Wall",
        clay_dir, clay_dir, clay_dir, clay_dir, clay_dir, c_file);
    for (int i = 0; clay_native_sources[i]; i++)
        if (append_source(cmd, sizeof(cmd), clay_dir, clay_native_sources[i]) != 0)
            return 1;
    size_t len = strlen(cmd);
    snprintf(cmd + len, sizeof(cmd) - len,
#ifdef __linux__
        "-lm -ldl -lpthread -o \"%s\"",
#else
        "-lm -ldl -o \"%s\"",
#endif
        output_name);
#endif
    fflush(stdout);
    return system(cmd);
}


/* --- REPL --- */

static void clay_completion(const char *buf, crossline_completions_t *pComp) {
    static const char *builtins[] = {
        "Object", "Number", "String", "Array", "Dict", "File",
        "System", "Date", "Http", "Regex", "Fiber", "Db", "FFI", "Math",
        "true", "false", "nil", "self", "super",
        "import", "clone", "println", "print",
        NULL
    };
    size_t len = strlen(buf);
    for (int i = 0; builtins[i]; i++) {
        if (strncmp(buf, builtins[i], len) == 0) {
            crossline_completion_add(pComp, builtins[i], NULL);
        }
    }
}

static const char *clay_history_path(void) {
    static char path[1024];
    if (path[0]) return path;
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (!home) home = ".";
    snprintf(path, sizeof(path), "%s/.clay_history", home);
    return path;
}

static void clay_repl_init(void) {
    crossline_completion_register(clay_completion);
    crossline_history_load(clay_history_path());
}

/* Count unmatched braces/parens/brackets to detect multi-line input */
static int count_open(const char *s) {
    int depth = 0;
    int in_string = 0;
    for (; *s; s++) {
        if (*s == '"' && !in_string) { in_string = 1; continue; }
        if (*s == '"' && in_string) { in_string = 0; continue; }
        if (*s == '\\' && in_string) { s++; continue; }
        if (in_string) continue;
        if (*s == '(' || *s == '{' || *s == '[') depth++;
        if (*s == ')' || *s == '}' || *s == ']') depth--;
    }
    return depth;
}

/* Check if a statement is a bare expression (not an assignment) */
static int is_bare_expression(AstNode *node) {
    return node && node->type != NODE_ASSIGN && node->type != NODE_IMPORT;
}

/* Clone a program's statement list into a new program node.
 * Shallow copy — does NOT own the child nodes. */
static AstNode *clone_program(AstNode *src) {
    AstNode *p = ast_new(NODE_PROGRAM, 1);
    nodelist_init(&p->as.program.stmts);
    for (int i = 0; i < src->as.program.stmts.count; i++) {
        nodelist_push(&p->as.program.stmts, src->as.program.stmts.items[i]);
    }
    return p;
}

static int run_repl(const char *argv0) {
    char clay_dir[1024];
    resolve_clay_dir(argv0, clay_dir, sizeof(clay_dir));

    printf("Clay REPL (type 'quit' or Ctrl+Z to exit)\n\n");

    clay_repl_init();

    /* Accumulated program */
    AstNode *accumulated = ast_new(NODE_PROGRAM, 1);
    nodelist_init(&accumulated->as.program.stmts);

    /* Temp file paths — in the system temp directory, not the source tree */
    char c_path[1024], exe_path[1024];
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(c_path, sizeof(c_path), "%s\\_clay_repl_%d.c", tmp, (int)_getpid());
    snprintf(exe_path, sizeof(exe_path), "%s\\_clay_repl_%d.exe", tmp, (int)_getpid());
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(c_path, sizeof(c_path), "%s/_clay_repl_%d.c", tmp, (int)getpid());
    snprintf(exe_path, sizeof(exe_path), "%s/_clay_repl_%d", tmp, (int)getpid());
#endif

    char line_buf[4096];
    char input_buf[16384];

    while (1) {
        /* Read input (possibly multi-line) */
        input_buf[0] = '\0';
        int open_depth = 0;
        int first_line = 1;

        while (1) {
            char *prompt = first_line ? "clay> " : "  ... ";
            crossline_prompt_color_set(CROSSLINE_FGCOLOR_CYAN);
            char *line = crossline_readline(prompt, line_buf, sizeof(line_buf));
            if (!line) { printf("\n"); goto cleanup; }

            /* Check for quit */
            if (first_line) {
                char *trimmed = line;
                while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
                size_t tlen = strlen(trimmed);
                while (tlen > 0 && (trimmed[tlen-1] == '\n' || trimmed[tlen-1] == '\r'))
                    tlen--;
                if ((tlen == 4 && strncmp(trimmed, "quit", 4) == 0) ||
                    (tlen == 4 && strncmp(trimmed, "exit", 4) == 0)) {
                    goto cleanup;
                }
            }

            /* Append to input buffer */
            size_t ll = strlen(line);
            if (strlen(input_buf) + ll < sizeof(input_buf) - 2) {
                strcat(input_buf, line);
                size_t cur = strlen(input_buf);
                if (cur > 0 && input_buf[cur - 1] != '\n') {
                    input_buf[cur] = '\n';
                    input_buf[cur + 1] = '\0';
                }
            }

            /* count_open scans the whole accumulated buffer, so assign rather
             * than accumulate — otherwise a multi-line block never closes */
            open_depth = count_open(input_buf);
            first_line = 0;

            if (open_depth <= 0) break;
        }

        /* Strip trailing whitespace */
        size_t ilen = strlen(input_buf);
        while (ilen > 0 && (input_buf[ilen-1] == '\n' || input_buf[ilen-1] == '\r'
               || input_buf[ilen-1] == ' ' || input_buf[ilen-1] == '\t'))
            ilen--;
        input_buf[ilen] = '\0';

        if (ilen == 0) continue; /* empty input */

        crossline_history_save(clay_history_path());

        /* Parse the new input */
        Parser parser;
        parser_init(&parser, input_buf, "<repl>");
        AstNode *new_prog = parser_parse(&parser);
        parser_free_imports(&parser);

        if (parser.had_error || !new_prog || new_prog->as.program.stmts.count == 0) {
            fprintf(stderr, "  (parse error — input discarded)\n");
            if (new_prog) ast_free(new_prog);
            continue;
        }

        /* Build a trial program: accumulated definitions + ALL new statements.
         * Accumulated contains only assignments/clones (state).
         * New bare expressions get auto-println on the last one. */
        AstNode *trial = clone_program(accumulated);

        int new_count = new_prog->as.program.stmts.count;
        for (int i = 0; i < new_count; i++) {
            AstNode *stmt = new_prog->as.program.stmts.items[i];

            if (i == new_count - 1 && is_bare_expression(stmt)) {
                /* Wrap: expr println() so result is visible */
                AstNode *println_send = ast_new(NODE_SEND, stmt->line);
                println_send->as.send.receiver = stmt;
                println_send->as.send.message = strdup("println");
                nodelist_init(&println_send->as.send.args);
                nodelist_push(&trial->as.program.stmts, println_send);
            } else {
                nodelist_push(&trial->as.program.stmts, stmt);
            }
        }

        /* Generate C */
        FILE *out = fopen(c_path, "w");
        if (!out) {
            fprintf(stderr, "  (cannot write temp file)\n");
            trial->as.program.stmts.items = NULL;
            trial->as.program.stmts.count = 0;
            ast_free(trial);
            ast_free(new_prog);
            continue;
        }

        CodeGen cg;
        codegen_init(&cg, out, "<repl>");
        codegen_emit(&cg, trial);
        fclose(out);

        /* Compile (suppress cl.exe output by redirecting to nul) */
        char quiet_c[1024], quiet_exe[1024];
        snprintf(quiet_c, sizeof(quiet_c), "%s", c_path);
        snprintf(quiet_exe, sizeof(quiet_exe), "%s", exe_path);
        int rc = compile_c_file(quiet_c, quiet_exe, clay_dir, 1);
        if (rc != 0) {
            fprintf(stderr, "  (compilation error — input discarded)\n");
            trial->as.program.stmts.items = NULL;
            trial->as.program.stmts.count = 0;
            ast_free(trial);
            ast_free(new_prog);
            continue;
        }

        /* Run */
        char run_cmd[2048];
        snprintf(run_cmd, sizeof(run_cmd), "\"%s\"", exe_path);
        fflush(stdout);  /* keep our output ordered before the child's */
        system(run_cmd);

        /* Success! Accumulate only assignments (state-preserving statements) */
        for (int i = 0; i < new_count; i++) {
            AstNode *stmt = new_prog->as.program.stmts.items[i];
            if (stmt->type == NODE_ASSIGN) {
                nodelist_push(&accumulated->as.program.stmts, stmt);
                new_prog->as.program.stmts.items[i] = NULL; /* take ownership */
            }
        }

        /* Free the println wrapper if we made one */
        if (new_count > 0 && trial->as.program.stmts.count > 0) {
            AstNode *last = trial->as.program.stmts.items[trial->as.program.stmts.count - 1];
            if (last && last->type == NODE_SEND &&
                last->as.send.message &&
                strcmp(last->as.send.message, "println") == 0) {
                last->as.send.receiver = NULL; /* don't free the user's expression */
                ast_free(last);
            }
        }
        trial->as.program.stmts.items = NULL;
        trial->as.program.stmts.count = 0;
        ast_free(trial);
        ast_free(new_prog);
    }

cleanup:
    /* Clean up temp files */
    remove(c_path);
    remove(exe_path);
    /* Note: accumulated program contains all user AST nodes — leak is acceptable on exit */
    printf("Bye!\n");
    return 0;
}

/* --- VM-based REPL (bytecode interpreter) --- */

/* ANSI color codes */
#define C_RESET   "\033[0m"
#define C_CYAN    "\033[36m"
#define C_GREEN   "\033[32m"
#define C_RED     "\033[31m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"

static int g_use_color = 0;

static void repl_enable_color(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        g_use_color = 1;
    }
    /* Also enable for stderr */
    h = GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#else
    if (isatty(fileno(stdout))) g_use_color = 1;
#endif
}

#define C(code) (g_use_color ? (code) : "")

#define C_YELLOW  "\033[33m"

/* --- Stepping debugger state --- */
typedef struct {
    BcCompiler *bc;
    const char *source;    /* source text for current input */
    int stepping;          /* 1 = step mode active */
    int last_line;         /* last printed source line */
    int continue_mode;     /* 1 = continue until end */
} StepState;

static StepState g_step;

/* Split source text into lines, return line N (1-based). Caller must free. */
static char *get_source_line(const char *src, int line_num) {
    if (!src || line_num < 1) return NULL;
    const char *p = src;
    int cur = 1;
    while (cur < line_num && *p) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line_num) return NULL;
    const char *end = p;
    while (*end && *end != '\n') end++;
    size_t len = (size_t)(end - p);
    char *result = (char *)malloc(len + 1);
    memcpy(result, p, len);
    result[len] = '\0';
    return result;
}

static void step_callback(void *vm_ptr, size_t ip) {
    clayvm_vm_t *vm = (clayvm_vm_t *)vm_ptr;
    (void)vm;

    if (!g_step.stepping || g_step.continue_mode) return;
    if (!g_step.bc || ip >= (size_t)g_step.bc->len) return;

    int line = g_step.bc->line_map[ip];
    if (line == g_step.last_line) return; /* same source line — skip */
    g_step.last_line = line;

    /* Show source line */
    char *src_line = get_source_line(g_step.source, line);
    if (src_line) {
        printf("%s%s  %d |%s %s\n", C(C_DIM), C(C_YELLOW), line, C(C_RESET), src_line);
        free(src_line);
    }

    /* Interactive command loop */
    for (;;) {
        printf("%s[step]%s ", C(C_YELLOW), C(C_RESET));
        fflush(stdout);
        char cmd[64];
        if (!fgets(cmd, sizeof(cmd), stdin)) { g_step.continue_mode = 1; return; }
        char *t = cmd;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '\n' || *t == '\r' || *t == 'n' || *t == '\0') {
            return; /* next */
        } else if (*t == 'c') {
            g_step.continue_mode = 1;
            return;
        } else if (*t == 's') {
            /* show stack */
            printf("%sStack (%d):%s\n", C(C_DIM), vm->sp, C(C_RESET));
            for (int i = vm->sp - 1; i >= 0 && i >= vm->sp - 8; i--) {
                clayvm_value_t v = vm->stack[i];
                if (v.type == CLAYVM_VAL_INT64) {
                    printf("  [%d] %lld\n", i, (long long)v.as.i64);
                } else if (v.type == CLAYVM_VAL_BOOL) {
                    printf("  [%d] %s\n", i, v.as.boolean ? "true" : "false");
                } else if (v.type == CLAYVM_VAL_NIL) {
                    printf("  [%d] nil\n", i);
                } else if (v.type == CLAYVM_VAL_OBJECT) {
                    ClayValue cv = vm_to_clay(v);
                    ClayValue sv = clay_send_val(cv, "toString", 0, NULL);
                    printf("  [%d] %s\n", i, clay_string_data(CLAY_OBJ(sv)));
                } else {
                    printf("  [%d] <unknown>\n", i);
                }
            }
        } else if (*t == 'q') {
            vm->running = 0;
            g_step.continue_mode = 1;
            return;
        } else {
            printf("  n=next  c=continue  s=stack  q=quit\n");
        }
    }
}

static int run_vm_repl(int trace_mode, int step_mode, int mem_stats) {
    clay_bootstrap();
    clay_plugin_auto_load(g_exe_path);
    repl_enable_color();

    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    if (trace_mode) vm.trace_enabled = 1;

    BcCompiler bc;
    bc_compiler_init(&bc, &vm);
    bc.source_file = "<repl>";
    clay_source_file = "<repl>";

    extern void bc_worker_thread_init(void);
    extern void bc_set_main_compiler(BcCompiler *bc);
    clay_thread_set_vm_init(bc_worker_thread_init);
    bc_set_main_compiler(&bc);

    if (step_mode) {
        g_step.stepping = 1;
        g_step.last_line = 0;
        g_step.continue_mode = 0;
        g_step.bc = &bc;
        g_step.source = NULL;
        clayvm_vm_set_step_callback(&vm, step_callback);
    }

    printf("%s%sClay VM REPL%s v0.1 (type 'quit' or 'exit' to leave)\n",
           C(C_BOLD), C(C_CYAN), C(C_RESET));

    clay_repl_init();

    char line_buf[4096];
    char input[16384];
    int input_len = 0;
    int continuation = 0;

    for (;;) {
        char *prompt = continuation ? "... " : ">>> ";
        if (g_use_color)
            crossline_prompt_color_set(CROSSLINE_FGCOLOR_CYAN);
        char *line = crossline_readline(prompt, line_buf, sizeof(line_buf));
        if (!line) break;  /* EOF / Ctrl-D */

        /* Check for quit */
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        size_t tl = strlen(trimmed);
        while (tl > 0 && (trimmed[tl-1] == '\n' || trimmed[tl-1] == '\r')) tl--;
        if ((tl == 4 && strncmp(trimmed, "quit", 4) == 0) ||
            (tl == 4 && strncmp(trimmed, "exit", 4) == 0)) {
            break;
        }

        /* Append to input buffer */
        size_t ll = strlen(line);
        if (input_len + (int)ll < (int)sizeof(input) - 2) {
            memcpy(input + input_len, line, ll);
            input_len += (int)ll;
            if (input_len > 0 && input[input_len - 1] != '\n') {
                input[input_len++] = '\n';
            }
            input[input_len] = '\0';
        }

        /* Track braces/parens for multi-line (string-aware) */
        int open = count_open(input);

        if (open > 0) {
            continuation = 1;
            continue;
        }
        continuation = 0;

        /* Parse the input */
        Parser parser;
        parser_init(&parser, input, "<repl>");
        AstNode *program = parser_parse(&parser);

        if (!program || parser.had_error) {
            fprintf(stderr, "%s%sclay: parse error%s\n", C(C_BOLD), C(C_RED), C(C_RESET));
            if (program) ast_free(program);
            input_len = 0;
            continue;
        }

        /* Reset bytecode (keep constants & globals) */
        bc.len = 0;
        bc.had_error = 0;

        /* Compile to bytecode */
        if (bc_compile(&bc, program) != 0) {
            fprintf(stderr, "clay: compilation error\n");
            ast_free(program);
            input_len = 0;
            continue;
        }

        /* Execute with error recovery */
        vm.sp = 0;
        ClayTryFrame _tf;
        int caught = clay_try_push(&_tf);
        if (caught != 0) {
            /* An error was raised during execution */
            ClayValue err = clay_error_stack.current_error;
            if (!CLAY_IS_NIL(err) && CLAY_IS_PTR(err)) {
                ClayValue msg = clay_send_val(err, "message", 0, NULL);
                fprintf(stderr, "%s%sError: %s%s\n", C(C_BOLD), C(C_RED),
                        clay_string_data(CLAY_OBJ(msg)), C(C_RESET));
                clay_release(CLAY_OBJ(msg));
            } else {
                fprintf(stderr, "%s%sError: unknown%s\n", C(C_BOLD), C(C_RED), C(C_RESET));
            }
            clay_release_val(err);
        } else {
            /* Set source for step debugger */
            if (step_mode) {
                g_step.source = input;
                g_step.last_line = 0;
                g_step.continue_mode = 0;
            }
            int result;
            if (step_mode) {
                /* Step-based execution for interactive debugging */
                vm.code = bc.code;
                vm.code_len = bc.len;
                vm.ip = 0;
                vm.sp = 0;
                vm.running = 1;
                result = 0;
                while (vm.running && vm.ip < vm.code_len) {
                    if (clayvm_vm_step(&vm) < 0) break;
                }
            } else {
                result = clayvm_vm_run(&vm, bc.code, bc.len);
            }
            clay_try_pop();

            if (result == 0 && vm.sp > 0) {
                /* Auto-print result */
                clayvm_value_t top = vm.stack[vm.sp - 1];
                if (top.type != CLAYVM_VAL_NIL) {
                    ClayValue cv = vm_to_clay(top);
                    if (CLAY_IS_INT(cv)) {
                        printf("%s=>%s %lld\n", C(C_GREEN), C(C_RESET), (long long)clay_to_int(cv));
                    } else if (cv == CLAY_TRUE) {
                        printf("%s=>%s true\n", C(C_GREEN), C(C_RESET));
                    } else if (cv == CLAY_FALSE) {
                        printf("%s=>%s false\n", C(C_GREEN), C(C_RESET));
                    } else if (!CLAY_IS_NIL(cv)) {
                        ClayValue sv = clay_send_val(cv, "toString", 0, NULL);
                        printf("%s=>%s %s\n", C(C_GREEN), C(C_RESET), clay_string_data(CLAY_OBJ(sv)));
                        clay_release(CLAY_OBJ(sv));
                    }
                }
            }
        }

        ast_free(program);
        crossline_history_save(clay_history_path());
        input_len = 0;
    }

    printf("%sBye!%s\n", C(C_DIM), C(C_RESET));
    if (mem_stats) {
        printf("\n%s--- Memory Stats ---%s\n", C(C_DIM), C(C_RESET));
        printf("  Objects allocated: %ld\n", clay_alloc_count);
        printf("  Objects freed:     %ld\n", clay_dealloc_count);
        printf("  Still live:        %ld\n", clay_alloc_count - clay_dealloc_count);
    }
    clayvm_vm_free(&vm);
    return 0;
}

/* --- --bytecode: what the bytecode compiler produced ---
 * The constant pool, the top-level code, every chunk and method slot, with
 * line numbers where they change. The Clay compiler (selfhost/clay-bytecode)
 * prints the same format; CTest compares them. */
static void dump_escaped(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\') fputs("\\\\", stdout);
        else if (c == '"') fputs("\\\"", stdout);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
        else putchar(c);
    }
}

static void dump_code(const char *what, const uint8_t *code, size_t len, const int *lines) {
    printf("%s %zu\n", what, len);
    for (size_t i = 0; i < len; i++) {
        printf("%s%d", (i % 16 == 0) ? "  " : " ", code[i]);
        if (i % 16 == 15 || i == len - 1) putchar('\n');
    }
    printf("lines");
    for (size_t i = 0; i < len; i++)
        if (i == 0 || lines[i] != lines[i - 1]) printf(" %zu:%d", i, lines[i]);
    printf("\n");
}

static int dump_bytecode(const char *filename) {
    char *source = read_file(filename);
    if (!source) { fprintf(stderr, "clay: cannot read %s\n", filename); return 1; }
    clay_bootstrap();
    clay_plugin_auto_load(g_exe_path);
    static clayvm_vm_t vm;
    static BcCompiler bc;
    clayvm_vm_init(&vm);
    bc_compiler_init(&bc, &vm);
    bc.source_file = filename;
    clay_source_file = filename;
    Parser parser;
    parser_init(&parser, source, filename);
    AstNode *program = parser_parse(&parser);
    if (!program || parser.had_error) {
        fprintf(stderr, "clay: compilation failed due to parse errors\n");
        return 1;
    }
    if (bc_compile(&bc, program) != 0) {
        fprintf(stderr, "clay: bytecode compilation failed\n");
        return 1;
    }
    printf("constants %d\n", vm.constants.count);
    for (int i = 0; i < vm.constants.count; i++) {
        clayvm_value_t v = vm.constants.values[i];
        printf("  %d ", i);
        if (v.type == CLAYVM_VAL_INT64) printf("int %lld\n", (long long)v.as.i64);
        else if (v.type == CLAYVM_VAL_DOUBLE) {
            char buf[64];
            clay_format_float((float)v.as.f64, buf, sizeof(buf));
            printf("float %s\n", buf);
        } else if (v.type == CLAYVM_VAL_BOOL) printf("%s\n", v.as.boolean ? "true" : "false");
        else if (v.type == CLAYVM_VAL_NIL) printf("nil\n");
        else if (v.type == CLAYVM_VAL_OBJECT) {
            printf("string \"");
            dump_escaped(clay_string_data((ClayObject *)v.as.obj));
            printf("\"\n");
        } else printf("?\n");
    }
    dump_code("code", bc.code, bc.len, bc.line_map);
    for (int c = 0; c < bc.chunk_count; c++) {
        BcChunk *ch = bc.chunks[c];
        printf("chunk %d locals %d blockReturn %d\n", c, ch->local_count, ch->has_block_return);
        printf("params");
        for (int i = 0; i < ch->param_count && i < 16; i++) printf(" %s", ch->param_names[i]);
        printf("\ncaptures");
        for (int i = 0; i < ch->capture_count; i++) printf(" %s@%d", ch->capture_names[i], ch->capture_slots[i]);
        printf("\n");
        dump_code("code", ch->code, ch->len, ch->line_map);
    }
    for (int m = 0; m < bc_method_slot_count(); m++) {
        int chunk_id, params;
        const char *name = bc_method_slot_info(m, &chunk_id, &params);
        printf("method %d chunk %d params %d %s\n", m, chunk_id, params, name ? name : "");
    }
    return 0;
}

/* --- Run a .clay file through the bytecode VM --- */
static int run_vm_file(const char *filename, int trace_mode, int step_mode) {
    char *source = read_file(filename);
    clay_bootstrap();
    clay_plugin_auto_load(g_exe_path);
    repl_enable_color();

    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    if (trace_mode) vm.trace_enabled = 1;

    BcCompiler bc;
    bc_compiler_init(&bc, &vm);
    bc.source_file = filename;
    clay_source_file = filename;

    /* Register worker thread VM init callback for Thread spawn */
    extern void bc_worker_thread_init(void);
    extern void bc_set_main_compiler(BcCompiler *bc);
    clay_thread_set_vm_init(bc_worker_thread_init);
    bc_set_main_compiler(&bc);

    if (step_mode) {
        g_step.stepping = 1;
        g_step.last_line = 0;
        g_step.continue_mode = 0;
        g_step.bc = &bc;
        g_step.source = source;
        clayvm_vm_set_step_callback(&vm, step_callback);
    }

    Parser parser;
    parser_init(&parser, source, filename);
    AstNode *program = parser_parse(&parser);

    if (!program || parser.had_error) {
        fprintf(stderr, "clay: compilation failed due to parse errors\n");
        if (program) ast_free(program);
        free(source);
        return 1;
    }

    if (bc_compile(&bc, program) != 0) {
        fprintf(stderr, "clay: bytecode compilation failed\n");
        ast_free(program);
        free(source);
        return 1;
    }

    int exit_code = 0;
    vm.sp = 0;
    ClayTryFrame _tf;
    int caught = clay_try_push(&_tf);
    if (caught != 0) {
        ClayValue err = clay_error_stack.current_error;
        if (!CLAY_IS_NIL(err) && CLAY_IS_PTR(err)) {
            ClayValue file = clay_send_val(err, "file", 0, NULL);
            ClayValue line = clay_send_val(err, "line", 0, NULL);
            ClayValue msg = clay_send_val(err, "message", 0, NULL);
            if (!CLAY_IS_NIL(file) && !CLAY_IS_NIL(line)) {
                fprintf(stderr, "%s:%ld: ", clay_string_data(CLAY_OBJ(file)),
                        (long)clay_to_int(line));
            }
            fprintf(stderr, "%s%sError: %s%s\n", C(C_BOLD), C(C_RED),
                    clay_string_data(CLAY_OBJ(msg)), C(C_RESET));
            clay_release(CLAY_OBJ(msg));
            if (!CLAY_IS_NIL(file)) clay_release(CLAY_OBJ(file));
        } else {
            fprintf(stderr, "%s%sError: unknown%s\n", C(C_BOLD), C(C_RED), C(C_RESET));
        }
        exit_code = 1;
    } else {
        int result;
        if (step_mode) {
            vm.code = bc.code;
            vm.code_len = bc.len;
            vm.ip = 0;
            vm.sp = 0;
            vm.running = 1;
            result = 0;
            while (vm.running && vm.ip < vm.code_len) {
                if (clayvm_vm_step(&vm) < 0) break;
            }
        } else {
            result = clayvm_vm_run(&vm, bc.code, bc.len);
        }
        clay_try_pop();

        if (result != 0) exit_code = 1;
    }

    ast_free(program);
    free(source);
    clayvm_vm_free(&vm);
    return exit_code;
}

int main(int argc, char *argv[]) {
    resolve_exe_path(argv[0]);
    g_exe_path = g_exe_path_buf[0] ? g_exe_path_buf : argv[0];
    clay_executable_path = g_exe_path_buf;   /* System executable */
    clay_set_args(argc, argv);
    const char *source_file = NULL;
    const char *output_name = NULL;
    int c_only = 0;
    int compile_mode = 0;
    int print_ast = 0;
    int print_bytecode = 0;
    int print_tokens = 0;
    int repl_mode = 0;
    int vm_repl_mode = 0;
    int trace_mode = 0;
    int step_mode = 0;
    int mem_stats = 0;
    int dap_mode = 0;

    /* Handle bricks subcommand */
    if (argc >= 2 && strcmp(argv[1], "bricks") == 0) {
        return brick_command(argc - 2, argv + 2);
    }

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_name = argv[++i];
            compile_mode = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            c_only = 1;
            compile_mode = 1;
        } else if (strcmp(argv[i], "--compile") == 0) {
            compile_mode = 1;
        } else if (strcmp(argv[i], "--ast") == 0) {
            print_ast = 1;
        } else if (strcmp(argv[i], "--tokens") == 0) {
            print_tokens = 1;
        } else if (strcmp(argv[i], "--bytecode") == 0) {
            print_bytecode = 1;
        } else if (strcmp(argv[i], "--repl") == 0) {
            repl_mode = 1;
        } else if (strcmp(argv[i], "--vm-repl") == 0) {
            vm_repl_mode = 1;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace_mode = 1;
        } else if (strcmp(argv[i], "--step") == 0) {
            step_mode = 1;
        } else if (strcmp(argv[i], "--mem-stats") == 0) {
            mem_stats = 1;
        } else if (strcmp(argv[i], "--dap") == 0) {
            dap_mode = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: clay [options] [source.clay]\n\n");
            printf("Options:\n");
            printf("  --compile     Compile to native binary via C (instead of running in VM)\n");
            printf("  -o <output>   Output binary name (implies --compile)\n");
            printf("  -c            Emit C code only, no compile (implies --compile)\n");
            printf("  --ast         Print AST and exit\n");
            printf("  --tokens      Print the lexer's tokens and exit\n");
            printf("  --bytecode    Print the compiled bytecode and exit\n");
            printf("  --repl        Launch interactive REPL (compiles to C)\n");
            printf("  --vm-repl     Launch bytecode VM REPL (instant)\n");
            printf("  --trace       Bytecode tracing (VM REPL or file execution)\n");
            printf("  --step        Interactive stepping debugger (VM REPL or file execution)\n");
            printf("  --dap         Run as Debug Adapter Protocol server (for VS Code)\n");
            printf("  --mem-stats   Show memory stats on REPL exit\n");
            printf("  -h, --help    Show this help message\n");
            printf("\nSubcommands:\n");
            printf("  bricks        Package manager (clay bricks --help)\n");
            printf("\nWith no arguments, launches the VM REPL.\n");
            printf("With a source file, runs it in the bytecode VM.\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "clay: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            source_file = argv[i];
            break; /* remaining args belong to the script */
        }
    }

    /* Set script args: pass only args after the source file to the script */
    if (source_file) {
        int script_arg_start = 0;
        for (int i = 1; i < argc; i++) {
            if (argv[i] == source_file) { script_arg_start = i + 1; break; }
        }
        /* `clay prog.clay -- args`: the separator is not an argument */
        if (script_arg_start > 0 && script_arg_start < argc &&
            strcmp(argv[script_arg_start], "--") == 0)
            script_arg_start++;
        clay_set_args(argc - script_arg_start, argv + script_arg_start);
    }

    if (!source_file && !repl_mode && !vm_repl_mode) {
        vm_repl_mode = 1; /* default to VM REPL if no source file */
    }

    /* --trace/--step without a source file → VM REPL */
    if (!source_file && (trace_mode || step_mode)) {
        vm_repl_mode = 1;
    }

    if (vm_repl_mode) {
        return run_vm_repl(trace_mode, step_mode, mem_stats);
    }

    if (repl_mode) {
        return run_repl(argv[0]);
    }

    if (!source_file) {
        fprintf(stderr, "Usage: clay [options] <source.clay>\n");
        fprintf(stderr, "  --compile     Compile to native binary via C\n");
        fprintf(stderr, "  -o <output>   Output binary name\n");
        fprintf(stderr, "  -c            Emit C code only\n");
        fprintf(stderr, "  --ast         Print AST and exit\n");
        return 1;
    }

    /* --tokens: one token per line, "<line> <type> <text>" with \\, \n, \r
     * and \t escaped in the text (strings: the raw text between the quotes;
     * ERROR: the message). Stops after EOF or the first ERROR. The Clay lexer
     * (selfhost/clay-lexer) prints the same format; CTest compares them. */
    if (print_tokens) {
        char *source = read_file(source_file);
        if (!source) { fprintf(stderr, "clay: cannot read %s\n", source_file); return 1; }
        Lexer lexer;
        lexer_init(&lexer, source);
        for (;;) {
            Token t = lexer_next(&lexer);
            printf("%d %s ", t.line, token_type_name(t.type));
            for (int k = 0; k < t.length; k++) {
                char ch = t.start[k];
                if (ch == '\\') fputs("\\\\", stdout);
                else if (ch == '\n') fputs("\\n", stdout);
                else if (ch == '\r') fputs("\\r", stdout);
                else if (ch == '\t') fputs("\\t", stdout);
                else putchar(ch);
            }
            putchar('\n');
            if (t.type == TOK_EOF || t.type == TOK_ERROR) break;
        }
        free(source);
        return 0;
    }

    /* --ast: parse and print AST, then exit */
    if (print_ast) {
        char *source = read_file(source_file);
        Parser parser;
        parser_init(&parser, source, source_file);
        AstNode *program = parser_parse(&parser);
        if (parser.had_error) {
            fprintf(stderr, "clay: compilation failed due to parse errors\n");
            if (program) ast_free(program);
            free(source);
            return 1;
        }
        ast_print(program, 0);
        ast_free(program);
        free(source);
        return 0;
    }

    if (print_bytecode) return dump_bytecode(source_file);

    /* DAP debug adapter mode */
    if (dap_mode) {
        if (!source_file) {
            fprintf(stderr, "clay: --dap requires a source file\n");
            return 1;
        }
        return dap_run(source_file);
    }

    /* Default: run in bytecode VM */
    if (!compile_mode) {
        return run_vm_file(source_file, trace_mode, step_mode);
    }

    /* --- C codegen path (--compile, -c, -o) --- */

    /* Read source */
    char *source = read_file(source_file);

    /* Parse */
    Parser parser;
    parser_init(&parser, source, source_file);
    AstNode *program = parser_parse(&parser);

    if (parser.had_error) {
        fprintf(stderr, "clay: compilation failed due to parse errors\n");
        ast_free(program);
        free(source);
        return 1;
    }

    /* Load plugins so codegen knows their prototypes (MathEx, ...): the
     * native program resolves them at run time from the same plugins/ */
    clay_bootstrap();
    clay_plugin_auto_load(g_exe_path);

    /* Generate C code: next to the binary when -o names one (keeps source
     * trees clean), else next to the source */
    char *c_file = !output_name ? replace_ext(source_file, ".c")
                 : c_only ? strdup(output_name)            /* -c -o file.c */
                 : replace_ext(output_name, ".c");
    FILE *out = fopen(c_file, "w");
    if (!out) {
        fprintf(stderr, "clay: cannot write '%s'\n", c_file);
        return 1;
    }

    CodeGen cg;
    codegen_init(&cg, out, source_file);
    cg.plugin_exe_path = g_exe_path_buf;   /* native program loads clay's plugins/ */
    codegen_emit(&cg, program);
    fclose(out);

    printf("clay: generated %s\n", c_file);

    if (!c_only) {
        /* Compile the generated C with the system compiler */
        if (!output_name) {
#ifdef _WIN32
            output_name = replace_ext(source_file, ".exe");
#else
            output_name = replace_ext(source_file, "");   /* hello.clay → hello */
#endif
        }

        char clay_dir[1024];
        resolve_clay_dir(argv[0], clay_dir, sizeof(clay_dir));

        printf("clay: compiling %s -> %s\n", c_file, output_name);
        int result = compile_c_file(c_file, output_name, clay_dir, 0);
        if (result != 0) {
            fprintf(stderr, "clay: C compilation failed\n");
            ast_free(program);
            free(source);
            free(c_file);
            return 1;
        }
        printf("clay: built %s\n", output_name);
    }

    ast_free(program);
    free(source);
    free(c_file);
    return 0;
}
