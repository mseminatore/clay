/*
 * bootstrap.c - Initialize core prototype objects
 */
#include "clay.h"
#include "crossline.h"
#include "ffi.h"
#include "version.h"
#include <time.h>
#ifndef _WIN32
#include <sys/wait.h>   /* WEXITSTATUS (System run) */
#endif
#ifdef _WIN32
#include <stdlib.h>  /* getenv, exit */
#else
#include <stdlib.h>
#endif

/* Core prototypes - global singletons */
ClayObject *clay_Object  = NULL;
ClayObject *clay_Number  = NULL;
ClayObject *clay_String  = NULL;
ClayObject *clay_Array   = NULL;
ClayObject *clay_Block   = NULL;
ClayObject *clay_Nil     = NULL;
ClayObject *clay_Boolean = NULL;
ClayObject *clay_Error   = NULL;
ClayObject *clay_Dict    = NULL;
ClayObject *clay_Math   = NULL;
ClayObject *clay_Console = NULL;
ClayObject *clay_File    = NULL;
ClayObject *clay_System  = NULL;
ClayObject *clay_FFI     = NULL;
ClayObject *clay_Date    = NULL;
ClayObject *clay_Http    = NULL;
ClayObject *clay_Regex   = NULL;
ClayObject *clay_Crypto  = NULL;
ClayObject *clay_Process = NULL;
ClayObject *clay_Fiber   = NULL;
ClayObject *clay_Db     = NULL;
ClayObject *clay_Range  = NULL;
ClayObject *clay_Graphics = NULL;
ClayObject *clay_Vec3 = NULL;
ClayObject *clay_Thread = NULL;
ClayObject *clay_VM = NULL;
ClayObject *clay_BreakSignal= NULL;
ClayObject *clay_ContinueSignal = NULL;

/* External init functions from each type module */
extern void clay_number_init_methods(ClayObject *proto);
extern void clay_string_init_methods(ClayObject *proto);
extern void clay_array_init_methods(ClayObject *proto);
extern void clay_block_init_methods(ClayObject *proto);
extern void clay_error_init_methods(ClayObject *proto);
extern void clay_dict_init_methods(ClayObject *proto);
extern void clay_math_init(ClayObject *proto);
extern void clay_file_init(ClayObject *proto);
extern void clay_date_init_methods(ClayObject *proto);
extern void clay_http_init(ClayObject *proto);
extern void clay_regex_init_methods(ClayObject *proto);
extern void clay_fiber_init_methods(ClayObject *proto);
extern void clay_db_init(ClayObject *proto);
extern void clay_range_init_methods(ClayObject *proto);
extern void clay_graphics_init(ClayObject *proto);

/* Object base methods */

static ClayValue object_clone(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return CLAY_VAL(clay_clone(self));
}

static ClayValue object_new(ClayObject *self, int argc, ClayValue *argv) {
    /* Call init(args) if the clone has an init method */
    if (clay_lookup_method(self, "init")) {
        ClayObject *obj = clay_clone(self);
        /* init often returns an argument (`init(c) { center = c }`): owned copy */
        ClayValue init_result = clay_send_owned(CLAY_VAL(obj), "init", argc, argv);
        clay_release_val(init_result);
        return CLAY_VAL(obj);
    }
    if (argc > 0) {
        /* e.g. Date new("2026-01-01") — the arguments would be silently dropped */
        clay_raise_argument_error("new", "this prototype has no init method to receive arguments");
        return CLAY_NIL;
    }
    return CLAY_VAL(clay_clone(self));
}

static ClayValue object_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char buf[64];
    snprintf(buf, sizeof(buf), "<object@%p>", (void *)self);
    return CLAY_VAL(clay_string_new(buf));
}

static ClayValue object_println(ClayObject *self, int argc, ClayValue *argv) {
    (void)argv;
    if (argc > 0) clay_raise_argument_error("println",
        "takes no arguments — it prints the receiver (to print a value, use Console println(value))");

    ClayValue s = clay_send(self, "toString", 0, NULL);
    ClayValue pr = clay_send(CLAY_OBJ(s), "println", 0, NULL);
    /* pr is often the same object as s (string_println returns self),
     * so only release each distinct object once. */
    if (pr != s) clay_release_val(pr);
    clay_release(CLAY_OBJ(s));
    return CLAY_VAL(self);
}

static ClayValue object_doesNotUnderstand(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *sel = (argc > 0 && CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0]))
        ? clay_string_data(CLAY_OBJ(argv[0]))
        : "???";
    char buf[256];
    snprintf(buf, sizeof(buf), "object does not understand '%s'%s", sel, clay_dnu_hint(sel));
    clay_raise(CLAY_VAL(clay_error_new(buf, "MethodNotFound")));
    return CLAY_NIL;
}

static ClayValue object_type(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    const char *name;
    if (self == &clay_true_obj || self == &clay_false_obj) {
        name = "Boolean";
    } else {
        switch (self->type) {
            case CLAY_TYPE_STRING: name = "String"; break;
            case CLAY_TYPE_ARRAY:  name = "Array"; break;
            case CLAY_TYPE_BLOCK:  name = "Block"; break;
            case CLAY_TYPE_FLOAT:  name = "Float"; break;
            case CLAY_TYPE_DICT:   name = "Dict"; break;
            case CLAY_TYPE_VEC3:   name = "Vec3"; break;
            default:
                /* Tagged ints are wrapped with parent=clay_Number */
                if (self->parent == clay_Number) name = "Number";
                else if (self->parent == clay_Error || self->type == CLAY_TYPE_OBJECT) {
                    /* Check if it's an Error by walking parent chain */
                    ClayObject *p = self->parent;
                    name = "Object";
                    while (p) {
                        if (p == clay_Error) { name = "Error"; break; }
                        p = p->parent;
                    }
                    /* Check _name slot for user-defined prototypes */
                    if (strcmp(name, "Object") == 0) {
                        ClayValue nv = clay_get_slot(self, "_name");
                        if (!CLAY_IS_NIL(nv) && CLAY_IS_PTR(nv) &&
                            CLAY_OBJ(nv)->type == CLAY_TYPE_STRING) {
                            name = clay_string_data(CLAY_OBJ(nv));
                        }
                    }
                }
                else name = "Object";
                break;
        }
    }
    return CLAY_VAL(clay_string_new(name));
}

static void collect_methods_chain(ClayObject *obj, ClayObject *arr, HashTable *seen, int inherited) {
    if (!obj) return;
    if (obj->methods) {
        size_t pos = 0;
        ht_key_t key;
        while (ht_next(obj->methods, &pos, &key, NULL)) {
            if (ht_find(seen, key) != NULL) continue;
            ht_insert(seen, key, (ht_value_t)1);
            if (inherited) {
                /* Mark inherited methods with trailing * */
                size_t len = strlen((const char *)key);
                char *buf = malloc(len + 2);
                memcpy(buf, (const char *)key, len);
                buf[len] = '*';
                buf[len + 1] = '\0';
                clay_array_append(arr, CLAY_VAL(clay_string_new(buf)));
                free(buf);
            } else {
                clay_array_append(arr, CLAY_VAL(clay_string_new((const char *)key)));
            }
        }
    }
    collect_methods_chain(obj->parent, arr, seen, 1);
}

static int str_compare(ht_key_t a, ht_key_t b) {
    return strcmp((const char *)a, (const char *)b) == 0;
}

static ClayValue object_methods(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *arr = clay_array_new();
    HashTable *seen = ht_create();
    ht_set_hash_func(seen, HT_HASH_STRING);
    ht_set_compare_func(seen, str_compare);
    collect_methods_chain(self, arr, seen, 0);
    ht_free(seen);
    return CLAY_VAL(arr);
}

static ClayValue object_slots(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *arr = clay_array_new();
    if (self->slots) {
        size_t pos = 0;
        ht_key_t key;
        while (ht_next(self->slots, &pos, &key, NULL)) {
            clay_array_append(arr, CLAY_VAL(clay_string_new((const char *)key)));
        }
    }
    return CLAY_VAL(arr);
}

static ClayValue object_parent(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return self->parent ? CLAY_VAL(self->parent) : CLAY_NIL;
}

static ClayValue object_has_slot(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_OBJ(argv[0])) return CLAY_FALSE;
    ClayObject *name_obj = CLAY_OBJ(argv[0]);
    if (name_obj->type != CLAY_TYPE_STRING) return CLAY_FALSE;
    return CLAY_BOOL(clay_has_slot(self, clay_string_data(name_obj)));
}

/* getSlot("name") — return raw slot value without invoking blocks */
static ClayValue object_get_slot(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_OBJ(argv[0])) return CLAY_NIL;
    ClayObject *name_obj = CLAY_OBJ(argv[0]);
    if (name_obj->type != CLAY_TYPE_STRING) return CLAY_NIL;
    ClayValue result = clay_get_slot(self, clay_string_data(name_obj));
    clay_retain_val(result);
    return result;
}

/* setSlot("name", value) — set a slot by name dynamically */
static ClayValue object_set_slot(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 2 || !CLAY_IS_OBJ(argv[0])) return CLAY_NIL;
    ClayObject *name_obj = CLAY_OBJ(argv[0]);
    if (name_obj->type != CLAY_TYPE_STRING) return CLAY_NIL;
    clay_set_slot(self, clay_string_data(name_obj), argv[1]);
    return argv[1];
}

static ClayValue object_has_method(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_OBJ(argv[0])) return CLAY_FALSE;
    ClayObject *name_obj = CLAY_OBJ(argv[0]);
    if (name_obj->type != CLAY_TYPE_STRING) return CLAY_FALSE;
    return CLAY_BOOL(clay_lookup_method(self, clay_string_data(name_obj)) != NULL);
}

/* setMethod(name, block): register a block as a proper method.
 * Uses clay_set_method_ctx so the method takes priority over inherited methods. */
static ClayValue block_method_trampoline(ClayObject *self, int argc, ClayValue *argv) {
    /* The block is stored as the method context */
    ClayObject *block = (ClayObject *)clay_method_context;
    if (!block || block->type != CLAY_TYPE_BLOCK || !block->as.block.func) return CLAY_NIL;
    ClayObject *prev_self = clay_slot_method_self;  /* restore: may nest */
    clay_slot_method_self = self;
    ClayValue result = block->as.block.func(block, argc, argv);
    clay_slot_method_self = prev_self;
    return result;
}

static ClayValue object_set_method(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 2 || !CLAY_IS_OBJ(argv[0])) return CLAY_NIL;
    ClayObject *name_obj = CLAY_OBJ(argv[0]);
    if (name_obj->type != CLAY_TYPE_STRING) return CLAY_NIL;
    /* Verify second arg is a block */
    if (!CLAY_IS_OBJ(argv[1])) return CLAY_NIL;
    ClayObject *block = CLAY_OBJ(argv[1]);
    if (block->type != CLAY_TYPE_BLOCK) return CLAY_NIL;
    clay_retain(block);
    clay_set_method_ctx(self, clay_string_data(name_obj), block_method_trampoline, block);
    return argv[1];
}

static ClayValue object_inspect(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    clay_inspect(self);
    return CLAY_VAL(self);
}

/* Boolean methods */

/* Identity-based equality for all objects */
static ClayValue object_eq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    return CLAY_BOOL(CLAY_VAL(self) == argv[0]);
}

static ClayValue object_neq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_TRUE;
    return CLAY_BOOL(CLAY_VAL(self) != argv[0]);
}

/*
 * ifTrue/ifFalse return a "condition result" object that supports chaining.
 * The result stores the value in a "_val" slot and the "_matched" flag.
 * ifFalse on a result where _matched==true returns _val unchanged.
 * ifFalse on a result where _matched==false evaluates the block.
 */

/* The prototype of condition results: holds their ifTrue/ifFalse once
 * (created in clay_bootstrap) */
static ClayObject *clay_CondResult = NULL;

static ClayObject *make_cond_result(ClayValue val, int matched) {
    ClayObject *obj = clay_clone(clay_CondResult);
    clay_set_slot(obj, "_val", val);
    clay_set_slot(obj, "_matched", matched ? CLAY_TRUE : CLAY_FALSE);
    return obj;
}

static ClayValue cond_ifFalse(ClayObject *self, int argc, ClayValue *argv) {
    ClayValue matched = clay_get_slot(self, "_matched");
    if (matched == CLAY_TRUE) {
        ClayValue result = clay_get_slot(self, "_val");
        clay_retain_val(result);
        return result;
    }
    if (argc < 1) return CLAY_NIL;
    return clay_send(CLAY_OBJ(argv[0]), "value", 0, NULL);
}

static ClayValue cond_ifTrue(ClayObject *self, int argc, ClayValue *argv) {
    ClayValue matched = clay_get_slot(self, "_matched");
    if (matched == CLAY_TRUE) {
        ClayValue result = clay_get_slot(self, "_val");
        clay_retain_val(result);
        return result;
    }
    if (argc < 1) return CLAY_VAL(self);
    ClayValue result = clay_send(CLAY_OBJ(argv[0]), "value", 0, NULL);
    ClayObject *cr = make_cond_result(result, 1);
    clay_release_val(result); /* make_cond_result took ownership via set_slot */
    return CLAY_VAL(cr);
}

static ClayValue bool_ifTrue(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_VAL(self);
    if (self == &clay_true_obj) {
        ClayValue result = clay_send(CLAY_OBJ(argv[0]), "value", 0, NULL);
        ClayObject *cr = make_cond_result(result, 1);
        clay_release_val(result); /* make_cond_result took ownership via set_slot */
        return CLAY_VAL(cr);
    }
    /* false: return a condition result with _matched=false */
    ClayObject *cr = make_cond_result(CLAY_NIL, 0);
    return CLAY_VAL(cr);
}

static ClayValue bool_ifFalse(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_VAL(self);
    if (self == &clay_false_obj) {
        ClayValue result = clay_send(CLAY_OBJ(argv[0]), "value", 0, NULL);
        ClayObject *cr = make_cond_result(result, 1);
        clay_release_val(result); /* make_cond_result took ownership via set_slot */
        return CLAY_VAL(cr);
    }
    /* true: return a condition result with _matched=false */
    ClayObject *cr = make_cond_result(CLAY_NIL, 0);
    return CLAY_VAL(cr);
}

static ClayValue bool_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self == &clay_true_obj) return CLAY_VAL(clay_string_new("true"));
    return CLAY_VAL(clay_string_new("false"));
}

static ClayValue bool_not(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return (self == &clay_true_obj) ? CLAY_FALSE : CLAY_TRUE;
}

static ClayValue nil_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv; (void)self;
    return CLAY_VAL(clay_string_new("nil"));
}

static ClayValue nil_type(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv; (void)self;
    return CLAY_VAL(clay_string_new("Nil"));
}

static ClayValue nil_eq(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return CLAY_FALSE;
    return CLAY_BOOL(CLAY_IS_NIL(argv[0]));
}

static ClayValue nil_neq(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return CLAY_TRUE;
    return CLAY_BOOL(!CLAY_IS_NIL(argv[0]));
}

/* Console methods */
/* A colour name for readLine's prompt: black, red, green, yellow, blue,
 * magenta, cyan, white or default; -1 if it is none of them */
static int prompt_color(const char *name) {
    static const char *names[] = { "default", "black", "red", "green", "yellow",
                                   "blue", "magenta", "cyan", "white" };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (strcmp(name, names[i]) == 0) return i;      /* CROSSLINE_FGCOLOR_* in this order */
    return -1;
}

/* Console readLine / readLine(prompt) / readLine(prompt, color) — one line of
 * input without its line end, nil at end of input. With a prompt, a terminal
 * gets line editing and history (crossline); color names the prompt's colour.
 * (Escape codes inside the prompt would be counted as visible characters and
 * put the cursor in the wrong place; this is how to colour it.) */
static ClayValue console_readLine(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    char buf[4096];
    if (argc > 0 && CLAY_IS_TYPE(argv[0], CLAY_TYPE_STRING)) {
        int color = CROSSLINE_FGCOLOR_DEFAULT;
        if (argc > 1 && CLAY_IS_TYPE(argv[1], CLAY_TYPE_STRING)) {
            color = prompt_color(clay_string_data(CLAY_OBJ(argv[1])));
            if (color < 0) clay_raise_argument_error("Console readLine", "unknown prompt colour");
        }
        fflush(stdout);
        crossline_prompt_color_set((crossline_color_e)color);
        char *line = crossline_readline(clay_string_data(CLAY_OBJ(argv[0])), buf, (int)sizeof(buf));
        crossline_prompt_color_set(CROSSLINE_FGCOLOR_DEFAULT);
        if (!line) return CLAY_NIL;
        return CLAY_VAL(clay_string_new(buf));
    }
    if (fgets(buf, sizeof(buf), stdin)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
        return CLAY_VAL(clay_string_new(buf));
    }
    return CLAY_NIL;
}

/* Write text to stdout, or to the debugger's output hook when one is set */
static void console_write(const char *text) {
    if (clay_output_fn) clay_output_fn(text);
    else fputs(text, stdout);
}

/* Console print(args...) — print each argument's toString, space-separated */
static ClayValue console_print(ClayObject *self, int argc, ClayValue *argv) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) console_write(" ");
        ClayValue s = clay_send_val(argv[i], "toString", 0, NULL);
        if (CLAY_IS_TYPE(s, CLAY_TYPE_STRING)) console_write(clay_string_data(CLAY_OBJ(s)));
        else console_write(CLAY_IS_NIL(argv[i]) ? "nil" : "?");
        clay_release_val(s);
    }
    fflush(stdout);
    return CLAY_VAL(self);
}

/* Console error(args...) — like print, to stderr */
static ClayValue console_error(ClayObject *self, int argc, ClayValue *argv) {
    fflush(stdout);
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputs(" ", stderr);
        ClayValue s = clay_send_val(argv[i], "toString", 0, NULL);
        if (CLAY_IS_TYPE(s, CLAY_TYPE_STRING)) fputs(clay_string_data(CLAY_OBJ(s)), stderr);
        else fputs(CLAY_IS_NIL(argv[i]) ? "nil" : "?", stderr);
        clay_release_val(s);
    }
    fflush(stderr);
    return CLAY_VAL(self);
}

/* Console println(args...) — like print, followed by a newline */
static ClayValue console_println(ClayObject *self, int argc, ClayValue *argv) {
    console_print(self, argc, argv);
    console_write("\n");
    fflush(stdout);
    return CLAY_VAL(self);
}

/* Console loadHistory(path) / saveHistory(path) — readLine's line history */
static ClayValue console_loadHistory(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_TYPE(argv[0], CLAY_TYPE_STRING))
        clay_raise(CLAY_VAL(clay_error_new("Console loadHistory requires a path string", "ArgumentError")));
    return CLAY_BOOL(crossline_history_load(clay_string_data(CLAY_OBJ(argv[0]))) == 0);
}

static ClayValue console_saveHistory(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_TYPE(argv[0], CLAY_TYPE_STRING))
        clay_raise(CLAY_VAL(clay_error_new("Console saveHistory requires a path string", "ArgumentError")));
    return CLAY_BOOL(crossline_history_save(clay_string_data(CLAY_OBJ(argv[0]))) == 0);
}

static ClayValue console_prompt(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc > 0 && CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0])) {
        ClayObject *msg = CLAY_OBJ(argv[0]);
        if (msg->type == CLAY_TYPE_STRING) {
            printf("%s", clay_string_data(msg));
            fflush(stdout);
        }
    }
    return console_readLine(self, 0, NULL);
}

/* System methods — memory diagnostics */
static ClayValue system_alloc_count(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int((intptr_t)clay_alloc_count);
}

/* System liveObjects - objects allocated and not yet freed (prototypes and
 * other permanent objects included) */
static ClayValue system_live_objects(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int((intptr_t)(clay_alloc_count - clay_dealloc_count));
}

/* System memory - the process's resident memory in bytes, as the OS reports
 * it (everything: objects, the VM, buffers); nil if it can't be read */
static ClayValue system_memory(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    extern long long clay_process_memory(void);
    long long bytes = clay_process_memory();
    return bytes < 0 ? CLAY_NIL : clay_int((intptr_t)bytes);
}

static ClayValue system_dealloc_count(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int((intptr_t)clay_dealloc_count);
}

/* Stored command-line args */
static int s_argc = 0;
static char **s_argv = NULL;

void clay_set_args(int argc, char **argv) {
    s_argc = argc;
    s_argv = argv;
}

/* System run("cc -o x x.c") → the command's exit status (via the shell);
 * output goes to this process's stdout/stderr */
static ClayValue system_run(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    const char *cmd = clay_arg_string(argc, argv, 0, "run");
    fflush(stdout);
    fflush(stderr);
    int rc = system(cmd);
#ifndef _WIN32
    if (rc != -1) {
        if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
        else if (WIFSIGNALED(rc)) rc = 128 + WTERMSIG(rc);
    }
#endif
    return clay_int(rc);
}

static ClayValue system_exit(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    int code = 0;
    if (argc > 0 && CLAY_IS_INT(argv[0]))
        code = (int)clay_to_int(argv[0]);
    exit(code);
    return CLAY_NIL; /* unreachable */
}

static ClayValue system_env(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0]))
        return CLAY_NIL;
    const char *name = clay_string_data(CLAY_OBJ(argv[0]));
    const char *val = getenv(name);
    if (!val) return CLAY_NIL;
    return CLAY_VAL(clay_string_new(val));
}

static ClayValue system_args(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    ClayObject *arr = clay_array_new();
    for (int i = 0; i < s_argc; i++) {
        clay_array_append(arr, CLAY_VAL(clay_string_new(s_argv[i])));
    }
    return CLAY_VAL(arr);
}

/* System version - the Clay version, e.g. "0.1.0" (version.h) */
static ClayValue system_version(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_VAL(clay_string_new(CLAY_VERSION));
}

/* Console isTerminal - true if output goes to an interactive terminal, so
 * colour (ANSI escape codes) can be used; false when piped or redirected */
static ClayValue console_isTerminal(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    extern int clay_stdout_is_terminal(void);
    return CLAY_BOOL(clay_stdout_is_terminal());
}

/* Console isInteractive - true if input comes from a terminal, so a prompt
 * will reach someone who can answer it (false when piped or redirected) */
static ClayValue console_isInteractive(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    extern int clay_stdin_is_terminal(void);
    return CLAY_BOOL(clay_stdin_is_terminal());
}

static ClayValue system_platform(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
#if defined(_WIN32)
    return CLAY_VAL(clay_string_new("windows"));
#elif defined(__APPLE__)
    return CLAY_VAL(clay_string_new("macos"));
#else
    return CLAY_VAL(clay_string_new("linux"));
#endif
}

/* Milliseconds on a monotonic wall clock: elapsed time, not CPU time, so it
 * keeps counting while the program waits and doesn't run fast with threads */
static long long monotonic_ms(void) {
#ifdef _WIN32
    /* declared by hand: windows.h conflicts with this file (see clay_bootstrap) */
    extern __declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *);
    extern __declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *);
    static long long freq = 0;
    long long now = 0;
    if (freq == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return freq > 0 ? (long long)((double)now * 1000.0 / (double)freq) : 0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static long long clock_start = 0;   /* set by clay_bootstrap */

/* System clock: milliseconds since the program started */
static ClayValue system_clock(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int((intptr_t)(monotonic_ms() - clock_start));
}

void clay_bootstrap(void) {
    clay_stack_init();       /* the main thread's C stack, for CLAY_STACK_CHECK */
#ifdef _WIN32
    /* Enable UTF-8 output and ANSI color sequences on Windows console */
    {
        typedef void *HANDLE;
        typedef unsigned long DWORD;
        extern __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
        extern __declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
        extern __declspec(dllimport) int __stdcall GetConsoleMode(HANDLE, DWORD *);
        extern __declspec(dllimport) int __stdcall SetConsoleMode(HANDLE, DWORD);
        #define STD_OUTPUT_HANDLE ((DWORD)-11)
        #define STD_ERROR_HANDLE  ((DWORD)-12)
        #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004

        SetConsoleOutputCP(65001);
        HANDLE h;
        DWORD mode;
        h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        h = GetStdHandle(STD_ERROR_HANDLE);
        if (GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
    if (clock_start == 0) clock_start = monotonic_ms();

    /* Create root Object prototype */
    clay_Object = clay_object_new();
    clay_Object->refcount = CLAY_REFCOUNT_IMMORTAL; /* never freed */

    clay_set_method(clay_Object, "clone", object_clone);
    clay_set_method(clay_Object, "new", object_new);
    clay_set_method(clay_Object, "toString", object_toString);
    clay_set_method(clay_Object, "println", object_println);
    clay_set_method(clay_Object, "doesNotUnderstand", object_doesNotUnderstand);
    clay_set_method(clay_Object, "==", object_eq);
    clay_set_method(clay_Object, "!=", object_neq);
    clay_set_method(clay_Object, "methods", object_methods);
    clay_set_method(clay_Object, "slots", object_slots);
    clay_set_method(clay_Object, "inspect", object_inspect);
    clay_set_method(clay_Object, "type", object_type);
    clay_set_method(clay_Object, "parent", object_parent);
    clay_set_method(clay_Object, "hasSlot", object_has_slot);
    clay_set_method(clay_Object, "getSlot", object_get_slot);
    clay_set_method(clay_Object, "setSlot", object_set_slot);
    clay_set_method(clay_Object, "hasMethod", object_has_method);
    clay_set_method(clay_Object, "setMethod", object_set_method);

    /* Number prototype */
    clay_Number = clay_object_new();
    clay_Number->parent = clay_Object;
    clay_Number->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_number_init_methods(clay_Number);

    /* String prototype */
    clay_String = clay_object_new();
    clay_String->parent = clay_Object;
    clay_String->type = CLAY_TYPE_STRING;
    clay_String->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_string_init_methods(clay_String);

    /* Array prototype */
    clay_Array = clay_object_new();
    clay_Array->parent = clay_Object;
    clay_Array->type = CLAY_TYPE_ARRAY;
    clay_Array->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_array_init_methods(clay_Array);

    /* Block prototype */
    clay_Block = clay_object_new();
    clay_Block->parent = clay_Object;
    clay_Block->type = CLAY_TYPE_BLOCK;
    clay_Block->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_block_init_methods(clay_Block);

    /* Boolean prototype */
    clay_Boolean = clay_object_new();
    clay_Boolean->parent = clay_Object;
    clay_Boolean->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_method(clay_Boolean, "ifTrue", bool_ifTrue);
    clay_set_method(clay_Boolean, "ifFalse", bool_ifFalse);
    clay_CondResult = clay_clone(clay_Object);
    clay_CondResult->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_method(clay_CondResult, "ifTrue", cond_ifTrue);
    clay_set_method(clay_CondResult, "ifFalse", cond_ifFalse);
    clay_set_method(clay_Boolean, "toString", bool_toString);
    clay_set_method(clay_Boolean, "not", bool_not);

    /* Wire up boolean singletons */
    clay_true_obj.parent = clay_Boolean;
    clay_false_obj.parent = clay_Boolean;
    /* Boolean singletons need slot/method tables for lookups */
    clay_true_obj.slots = NULL;
    clay_true_obj.methods = NULL;
    clay_false_obj.slots = NULL;
    clay_false_obj.methods = NULL;

    /* Nil prototype */
    clay_Nil = clay_object_new();
    clay_Nil->parent = clay_Object;
    clay_Nil->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_method(clay_Nil, "toString", nil_toString);
    clay_set_method(clay_Nil, "type", nil_type);
    clay_set_method(clay_Nil, "==", nil_eq);
    clay_set_method(clay_Nil, "!=", nil_neq);

    /* Error prototype */
    clay_Error = clay_object_new();
    clay_Error->parent = clay_Object;
    clay_Error->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_error_init_methods(clay_Error);

    /* Loop control signal singletons (Error clones for break/continue) */
    clay_BreakSignal = clay_clone(clay_Error);
    clay_BreakSignal->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_BreakSignal, "_message", CLAY_VAL(clay_string_new("break")));
    clay_set_slot_take(clay_BreakSignal, "_type", CLAY_VAL(clay_string_new("BreakSignal")));
    clay_set_slot_take(clay_BreakSignal, "_name", CLAY_VAL(clay_string_new("BreakSignal")));

    clay_ContinueSignal = clay_clone(clay_Error);
    clay_ContinueSignal->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_ContinueSignal, "_message", CLAY_VAL(clay_string_new("continue")));
    clay_set_slot_take(clay_ContinueSignal, "_type", CLAY_VAL(clay_string_new("ContinueSignal")));
    clay_set_slot_take(clay_ContinueSignal, "_name", CLAY_VAL(clay_string_new("ContinueSignal")));

    /* Dict prototype */
    clay_Dict = clay_object_new();
    clay_Dict->parent = clay_Object;
    clay_Dict->type = CLAY_TYPE_DICT;
    clay_Dict->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_dict_init_methods(clay_Dict);

    /* Range prototype */
    clay_Range = clay_object_new();
    clay_Range->parent = clay_Object;
    clay_Range->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Range, "_name", CLAY_VAL(clay_string_new("Range")));
    clay_range_init_methods(clay_Range);

    /* Math object */
    clay_Math = clay_object_new();
    clay_Math->parent = clay_Object;
    clay_Math->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_math_init(clay_Math);

    /* Console object */
    clay_Console = clay_object_new();
    clay_Console->parent = clay_Object;
    clay_Console->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_method(clay_Console, "readLine", console_readLine);
    clay_set_method(clay_Console, "loadHistory", console_loadHistory);
    clay_set_method(clay_Console, "saveHistory", console_saveHistory);
    clay_set_method(clay_Console, "prompt", console_prompt);
    clay_set_method(clay_Console, "print", console_print);
    clay_set_method(clay_Console, "println", console_println);
    clay_set_method(clay_Console, "error", console_error);
    clay_set_method(clay_Console, "isTerminal", console_isTerminal);
    clay_set_method(clay_Console, "isInteractive", console_isInteractive);

    /* File object */
    clay_File = clay_object_new();
    clay_File->parent = clay_Object;
    clay_File->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_file_init(clay_File);

    /* System object — memory diagnostics */
    clay_System = clay_object_new();
    clay_System->parent = clay_Object;
    clay_System->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_System, "_name", CLAY_VAL(clay_string_new("System")));
    clay_set_method(clay_System, "allocCount", system_alloc_count);
    clay_set_method(clay_System, "deallocCount", system_dealloc_count);
    clay_set_method(clay_System, "liveObjects", system_live_objects);
    clay_set_method(clay_System, "memory", system_memory);
    clay_set_method(clay_System, "exit", system_exit);
    clay_set_method(clay_System, "run", system_run);
    clay_set_method(clay_System, "env", system_env);
    clay_set_method(clay_System, "args", system_args);
    clay_set_method(clay_System, "platform", system_platform);
    clay_set_method(clay_System, "version", system_version);
    clay_set_method(clay_System, "clock", system_clock);

    /* Register plugin system on System prototype */
    extern void clay_plugin_init_system(ClayObject *system_proto);
    clay_plugin_init_system(clay_System);

    /* FFI object — foreign function interface */
    clay_FFI = clay_object_new();
    clay_FFI->parent = clay_Object;
    clay_FFI->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_FFI, "_name", CLAY_VAL(clay_string_new("FFI")));
    clay_ffi_init(clay_FFI);

    /* Date prototype */
    clay_Date = clay_object_new();
    clay_Date->parent = clay_Object;
    clay_Date->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Date, "_name", CLAY_VAL(clay_string_new("Date")));
    clay_date_init_methods(clay_Date);

    /* Http prototype */
    clay_Http = clay_object_new();
    clay_Http->parent = clay_Object;
    clay_Http->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Http, "_name", CLAY_VAL(clay_string_new("Http")));
    clay_http_init(clay_Http);

    /* Regex prototype */
    clay_Regex = clay_object_new();
    clay_Regex->parent = clay_Object;
    clay_Regex->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Regex, "_name", CLAY_VAL(clay_string_new("Regex")));
    clay_regex_init_methods(clay_Regex);

    /* Crypto prototype: hashes, HMAC, secure random bytes (crypto.c) */
    clay_Crypto = clay_object_new();
    clay_Crypto->parent = clay_Object;
    clay_Crypto->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Crypto, "_name", CLAY_VAL(clay_string_new("Crypto")));
    {
        extern void clay_crypto_init_methods(ClayObject *proto);
        clay_crypto_init_methods(clay_Crypto);
    }

    /* Process prototype: run programs, capture or stream output (process.c) */
    clay_Process = clay_object_new();
    clay_Process->parent = clay_Object;
    clay_Process->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Process, "_name", CLAY_VAL(clay_string_new("Process")));
    {
        extern void clay_process_init_methods(ClayObject *proto);
        clay_process_init_methods(clay_Process);
    }

    /* Fiber prototype */
    clay_Fiber = clay_object_new();
    clay_Fiber->parent = clay_Object;
    clay_Fiber->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Fiber, "_name", CLAY_VAL(clay_string_new("Fiber")));
    clay_fiber_init_methods(clay_Fiber);

    /* Db prototype */
    clay_Db = clay_object_new();
    clay_Db->parent = clay_Object;
    clay_Db->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Db, "_name", CLAY_VAL(clay_string_new("Db")));
    clay_db_init(clay_Db);

    /* Vec3 prototype — 3D vector with inline float x/y/z */
    clay_Vec3 = clay_object_new();
    clay_Vec3->parent = clay_Object;
    clay_Vec3->type = CLAY_TYPE_VEC3;
    clay_Vec3->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Vec3, "_name", CLAY_VAL(clay_string_new("Vec3")));
    clay_vec3_init_methods(clay_Vec3);

    /* Thread prototype (requires -DCLAY_THREAD_SAFE for real threading) */
    clay_Thread = clay_object_new();
    clay_Thread->parent = clay_Object;
    clay_Thread->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Thread, "_name", CLAY_VAL(clay_string_new("Thread")));
    {
        extern void clay_thread_init(ClayObject *proto);
        clay_thread_init(clay_Thread);
    }

    /* Graphics prototype (SDL2 backend — may be stub if SDL2 not found) */
    clay_Graphics = clay_object_new();
    clay_Graphics->parent = clay_Object;
    clay_Graphics->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_Graphics, "_name", CLAY_VAL(clay_string_new("Graphics")));
    clay_graphics_init(clay_Graphics);

    /* VM prototype: run bytecode compiled by Clay programs (vm_bridge.c) */
    clay_VM = clay_object_new();
    clay_VM->parent = clay_Object;
    clay_VM->refcount = CLAY_REFCOUNT_IMMORTAL;
    clay_set_slot_take(clay_VM, "_name", CLAY_VAL(clay_string_new("VM")));
    {
        extern void clay_vm_init_methods(ClayObject *proto);
        clay_vm_init_methods(clay_VM);
    }
}

void clay_shutdown(void) {
    extern void clay_graphics_shutdown(void);
    extern void clay_plugin_shutdown(void);
    clay_graphics_shutdown();
    clay_plugin_shutdown();
}

/* Debug helper: print object details to stderr */
/* Return a human-readable name for an object (for inspect output) */
static const char *object_name(ClayObject *obj) {
    if (!obj) return "nil";
    /* Check well-known prototypes */
    if (obj == clay_Object)  return "Object";
    if (obj == clay_Number)  return "Number";
    if (obj == clay_String)  return "String";
    if (obj == clay_Array)   return "Array";
    if (obj == clay_Block)   return "Block";
    if (obj == clay_Nil)     return "Nil";
    if (obj == clay_Boolean) return "Boolean";
    if (obj == clay_Error)   return "Error";
    if (obj == clay_Dict)    return "Dict";
    if (obj == clay_Math)    return "Math";
    if (obj == clay_Console) return "Console";
    if (obj == clay_File)    return "File";
    if (obj == &clay_true_obj)  return "true";
    if (obj == &clay_false_obj) return "false";
    /* Check for _name slot */
    if (obj->slots) {
        ht_value_t v = ht_find(obj->slots, (ht_key_t)"_name");
        if (v) {
            ClayValue cv = (ClayValue)(uintptr_t)v;
            if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && CLAY_OBJ(cv)->type == CLAY_TYPE_STRING) {
                return clay_string_data(CLAY_OBJ(cv));
            }
        }
    }
    /* Fall back to type field */
    switch (obj->type) {
        case CLAY_TYPE_STRING: return "String";
        case CLAY_TYPE_ARRAY:  return "Array";
        case CLAY_TYPE_BLOCK:  return "Block";
        case CLAY_TYPE_FLOAT:  return "Float";
        case CLAY_TYPE_DICT:   return "Dict";
        default:               return "Object";
    }
}

void clay_inspect(ClayObject *obj) {
    if (!obj) {
        fprintf(stderr, "[nil]\n");
        return;
    }
    const char *type_name = object_name(obj);
    const char *parent_name = object_name(obj->parent);
    fprintf(stderr, "[%s @%p refcount=%d parent=%s", type_name, (void *)obj, obj->refcount, parent_name);

    if (obj->type == CLAY_TYPE_STRING) {
        fprintf(stderr, " data=\"%s\" len=%zu", obj->as.string.data, obj->as.string.length);
    } else if (obj->type == CLAY_TYPE_ARRAY) {
        fprintf(stderr, " size=%zu cap=%zu", obj->as.array.size, obj->as.array.capacity);
    }

    /* Print slots */
    if (obj->slots) {
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        fprintf(stderr, " slots={");
        int first = 1;
        while (ht_next(obj->slots, &pos, &key, &val)) {
            ClayValue cv = (ClayValue)(uintptr_t)val;
            if (!first) fprintf(stderr, ", ");
            first = 0;
            fprintf(stderr, "%s=", (const char *)key);
            if (CLAY_IS_INT(cv)) {
                fprintf(stderr, "%lld", (long long)clay_to_int(cv));
            } else if (CLAY_IS_NIL(cv)) {
                fprintf(stderr, "nil");
            } else if (cv == CLAY_TRUE) {
                fprintf(stderr, "true");
            } else if (cv == CLAY_FALSE) {
                fprintf(stderr, "false");
            } else if (CLAY_IS_PTR(cv) && CLAY_OBJ(cv)->type == CLAY_TYPE_STRING) {
                fprintf(stderr, "\"%s\"", clay_string_data(CLAY_OBJ(cv)));
            } else {
                fprintf(stderr, "@%p", (void *)(uintptr_t)cv);
            }
        }
        fprintf(stderr, "}");
    }
    fprintf(stderr, "]\n");
}
