/*
 * main.c - Clay compiler driver
 *
 * Usage: clay [options] <source.clay>
 *   -o <output>   Output binary name (default: a.out / a.exe)
 *   -c            Emit C code only (don't compile)
 *   --ast         Print AST and exit
 *   --repl        Interactive REPL (C compilation, also activates with no source file)
 *   --vm-repl     Interactive REPL (bytecode VM, faster)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "bc_compiler.h"
#include "ast.h"

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
    const char *dot = strrchr(path, '.');
    size_t base_len = dot ? (size_t)(dot - path) : strlen(path);
    char *result = (char *)malloc(base_len + strlen(new_ext) + 1);
    memcpy(result, path, base_len);
    strcpy(result + base_len, new_ext);
    return result;
}

/* Resolve the Clay project root directory from argv[0] */
static void resolve_clay_dir(const char *argv0, char *clay_dir, size_t sz) {
    strncpy(clay_dir, ".", sz - 1);
    clay_dir[sz - 1] = '\0';
#ifdef _WIN32
    {
        char exe_path[1024];
        strncpy(exe_path, argv0, sizeof(exe_path) - 1);
        exe_path[sizeof(exe_path) - 1] = '\0';
        char full[1024];
        if (_fullpath(full, exe_path, sizeof(full))) {
            strncpy(exe_path, full, sizeof(exe_path) - 1);
        }
        char *sep = strrchr(exe_path, '\\');
        if (!sep) sep = strrchr(exe_path, '/');
        if (sep) *sep = '\0';
        sep = strrchr(exe_path, '\\');
        if (!sep) sep = strrchr(exe_path, '/');
        if (sep) *sep = '\0';
        sep = strrchr(exe_path, '\\');
        if (!sep) sep = strrchr(exe_path, '/');
        if (sep) *sep = '\0';
        strncpy(clay_dir, exe_path, sz - 1);
    }
#endif
}

/* Compile a C file to an executable. Returns 0 on success. */
static int compile_c_file(const char *c_file, const char *output_name,
                          const char *clay_dir, int quiet) {
    char cmd[4096];
#ifdef _WIN32
    {
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
            return 1;
        }
        snprintf(cmd, sizeof(cmd),
            "cmd /c \"\"%s\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x64 >nul 2>&1 && "
            "cl /nologo %s "
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
            "\"%s\\src\\runtime\\error.c\" "
            "\"%s\\src\\runtime\\clay_dict.c\" "
            "\"%s\\src\\runtime\\bootstrap.c\" "
            "\"%s\\deps\\hash\\hash.c\" "
            "/Fe:\"%s\" %s\"",
            vs_path,
            quiet ? "/W0" : "/W3",
            clay_dir, clay_dir,
            c_file,
            clay_dir, clay_dir, clay_dir, clay_dir,
            clay_dir, clay_dir, clay_dir, clay_dir,clay_dir,
            clay_dir, clay_dir, clay_dir,
            output_name,
            quiet ? ">nul 2>&1" : "");
    }
#else
    snprintf(cmd, sizeof(cmd),
        "cc -std=c99 %s "
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
        "\"%s/src/runtime/error.c\" "
        "\"%s/src/runtime/clay_dict.c\" "
        "\"%s/src/runtime/bootstrap.c\" "
        "\"%s/deps/hash/hash.c\" "
        "-o \"%s\"",
        quiet ? "-w" : "-Wall",
        clay_dir, clay_dir,
        c_file,
        clay_dir, clay_dir, clay_dir, clay_dir,
        clay_dir, clay_dir, clay_dir, clay_dir,
        clay_dir, clay_dir, clay_dir, clay_dir,
        output_name);
#endif
    return system(cmd);
}

/* --- REPL --- */

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

    /* Accumulated program */
    AstNode *accumulated = ast_new(NODE_PROGRAM, 1);
    nodelist_init(&accumulated->as.program.stmts);

    /* Temp file paths */
    char c_path[1024], exe_path[1024];
    snprintf(c_path, sizeof(c_path), "%s\\_clay_repl.c", clay_dir);
#ifdef _WIN32
    snprintf(exe_path, sizeof(exe_path), "%s\\_clay_repl.exe", clay_dir);
#else
    snprintf(exe_path, sizeof(exe_path), "%s/_clay_repl", clay_dir);
#endif

    char line_buf[4096];
    char input_buf[16384];

    while (1) {
        /* Read input (possibly multi-line) */
        printf("clay> ");
        fflush(stdout);

        input_buf[0] = '\0';
        int open_depth = 0;
        int first_line = 1;

        while (1) {
            if (!fgets(line_buf, sizeof(line_buf), stdin)) {
                /* EOF (Ctrl+Z on Windows, Ctrl+D on Unix) */
                printf("\n");
                goto cleanup;
            }

            /* Check for quit */
            if (first_line) {
                char *trimmed = line_buf;
                while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
                /* Strip trailing newline for comparison */
                size_t tlen = strlen(trimmed);
                while (tlen > 0 && (trimmed[tlen-1] == '\n' || trimmed[tlen-1] == '\r'))
                    tlen--;
                if (tlen == 4 && strncmp(trimmed, "quit", 4) == 0) {
                    goto cleanup;
                }
                if (tlen == 4 && strncmp(trimmed, "exit", 4) == 0) {
                    goto cleanup;
                }
            }

            /* Append to input buffer */
            if (strlen(input_buf) + strlen(line_buf) < sizeof(input_buf) - 1) {
                strcat(input_buf, line_buf);
            }

            open_depth += count_open(line_buf);
            first_line = 0;

            if (open_depth <= 0) break;

            /* Multi-line continuation */
            printf("  ... ");
            fflush(stdout);
        }

        /* Strip trailing whitespace */
        size_t ilen = strlen(input_buf);
        while (ilen > 0 && (input_buf[ilen-1] == '\n' || input_buf[ilen-1] == '\r'
               || input_buf[ilen-1] == ' ' || input_buf[ilen-1] == '\t'))
            ilen--;
        input_buf[ilen] = '\0';

        if (ilen == 0) continue; /* empty input */

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

static int run_vm_repl(void) {
    clay_bootstrap();

    clayvm_vm_t vm;
    clayvm_vm_init(&vm);

    BcCompiler bc;
    bc_compiler_init(&bc, &vm);

    printf("Clay VM REPL v0.1 (type 'quit' or 'exit' to leave)\n");
    printf(">>> ");
    fflush(stdout);

    char line[4096];
    char input[16384];
    int input_len = 0;
    int brace_depth = 0;
    int paren_depth = 0;

    while (fgets(line, sizeof(line), stdin)) {
        /* Check for quit */
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (strcmp(trimmed, "quit\n") == 0 || strcmp(trimmed, "exit\n") == 0 ||
            strcmp(trimmed, "quit\r\n") == 0 || strcmp(trimmed, "exit\r\n") == 0) {
            break;
        }

        /* Append to input buffer */
        size_t ll = strlen(line);
        if (input_len + (int)ll < (int)sizeof(input) - 1) {
            memcpy(input + input_len, line, ll);
            input_len += (int)ll;
            input[input_len] = '\0';
        }

        /* Track braces/parens for multi-line */
        for (size_t i = 0; i < ll; i++) {
            if (line[i] == '{') brace_depth++;
            else if (line[i] == '}') brace_depth--;
            else if (line[i] == '(') paren_depth++;
            else if (line[i] == ')') paren_depth--;
        }

        if (brace_depth > 0 || paren_depth > 0) {
            printf("... ");
            fflush(stdout);
            continue;
        }

        /* Parse the input */
        Parser parser;
        parser_init(&parser, input, "<repl>");
        AstNode *program = parser_parse(&parser);

        if (!program || parser.had_error) {
            fprintf(stderr, "clay: parse error\n");
            if (program) ast_free(program);
            input_len = 0;
            brace_depth = 0;
            paren_depth = 0;
            printf(">>> ");
            fflush(stdout);
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
            printf(">>> ");
            fflush(stdout);
            continue;
        }

        /* Execute with error recovery */
        vm.sp = 0;
        int caught = clay_try_push();
        if (caught != 0) {
            /* An error was raised during execution */
            ClayValue err = clay_error_stack.current_error;
            if (!CLAY_IS_NIL(err) && !CLAY_IS_INT(err)) {
                ClayValue msg = clay_send_val(err, "message", 0, NULL);
                fprintf(stderr, "Error: %s\n", clay_string_data(CLAY_OBJ(msg)));
            } else {
                fprintf(stderr, "Error: unknown\n");
            }
        } else {
            int result = clayvm_vm_run(&vm, bc.code, bc.len);
            clay_try_pop();

            if (result == 0 && vm.sp > 0) {
                /* Auto-print result */
                clayvm_value_t top = vm.stack[vm.sp - 1];
                if (top.type != CLAYVM_VAL_NIL) {
                    ClayValue cv = vm_to_clay(top);
                    if (CLAY_IS_INT(cv)) {
                        printf("=> %ld\n", (long)clay_to_int(cv));
                    } else if (cv == CLAY_TRUE) {
                        printf("=> true\n");
                    } else if (cv == CLAY_FALSE) {
                        printf("=> false\n");
                    } else if (!CLAY_IS_NIL(cv)) {
                        ClayValue sv = clay_send_val(cv, "toString", 0, NULL);
                        printf("=> %s\n", clay_string_data(CLAY_OBJ(sv)));
                    }
                }
            }
        }

        ast_free(program);
        input_len = 0;
        brace_depth = 0;
        paren_depth = 0;
        printf(">>> ");
        fflush(stdout);
    }

    printf("Bye!\n");
    return 0;
}

int main(int argc, char *argv[]) {
    const char *source_file = NULL;
    const char *output_name = NULL;
    int c_only = 0;
    int print_ast = 0;
    int repl_mode = 0;
    int vm_repl_mode = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_name = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0) {
            c_only = 1;
        } else if (strcmp(argv[i], "--ast") == 0) {
            print_ast = 1;
        } else if (strcmp(argv[i], "--repl") == 0) {
            repl_mode = 1;
        } else if (strcmp(argv[i], "--vm-repl") == 0) {
            vm_repl_mode = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "clay: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            source_file = argv[i];
        }
    }

    if (!source_file && !repl_mode && !vm_repl_mode) {
        repl_mode = 1; /* default to REPL if no source file */
    }

    if (vm_repl_mode) {
        return run_vm_repl();
    }

    if (repl_mode) {
        return run_repl(argv[0]);
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
