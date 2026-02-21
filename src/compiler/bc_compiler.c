/*
 * bc_compiler.c - Bytecode compiler and native bridge for Clay VM REPL
 *
 * Compiles Clay AST to bytecode for the vmgen-generated VM.
 * Clay runtime objects are wrapped in CLAYVM_VAL_OBJECT values.
 * All Clay-specific operations (send, clone, etc.) go through native functions.
 */
#include "bc_compiler.h"
#include "clay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

/* --- Global state for trampoline access --- */

static BcCompiler *g_bc = NULL;  /* current compiler (for trampoline) */
static clayvm_vm_t *g_vm = NULL; /* current VM (for trampoline) */

/* Dispatch table from vm.c for direct chunk execution */
extern int (*clayvm_dispatch_table[])(clayvm_vm_t *vm);

/* --- Block trampoline ---
 * When a block's value() method is called, this trampoline:
 * 1. Reads chunk_id from self->as.block.env[0]
 * 2. Stores block params as temporary globals
 * 3. Runs the bytecode chunk on the VM
 * 4. Restores old global values
 * 5. Returns the result
 */
static ClayValue bc_block_trampoline(ClayObject *self, int argc, ClayValue *argv) {
    if (!g_bc || !g_vm) return CLAY_NIL;

    /* Read chunk_id from env[0] */
    int chunk_id = (int)clay_to_int(self->as.block.env[0]);
    if (chunk_id < 0 || chunk_id >= g_bc->chunk_count) return CLAY_NIL;

    BcChunk *chunk = g_bc->chunks[chunk_id];

    /* Save VM state for re-entrant execution */
    const uint8_t *saved_code = g_vm->code;
    size_t saved_code_len = g_vm->code_len;
    size_t saved_ip = g_vm->ip;
    size_t saved_sp = g_vm->sp;
    int saved_running = g_vm->running;
    int saved_frame_count = g_vm->frame_count;

    /* Set up the chunk's code — keep sp where it is to avoid clobbering
     * the caller's stack.  Push a call frame with base_sp = current sp
     * so GET_LOCAL/SET_LOCAL address our locals correctly. */
    g_vm->code = chunk->code;
    g_vm->code_len = chunk->len;
    g_vm->ip = 0;
    g_vm->running = 1;

    size_t base = g_vm->sp;   /* locals start here */

    if (g_vm->frame_count < CLAYVM_MAX_FRAMES) {
        g_vm->frames[g_vm->frame_count].return_ip = 0;
        g_vm->frames[g_vm->frame_count].base_sp = base;
        g_vm->frames[g_vm->frame_count].func_index = 0;
        g_vm->frame_count++;
    }

    /* Push params as stack locals (slots 0, 1, ...) */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        ClayValue val = (i < argc) ? argv[i] : CLAY_NIL;
        clayvm_vm_push(g_vm, clay_to_vm(val));
    }

    /* Push captured values as additional locals (slots param_count, param_count+1, ...) */
    int capture_count = chunk->capture_count;
    for (int i = 0; i < capture_count; i++) {
        ClayValue cv = (1 + i < self->as.block.env_size) ?
                        self->as.block.env[1 + i] : CLAY_NIL;
        clayvm_vm_push(g_vm, clay_to_vm(cv));
    }

    /* Run the dispatch loop manually (can't use vm_run — it resets sp) */
    while (g_vm->running && g_vm->ip < g_vm->code_len) {
        uint8_t opcode = g_vm->code[g_vm->ip++];
        if (opcode >= CLAYVM_OP_COUNT) {
            g_vm->running = 0;
            break;
        }
        if (g_vm->step_callback) g_vm->step_callback(g_vm, g_vm->ip - 1);

        int rc = clayvm_dispatch_table[opcode](g_vm);
        if (rc != 0) break;
    }

    /* Write back captured values from locals to env (by-reference semantics) */
    for (int i = 0; i < capture_count; i++) {
        size_t slot = base + (size_t)chunk->param_count + (size_t)i;
        if (slot < g_vm->sp && (1 + i) < self->as.block.env_size) {
            self->as.block.env[1 + i] = vm_to_clay(g_vm->stack[slot]);
        }
    }

    /* Get return value (last value on stack) */
    ClayValue retval = CLAY_NIL;
    if (g_vm->sp > base) {
        retval = vm_to_clay(g_vm->stack[g_vm->sp - 1]);
    }

    /* Restore full VM state */
    g_vm->code = saved_code;
    g_vm->code_len = saved_code_len;
    g_vm->ip = saved_ip;
    g_vm->sp = saved_sp;
    g_vm->running = saved_running;
    g_vm->frame_count = saved_frame_count;

    return retval;
}

/* --- Method trampolines ---
 * Methods need unique ClayMethodFunc pointers. We generate a fixed pool
 * of trampoline functions, each with a unique index.
 */

typedef struct {
    int chunk_id;
    int param_count;
} MethodSlot;

static MethodSlot g_method_slots[BC_MAX_METHOD_SLOTS];
static int g_method_slot_count = 0;

static ClayValue bc_method_dispatch(int slot_index, ClayObject *self, int argc, ClayValue *argv) {
    if (!g_bc || !g_vm || slot_index < 0 || slot_index >= g_method_slot_count) return CLAY_NIL;

    MethodSlot *ms = &g_method_slots[slot_index];
    if (ms->chunk_id < 0 || ms->chunk_id >= g_bc->chunk_count) return CLAY_NIL;

    BcChunk *chunk = g_bc->chunks[ms->chunk_id];

    /* Save and set self as global (methods access self via global) */
    HashTable *globals = (HashTable *)g_vm->globals;
    ClayValue saved_self = CLAY_NIL;
    int self_existed = 0;
    if (globals) {
        ht_value_t old_self = ht_find(globals, (ht_key_t)"self");
        if (old_self) {
            saved_self = vm_to_clay(*(clayvm_value_t *)old_self);
            self_existed = 1;
            free((void *)old_self);
            ht_remove(globals, (ht_key_t)"self");
        }
    }
    clayvm_value_t *self_stored = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *self_stored = CLAYVM_OBJ(self);
    ht_add(globals, (ht_key_t)strdup("self"), (ht_value_t)self_stored);

    /* Save full VM state */
    const uint8_t *saved_code = g_vm->code;
    size_t saved_code_len = g_vm->code_len;
    size_t saved_ip = g_vm->ip;
    size_t saved_sp = g_vm->sp;
    int saved_running = g_vm->running;
    int saved_frame_count = g_vm->frame_count;

    /* Set up chunk's code — keep sp to avoid clobbering caller's stack */
    g_vm->code = chunk->code;
    g_vm->code_len = chunk->len;
    g_vm->ip = 0;
    g_vm->running = 1;

    size_t base = g_vm->sp;

    if (g_vm->frame_count < CLAYVM_MAX_FRAMES) {
        g_vm->frames[g_vm->frame_count].return_ip = 0;
        g_vm->frames[g_vm->frame_count].base_sp = base;
        g_vm->frames[g_vm->frame_count].func_index = 0;
        g_vm->frame_count++;
    }

    /* Push params as stack locals (slots 0, 1, ...) */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        ClayValue val = (i < argc) ? argv[i] : CLAY_NIL;
        clayvm_vm_push(g_vm, clay_to_vm(val));
    }

    /* Run the dispatch loop */
    while (g_vm->running && g_vm->ip < g_vm->code_len) {
        uint8_t opcode = g_vm->code[g_vm->ip++];
        if (opcode >= CLAYVM_OP_COUNT) { g_vm->running = 0; break; }
        if (g_vm->step_callback) g_vm->step_callback(g_vm, g_vm->ip - 1);
        int rc = clayvm_dispatch_table[opcode](g_vm);
        if (rc != 0) break;
    }

    ClayValue retval = CLAY_NIL;
    if (g_vm->sp > base)
        retval = vm_to_clay(g_vm->stack[g_vm->sp - 1]);

    /* Restore full VM state */
    g_vm->code = saved_code;
    g_vm->code_len = saved_code_len;
    g_vm->ip = saved_ip;
    g_vm->sp = saved_sp;
    g_vm->running = saved_running;
    g_vm->frame_count = saved_frame_count;

    /* Restore self */
    ht_value_t cur_self = ht_find(globals, (ht_key_t)"self");
    if (cur_self) { free((void *)cur_self); ht_remove(globals, (ht_key_t)"self"); }
    if (self_existed) {
        clayvm_value_t *sv = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
        *sv = clay_to_vm(saved_self);
        ht_add(globals, (ht_key_t)strdup("self"), (ht_value_t)sv);
    }

    return retval;
}

/* Generate unique trampoline functions via macro */
#define METHOD_TRAMP(N) \
    static ClayValue bc_mtramp_##N(ClayObject *s, int ac, ClayValue *av) { \
        return bc_method_dispatch(N, s, ac, av); \
    }

METHOD_TRAMP(0)  METHOD_TRAMP(1)  METHOD_TRAMP(2)  METHOD_TRAMP(3)
METHOD_TRAMP(4)  METHOD_TRAMP(5)  METHOD_TRAMP(6)  METHOD_TRAMP(7)
METHOD_TRAMP(8)  METHOD_TRAMP(9)  METHOD_TRAMP(10) METHOD_TRAMP(11)
METHOD_TRAMP(12) METHOD_TRAMP(13) METHOD_TRAMP(14) METHOD_TRAMP(15)
METHOD_TRAMP(16) METHOD_TRAMP(17) METHOD_TRAMP(18) METHOD_TRAMP(19)
METHOD_TRAMP(20) METHOD_TRAMP(21) METHOD_TRAMP(22) METHOD_TRAMP(23)
METHOD_TRAMP(24) METHOD_TRAMP(25) METHOD_TRAMP(26) METHOD_TRAMP(27)
METHOD_TRAMP(28) METHOD_TRAMP(29) METHOD_TRAMP(30) METHOD_TRAMP(31)

static ClayMethodFunc g_method_trampoline_table[] = {
    bc_mtramp_0,  bc_mtramp_1,  bc_mtramp_2,  bc_mtramp_3,
    bc_mtramp_4,  bc_mtramp_5,  bc_mtramp_6,  bc_mtramp_7,
    bc_mtramp_8,  bc_mtramp_9,  bc_mtramp_10, bc_mtramp_11,
    bc_mtramp_12, bc_mtramp_13, bc_mtramp_14, bc_mtramp_15,
    bc_mtramp_16, bc_mtramp_17, bc_mtramp_18, bc_mtramp_19,
    bc_mtramp_20, bc_mtramp_21, bc_mtramp_22, bc_mtramp_23,
    bc_mtramp_24, bc_mtramp_25, bc_mtramp_26, bc_mtramp_27,
    bc_mtramp_28, bc_mtramp_29, bc_mtramp_30, bc_mtramp_31,
};

/* --- Bytecode emission helpers --- */

static void emit_byte(BcCompiler *bc, uint8_t b) {
    if (bc->len >= BC_MAX_CODE) { bc->had_error = 1; return; }
    bc->line_map[bc->len] = bc->current_line;
    bc->code[bc->len++] = b;
}

static void emit_u16(BcCompiler *bc, uint16_t v) {
    emit_byte(bc, (uint8_t)(v & 0xFF));
    emit_byte(bc, (uint8_t)(v >> 8));
}

static void emit_op(BcCompiler *bc, uint8_t op) {
    emit_byte(bc, op);
}

static void emit_op_u16(BcCompiler *bc, uint8_t op, uint16_t arg) {
    emit_op(bc, op);
    emit_u16(bc, arg);
}

static int add_constant_int(BcCompiler *bc, int64_t val) {
    return clayvm_vm_add_constant(bc->vm, CLAYVM_INT64(val));
}

static int add_constant_obj(BcCompiler *bc, void *obj) {
    return clayvm_vm_add_constant(bc->vm, CLAYVM_OBJ(obj));
}

static int add_constant_string(BcCompiler *bc, const char *s) {
    ClayObject *str = clay_string_new(s);
    clay_retain(str);
    return clayvm_vm_add_constant(bc->vm, CLAYVM_OBJ(str));
}

/* --- Conversion helpers --- */

ClayValue vm_to_clay(clayvm_value_t v) {
    switch (v.type) {
        case CLAYVM_VAL_INT64:  return clay_int((intptr_t)v.as.i64);
        case CLAYVM_VAL_BOOL:   return v.as.boolean ? CLAY_TRUE : CLAY_FALSE;
        case CLAYVM_VAL_NIL:    return CLAY_NIL;
        case CLAYVM_VAL_OBJECT: return CLAY_VAL(v.as.obj);
        case CLAYVM_VAL_DOUBLE: return clay_int((intptr_t)v.as.f64); /* truncate */
    }
    return CLAY_NIL;
}

clayvm_value_t clay_to_vm(ClayValue cv) {
    if (CLAY_IS_NIL(cv))   return CLAYVM_NIL_VAL;
    if (CLAY_IS_INT(cv))   return CLAYVM_INT64(clay_to_int(cv));
    if (cv == CLAY_TRUE)   return CLAYVM_BOOL(1);
    if (cv == CLAY_FALSE)  return CLAYVM_BOOL(0);
    return CLAYVM_OBJ((void*)(uintptr_t)cv);
}

/* --- Native bridge functions --- */

/* native_send(receiver, selector_str, arg1, arg2, ...) */
static int native_send_fn(clayvm_vm_t *vm, int argc) {
    if (argc < 2) { clayvm_vm_push(vm, CLAYVM_NIL_VAL); return 0; }

    /* Pop args in reverse: first popped = last arg */
    int msg_argc = argc - 2;
    ClayValue *args = NULL;
    if (msg_argc > 0) {
        args = (ClayValue *)malloc(sizeof(ClayValue) * msg_argc);
        for (int i = msg_argc - 1; i >= 0; i--)
            args[i] = vm_to_clay(clayvm_vm_pop(vm));
    }

    clayvm_value_t sel_val = clayvm_vm_pop(vm);
    clayvm_value_t recv_val = clayvm_vm_pop(vm);

    ClayValue receiver = vm_to_clay(recv_val);
    const char *selector = clay_string_data((ClayObject *)sel_val.as.obj);

    ClayValue result = clay_send_val(receiver, selector, msg_argc, args);
    free(args);

    clayvm_vm_push(vm, clay_to_vm(result));
    return 0;
}

/* native_clone(proto) -> cloned object */
static int native_clone_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue proto = vm_to_clay(clayvm_vm_pop(vm));
    ClayObject *obj = clay_clone(CLAY_OBJ(proto));
    clayvm_vm_push(vm, CLAYVM_OBJ(obj));
    return 0;
}

/* native_set_slot(obj, name_str, value) */
static int native_set_slot_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue val = vm_to_clay(clayvm_vm_pop(vm));
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    ClayValue obj = vm_to_clay(clayvm_vm_pop(vm));
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);
    clay_set_slot(CLAY_OBJ(obj), name, val);
    clayvm_vm_push(vm, clay_to_vm(obj));
    return 0;
}

/* native_println(value) */
static int native_println_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val = clayvm_vm_pop(vm);
    ClayValue cv = vm_to_clay(val);
    if (CLAY_IS_INT(cv)) {
        printf("%ld\n", (long)clay_to_int(cv));
    } else if (CLAY_IS_NIL(cv)) {
        printf("nil\n");
    } else if (cv == CLAY_TRUE) {
        printf("true\n");
    } else if (cv == CLAY_FALSE) {
        printf("false\n");
    } else {
        ClayValue sv = clay_send_val(cv, "toString", 0, NULL);
        printf("%s\n", clay_string_data(CLAY_OBJ(sv)));
        clay_release(CLAY_OBJ(sv));
    }
    clayvm_vm_push(vm, CLAYVM_NIL_VAL);
    return 0;
}

/* native_make_string(const_index) — push string from constant pool */
static int native_make_string_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t v = clayvm_vm_pop(vm);
    /* v is an OBJ pointing to a ClayObject string */
    clayvm_vm_push(vm, v);
    return 0;
}

/* native_make_block(chunk_id, param_count, capture0, ..., captureN, capture_count) */
static int native_make_block_fn(clayvm_vm_t *vm, int argc) {
    /* Pop capture_count (last arg) */
    clayvm_value_t cc_val = clayvm_vm_pop(vm);
    int capture_count = (int)cc_val.as.i64;

    /* Pop captured values in reverse order */
    clayvm_value_t captured[16];
    for (int i = capture_count - 1; i >= 0; i--) {
        captured[i] = clayvm_vm_pop(vm);
    }

    /* Pop param_count and chunk_id */
    clayvm_value_t pc_val = clayvm_vm_pop(vm);
    clayvm_value_t id_val = clayvm_vm_pop(vm);
    int chunk_id = (int)id_val.as.i64;

    (void)pc_val;
    (void)argc;

    /* Create env: env[0] = chunk_id, env[1..n] = captured values */
    int env_size = 1 + capture_count;
    ClayValue *env = (ClayValue *)malloc(sizeof(ClayValue) * (size_t)env_size);
    env[0] = clay_int(chunk_id);
    for (int i = 0; i < capture_count; i++) {
        env[1 + i] = vm_to_clay(captured[i]);
    }

    ClayObject *block = clay_block_new(bc_block_trampoline, env, env_size);
    clayvm_vm_push(vm, CLAYVM_OBJ(block));
    return 0;
}

/* native_make_array() — create empty array */
static int native_make_array_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayObject *arr = clay_array_new();
    clayvm_vm_push(vm, CLAYVM_OBJ(arr));
    return 0;
}

/* native_array_append(arr, val) — append to array, return array */
static int native_array_append_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val = clayvm_vm_pop(vm);
    clayvm_value_t arr_val = clayvm_vm_pop(vm);
    ClayObject *arr = (ClayObject *)arr_val.as.obj;
    clay_array_append(arr, vm_to_clay(val));
    clayvm_vm_push(vm, arr_val); /* return array for chaining */
    return 0;
}

/* native_set_method(obj, name_str, method_slot_index) */
static int native_set_method_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t slot_val = clayvm_vm_pop(vm);
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    clayvm_value_t obj_val = clayvm_vm_pop(vm);

    int slot_idx = (int)slot_val.as.i64;
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);
    ClayObject *obj = (ClayObject *)obj_val.as.obj;

    if (slot_idx >= 0 && slot_idx < 32) {
        clay_set_method(obj, name, g_method_trampoline_table[slot_idx]);
    }

    clayvm_vm_push(vm, obj_val);
    return 0;
}

/* native_load_global(name_str) */
static int native_load_global_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);

    /* Look up in globals hash table */
    HashTable *globals = (HashTable *)vm->globals;
    if (globals) {
        ht_value_t found = ht_find(globals, (ht_key_t)name);
        if (found) {
            clayvm_value_t *stored = (clayvm_value_t *)found;
            clayvm_vm_push(vm, *stored);
            return 0;
        }
    }
    clayvm_vm_push(vm, CLAYVM_NIL_VAL);
    return 0;
}

/* native_store_global(name_str, value) */
static int native_store_global_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val = clayvm_vm_pop(vm);
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);

    HashTable *globals = (HashTable *)vm->globals;
    if (!globals) return 0;

    /* Retain new object value */
    if (val.type == CLAYVM_VAL_OBJECT && val.as.obj) {
        clay_retain((ClayObject *)val.as.obj);
    }

    /* Allocate storage for the value */
    clayvm_value_t *stored = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *stored = val;

    /* Remove old if exists, releasing any object it held */
    ht_value_t old = ht_find(globals, (ht_key_t)name);
    if (old) {
        clayvm_value_t *old_val = (clayvm_value_t *)old;
        if (old_val->type == CLAYVM_VAL_OBJECT && old_val->as.obj) {
            clay_release((ClayObject *)old_val->as.obj);
        }
        free((void *)old);
        ht_remove(globals, (ht_key_t)name);
    }

    char *key_copy = strdup(name);
    ht_add(globals, (ht_key_t)key_copy, (ht_value_t)stored);

    clayvm_vm_push(vm, val);
    return 0;
}

/* --- Compiler init --- */

static int globals_str_compare(ht_key_t a, ht_key_t b) {
    return strcmp((const char *)a, (const char *)b) == 0;
}

void bc_compiler_init(BcCompiler *bc, clayvm_vm_t *vm) {
    memset(bc, 0, sizeof(*bc));
    bc->vm = vm;

    /* Set global pointers for trampoline access */
    g_bc = bc;
    g_vm = vm;

    /* Set up globals hash table */
    if (!vm->globals) {
        HashTable *g = ht_create();
        ht_set_hash_func(g, HT_HASH_STRING);
        ht_set_compare_func(g, globals_str_compare);
        vm->globals = g;
    }

    /* Register native functions */
    bc->native_send = clayvm_native_register(&vm->natives, "send", native_send_fn, -1);
    bc->native_clone = clayvm_native_register(&vm->natives, "clone", native_clone_fn, 1);
    bc->native_set_slot = clayvm_native_register(&vm->natives, "set_slot", native_set_slot_fn, 3);
    bc->native_set_method = clayvm_native_register(&vm->natives, "set_method", native_set_method_fn, 3);
    bc->native_println = clayvm_native_register(&vm->natives, "println", native_println_fn, 1);
    bc->native_make_block = clayvm_native_register(&vm->natives, "make_block", native_make_block_fn, 2);
    bc->native_make_array = clayvm_native_register(&vm->natives, "make_array", native_make_array_fn, 0);
    bc->native_load_global = clayvm_native_register(&vm->natives, "load_global", native_load_global_fn, 1);
    bc->native_store_global = clayvm_native_register(&vm->natives, "store_global", native_store_global_fn, 2);

    /* Pre-register Clay builtin prototypes as globals */
    struct { const char *name; ClayObject *obj; } builtins[] = {
        {"Object", clay_Object}, {"Number", clay_Number}, {"String", clay_String},
        {"Array", clay_Array}, {"Block", clay_Block}, {"Nil", clay_Nil},
        {"Boolean", clay_Boolean}, {"Error", clay_Error}, {"Dict", clay_Dict},
        {"Math", clay_Math}, {"Console", clay_Console}, {"File", clay_File},
        {NULL, NULL}
    };

    HashTable *g = (HashTable *)vm->globals;
    for (int i = 0; builtins[i].name; i++) {
        clayvm_value_t *stored = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
        *stored = CLAYVM_OBJ(builtins[i].obj);
        char *key = strdup(builtins[i].name);
        ht_add(g, (ht_key_t)key, (ht_value_t)stored);
    }

    /* Also store true/false/nil */
    clayvm_value_t *tv = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *tv = CLAYVM_BOOL(1);
    ht_add(g, (ht_key_t)strdup("true"), (ht_value_t)tv);

    clayvm_value_t *fv = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *fv = CLAYVM_BOOL(0);
    ht_add(g, (ht_key_t)strdup("false"), (ht_value_t)fv);

    clayvm_value_t *nv = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *nv = CLAYVM_NIL_VAL;
    ht_add(g, (ht_key_t)strdup("nil"), (ht_value_t)nv);
}

/* --- AST → Bytecode compilation --- */

static void bc_emit_expr(BcCompiler *bc, AstNode *node);
static void bc_emit_stmt(BcCompiler *bc, AstNode *node);

/* --- Local variable helpers --- */

static int is_builtin_global(const char *name) {
    return strcmp(name, "Object") == 0 ||
           strcmp(name, "Number") == 0 ||
           strcmp(name, "String") == 0 ||
           strcmp(name, "Array") == 0 ||
           strcmp(name, "Block") == 0 ||
           strcmp(name, "Nil") == 0 ||
           strcmp(name, "Boolean") == 0 ||
           strcmp(name, "Error") == 0 ||
           strcmp(name, "Dict") == 0 ||
           strcmp(name, "Math") == 0 ||
           strcmp(name, "Console") == 0 ||
           strcmp(name, "File") == 0 ||
           strcmp(name, "true") == 0 ||
           strcmp(name, "false") == 0 ||
           strcmp(name, "nil") == 0;
}

static int bc_resolve_local(BcCompiler *bc, const char *name) {
    for (int i = bc->local_count - 1; i >= 0; i--) {
        if (strcmp(bc->locals[i].name, name) == 0)
            return bc->locals[i].slot;
    }
    return -1; /* not a local */
}

static int bc_add_local(BcCompiler *bc, const char *name) {
    if (bc->local_count >= BC_MAX_LOCALS) return -1;
    int slot = bc->local_count;
    bc->locals[slot].name = strdup(name);
    bc->locals[slot].slot = slot;
    bc->locals[slot].depth = bc->scope_depth;
    bc->locals[slot].is_captured = 0;
    bc->local_count++;
    return slot;
}

static void bc_begin_scope(BcCompiler *bc) {
    bc->scope_depth++;
}

static void bc_end_scope(BcCompiler *bc) {
    while (bc->local_count > 0 &&
           bc->locals[bc->local_count - 1].depth >= bc->scope_depth) {
        bc->local_count--;
        free(bc->locals[bc->local_count].name);
        bc->locals[bc->local_count].name = NULL;
    }
    bc->scope_depth--;
}

/* --- Capture scanning --- */

typedef struct {
    char **names;
    int   *slots;  /* enclosing scope slot indices */
    int    count;
    int    capacity;
} BcCaptureList;

static void bc_capture_init(BcCaptureList *cl) {
    cl->names = NULL;
    cl->slots = NULL;
    cl->count = 0;
    cl->capacity = 0;
}

static int bc_capture_has(BcCaptureList *cl, const char *name) {
    for (int i = 0; i < cl->count; i++)
        if (strcmp(cl->names[i], name) == 0) return 1;
    return 0;
}

static void bc_capture_add(BcCaptureList *cl, const char *name, int slot) {
    if (bc_capture_has(cl, name)) return;
    if (cl->count >= cl->capacity) {
        cl->capacity = cl->capacity == 0 ? 4 : cl->capacity * 2;
        cl->names = (char **)realloc(cl->names, sizeof(char *) * (size_t)cl->capacity);
        cl->slots = (int *)realloc(cl->slots, sizeof(int) * (size_t)cl->capacity);
    }
    cl->names[cl->count] = strdup(name);
    cl->slots[cl->count] = slot;
    cl->count++;
}

static void bc_capture_free(BcCaptureList *cl) {
    for (int i = 0; i < cl->count; i++) free(cl->names[i]);
    free(cl->names);
    free(cl->slots);
    cl->names = NULL;
    cl->slots = NULL;
    cl->count = 0;
    cl->capacity = 0;
}

static int bc_is_builtin_global(const char *name) {
    return strcmp(name, "Object") == 0 || strcmp(name, "Number") == 0 ||
           strcmp(name, "String") == 0 || strcmp(name, "Array") == 0 ||
           strcmp(name, "Block") == 0 || strcmp(name, "Nil") == 0 ||
           strcmp(name, "Boolean") == 0 || strcmp(name, "Error") == 0 ||
           strcmp(name, "Dict") == 0 || strcmp(name, "Math") == 0 ||
           strcmp(name, "Console") == 0 || strcmp(name, "File") == 0 ||
           strcmp(name, "true") == 0 || strcmp(name, "false") == 0 ||
           strcmp(name, "nil") == 0;
}

static int bc_name_in_list(const char *name, char **list, int count) {
    for (int i = 0; i < count; i++)
        if (strcmp(list[i], name) == 0) return 1;
    return 0;
}

/* Resolve a name against the current scope's locals (for capture scanning).
 * Called from NODE_BLOCK before entering bc_compile_chunk, so bc->locals
 * contains the enclosing scope's locals.
 * Returns the slot index if found, -1 otherwise. */
static int bc_resolve_enclosing(BcCompiler *bc, const char *name) {
    for (int i = bc->local_count - 1; i >= 0; i--) {
        if (strcmp(bc->locals[i].name, name) == 0)
            return bc->locals[i].slot;
    }
    return -1; /* not in enclosing scope */
}

static void bc_scan_free_vars(AstNode *node, char **block_params, int param_count,
                              BcCaptureList *caps, BcCompiler *bc);

static void bc_scan_free_vars_list(NodeList *list, char **block_params, int param_count,
                                   BcCaptureList *caps, BcCompiler *bc) {
    for (int i = 0; i < list->count; i++)
        bc_scan_free_vars(list->items[i], block_params, param_count, caps, bc);
}

static void bc_scan_free_vars(AstNode *node, char **block_params, int param_count,
                              BcCaptureList *caps, BcCompiler *bc) {
    if (!node) return;
    switch (node->type) {
        case NODE_IDENT: {
            const char *name = node->as.ident.name;
            if (bc_is_builtin_global(name)) break;
            if (bc_name_in_list(name, block_params, param_count)) break;
            if (bc_capture_has(caps, name)) break;
            /* Check if it's a local in the enclosing scope */
            int slot = bc_resolve_enclosing(bc, name);
            if (slot >= 0) {
                bc_capture_add(caps, name, slot);
            }
            break;
        }
        case NODE_BLOCK:
            /* Scan INTO nested blocks to find transitive captures.
             * Variables used in a nested block that aren't the nested
             * block's own params still need to be captured at THIS level
             * so they're available when the nested block captures them. */
            {
                int nested_pc = node->as.block.params.count;
                char **nested_params = NULL;
                /* Build combined exclusion list: current params + nested params */
                int combined_count = param_count + nested_pc;
                char **combined = NULL;
                if (combined_count > 0) {
                    combined = (char **)malloc(sizeof(char *) * (size_t)combined_count);
                    for (int i = 0; i < param_count; i++)
                        combined[i] = block_params[i];
                    for (int i = 0; i < nested_pc; i++)
                        combined[param_count + i] = node->as.block.params.items[i]->as.ident.name;
                    nested_params = combined;
                }
                bc_scan_free_vars_list(&node->as.block.body,
                                       nested_params, combined_count, caps, bc);
                free(combined);
            }
            break;
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_STRING_LIT:
        case NODE_SELF:
            break;
        case NODE_SEND:
            bc_scan_free_vars(node->as.send.receiver, block_params, param_count, caps, bc);
            bc_scan_free_vars_list(&node->as.send.args, block_params, param_count, caps, bc);
            break;
        case NODE_BINARY:
            bc_scan_free_vars(node->as.binary.left, block_params, param_count, caps, bc);
            bc_scan_free_vars(node->as.binary.right, block_params, param_count, caps, bc);
            break;
        case NODE_ASSIGN:
            bc_scan_free_vars(node->as.assign.value, block_params, param_count, caps, bc);
            break;
        case NODE_CLONE:
            bc_scan_free_vars(node->as.clone.proto, block_params, param_count, caps, bc);
            bc_scan_free_vars_list(&node->as.clone.members, block_params, param_count, caps, bc);
            break;
        case NODE_METHOD_DEF:
            bc_scan_free_vars_list(&node->as.method_def.body, block_params, param_count, caps, bc);
            break;
        case NODE_SLOT_DEF:
            bc_scan_free_vars(node->as.slot_def.value, block_params, param_count, caps, bc);
            break;
        case NODE_ARRAY_LIT:
            bc_scan_free_vars_list(&node->as.array_lit.elements, block_params, param_count, caps, bc);
            break;
        case NODE_TRY_CATCH:
            bc_scan_free_vars(node->as.try_catch.try_block, block_params, param_count, caps, bc);
            if (node->as.try_catch.catch_block)
                bc_scan_free_vars(node->as.try_catch.catch_block, block_params, param_count, caps, bc);
            break;
        case NODE_RAISE:
            bc_scan_free_vars(node->as.raise.expr, block_params, param_count, caps, bc);
            break;
        case NODE_SLOT_ASSIGN:
            bc_scan_free_vars(node->as.slot_assign.receiver, block_params, param_count, caps, bc);
            bc_scan_free_vars(node->as.slot_assign.value, block_params, param_count, caps, bc);
            break;
        default:
            break;
    }
}

/* Compile a block/method body into a new bytecode chunk.
 * If captures is non-NULL, register captured variable names as locals
 * after params (slots param_count, param_count+1, ...).
 * Returns the chunk_id, or -1 on error. */
static int bc_compile_chunk(BcCompiler *bc, NodeList *body, NodeList *params,
                            BcCaptureList *captures, int is_method) {
    if (bc->chunk_count >= BC_MAX_CHUNKS) {
        bc->had_error = 1;
        return -1;
    }

    int chunk_id = bc->chunk_count++;

    /* Allocate chunk on heap */
    BcChunk *chunk = (BcChunk *)calloc(1, sizeof(BcChunk));
    chunk->code = (uint8_t *)malloc(BC_CHUNK_CODE);

    /* Record param names (strdup since AST may be freed) */
    chunk->param_count = params ? params->count : 0;
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        chunk->param_names[i] = strdup(params->items[i]->as.ident.name);
    }

    /* Save current bytecode and local scope */
    uint8_t *outer_buf = NULL;
    size_t saved_len = bc->len;
    if (saved_len > 0) {
        outer_buf = (uint8_t *)malloc(saved_len);
        memcpy(outer_buf, bc->code, saved_len);
    }

    /* Save outer local state */
    BcLocal saved_locals[BC_MAX_LOCALS];
    int saved_local_count = bc->local_count;
    int saved_scope_depth = bc->scope_depth;
    int saved_in_method = bc->in_method;
    BcLocal *saved_enclosing = bc->enclosing_locals;
    int saved_enclosing_count = bc->enclosing_local_count;
    if (saved_local_count > 0)
        memcpy(saved_locals, bc->locals, sizeof(BcLocal) * (size_t)saved_local_count);

    /* Set enclosing locals for capture scanning later */
    bc->enclosing_locals = saved_local_count > 0 ? saved_locals : NULL;
    bc->enclosing_local_count = saved_local_count;

    /* Start fresh local scope for chunk */
    bc->local_count = 0;
    bc->scope_depth = 0;
    bc->len = 0;
    if (is_method) bc->in_method = 1;

    /* Register params as locals (slot 0, 1, ...) */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        bc_add_local(bc, chunk->param_names[i]);
    }

    /* Register captured variables as locals (slots after params) */
    if (captures) {
        for (int i = 0; i < captures->count && i < 16; i++) {
            bc_add_local(bc, captures->names[i]);
        }
    }

    /* Compile body: intermediate stmts POP, last expr stays on stack */
    for (int i = 0; i < body->count; i++) {
        AstNode *stmt = body->items[i];
        int is_last = (i == body->count - 1);
        if (is_last && stmt->type != NODE_ASSIGN && stmt->type != NODE_SLOT_ASSIGN) {
            bc_emit_expr(bc, stmt);
        } else {
            bc_emit_stmt(bc, stmt);
        }
    }

    /* If body was empty, push nil */
    if (body->count == 0) {
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
    }

    chunk->local_count = bc->local_count;

    emit_op(bc, CLAYVM_OP_HALT);

    /* Copy compiled bytecode to chunk */
    chunk->len = bc->len;
    if (chunk->len > BC_CHUNK_CODE) chunk->len = BC_CHUNK_CODE;
    memcpy(chunk->code, bc->code, chunk->len);

    /* Restore outer code buffer and local state */
    bc->len = saved_len;
    if (outer_buf) {
        memcpy(bc->code, outer_buf, saved_len);
        free(outer_buf);
    }

    /* Free chunk-local names (they were strdup'd by bc_add_local) */
    for (int i = 0; i < bc->local_count; i++) {
        free(bc->locals[i].name);
    }

    bc->local_count = saved_local_count;
    bc->scope_depth = saved_scope_depth;
    bc->in_method = saved_in_method;
    bc->enclosing_locals = saved_enclosing;
    bc->enclosing_local_count = saved_enclosing_count;
    if (saved_local_count > 0)
        memcpy(bc->locals, saved_locals, sizeof(BcLocal) * (size_t)saved_local_count);

    bc->chunks[chunk_id] = chunk;
    return chunk_id;
}

static void bc_emit_expr(BcCompiler *bc, AstNode *node) {
    if (!node || bc->had_error) return;
    bc->current_line = node->line;

    switch (node->type) {
        case NODE_INT_LIT: {
            int idx = add_constant_int(bc, node->as.int_lit.value);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_FLOAT_LIT: {
            int idx = add_constant_obj(bc, clay_float_new(node->as.float_lit.value));
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_STRING_LIT: {
            int idx = add_constant_string(bc, node->as.string_lit.value);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_IDENT: {
            const char *ident_name = node->as.ident.name;
            /* Check locals first */
            int slot = bc_resolve_local(bc, ident_name);
            if (slot >= 0) {
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)slot);
            } else if (bc->in_method && !is_builtin_global(ident_name)) {
                /* Inside a method: bare identifier → implicit self slot lookup.
                 * Emit: load self, push slot name, send(name, 0 args) */
                int self_idx = add_constant_string(bc, "self");
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)self_idx);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_global);
                emit_byte(bc, 1);
                int sel_idx = add_constant_string(bc, ident_name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_send);
                emit_byte(bc, 2);
            } else {
                /* Load global variable via native call */
                int name_idx = add_constant_string(bc, ident_name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_global);
                emit_byte(bc, 1);
            }
            break;
        }

        case NODE_SELF: {
            /* Load self from globals (set by method trampoline) */
            int name_idx = add_constant_string(bc, "self");
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_load_global);
            emit_byte(bc, 1);
            break;
        }

        case NODE_ASSIGN: {
            /* Check if assigning to an existing local */
            int slot = bc_resolve_local(bc, node->as.assign.name);
            if (slot >= 0) {
                bc_emit_expr(bc, node->as.assign.value);
                emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)slot);
                /* SET_LOCAL doesn't pop — value stays on stack */
            } else if (bc->in_method && !is_builtin_global(node->as.assign.name)) {
                /* Inside a method, unknown vars are self slot writes (matches C codegen).
                 * Emit: push self, push slot name, push value, call set_slot */
                int self_idx = add_constant_string(bc, "self");
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)self_idx);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_global);
                emit_byte(bc, 1);
                int name_idx = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                bc_emit_expr(bc, node->as.assign.value);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_set_slot);
                emit_byte(bc, 3);
            } else {
                /* Global: push name first, compile value, call store_global */
                int name_idx = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                bc_emit_expr(bc, node->as.assign.value);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_store_global);
                emit_byte(bc, 2);
                /* Result left on stack for expression use */
            }
            break;
        }

        case NODE_SEND: {
            /* Push receiver */
            bc_emit_expr(bc, node->as.send.receiver);
            /* Push selector as string constant */
            int sel_idx = add_constant_string(bc, node->as.send.message);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
            /* Push args */
            for (int i = 0; i < node->as.send.args.count; i++) {
                bc_emit_expr(bc, node->as.send.args.items[i]);
            }
            /* Call native send: argc = 2 + arg_count (receiver + selector + args) */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_send);
            emit_byte(bc, (uint8_t)(2 + node->as.send.args.count));
            break;
        }

        case NODE_BINARY: {
            const char *op = node->as.binary.op;
            /* Check for pure integer ops */
            if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                strcmp(op, "%") == 0 || strcmp(op, "==") == 0 ||
                strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">=") == 0) {
                /* Emit as message send for type safety */
                bc_emit_expr(bc, node->as.binary.left);
                int sel_idx = add_constant_string(bc, op);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
                bc_emit_expr(bc, node->as.binary.right);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_send);
                emit_byte(bc, 3); /* receiver + selector + 1 arg */
            } else {
                /* Unknown op — treat as send */
                bc_emit_expr(bc, node->as.binary.left);
                int sel_idx = add_constant_string(bc, op);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
                bc_emit_expr(bc, node->as.binary.right);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_send);
                emit_byte(bc, 3);
            }
            break;
        }

        case NODE_BLOCK: {
            /* Scan for free variables BEFORE compiling the chunk
             * (enclosing_locals is set up inside bc_compile_chunk) */
            char **param_names = NULL;
            int param_count = node->as.block.params.count;
            if (param_count > 0) {
                param_names = (char **)malloc(sizeof(char *) * (size_t)param_count);
                for (int i = 0; i < param_count; i++)
                    param_names[i] = node->as.block.params.items[i]->as.ident.name;
            }

            BcCaptureList caps;
            bc_capture_init(&caps);
            for (int i = 0; i < node->as.block.body.count; i++)
                bc_scan_free_vars(node->as.block.body.items[i],
                                  param_names, param_count, &caps, bc);
            free(param_names);

            /* Compile block body to a new chunk (with capture locals) */
            int chunk_id = bc_compile_chunk(bc, &node->as.block.body,
                                            &node->as.block.params, &caps, 0);
            if (chunk_id < 0) {
                bc_capture_free(&caps);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
                break;
            }

            /* Store capture info in the chunk */
            BcChunk *chunk = bc->chunks[chunk_id];
            chunk->capture_count = caps.count < 16 ? caps.count : 16;
            for (int i = 0; i < chunk->capture_count; i++) {
                chunk->capture_names[i] = strdup(caps.names[i]);
                chunk->capture_slots[i] = caps.slots[i];
            }

            /* Emit: push chunk_id, push param_count */
            int id_idx = add_constant_int(bc, chunk_id);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)id_idx);
            int pc_idx = add_constant_int(bc, node->as.block.params.count);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)pc_idx);

            /* Push captured values (read from enclosing scope's locals) */
            for (int i = 0; i < chunk->capture_count; i++) {
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)caps.slots[i]);
            }

            /* Push capture_count */
            int cc_idx = add_constant_int(bc, chunk->capture_count);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)cc_idx);

            /* call native_make_block(chunk_id, param_count, captures..., capture_count) */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_make_block);
            emit_byte(bc, (uint8_t)(3 + chunk->capture_count));

            bc_capture_free(&caps);
            break;
        }

        case NODE_ARRAY_LIT: {
            /* Create empty array via native */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_make_array);
            emit_byte(bc, 0);
            /* For each element: DUP array, compile element, send append */
            int append_idx = add_constant_string(bc, "append");
            for (int i = 0; i < node->as.array_lit.elements.count; i++) {
                emit_op(bc, CLAYVM_OP_DUP); /* keep array on stack */
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)append_idx);
                bc_emit_expr(bc, node->as.array_lit.elements.items[i]);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_send);
                emit_byte(bc, 3); /* arr, "append", elem */
                emit_op(bc, CLAYVM_OP_POP); /* discard send result */
            }
            /* Array is left on stack */
            break;
        }

        case NODE_CLONE: {
            /* Push proto, call clone, then set slots/methods */
            bc_emit_expr(bc, node->as.clone.proto);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_clone);
            emit_byte(bc, 1);

            /* Set slots and methods on the cloned object */
            for (int i = 0; i < node->as.clone.members.count; i++) {
                AstNode *member = node->as.clone.members.items[i];
                if (member->type == NODE_SLOT_DEF) {
                    emit_op(bc, CLAYVM_OP_DUP); /* keep obj on stack */
                    int name_idx = add_constant_string(bc, member->as.slot_def.name);
                    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                    bc_emit_expr(bc, member->as.slot_def.value);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_set_slot);
                    emit_byte(bc, 3);
                    emit_op(bc, CLAYVM_OP_POP); /* discard set_slot return */
                } else if (member->type == NODE_METHOD_DEF) {
                    /* Compile method body to a chunk */
                    int chunk_id = bc_compile_chunk(bc,
                        &member->as.method_def.body,
                        &member->as.method_def.params, NULL, 1);
                    if (chunk_id < 0 || g_method_slot_count >= 32) continue;

                    /* Register in method slot table */
                    int slot_idx = g_method_slot_count++;
                    g_method_slots[slot_idx].chunk_id = chunk_id;
                    g_method_slots[slot_idx].param_count = member->as.method_def.params.count;

                    /* Emit: DUP obj, push name, push slot_idx, call set_method */
                    emit_op(bc, CLAYVM_OP_DUP);
                    int name_idx = add_constant_string(bc, member->as.method_def.name);
                    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                    int slot_const = add_constant_int(bc, slot_idx);
                    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)slot_const);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_set_method);
                    emit_byte(bc, 3);
                    emit_op(bc, CLAYVM_OP_POP); /* discard set_method return */
                }
            }
            break;
        }

        default:
            /* Unsupported node — push nil */
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
            break;
    }
}

static int is_clone_expr(AstNode *node) {
    if (!node) return 0;
    if (node->type == NODE_CLONE) return 1;
    if (node->type == NODE_SEND && strcmp(node->as.send.message, "clone") == 0)
        return 1;
    return 0;
}

static void bc_emit_stmt(BcCompiler *bc, AstNode *node) {
    if (!node || bc->had_error) return;
    bc->current_line = node->line;

    if (node->type == NODE_ASSIGN) {
        /* Check if assigning to an existing local */
        int slot = bc_resolve_local(bc, node->as.assign.name);
        if (slot >= 0) {
            bc_emit_expr(bc, node->as.assign.value);
            emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)slot);
            emit_op(bc, CLAYVM_OP_POP); /* discard (stmt context) */
        } else if (bc->in_method && !is_builtin_global(node->as.assign.name)) {
            /* Inside a method, unknown vars are self slot writes (matches C codegen) */
            bc_emit_expr(bc, node);  /* delegates to bc_emit_expr NODE_ASSIGN */
            emit_op(bc, CLAYVM_OP_POP);
        } else {
            /* Global: push name first, then compile value */
            int name_idx = add_constant_string(bc, node->as.assign.name);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
            bc_emit_expr(bc, node->as.assign.value);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_store_global);
            emit_byte(bc, 2);
            emit_op(bc, CLAYVM_OP_POP); /* discard result */

            /* Auto-set _name slot on clone results: Cat = Object clone → Cat._name = "Cat" */
            if (is_clone_expr(node->as.assign.value)) {
                /* Load the just-stored global back */
                int load_name = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)load_name);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_global);
                emit_byte(bc, 1);
                /* Push slot name "_name" */
                int slot_name = add_constant_string(bc, "_name");
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)slot_name);
                /* Push the variable name as value */
                int val_name = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)val_name);
                /* set_slot(obj, name, value) */
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_set_slot);
                emit_byte(bc, 3);
                emit_op(bc, CLAYVM_OP_POP);
            }
        }
        return;
    }

    /* Slot assignment: receiver slot = value */
    if (node->type == NODE_SLOT_ASSIGN) {
        /* Push receiver, slot name, value → call set_slot */
        bc_emit_expr(bc, node->as.slot_assign.receiver);
        int name_idx = add_constant_string(bc, node->as.slot_assign.slot_name);
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
        bc_emit_expr(bc, node->as.slot_assign.value);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_set_slot);
        emit_byte(bc, 3);
        emit_op(bc, CLAYVM_OP_POP);
        return;
    }

    /* Expression statement */
    bc_emit_expr(bc, node);
    emit_op(bc, CLAYVM_OP_POP);
}

int bc_compile(BcCompiler *bc, AstNode *program) {
    if (!program || program->type != NODE_PROGRAM) return -1;

    bc->len = 0;
    bc->had_error = 0;

    for (int i = 0; i < program->as.program.stmts.count; i++) {
        AstNode *stmt = program->as.program.stmts.items[i];
        int is_last = (i == program->as.program.stmts.count - 1);

        if (is_last && stmt->type != NODE_ASSIGN && stmt->type != NODE_SLOT_ASSIGN) {
            /* Last expression: leave on stack for REPL auto-print */
            bc_emit_expr(bc, stmt);
        } else {
            bc_emit_stmt(bc, stmt);
        }
    }

    emit_op(bc, CLAYVM_OP_HALT);
    return bc->had_error ? -1 : 0;
}
