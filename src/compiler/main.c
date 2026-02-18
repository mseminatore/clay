/*
 * main.c - Clay compiler driver
 *
 * Usage: clay [options] <source.clay>
 *   -o <output>   Output binary name (default: a.out / a.exe)
 *   -c            Emit C code only (don't compile)
 *   --ast         Print AST and exit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "codegen.h"

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
    const char *dot = strrchr(path, '.');
    size_t base_len = dot ? (size_t)(dot - path) : strlen(path);
    char *result = (char *)malloc(base_len + strlen(new_ext) + 1);
    memcpy(result, path, base_len);
    strcpy(result + base_len, new_ext);
    return result;
}

int main(int argc, char *argv[]) {
    const char *source_file = NULL;
    const char *output_name = NULL;
    int c_only = 0;
    int print_ast = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_name = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0) {
            c_only = 1;
        } else if (strcmp(argv[i], "--ast") == 0) {
            print_ast = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "clay: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            source_file = argv[i];
        }
    }

    if (!source_file) {
        fprintf(stderr, "Usage: clay [options] <source.clay>\n");
        fprintf(stderr, "  -o <output>   Output binary name\n");
        fprintf(stderr, "  -c            Emit C code only\n");
        fprintf(stderr, "  --ast         Print AST and exit\n");
        return 1;
    }

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

    if (print_ast) {
        ast_print(program, 0);
        ast_free(program);
        free(source);
        return 0;
    }

    /* Generate C code */
    char *c_file = replace_ext(source_file, ".c");
    FILE *out = fopen(c_file, "w");
    if (!out) {
        fprintf(stderr, "clay: cannot write '%s'\n", c_file);
        return 1;
    }

    CodeGen cg;
    codegen_init(&cg, out, source_file);
    codegen_emit(&cg, program);
    fclose(out);

    printf("clay: generated %s\n", c_file);

    if (!c_only) {
        /* Compile the generated C with the system compiler */
        if (!output_name) {
#ifdef _WIN32
            output_name = replace_ext(source_file, ".exe");
#else
            output_name = "a.out";
#endif
        }

        /* Resolve paths relative to the compiler's own location */
        char clay_dir[1024] = ".";
#ifdef _WIN32
        {
            /* Use argv[0] to find the compiler directory */
            char exe_path[1024];
            strncpy(exe_path, argv[0], sizeof(exe_path) - 1);
            exe_path[sizeof(exe_path) - 1] = '\0';
            /* Try _fullpath for absolute resolution */
            char full[1024];
            if (_fullpath(full, exe_path, sizeof(full))) {
                strncpy(exe_path, full, sizeof(exe_path) - 1);
            }
            /* Walk back to find project root (strip exe name, then up from build/Debug/) */
            char *sep = strrchr(exe_path, '\\');
            if (!sep) sep = strrchr(exe_path, '/');
            if (sep) *sep = '\0'; /* strip clay.exe */
            sep = strrchr(exe_path, '\\');
            if (!sep) sep = strrchr(exe_path, '/');
            if (sep) *sep = '\0'; /* strip Debug */
            sep = strrchr(exe_path, '\\');
            if (!sep) sep = strrchr(exe_path, '/');
            if (sep) *sep = '\0'; /* strip build */
            strncpy(clay_dir, exe_path, sizeof(clay_dir) - 1);
        }
#endif

        char cmd[4096];
#ifdef _WIN32
        {
            /* Find Visual Studio via vswhere */
            char vs_path[512] = "";
            {
                const char *pf86 = getenv("ProgramFiles(x86)");
                if (!pf86) pf86 = "C:\\Program Files (x86)";
                char vswhere_cmd[512];
                snprintf(vswhere_cmd, sizeof(vswhere_cmd),
                    "\"%s\\Microsoft Visual Studio\\Installer\\vswhere.exe\" "
                    "-latest -property installationPath", pf86);
                FILE *fp = _popen(vswhere_cmd, "r");
                if (fp) {
                    if (fgets(vs_path, sizeof(vs_path), fp)) {
                        size_t len = strlen(vs_path);
                        while (len > 0 && (vs_path[len-1] == '\n' || vs_path[len-1] == '\r'))
                            vs_path[--len] = '\0';
                    }
                    _pclose(fp);
                }
            }

            if (vs_path[0] == '\0') {
                fprintf(stderr, "clay: cannot find Visual Studio installation\n");
                fprintf(stderr, "clay: run from a Developer Command Prompt, or install Visual Studio\n");
                return 1;
            }

            snprintf(cmd, sizeof(cmd),
                "cmd /c \"\"%s\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x64 >nul 2>&1 && "
                "cl /nologo /W3 "
                "/I\"%s\\src\\runtime\" /I\"%s\\deps\\hash\" "
                "/D_CRT_SECURE_NO_WARNINGS "
                "\"%s\" "
                "\"%s\\src\\runtime\\object.c\" "
                "\"%s\\src\\runtime\\memory.c\" "
                "\"%s\\src\\runtime\\dispatch.c\" "
                "\"%s\\src\\runtime\\tagged.c\" "
                "\"%s\\src\\runtime\\string.c\" "
                "\"%s\\src\\runtime\\number.c\" "
                "\"%s\\src\\runtime\\block.c\" "
                "\"%s\\src\\runtime\\array.c\" "
                "\"%s\\src\\runtime\\bootstrap.c\" "
                "\"%s\\deps\\hash\\hash.c\" "
                "/Fe:\"%s\"\"",
                vs_path,
                clay_dir, clay_dir,
                c_file,
                clay_dir, clay_dir, clay_dir, clay_dir,
                clay_dir, clay_dir, clay_dir, clay_dir,
                clay_dir, clay_dir,
                output_name);
        }
#else
        snprintf(cmd, sizeof(cmd),
            "cc -std=c99 -Wall "
            "-I\"%s/src/runtime\" -I\"%s/deps/hash\" "
            "\"%s\" "
            "\"%s/src/runtime/object.c\" "
            "\"%s/src/runtime/memory.c\" "
            "\"%s/src/runtime/dispatch.c\" "
            "\"%s/src/runtime/tagged.c\" "
            "\"%s/src/runtime/string.c\" "
            "\"%s/src/runtime/number.c\" "
            "\"%s/src/runtime/block.c\" "
            "\"%s/src/runtime/array.c\" "
            "\"%s/src/runtime/bootstrap.c\" "
            "\"%s/deps/hash/hash.c\" "
            "-o \"%s\"",
            clay_dir, clay_dir,
            c_file,
            clay_dir, clay_dir, clay_dir, clay_dir,
            clay_dir, clay_dir, clay_dir, clay_dir,
            clay_dir, clay_dir,
            output_name);
#endif

        printf("clay: compiling %s -> %s\n", c_file, output_name);
        int result = system(cmd);
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
