/*
 * bc_compiler.c - Bytecode compiler for the Clay VM
 *
 * Compiles the Clay AST to bytecode for the vmgen-generated VM. Every
 * Clay-specific operation (send, clone, blocks, ...) is a call to a native
 * function of the runtime bridge (src/runtime/vm_bridge.c).
 */
#include "bc_compiler.h"
#include "clay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif


/* --- Bytecode emission helpers --- */

/* The source location the next bytes come from: node's line and file */
static void bc_set_loc(BcCompiler *bc, AstNode *node) {
    bc->current_line = node->line;
    if (node->file) bc->current_file = bc_file_index(bc, node->file);
}

static void emit_byte(BcCompiler *bc, uint8_t b) {
    if (bc->len >= BC_MAX_CODE) { bc->had_error = 1; return; }
    bc->line_map[bc->len] = bc->current_line;
    if (!bc->file_map) bc->file_map = (uint16_t *)calloc(BC_MAX_CODE, sizeof(uint16_t));
    if (bc->file_map) bc->file_map[bc->len] = bc->current_file;
    bc->code[bc->len++] = b;
}

static void emit_u16(BcCompiler *bc, uint16_t v) {
    emit_byte(bc, (uint8_t)(v & 0xFF));
    emit_byte(bc, (uint8_t)(v >> 8));
}

static void emit_op(BcCompiler *bc, uint8_t op) {
    bc->op_start[2] = bc->op_start[1];
    bc->op_start[1] = bc->op_start[0];
    bc->op_start[0] = (int)bc->len;
    emit_byte(bc, op);
}

/* Start a new piece of code (top level or a chunk): nothing to look back on */
static void bc_reset_ops(BcCompiler *bc) {
    bc->op_start[0] = bc->op_start[1] = bc->op_start[2] = -1;
    bc->last_target = 0;
}

/* JUMP_IF_FALSE tests Clay truthiness itself (nil, false, 0, +0.0), so a
 * to_bool or is_truthy call just before it adds nothing: drop them, unless a
 * jump lands after their start (its code would then skip part of the test).
 * selfhost/clay-bytecode dropTests does the same. */
static void bc_drop_truth_tests(BcCompiler *bc) {
    while (bc->op_start[0] >= 0 && bc->op_start[0] == (int)bc->len - 4 &&
           bc->op_start[0] >= bc->last_target &&
           bc->code[bc->len - 4] == CLAYVM_OP_CALL_NATIVE && bc->code[bc->len - 1] == 1) {
        int fn = bc->code[bc->len - 3] | (bc->code[bc->len - 2] << 8);
        if (fn != bc->native_is_truthy && fn != bc->native_to_bool) break;
        bc->len -= 4;
        bc->op_start[0] = bc->op_start[1];
        bc->op_start[1] = bc->op_start[2];
        bc->op_start[2] = -1;
    }
}

static void emit_op_u16(BcCompiler *bc, uint8_t op, uint16_t arg) {
    emit_op(bc, op);
    emit_u16(bc, arg);
}

/* Emit a jump instruction with a placeholder offset, return patch position */
static int emit_jump(BcCompiler *bc, uint8_t jump_op) {
    if (jump_op == CLAYVM_OP_JUMP_IF_FALSE) bc_drop_truth_tests(bc);
    emit_op(bc, jump_op);
    int patch_pos = (int)bc->len;
    emit_u16(bc, 0); /* placeholder */
    return patch_pos;
}

/* Patch a previously emitted jump to target the current position */
static void patch_jump(BcCompiler *bc, int patch_pos) {
    bc->last_target = (int)bc->len;
    int offset = (int)bc->len - (patch_pos + 2);
    bc->code[patch_pos]     = (uint8_t)(offset & 0xFF);
    bc->code[patch_pos + 1] = (uint8_t)((offset >> 8) & 0xFF);
}

/* Add a constant and return its index. Operands that name a constant are
 * u16, so a program (or a REPL session: one pool for every input) has at most
 * BC_MAX_CONSTANTS of them; past that it can't be compiled. */
#define BC_MAX_CONSTANTS 65536

static int bc_const_checked(BcCompiler *bc, clayvm_value_t v) {
    int idx = clayvm_vm_add_constant(bc->vm, v);
    if (idx >= BC_MAX_CONSTANTS) {
        if (!bc->had_error)
            fprintf(stderr, "%s:%d: error: too many constants (max %d)\n",
                    clay_source_file ? clay_source_file : "<input>",
                    bc->current_line, BC_MAX_CONSTANTS);
        bc->had_error = 1;
    }
    return idx;
}

/* nil, true, false, ints and floats are added once each (strings too, in
 * add_constant_string): constants are never changed, so uses share them.
 * selfhost/clay-bytecode addConstant does the same. */
static int bc_add_const(BcCompiler *bc, clayvm_value_t v) {
    if (v.type == CLAYVM_VAL_NIL || v.type == CLAYVM_VAL_BOOL ||
        v.type == CLAYVM_VAL_INT64 || v.type == CLAYVM_VAL_DOUBLE) {
        for (int i = 0; i < bc->vm->constants.count; i++) {
            clayvm_value_t c = bc->vm->constants.values[i];
            if (c.type != v.type) continue;
            if (v.type == CLAYVM_VAL_NIL ||
                (v.type == CLAYVM_VAL_BOOL && c.as.boolean == v.as.boolean) ||
                (v.type == CLAYVM_VAL_INT64 && c.as.i64 == v.as.i64) ||
                (v.type == CLAYVM_VAL_DOUBLE &&
                 memcmp(&c.as.f64, &v.as.f64, sizeof(double)) == 0))
                return i;
        }
    }
    return bc_const_checked(bc, v);
}

static int add_constant_int(BcCompiler *bc, int64_t val) {
    return bc_add_const(bc, CLAYVM_INT64(val));
}

static int add_constant_obj(BcCompiler *bc, void *obj) {
    return bc_const_checked(bc, CLAYVM_OBJ(obj));
}

static int add_constant_string(BcCompiler *bc, const char *s) {
    /* Deduplicate: reuse existing constant with the same string value */
    for (int i = 0; i < bc->vm->constants.count; i++) {
        clayvm_value_t v = bc->vm->constants.values[i];
        if (v.type == CLAYVM_VAL_OBJECT && v.as.obj) {
            ClayObject *obj = (ClayObject *)v.as.obj;
            if (obj->type == CLAY_TYPE_STRING &&
                strcmp(clay_string_data(obj), s) == 0) {
                return i;
            }
        }
    }
    ClayObject *str = clay_string_new(s);
    clay_retain(str);
    return bc_const_checked(bc, CLAYVM_OBJ(str));
}

/* --- AST → Bytecode compilation --- */

static void bc_emit_expr(BcCompiler *bc, AstNode *node);
static void bc_emit_stmt(BcCompiler *bc, AstNode *node);

/* --- Local variable helpers --- */

static int is_builtin_global(const char *name) {
    if (clay_plugin_is_global(name)) return 1;
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
           strcmp(name, "System") == 0 ||
           strcmp(name, "FFI") == 0 ||
           strcmp(name, "Date") == 0 ||
           strcmp(name, "Http") == 0 ||
           strcmp(name, "Regex") == 0 ||
           strcmp(name, "Fiber") == 0 ||
           strcmp(name, "Db") == 0 ||
           strcmp(name, "Graphics") == 0 ||
           strcmp(name, "BreakSignal") == 0 ||
           strcmp(name, "ContinueSignal") == 0 ||
           strcmp(name, "true") == 0 ||
           strcmp(name, "false") == 0 ||
           strcmp(name, "nil") == 0;
}

/* Check if a name exists as a user-defined global (compile-time tracking) */
static int bc_is_user_global(BcCompiler *bc, const char *name) {
    for (int i = 0; i < bc->known_global_count; i++) {
        if (strcmp(bc->known_globals[i], name) == 0) return 1;
    }
    /* Also check the VM's runtime globals table */
    if (bc->vm) {
        HashTable *globals = (HashTable *)bc->vm->globals;
        if (globals && ht_find(globals, (ht_key_t)name)) return 1;
    }
    return 0;
}

static int bc_same_file(const char *a, const char *b) {
    return a == b || (a && b && strcmp(a, b) == 0);
}

/* Is `name` a top-level variable of `file`? An assignment in a top-level
 * block writes a global only then: a variable that some other file happens to
 * assign at its top level stays the block's own. (A global that exists only
 * at run time has no known file and counts for every file.) */
static int bc_is_file_global(BcCompiler *bc, const char *name, const char *file) {
    int known = 0;
    for (int i = 0; i < bc->known_global_count; i++) {
        if (strcmp(bc->known_globals[i], name) != 0) continue;
        if (bc_same_file(bc->known_global_files[i], file)) return 1;
        known = 1;
    }
    return !known && bc_is_user_global(bc, name);
}

/* Register a name as a known global, assigned at the top level of `file` */
static void bc_register_global(BcCompiler *bc, const char *name, const char *file) {
    /* Skip if already known */
    for (int i = 0; i < bc->known_global_count; i++) {
        if (strcmp(bc->known_globals[i], name) == 0 &&
            bc_same_file(bc->known_global_files[i], file)) return;
    }
    if (bc->known_global_count >= bc->known_global_capacity) {
        bc->known_global_capacity = bc->known_global_capacity ? bc->known_global_capacity * 2 : 32;
        bc->known_globals = (char **)realloc(bc->known_globals,
            (size_t)bc->known_global_capacity * sizeof(char *));
        bc->known_global_files = (char **)realloc(bc->known_global_files,
            (size_t)bc->known_global_capacity * sizeof(char *));
    }
    bc->known_global_files[bc->known_global_count] = file ? strdup(file) : NULL;
    bc->known_globals[bc->known_global_count++] = strdup(name);
}

static int bc_resolve_local(BcCompiler *bc, const char *name) {
    for (int i = bc->local_count - 1; i >= 0; i--) {
        if (strcmp(bc->locals[i].name, name) == 0)
            return bc->locals[i].slot;
    }
    return -1; /* not a local */
}

/* Check if a resolved local slot is boxed in a cell (captured by inner block) */
static int bc_local_is_captured(BcCompiler *bc, int slot) {
    for (int i = 0; i < bc->local_count; i++) {
        if (bc->locals[i].slot == slot)
            return bc->locals[i].is_captured;
    }
    return 0;
}

static int bc_add_local(BcCompiler *bc, const char *name) {
    if (bc->local_count >= BC_MAX_LOCALS) return -1;
    int slot = bc->local_count;
    bc->locals[slot].name = strdup(name);
    bc->locals[slot].slot = slot;
    bc->locals[slot].depth = bc->scope_depth;
    bc->locals[slot].is_captured = 0;
    bc->local_count++;
    if (bc->local_count > bc->max_local_count) bc->max_local_count = bc->local_count;
    return slot;
}

/* --- Implicit-slot fallback ---
 * A bare assignment `x = v` at the top level of a method body normally makes
 * `x` a method local. If, when the method is entered, self (or its prototype
 * chain) already has a slot `x`, the name refers to that slot for the whole
 * invocation instead (Io-style updateSlot). Each such local gets a hidden
 * companion flag local named "?x" (not a valid identifier), set at entry;
 * reads and writes of `x` branch on it. Nested blocks that capture `x` also
 * capture "?x" so they make the same choice. The compiler's own hidden locals
 * are named "?:name" ("?:ret", "?:loop", "?:k", ...), so no flag can be taken
 * for one: a hidden "?last" was the flag of a variable named `last`. */

#define BC_SLOT_FLAG_MAX 136

static void bc_slot_flag_name(const char *name, char *out, size_t sz) {
    snprintf(out, sz, "?%s", name);
}

/* Slot index of the flag local for `name`, or -1 if `name` has none */
static int bc_slot_flag_local(BcCompiler *bc, const char *name) {
    char flag[BC_SLOT_FLAG_MAX];
    bc_slot_flag_name(name, flag, sizeof(flag));
    int flag_slot = bc_resolve_local(bc, flag);
    /* not the flag of an outer variable the name shadows (an inlined
     * counting loop's parameter) */
    if (flag_slot >= 0 && flag_slot < bc_resolve_local(bc, name)) return -1;
    return flag_slot;
}

/* Push the value of local `slot` (unwrapping its cell if captured) */
static void bc_emit_get_local_value(BcCompiler *bc, int slot) {
    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)slot);
    if (bc_local_is_captured(bc, slot)) {
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_cell_get);
        emit_byte(bc, 1);
    }
}

/* Read `name`: flag ? (self name) : local, either local possibly a captured
 * cell (GET_LOCAL_FLAGGED; the uncommon cases run in vm_bridge.c) */
static void bc_emit_flagged_read(BcCompiler *bc, const char *name, int slot, int flag) {
    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL_FLAGGED, (uint16_t)slot);
    emit_u16(bc, (uint16_t)flag);
    emit_u16(bc, (uint16_t)add_constant_string(bc, name));
}

/* Write the value on top of the stack to `name` (value stays on the stack):
 * flag ? self.name = v : local = v (SET_LOCAL_FLAGGED) */
static void bc_emit_flagged_write(BcCompiler *bc, const char *name, int slot, int flag) {
    emit_op_u16(bc, CLAYVM_OP_SET_LOCAL_FLAGGED, (uint16_t)slot);
    emit_u16(bc, (uint16_t)flag);
    emit_u16(bc, (uint16_t)add_constant_string(bc, name));
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
    if (clay_plugin_is_global(name)) return 1;
    return strcmp(name, "Object") == 0 || strcmp(name, "Number") == 0 ||
           strcmp(name, "String") == 0 || strcmp(name, "Array") == 0 ||
           strcmp(name, "Block") == 0 || strcmp(name, "Nil") == 0 ||
           strcmp(name, "Boolean") == 0 || strcmp(name, "Error") == 0 ||
           strcmp(name, "Dict") == 0 || strcmp(name, "Math") == 0 ||
           strcmp(name, "Console") == 0 || strcmp(name, "File") == 0 ||
           strcmp(name, "System") == 0 || strcmp(name, "FFI") == 0 ||
           strcmp(name, "Date") == 0 ||
           strcmp(name, "Http") == 0 ||
           strcmp(name, "Regex") == 0 ||
           strcmp(name, "Fiber") == 0 ||
           strcmp(name, "Db") == 0 ||
           strcmp(name, "Graphics") == 0 ||
           strcmp(name, "BreakSignal") == 0 ||
           strcmp(name, "ContinueSignal") == 0 ||
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

/* Capture `name` and, if it has one, its implicit-slot flag local "?name" */
static void bc_capture_add_with_flag(BcCaptureList *caps, const char *name, int slot,
                                     BcCompiler *bc) {
    bc_capture_add(caps, name, slot);
    char flag[BC_SLOT_FLAG_MAX];
    bc_slot_flag_name(name, flag, sizeof(flag));
    int fslot = bc_resolve_enclosing(bc, flag);
    if (fslot >= 0) bc_capture_add(caps, flag, fslot);
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
                bc_capture_add_with_flag(caps, name, slot, bc);
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
        case NODE_ASSIGN: {
            /* Check if assignment target is an enclosing local (needs capture) */
            const char *name = node->as.assign.name;
            if (!bc_is_builtin_global(name) &&
                !bc_name_in_list(name, block_params, param_count) &&
                !bc_capture_has(caps, name)) {
                int slot = bc_resolve_enclosing(bc, name);
                if (slot >= 0) bc_capture_add_with_flag(caps, name, slot, bc);
            }
            bc_scan_free_vars(node->as.assign.value, block_params, param_count, caps, bc);
            break;
        }
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
            if (node->as.try_catch.finally_block)
                bc_scan_free_vars(node->as.try_catch.finally_block, block_params, param_count, caps, bc);
            break;
        case NODE_RAISE:
            bc_scan_free_vars(node->as.raise.expr, block_params, param_count, caps, bc);
            break;
        case NODE_SLOT_ASSIGN:
            bc_scan_free_vars(node->as.slot_assign.receiver, block_params, param_count, caps, bc);
            bc_scan_free_vars(node->as.slot_assign.value, block_params, param_count, caps, bc);
            break;
        case NODE_RETURN: {
            /* A block's `return` returns from its method: capture the id */
            int rslot = bc_resolve_enclosing(bc, "?:ret");
            if (rslot >= 0) bc_capture_add(caps, "?:ret", rslot);
            if (node->as.return_stmt.expr)
                bc_scan_free_vars(node->as.return_stmt.expr, block_params, param_count, caps, bc);
            break;
        }
        case NODE_DICT_LIT:
            bc_scan_free_vars_list(&node->as.dict_lit.values, block_params, param_count, caps, bc);
            break;
        default:
            break;
    }
}

/* Pre-scan: identify which locals in the current scope will be captured
 * by nested blocks, so we can box them with cells before use. */
static void bc_prescan_list(BcCompiler *bc, NodeList *list);

static void bc_prescan_node(BcCompiler *bc, AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case NODE_BLOCK: {
            /* This block captures free vars from our scope — mark them */
            char **bparams = NULL;
            int pcount = node->as.block.params.count;
            if (pcount > 0) {
                bparams = (char **)malloc(sizeof(char *) * (size_t)pcount);
                for (int j = 0; j < pcount; j++)
                    bparams[j] = node->as.block.params.items[j]->as.ident.name;
            }
            BcCaptureList caps = {NULL, NULL, 0, 0};
            bc_scan_free_vars_list(&node->as.block.body, bparams, pcount, &caps, bc);
            for (int j = 0; j < caps.count; j++) {
                int slot = bc_resolve_local(bc, caps.names[j]);
                if (slot >= 0)
                    bc->locals[slot].is_captured = 1;
            }
            bc_capture_free(&caps);
            free(bparams);
            break;
        }
        case NODE_ASSIGN:
            bc_prescan_node(bc, node->as.assign.value);
            break;
        case NODE_SEND:
            if (ast_is_inline_while(node)) {
                /* an inlined loop's blocks' statements are part of this scope */
                bc_prescan_list(bc, &node->as.send.receiver->as.block.body);
                bc_prescan_list(bc, &node->as.send.args.items[0]->as.block.body);
                break;
            }
            if (ast_inline_count_kind(node)) {
                bc_prescan_node(bc, node->as.send.receiver);
                bc_prescan_list(bc, &node->as.send.args.items[0]->as.block.body);
                break;
            }
            bc_prescan_node(bc, node->as.send.receiver);
            if (ast_is_inline_if(node)) {
                /* an inlined block's statements are part of this scope */
                bc_prescan_list(bc, &node->as.send.args.items[0]->as.block.body);
                break;
            }
            bc_prescan_list(bc, &node->as.send.args);
            break;
        case NODE_BINARY:
            bc_prescan_node(bc, node->as.binary.left);
            bc_prescan_node(bc, node->as.binary.right);
            break;
        case NODE_TRY_CATCH:
            bc_prescan_node(bc, node->as.try_catch.try_block);
            if (node->as.try_catch.catch_block)
                bc_prescan_node(bc, node->as.try_catch.catch_block);
            if (node->as.try_catch.finally_block)
                bc_prescan_node(bc, node->as.try_catch.finally_block);
            break;
        case NODE_SLOT_ASSIGN:
            bc_prescan_node(bc, node->as.slot_assign.receiver);
            bc_prescan_node(bc, node->as.slot_assign.value);
            break;
        case NODE_RETURN:
            if (node->as.return_stmt.expr)
                bc_prescan_node(bc, node->as.return_stmt.expr);
            break;
        case NODE_DICT_LIT:
            bc_prescan_list(bc, &node->as.dict_lit.values);
            break;
        case NODE_ARRAY_LIT:
            bc_prescan_list(bc, &node->as.array_lit.elements);
            break;
        case NODE_RAISE:
            bc_prescan_node(bc, node->as.raise.expr);
            break;
        case NODE_DESTRUCTURE:
            bc_prescan_node(bc, node->as.destructure.value);
            break;
        case NODE_CLONE:
            /* Slot values run in this scope; method bodies are their own */
            bc_prescan_node(bc, node->as.clone.proto);
            for (int i = 0; i < node->as.clone.members.count; i++) {
                AstNode *m = node->as.clone.members.items[i];
                if (m && m->type == NODE_SLOT_DEF)
                    bc_prescan_node(bc, m->as.slot_def.value);
            }
            break;
        default:
            break;
    }
}

static void bc_prescan_list(BcCompiler *bc, NodeList *list) {
    for (int i = 0; i < list->count; i++)
        bc_prescan_node(bc, list->items[i]);
}

/* Check if a name is a known slot from the enclosing clone block */
/* A slot or method declared in the enclosing clone block? */
static int bc_is_known_member(BcCompiler *bc, const char *name) {
    for (int i = 0; i < bc->known_member_count; i++) {
        if (strcmp(bc->known_slots[i], name) == 0) return 1;
    }
    return 0;
}

static int bc_is_known_slot(BcCompiler *bc, const char *name) {
    for (int i = 0; i < bc->known_slot_count; i++) {
        if (strcmp(bc->known_slots[i], name) == 0) return 1;
    }
    return 0;
}

/* Does a bare assignment to `name` (in a statement of `file`) make a local of
 * the method or block being compiled? */
static int bc_assign_makes_local(BcCompiler *bc, const char *name, const char *file) {
    return !is_builtin_global(name) &&
           (bc->in_method || !bc_is_file_global(bc, name, file)) &&
           !bc_is_known_slot(bc, name) && bc_resolve_local(bc, name) < 0;
}

/* Pre-register method-local variables: walk the body for bare NODE_ASSIGN
 * names that aren't already locals or builtins, and register them as locals.
 * In a method (or a block inside one) this includes names of top-level
 * variables: assignment there never changes the program's variables. A
 * top-level block writes the top-level variables of its own file; any other
 * name it assigns is its local, even if another file has a top-level variable
 * of that name.
 * Skip names that are known slots — those should be slot writes to self.
 * This ensures prescan can correctly identify which locals nested blocks capture. */
static void bc_preregister_method_locals(BcCompiler *bc, NodeList *body) {
    for (int i = 0; i < body->count; i++) {
        AstNode *stmt = body->items[i];
        if (stmt && stmt->type == NODE_ASSIGN) {
            const char *name = stmt->as.assign.name;
            if (bc_assign_makes_local(bc, name, stmt->file))
                bc_add_local(bc, name);
        } else if (stmt && stmt->type == NODE_DESTRUCTURE) {
            for (int j = 0; j < stmt->as.destructure.targets.count; j++) {
                const char *name = stmt->as.destructure.targets.items[j]->as.ident.name;
                if (bc_assign_makes_local(bc, name, stmt->file))
                    bc_add_local(bc, name);
            }
        }
    }
}

/* Compile a block/method body into a new bytecode chunk.
 * If captures is non-NULL, register captured variable names as locals
 * after params (slots param_count, param_count+1, ...).
 * Returns the chunk_id, or -1 on error. */
static int bc_compile_chunk(BcCompiler *bc, NodeList *body, NodeList *params,
                            BcCaptureList *captures, int is_method) {
    if (bc->chunk_count >= bc->chunk_capacity) {
        bc->chunk_capacity *= 2;
        bc->chunks = (BcChunk **)realloc(bc->chunks,
            (size_t)bc->chunk_capacity * sizeof(BcChunk *));
    }

    int chunk_id = bc->chunk_count++;

    /* Allocate chunk on heap */
    BcChunk *chunk = (BcChunk *)calloc(1, sizeof(BcChunk));
    /* a body is all from one file: the file of the block or method node */
    chunk->file = bc->current_file ? bc->files[bc->current_file - 1] : NULL;

    /* Record param names (strdup since AST may be freed).
     * Default params (NODE_ASSIGN) encode name in assign.name. */
    chunk->param_count = params ? params->count : 0;
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        if (params->items[i]->type == NODE_ASSIGN) {
            chunk->param_names[i] = strdup(params->items[i]->as.assign.name);
        } else {
            chunk->param_names[i] = strdup(params->items[i]->as.ident.name);
        }
    }

    /* Save current bytecode, line map, and local scope */
    uint8_t *outer_buf = NULL;
    int *outer_line_map = NULL;
    uint16_t *outer_file_map = NULL;
    size_t saved_len = bc->len;
    int saved_op_start[3] = { bc->op_start[0], bc->op_start[1], bc->op_start[2] };
    int saved_last_target = bc->last_target;
    bc_reset_ops(bc);
    if (saved_len > 0) {
        outer_buf = (uint8_t *)malloc(saved_len);
        memcpy(outer_buf, bc->code, saved_len);
        outer_line_map = (int *)malloc(saved_len * sizeof(int));
        memcpy(outer_line_map, bc->line_map, saved_len * sizeof(int));
        if (bc->file_map) {
            outer_file_map = (uint16_t *)malloc(saved_len * sizeof(uint16_t));
            memcpy(outer_file_map, bc->file_map, saved_len * sizeof(uint16_t));
        }
    }
    int saved_current_line = bc->current_line;
    uint16_t saved_current_file = bc->current_file;

    /* Save outer local state */
    BcLocal saved_locals[BC_MAX_LOCALS];
    int saved_local_count = bc->local_count;
    int saved_scope_depth = bc->scope_depth;
    int saved_in_method = bc->in_method;
    int saved_chunk_is_method = bc->chunk_is_method;
    int saved_chunk_loop_owner = bc->chunk_loop_owner;
    int saved_max_local_count = bc->max_local_count;
    void *saved_inline_exit = bc->inline_exit;
    void *saved_inline_loop = bc->inline_loop;
    BcLocal *saved_enclosing = bc->enclosing_locals;
    int saved_enclosing_count = bc->enclosing_local_count;
    if (saved_local_count > 0)
        memcpy(saved_locals, bc->locals, sizeof(BcLocal) * (size_t)saved_local_count);

    /* Set enclosing locals for capture scanning later */
    bc->enclosing_locals = saved_local_count > 0 ? saved_locals : NULL;
    bc->enclosing_local_count = saved_local_count;

    /* Start fresh local scope for chunk */
    bc->local_count = 0;
    bc->max_local_count = 0;
    bc->scope_depth = 0;
    bc->chunk_depth++;
    bc->inline_exit = NULL;
    bc->inline_loop = NULL;
    bc->len = 0;
    if (is_method) bc->in_method = 1;
    bc->chunk_is_method = is_method;
    /* a block passed to a message: its continue ends this run (nil) */
    bc->chunk_loop_owner = !is_method && bc->next_loop_owner;
    bc->next_loop_owner = 0;
    chunk->loop_owner = bc->chunk_loop_owner;

    /* Register params as locals (slot 0, 1, ...) */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        bc_add_local(bc, chunk->param_names[i]);
    }

    /* Register captured variables as locals (slots after params).
     * Mark them as captured so reads/writes go through cell indirection. */
    if (captures) {
        for (int i = 0; i < captures->count && i < BC_MAX_CAPTURES; i++) {
            int slot = bc_add_local(bc, captures->names[i]);
            bc->locals[slot].is_captured = 1;
        }
    }

    /* Pre-register locals: scan body for bare assignments that create new
     * local variables (not params, captures, builtins, or user globals).
     * This ensures prescan can identify which locals nested blocks capture,
     * enabling proper cell-boxing for closures that escape their scope. */
    /* Non-local return: a method whose blocks `return` gets a hidden "?:ret"
     * local holding this activation's id (registered before the prescan so
     * nested blocks capture it) */
    int ret_slot = -1;
    if (is_method && ast_has_block_return_inlined(body)) {
        chunk->has_block_return = 1;
        ret_slot = bc_add_local(bc, "?:ret");
    }

    int prereg_start = bc->local_count;
    bc_preregister_method_locals(bc, body);
    int prereg_end = bc->local_count;

    /* Implicit-slot fallback: give each local created by a bare assignment in
     * a method (or in a block inside a method) a flag local, registered before
     * the prescan so nested blocks can capture it too. */
    int flag_start = bc->local_count;
    if (bc->in_method) {
        for (int i = prereg_start; i < prereg_end; i++) {
            char flag[BC_SLOT_FLAG_MAX];
            bc_slot_flag_name(bc->locals[i].name, flag, sizeof(flag));
            bc_add_local(bc, flag);
        }
    }
    int flag_end = bc->local_count;

    /* Pre-scan body for nested blocks to identify params that will be captured.
     * This must happen AFTER params are registered as locals. */
    bc_prescan_list(bc, body);

    /* First thing on entry: read this activation's return id (before any
     * nested call can start another method) */
    if (ret_slot >= 0) {
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_method_return_id);
        emit_byte(bc, 0);
        emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)ret_slot);
        emit_op(bc, CLAYVM_OP_POP);
    }

    /* Set each flag at method entry: does self already have this slot?
     * (Before cell boxing below, so a captured flag is boxed with its value.) */
    for (int i = flag_start; i < flag_end; i++) {
        const char *name = bc->locals[prereg_start + (i - flag_start)].name;
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)add_constant_string(bc, name));
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_self_has_slot);
        emit_byte(bc, 1);
        emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)i);
        emit_op(bc, CLAYVM_OP_POP);
    }

    /* Box any locals that are marked as captured (params + method-created locals).
     * Skip capture locals (they're already cells from enclosing scope).
     * Emit: GET_LOCAL(slot) → cell_new → SET_LOCAL(slot) */
    int capture_start = chunk->param_count;
    int capture_end = chunk->param_count + (captures ? captures->count : 0);
    for (int i = 0; i < bc->local_count; i++) {
        if (bc->locals[i].is_captured && (i < capture_start || i >= capture_end)) {
            emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)i);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_cell_new);
            emit_byte(bc, 1);
            emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)i);
            emit_op(bc, CLAYVM_OP_POP);
        }
    }

    /* Emit default parameter values: for each param with a default expression,
     * check if it's nil (not provided by caller) and set the default. */
    if (params) {
        for (int i = 0; i < params->count && i < 16; i++) {
            if (params->items[i]->type == NODE_ASSIGN) {
                AstNode *default_expr = params->items[i]->as.assign.value;
                /* Check if param is nil (wasn't provided by caller) */
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)i);
                if (bc_local_is_captured(bc, i)) {
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_cell_get);
                    emit_byte(bc, 1);
                }
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_is_nil);
                emit_byte(bc, 1);
                int skip_jump = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
                /* Param was nil — emit default and store */
                if (bc_local_is_captured(bc, i)) {
                    /* Cell-boxed: cell_set(cell, default_val) */
                    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)i);
                    bc_emit_expr(bc, default_expr);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_cell_set);
                    emit_byte(bc, 2);
                    emit_op(bc, CLAYVM_OP_POP);
                } else {
                    bc_emit_expr(bc, default_expr);
                    emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)i);
                    emit_op(bc, CLAYVM_OP_POP);
                }
                patch_jump(bc, skip_jump);
            }
        }
    }

    /* Compile body: intermediate stmts POP, last expr stays on stack */
    for (int i = 0; i < body->count; i++) {
        AstNode *stmt = body->items[i];
        int is_last = (i == body->count - 1);
        if (is_last && stmt->type != NODE_RETURN &&
            stmt->type != NODE_BREAK && stmt->type != NODE_CONTINUE) {
            bc_emit_expr(bc, stmt);
        } else {
            bc_emit_stmt(bc, stmt);
        }
    }

    /* If body was empty, push nil as return value */
    if (body->count == 0) {
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
    }

    chunk->local_count = bc->max_local_count;

    emit_op(bc, CLAYVM_OP_HALT);

    /* Copy compiled bytecode and line map to chunk */
    chunk->len = bc->len;
    chunk->code = (uint8_t *)malloc(chunk->len ? chunk->len : 1);
    memcpy(chunk->code, bc->code, chunk->len);
    chunk->line_map = (int *)malloc(chunk->len * sizeof(int));
    memcpy(chunk->line_map, bc->line_map, chunk->len * sizeof(int));

    /* Restore outer code buffer, line map, and local state */
    bc->len = saved_len;
    memcpy(bc->op_start, saved_op_start, sizeof(saved_op_start));
    bc->last_target = saved_last_target;
    if (outer_buf) {
        memcpy(bc->code, outer_buf, saved_len);
        free(outer_buf);
    }
    if (outer_line_map) {
        memcpy(bc->line_map, outer_line_map, saved_len * sizeof(int));
        free(outer_line_map);
    }
    if (outer_file_map) {
        memcpy(bc->file_map, outer_file_map, saved_len * sizeof(uint16_t));
        free(outer_file_map);
    }
    bc->current_line = saved_current_line;
    bc->current_file = saved_current_file;

    /* Free chunk-local names (they were strdup'd by bc_add_local) */
    for (int i = 0; i < bc->local_count; i++) {
        free(bc->locals[i].name);
    }

    bc->local_count = saved_local_count;
    bc->max_local_count = saved_max_local_count;
    bc->inline_exit = saved_inline_exit;
    bc->inline_loop = saved_inline_loop;
    bc->chunk_depth--;
    bc->scope_depth = saved_scope_depth;
    bc->in_method = saved_in_method;
    bc->chunk_is_method = saved_chunk_is_method;
    bc->chunk_loop_owner = saved_chunk_loop_owner;
    bc->enclosing_locals = saved_enclosing;
    bc->enclosing_local_count = saved_enclosing_count;
    if (saved_local_count > 0)
        memcpy(bc->locals, saved_locals, sizeof(BcLocal) * (size_t)saved_local_count);

    bc->chunks[chunk_id] = chunk;
    return chunk_id;
}

/* Does a bare name that isn't a local resolve to a slot or method of self?
 * Inside a method: yes unless it's a builtin global, or a top-level variable
 * not shadowed by a slot declared in the clone body (codegen.c's
 * call_targets_self and clay-bytecode's selfSlotName agree). */
static int bc_name_is_self_slot(BcCompiler *bc, const char *name) {
    return bc->in_method && !is_builtin_global(name) &&
           (bc_is_known_member(bc, name) || !bc_is_user_global(bc, name));
}

/* --- Inlined ifTrue/ifFalse ---
 * In a method or block body, `c ifTrue { ... }` / `c ifFalse { ... }` with a
 * parameterless block literal (ast_is_inline_if) runs the block's statements
 * in place: no block is made or sent. Variables the block assigns are still
 * its own: locals of an inner scope, nil and with their implicit-slot flags
 * set each time it runs (as bc_compile_chunk does for a block). Its `return`
 * returns from the method directly when the body is a method's. The test
 * itself (clay_cond_test) accepts what the methods do: true and false, and
 * for a lone ifTrue/ifFalse a condition result. Two in a row,
 * `c ifTrue { a } ifFalse { b }`, is one test choosing a or b. Codegen and
 * clay-bytecode inline the same sends. */

static void bc_emit_native(BcCompiler *bc, int fn, int argc) {
    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
    emit_u16(bc, (uint16_t)fn);
    emit_byte(bc, (uint8_t)argc);
}

static int bc_compare_op(AstNode *n) {
    if (!n || n->type != NODE_BINARY) return -1;
    const char *op = n->as.binary.op;
    if (strcmp(op, "==") == 0) return CLAYVM_OP_EQ;
    if (strcmp(op, "!=") == 0) return CLAYVM_OP_NEQ;
    if (strcmp(op, "<")  == 0) return CLAYVM_OP_LT;
    if (strcmp(op, ">")  == 0) return CLAYVM_OP_GT;
    if (strcmp(op, "<=") == 0) return CLAYVM_OP_LTE;
    if (strcmp(op, ">=") == 0) return CLAYVM_OP_GTE;
    return -1;
}

/* An inlined whileTrue/whileFalse (bc_emit_inline_while) or counting loop
 * (bc_emit_inline_count) */
typedef struct {
    int  top;       /* the condition's start (continue's target) */
    int  depth;     /* values above the loop's stack (lone ifTrue values) */
    int *breaks;    /* patch positions of the breaks' jumps */
    int  count, capacity;
    int  fwd_cont;  /* continue jumps forward, to the step (conts) */
    int *conts;
    int  ccount, ccapacity;
    int  nil_res;   /* >= 0: a break or continue sets this local to nil (times' value) */
} BcLoop;

/* An inlined block body being compiled: outside methods a `return` in it
 * ends just that block (as it would end the block sent ifTrue), so it jumps
 * to the body's end */
typedef struct {
    int  want_value;
    int *jumps;     /* patch positions of the returns' jumps */
    int  count, capacity;
} BcInlineExit;

/* Run an inlined block's statements; with want_value its value is left on
 * the stack */
/* pname: a counting loop's parameter, a local of the body set from local
 * src each run (NULL for none) */
static void bc_emit_inline_body_p(BcCompiler *bc, NodeList *body, int want_value,
                                  const char *pname, int src) {
    BcInlineExit exit = { want_value, NULL, 0, 0 };
    void *outer_exit = bc->inline_exit;
    bc->inline_exit = &exit;
    bc_begin_scope(bc);
    int pslot = pname ? bc_add_local(bc, pname) : -1;
    int pre_start = bc->local_count;
    bc_preregister_method_locals(bc, body);
    int pre_end = bc->local_count;
    if (bc->in_method) {
        for (int i = pre_start; i < pre_end; i++) {
            char flag[BC_SLOT_FLAG_MAX];
            bc_slot_flag_name(bc->locals[i].name, flag, sizeof(flag));
            bc_add_local(bc, flag);
        }
    }
    int flag_end = bc->local_count;
    bc_prescan_list(bc, body);

    if (pslot >= 0) {
        emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)src);
        emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)pslot);
        emit_op(bc, CLAYVM_OP_POP);
        if (bc->locals[pslot].is_captured) {   /* a fresh cell each run */
            emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)pslot);
            bc_emit_native(bc, bc->native_cell_new, 1);
            emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)pslot);
            emit_op(bc, CLAYVM_OP_POP);
        }
    }

    /* the block's variables start out nil each time it runs */
    for (int i = pre_start; i < pre_end; i++) {
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
        emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)i);
        emit_op(bc, CLAYVM_OP_POP);
    }
    for (int i = pre_end; i < flag_end; i++) {
        const char *name = bc->locals[pre_start + (i - pre_end)].name;
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)add_constant_string(bc, name));
        bc_emit_native(bc, bc->native_self_has_slot, 1);
        emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)i);
        emit_op(bc, CLAYVM_OP_POP);
    }
    for (int i = pre_start; i < flag_end; i++) {
        if (bc->locals[i].is_captured) {
            emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)i);
            bc_emit_native(bc, bc->native_cell_new, 1);
            emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)i);
            emit_op(bc, CLAYVM_OP_POP);
        }
    }

    int pushed = 0;
    for (int i = 0; i < body->count; i++) {
        AstNode *stmt = body->items[i];
        if (want_value && i == body->count - 1 && stmt->type != NODE_RETURN &&
            stmt->type != NODE_BREAK && stmt->type != NODE_CONTINUE) {
            bc_emit_expr(bc, stmt);
            pushed = 1;
        } else {
            bc_emit_stmt(bc, stmt);
        }
    }
    if (want_value && !pushed)
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
    for (int i = 0; i < exit.count; i++) patch_jump(bc, exit.jumps[i]);
    free(exit.jumps);
    bc_end_scope(bc);
    bc->inline_exit = outer_exit;
}

static void bc_emit_inline_body(BcCompiler *bc, NodeList *body, int want_value) {
    bc_emit_inline_body_p(bc, body, want_value, NULL, -1);
}

/* Push 1 if `cond <sel>` should run its block, else 0. A comparison is
 * tested directly (it always gives true or false); anything else goes
 * through test_fn (if_true, chain_true, ...), keeping a copy of the value
 * under the result when keep is set. */
static void bc_emit_if_test(BcCompiler *bc, AstNode *send, AstNode *cond,
                            int want_true, int test_fn, int keep) {
    int cmp = bc_compare_op(cond);
    if (cmp >= 0) {
        bc_set_loc(bc, cond);
        bc_emit_expr(bc, cond->as.binary.left);
        bc_emit_expr(bc, cond->as.binary.right);
        emit_op(bc, (uint8_t)cmp);
        bc_emit_native(bc, bc->native_is_truthy, 1);
        if (!want_true) emit_op(bc, CLAYVM_OP_NOT);
        return;
    }
    bc_emit_expr(bc, cond);
    if (keep) emit_op(bc, CLAYVM_OP_DUP);
    bc_set_loc(bc, send);
    bc_emit_native(bc, test_fn, 1);
}

static void bc_emit_inline_if(BcCompiler *bc, AstNode *node, int want_value) {
    AstNode *recv = node->as.send.receiver;
    NodeList *body = &node->as.send.args.items[0]->as.block.body;
    int is_true = strcmp(node->as.send.message, "ifTrue") == 0;

    if (ast_is_inline_if(recv)) {
        /* c s1 { a } s2 { b }: a when s1 matches c, else b */
        AstNode *cond = recv->as.send.receiver;
        int first_true = strcmp(recv->as.send.message, "ifTrue") == 0;
        bc_emit_if_test(bc, recv, cond, first_true,
                        first_true ? bc->native_chain_true : bc->native_chain_false, 0);
        int to_second = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
        bc_emit_inline_body(bc, &recv->as.send.args.items[0]->as.block.body, want_value);
        int to_end = emit_jump(bc, CLAYVM_OP_JUMP);
        patch_jump(bc, to_second);
        bc_emit_inline_body(bc, body, want_value);
        patch_jump(bc, to_end);
        return;
    }

    int test_fn = is_true ? bc->native_if_true : bc->native_if_false;
    if (!want_value) {
        bc_emit_if_test(bc, node, recv, is_true, test_fn, 0);
        int to_end = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
        bc_emit_inline_body(bc, body, 0);
        patch_jump(bc, to_end);
        return;
    }

    /* the value of a lone ifTrue/ifFalse: a condition result (if_ran /
     * if_skipped), from the receiver kept on the stack; a comparison's is
     * true or false, so `true` stands in for it */
    int is_cmp = bc_compare_op(recv) >= 0;
    bc_emit_if_test(bc, node, recv, is_true, test_fn, 1);
    int to_skip = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
    if (is_cmp)
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, clay_to_vm(CLAY_TRUE)));
    BcLoop *loop = (BcLoop *)bc->inline_loop;
    if (loop) loop->depth++;
    bc_emit_inline_body(bc, body, 1);
    if (loop) loop->depth--;
    bc_emit_native(bc, bc->native_if_ran, 2);
    int to_end = emit_jump(bc, CLAYVM_OP_JUMP);
    patch_jump(bc, to_skip);
    if (is_cmp)
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, clay_to_vm(CLAY_TRUE)));
    bc_emit_native(bc, bc->native_if_skipped, 1);
    patch_jump(bc, to_end);
}

/* --- Inlined whileTrue/whileFalse ---
 * `{ c } whileTrue { ... }` whose breaks and continues can all be jumps
 * (ast_is_inline_while) runs both blocks in place, as inlined bodies: a
 * break jumps past the loop, a continue back to the condition, after popping
 * what the inlined ifTrue/ifFalse around it left on the stack (`depth`). The
 * loop's value, the body's last value, is kept in a hidden local. A break or
 * continue in the condition is raised, as it was from the block. */

static void bc_emit_jump_back(BcCompiler *bc, int target) {
    emit_op(bc, CLAYVM_OP_JUMP);
    int offset = target - ((int)bc->len + 2);
    emit_u16(bc, (uint16_t)(int16_t)offset);
}

/* break / continue of the inlined loop being compiled */
static void bc_emit_loop_jump(BcCompiler *bc, BcLoop *loop, int is_break) {
    for (int i = 0; i < loop->depth; i++) emit_op(bc, CLAYVM_OP_POP);
    if (loop->nil_res >= 0) {
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
        emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)loop->nil_res);
        emit_op(bc, CLAYVM_OP_POP);
    }
    if (!is_break && loop->fwd_cont) {
        if (loop->ccount >= loop->ccapacity) {
            loop->ccapacity = loop->ccapacity ? loop->ccapacity * 2 : 4;
            loop->conts = (int *)realloc(loop->conts, (size_t)loop->ccapacity * sizeof(int));
        }
        loop->conts[loop->ccount++] = emit_jump(bc, CLAYVM_OP_JUMP);
        return;
    }
    if (!is_break) {
        bc_emit_jump_back(bc, loop->top);
        return;
    }
    if (loop->count >= loop->capacity) {
        loop->capacity = loop->capacity ? loop->capacity * 2 : 4;
        loop->breaks = (int *)realloc(loop->breaks, (size_t)loop->capacity * sizeof(int));
    }
    loop->breaks[loop->count++] = emit_jump(bc, CLAYVM_OP_JUMP);
}

static void bc_emit_inline_while(BcCompiler *bc, AstNode *node, int want_value) {
    NodeList *cond = &node->as.send.receiver->as.block.body;
    NodeList *body = &node->as.send.args.items[0]->as.block.body;
    int is_true = strcmp(node->as.send.message, "whileTrue") == 0;
    int res = -1;
    if (want_value) {
        bc_begin_scope(bc);
        res = bc_add_local(bc, "?:loop");
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
        emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)res);
        emit_op(bc, CLAYVM_OP_POP);
    }
    BcLoop loop = { (int)bc->len, 0, NULL, 0, 0, 0, NULL, 0, 0, -1 };
    void *outer = bc->inline_loop;
    bc->inline_loop = NULL;
    bc_emit_inline_body(bc, cond, 1);
    bc_emit_native(bc, bc->native_is_truthy, 1);
    if (!is_true) emit_op(bc, CLAYVM_OP_NOT);
    int to_end = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
    bc->inline_loop = &loop;
    bc_emit_inline_body(bc, body, want_value);
    if (want_value) {
        emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)res);
        emit_op(bc, CLAYVM_OP_POP);
    }
    bc_emit_jump_back(bc, loop.top);
    patch_jump(bc, to_end);
    for (int i = 0; i < loop.count; i++) patch_jump(bc, loop.breaks[i]);
    free(loop.breaks);
    bc->inline_loop = outer;
    if (want_value) {
        emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)res);
        bc_end_scope(bc);
    }
}

/* --- Inlined counting loops ---
 * `n times { |i| ... }` / `(a..b) each { |i| ... }` (ast_inline_count_kind):
 * the counter runs from first to last in hidden locals (times_count /
 * range_bound check the receiver as the methods do), tested before the
 * first run and after each, so it never steps past last. The parameter is a
 * local of the body, set from the counter each run. A continue jumps to the
 * step. The value: times', the body's last (nil after a break or continue);
 * each's, the range (nil after a break). */
static void bc_emit_push_int(BcCompiler *bc, long long v) {
    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)add_constant_int(bc, v));
}

static void bc_emit_set_pop(BcCompiler *bc, int slot) {
    emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)slot);
    emit_op(bc, CLAYVM_OP_POP);
}

static void bc_emit_inline_count(BcCompiler *bc, AstNode *node, int want_value) {
    int kind = ast_inline_count_kind(node);
    AstNode *recv = node->as.send.receiver;
    NodeList *body = &node->as.send.args.items[0]->as.block.body;
    int times_value = want_value && kind == AST_COUNT_TIMES;
    bc_begin_scope(bc);
    int res = -1, first = -1;
    if (want_value) {
        res = bc_add_local(bc, "?:loop");
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
        bc_emit_set_pop(bc, res);
    }
    int k = bc_add_local(bc, "?:k");
    int last = bc_add_local(bc, "?:last");
    if (kind == AST_COUNT_TIMES) {
        bc_emit_expr(bc, recv);
        bc_set_loc(bc, node);
        bc_emit_native(bc, bc->native_times_count, 1);
        bc_emit_push_int(bc, 1);
        emit_op(bc, CLAYVM_OP_SUB);
        bc_emit_set_pop(bc, last);
        bc_emit_push_int(bc, 0);
        bc_emit_set_pop(bc, k);
    } else {
        bc_emit_expr(bc, recv->as.binary.left);
        bc_set_loc(bc, node);
        bc_emit_native(bc, bc->native_range_bound, 1);
        bc_emit_set_pop(bc, k);
        bc_emit_expr(bc, recv->as.binary.right);
        bc_set_loc(bc, node);
        bc_emit_native(bc, bc->native_range_bound, 1);
        bc_emit_set_pop(bc, last);
        if (want_value) {
            first = bc_add_local(bc, "?:first");
            emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)k);
            bc_emit_set_pop(bc, first);
        }
    }
    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)k);
    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)last);
    emit_op(bc, CLAYVM_OP_LTE);
    int to_skip = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
    BcLoop loop = { (int)bc->len, 0, NULL, 0, 0, 1, NULL, 0, 0, times_value ? res : -1 };
    void *outer = bc->inline_loop;
    bc->inline_loop = &loop;
    bc_emit_inline_body_p(bc, body, times_value, ast_count_param(node), k);
    bc->inline_loop = outer;
    if (times_value) bc_emit_set_pop(bc, res);
    for (int i = 0; i < loop.ccount; i++) patch_jump(bc, loop.conts[i]);
    free(loop.conts);
    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)k);
    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)last);
    emit_op(bc, CLAYVM_OP_LT);
    int to_done = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)k);
    bc_emit_push_int(bc, 1);
    emit_op(bc, CLAYVM_OP_ADD);
    bc_emit_set_pop(bc, k);
    bc_emit_jump_back(bc, loop.top);
    patch_jump(bc, to_skip);
    patch_jump(bc, to_done);
    if (want_value && kind == AST_COUNT_RANGE) {
        emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)first);
        emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)last);
        bc_emit_native(bc, bc->native_make_range, 2);
        bc_emit_set_pop(bc, res);
    }
    for (int i = 0; i < loop.count; i++) patch_jump(bc, loop.breaks[i]);
    free(loop.breaks);
    if (want_value) emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)res);
    bc_end_scope(bc);
}

static void bc_emit_expr(BcCompiler *bc, AstNode *node) {
    if (!node || bc->had_error) return;

    /* foo(args): a block in a variable runs (`foo value(args)`, the node as
     * parsed); a name that resolves to self sends `self foo(args)` */
    if (node->type == NODE_SEND && node->as.send.is_call) {
        const char *name = node->as.send.receiver->as.ident.name;
        if (bc_resolve_local(bc, name) < 0 && bc_name_is_self_slot(bc, name)) {
            AstNode self_node = *node->as.send.receiver;
            self_node.type = NODE_SELF;
            self_node.as.int_lit.value = 0;
            AstNode send = *node;
            send.as.send.receiver = &self_node;
            send.as.send.message = (char *)name;
            send.as.send.is_call = 0;
            bc_emit_expr(bc, &send);
            return;
        }
    }
    bc_set_loc(bc, node);

    switch (node->type) {
        case NODE_INT_LIT: {
            int idx = add_constant_int(bc, node->as.int_lit.value);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_FLOAT_LIT: {
            int idx = bc_add_const(bc, CLAYVM_DOUBLE((double)node->as.float_lit.value));
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
            int flag = slot >= 0 ? bc_slot_flag_local(bc, ident_name) : -1;
            if (flag >= 0) {
                /* Method local that may be an existing slot of self */
                bc_emit_flagged_read(bc, ident_name, slot, flag);
            } else if (slot >= 0) {
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)slot);
                /* If this local is boxed in a cell, unwrap it */
                if (bc_local_is_captured(bc, slot)) {
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_cell_get);
                    emit_byte(bc, 1);
                }
            } else if (bc_name_is_self_slot(bc, ident_name)) {
                /* Inside a method: bare identifier → implicit self slot lookup.
                 * A slot declared in the clone body wins over a global of the
                 * same name (codegen.c and clay-bytecode do the same).
                 * Emit: load_self, push slot name, send(name, 0 args) */
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_self);
                emit_byte(bc, 0);
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
            /* Load self via fast cached pointer */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_load_self);
            emit_byte(bc, 0);
            break;
        }

        case NODE_ASSIGN: {
            /* Check if assigning to an existing local */
            int slot = bc_resolve_local(bc, node->as.assign.name);
            int flag = slot >= 0 ? bc_slot_flag_local(bc, node->as.assign.name) : -1;
            if (flag >= 0) {
                /* Method local that may be an existing slot of self */
                bc_emit_expr(bc, node->as.assign.value);
                bc_emit_flagged_write(bc, node->as.assign.name, slot, flag);
            } else if (slot >= 0) {
                if (bc_local_is_captured(bc, slot)) {
                    /* Cell-boxed local: GET_LOCAL(cell), value, cell_set */
                    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)slot);
                    bc_emit_expr(bc, node->as.assign.value);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_cell_set);
                    emit_byte(bc, 2);
                } else {
                    bc_emit_expr(bc, node->as.assign.value);
                    emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)slot);
                }
                /* Value stays on stack (expression context) */
            } else if (bc->in_method && !is_builtin_global(node->as.assign.name)) {
                /* Inside a method scope: write to self slot (never a global).
                 * Method-level locals are pre-registered; this path handles
                 * bare assigns inside blocks within methods. */
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_self);
                emit_byte(bc, 0);
                int slot_name = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)slot_name);
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

        /* NODE_DESTRUCTURE is desugared by the parser into NODE_ASSIGN + NODE_SEND
         * chains, so it should never reach this point. */
        case NODE_DESTRUCTURE:
            break;

        case NODE_SEND: {
            if (bc->chunk_depth > 0 && ast_is_inline_if(node)) {
                bc_emit_inline_if(bc, node, 1);
                break;
            }
            if (bc->chunk_depth > 0 && ast_is_inline_while(node)) {
                bc_emit_inline_while(bc, node, 1);
                break;
            }
            if (bc->chunk_depth > 0 && ast_inline_count_kind(node)) {
                bc_emit_inline_count(bc, node, 1);
                break;
            }
            /* -x: NEGATE does numbers and sends negated to anything else */
            if (!node->as.send.is_super && node->as.send.args.count == 0 &&
                strcmp(node->as.send.message, "negated") == 0) {
                bc_emit_expr(bc, node->as.send.receiver);
                emit_op(bc, CLAYVM_OP_NEGATE);
                break;
            }
            /* Push receiver */
            bc_emit_expr(bc, node->as.send.receiver);
            /* Push selector as string constant */
            int sel_idx = add_constant_string(bc, node->as.send.message);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
            /* Push args (a block literal passed here owns its break/continue) */
            for (int i = 0; i < node->as.send.args.count; i++) {
                AstNode *arg = node->as.send.args.items[i];
                bc->next_loop_owner = ast_block_is_loop_arg(node, arg);
                bc_emit_expr(bc, arg);
                bc->next_loop_owner = 0;
            }
            /* Call native send or super_send; a break in one of its blocks
             * ends the call (send_break) */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            int catches = ast_send_catches_break(node);
            int send_fn = node->as.send.is_super
                ? (catches ? bc->native_super_send_break : bc->native_super_send)
                : (catches ? bc->native_send_break : bc->native_send);
            emit_u16(bc, (uint16_t)send_fn);
            emit_byte(bc, (uint8_t)(2 + node->as.send.args.count));
            break;
        }

        case NODE_BINARY: {
            const char *op = node->as.binary.op;

            if (strcmp(op, "&&") == 0) {
                /* Short-circuit AND: if left is falsy, skip right */
                bc_emit_expr(bc, node->as.binary.left);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_is_truthy);
                emit_byte(bc, 1);
                int false_jump = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
                bc_emit_expr(bc, node->as.binary.right);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_to_bool);
                emit_byte(bc, 1);
                int end_jump = emit_jump(bc, CLAYVM_OP_JUMP);
                patch_jump(bc, false_jump);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)bc_add_const(bc,
                        clay_to_vm(CLAY_FALSE)));
                patch_jump(bc, end_jump);
            } else if (strcmp(op, "||") == 0) {
                /* Short-circuit OR: if left is truthy, skip right */
                bc_emit_expr(bc, node->as.binary.left);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_is_truthy);
                emit_byte(bc, 1);
                int true_jump = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)bc_add_const(bc,
                        clay_to_vm(CLAY_TRUE)));
                int end_jump = emit_jump(bc, CLAYVM_OP_JUMP);
                patch_jump(bc, true_jump);
                bc_emit_expr(bc, node->as.binary.right);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_to_bool);
                emit_byte(bc, 1);
                patch_jump(bc, end_jump);
            } else if (strcmp(op, "??") == 0) {
                /* Null-coalescing: if left is not nil, use it; else use right.
                 * Stack protocol: emit left, DUP, is_nil, branch. */
                bc_emit_expr(bc, node->as.binary.left);
                emit_op(bc, CLAYVM_OP_DUP);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_is_nil);
                emit_byte(bc, 1);
                /* is_nil returns 1 if nil, 0 if not nil.
                 * JUMP_IF_FALSE jumps when value is 0 (not nil). */
                int not_nil_jump = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
                /* Fall through: IS nil → discard nil, emit right */
                emit_op(bc, CLAYVM_OP_POP);
                bc_emit_expr(bc, node->as.binary.right);
                int end_jump = emit_jump(bc, CLAYVM_OP_JUMP);
                /* NOT nil → left value is already on stack */
                patch_jump(bc, not_nil_jump);
                patch_jump(bc, end_jump);
            } else if (strcmp(op, "..") == 0) {
                bc_emit_expr(bc, node->as.binary.left);
                bc_emit_expr(bc, node->as.binary.right);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_make_range);
                emit_byte(bc, 2);
            } else {
                /* Map arithmetic operators to VM opcodes for fast path */
                int vm_op = -1;
                int is_cmp = 0;
                if      (strcmp(op, "+")  == 0) vm_op = CLAYVM_OP_ADD;
                else if (strcmp(op, "-")  == 0) vm_op = CLAYVM_OP_SUB;
                else if (strcmp(op, "*")  == 0) vm_op = CLAYVM_OP_MUL;
                else if (strcmp(op, "/")  == 0) vm_op = CLAYVM_OP_DIV;
                else if (strcmp(op, "%")  == 0) vm_op = CLAYVM_OP_MOD;
                else if (strcmp(op, "==") == 0) { vm_op = CLAYVM_OP_EQ;  is_cmp = 1; }
                else if (strcmp(op, "!=") == 0) { vm_op = CLAYVM_OP_NEQ; is_cmp = 1; }
                else if (strcmp(op, "<")  == 0) { vm_op = CLAYVM_OP_LT;  is_cmp = 1; }
                else if (strcmp(op, ">")  == 0) { vm_op = CLAYVM_OP_GT;  is_cmp = 1; }
                else if (strcmp(op, "<=") == 0) { vm_op = CLAYVM_OP_LTE; is_cmp = 1; }
                else if (strcmp(op, ">=") == 0) { vm_op = CLAYVM_OP_GTE; is_cmp = 1; }

                if (vm_op >= 0) {
                    /* Emit operands then VM opcode */
                    bc_emit_expr(bc, node->as.binary.left);
                    bc_emit_expr(bc, node->as.binary.right);
                    emit_op(bc, (uint8_t)vm_op);
                    if (is_cmp) {
                        /* Normalize int64 0/1 to Clay boolean */
                        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                        emit_u16(bc, (uint16_t)bc->native_to_bool);
                        emit_byte(bc, 1);
                    }
                } else {
                    /* Comparisons and other ops: use native_send
                     * (comparisons must return Clay booleans, not VM ints) */
                    bc_emit_expr(bc, node->as.binary.left);
                    int sel_idx = add_constant_string(bc, op);
                    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
                    bc_emit_expr(bc, node->as.binary.right);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_send);
                    emit_byte(bc, 3); /* receiver + selector + 1 arg */
                }
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
            if (caps.count > BC_MAX_CAPTURES) {
                fprintf(stderr, "%s:%d: error: block captures too many variables "
                        "(%d, max %d)\n", clay_source_file ? clay_source_file : "<input>",
                        node->line, caps.count, BC_MAX_CAPTURES);
                bc->had_error = 1;
            }

            /* Compile block body to a new chunk (with capture locals) */
            int chunk_id = bc_compile_chunk(bc, &node->as.block.body,
                                            &node->as.block.params, &caps, 0);
            if (chunk_id < 0) {
                bc_capture_free(&caps);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
                break;
            }

            /* Store capture info in the chunk */
            BcChunk *chunk = bc->chunks[chunk_id];
            chunk->capture_count = caps.count < BC_MAX_CAPTURES ? caps.count : BC_MAX_CAPTURES;
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

        case NODE_DICT_LIT: {
            /* Create empty Dict via native */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_make_dict);
            emit_byte(bc, 0);
            /* For each key-value pair: DUP dict, send set(key, val) */
            int set_idx = add_constant_string(bc, "set");
            for (int i = 0; i < node->as.dict_lit.keys.count; i++) {
                emit_op(bc, CLAYVM_OP_DUP); /* keep dict on stack */
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)set_idx);
                /* Push key as string constant */
                AstNode *key_node = node->as.dict_lit.keys.items[i];
                const char *key_str = key_node->as.string_lit.value;
                int key_idx = add_constant_string(bc, key_str);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)key_idx);
                /* Push value expression */
                bc_emit_expr(bc, node->as.dict_lit.values.items[i]);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_send);
                emit_byte(bc, 4); /* dict, "set", key, val */
                emit_op(bc, CLAYVM_OP_POP); /* discard send result */
            }
            /* Dict is left on stack */
            break;
        }

        case NODE_CLONE: {
            /* Push proto, call clone, then set slots/methods */
            bc_emit_expr(bc, node->as.clone.proto);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_clone);
            emit_byte(bc, 1);

            /* Collect slot names for Io-style assignment in methods */
            const char **saved_slots = bc->known_slots;
            int saved_slot_count = bc->known_slot_count;
            int saved_member_count = bc->known_member_count;
            int slot_name_count = 0, member_count = 0;
            for (int i = 0; i < node->as.clone.members.count; i++) {
                NodeType t = node->as.clone.members.items[i]->type;
                if (t == NODE_SLOT_DEF) slot_name_count++;
                if (t == NODE_SLOT_DEF || t == NODE_METHOD_DEF) member_count++;
            }
            /* slot names first, then method names */
            const char **slot_names = NULL;
            if (member_count > 0) {
                slot_names = (const char **)malloc((size_t)member_count * sizeof(const char *));
                int si = 0;
                for (int i = 0; i < node->as.clone.members.count; i++) {
                    if (node->as.clone.members.items[i]->type == NODE_SLOT_DEF)
                        slot_names[si++] = node->as.clone.members.items[i]->as.slot_def.name;
                }
                for (int i = 0; i < node->as.clone.members.count; i++) {
                    if (node->as.clone.members.items[i]->type == NODE_METHOD_DEF)
                        slot_names[si++] = node->as.clone.members.items[i]->as.method_def.name;
                }
            }
            bc->known_slots = slot_names;
            bc->known_slot_count = slot_name_count;
            bc->known_member_count = member_count;

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
                    if (chunk_id < 0) {
                        continue;
                    }

                    /* Register in method slot table (dynamic, no limit) */
                    int slot_idx = bc_method_slot_new(chunk_id,
                        member->as.method_def.params.count, member->as.method_def.name);

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

            /* Restore slot context */
            free(slot_names);
            bc->known_slots = saved_slots;
            bc->known_slot_count = saved_slot_count;
            bc->known_member_count = saved_member_count;
            break;
        }

        case NODE_TRY_CATCH: {
            /* Compile try block (produces a block object on stack) */
            bc_emit_expr(bc, node->as.try_catch.try_block);
            /* Compile catch block (or push nil if absent) */
            if (node->as.try_catch.catch_block) {
                bc_emit_expr(bc, node->as.try_catch.catch_block);
            } else {
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
            }
            /* Compile finally block (or push nil if absent) */
            if (node->as.try_catch.finally_block) {
                bc_emit_expr(bc, node->as.try_catch.finally_block);
            } else {
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
            }
            /* the call takes the try's line (not the catch/finally block's
             * just compiled), for the try block's stack-trace frame */
            bc_set_loc(bc, node);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_try_catch);
            emit_byte(bc, 3);
            break;
        }

        case NODE_RAISE: {
            bc_emit_expr(bc, node->as.raise.expr);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_raise);
            emit_byte(bc, 1);
            break;
        }

        case NODE_SLOT_ASSIGN: {
            /* receiver slot = value → set_slot, return assigned value */
            bc_emit_expr(bc, node->as.slot_assign.receiver);
            int sa_name_idx = add_constant_string(bc, node->as.slot_assign.slot_name);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sa_name_idx);
            bc_emit_expr(bc, node->as.slot_assign.value);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_set_slot);
            emit_byte(bc, 3);
            /* native_set_slot now returns the assigned value */
            break;
        }

        default:
            /* Unsupported node — push nil */
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
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
    bc_set_loc(bc, node);

    if (node->type == NODE_ASSIGN) {
        /* Check if assigning to an existing local */
        int slot = bc_resolve_local(bc, node->as.assign.name);
        if (slot >= 0 && bc_slot_flag_local(bc, node->as.assign.name) >= 0) {
            /* Possible implicit slot: the expression form handles the branch */
            bc_emit_expr(bc, node);
            emit_op(bc, CLAYVM_OP_POP);
        } else if (slot >= 0) {
            if (bc_local_is_captured(bc, slot)) {
                /* Cell-boxed local: GET_LOCAL(cell), value, cell_set, POP */
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)slot);
                bc_emit_expr(bc, node->as.assign.value);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_cell_set);
                emit_byte(bc, 2);
            } else {
                bc_emit_expr(bc, node->as.assign.value);
                emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)slot);
            }
            emit_op(bc, CLAYVM_OP_POP); /* discard (stmt context) */
        } else if (bc->in_method && !is_builtin_global(node->as.assign.name)) {
            bc_emit_expr(bc, node);  /* delegates to bc_emit_expr NODE_ASSIGN */
            emit_op(bc, CLAYVM_OP_POP);
        } else {
            /* Global: register name, push name, then compile value */
            bc_register_global(bc, node->as.assign.name, node->file);
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

    /* Return statement: emit expression (or nil), then stop VM to exit chunk */
    if (node->type == NODE_RETURN) {
        if (node->as.return_stmt.expr) {
            bc_emit_expr(bc, node->as.return_stmt.expr);
        } else {
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
        }
        int ret_id = bc->chunk_is_method ? -1 : bc_resolve_local(bc, "?:ret");
        if (ret_id >= 0) {
            /* `return` in a block: return from the enclosing method */
            bc_emit_get_local_value(bc, ret_id);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_raise_return);
            emit_byte(bc, 2);
            return;
        }
        BcInlineExit *exit = (BcInlineExit *)bc->inline_exit;
        if (exit && !bc->chunk_is_method) {
            /* outside methods: end the inlined block, with this value */
            if (!exit->want_value) emit_op(bc, CLAYVM_OP_POP);
            if (exit->count >= exit->capacity) {
                exit->capacity = exit->capacity ? exit->capacity * 2 : 4;
                exit->jumps = (int *)realloc(exit->jumps, (size_t)exit->capacity * sizeof(int));
            }
            exit->jumps[exit->count++] = emit_jump(bc, CLAYVM_OP_JUMP);
            return;
        }
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_stop_vm);
        emit_byte(bc, 0);
        return;
    }

    /* In an inlined loop's body, break and continue jump */
    if ((node->type == NODE_BREAK || node->type == NODE_CONTINUE) && bc->inline_loop) {
        bc_emit_loop_jump(bc, (BcLoop *)bc->inline_loop, node->type == NODE_BREAK);
        return;
    }

    /* In a block passed to a message, continue ends this run: nil */
    if (node->type == NODE_CONTINUE && bc->chunk_loop_owner) {
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)bc_add_const(bc, CLAYVM_NIL_VAL));
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_stop_vm);
        emit_byte(bc, 0);
        return;
    }

    /* Break: raise a BreakSignal */
    if (node->type == NODE_BREAK) {
        int name_idx = add_constant_string(bc, "BreakSignal");
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_load_global);
        emit_byte(bc, 1);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_raise);
        emit_byte(bc, 1);
        return;
    }

    /* Continue: raise a ContinueSignal */
    if (node->type == NODE_CONTINUE) {
        int name_idx = add_constant_string(bc, "ContinueSignal");
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_load_global);
        emit_byte(bc, 1);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_raise);
        emit_byte(bc, 1);
        return;
    }

    if (bc->chunk_depth > 0 && ast_is_inline_if(node)) {
        bc_emit_inline_if(bc, node, 0);
        return;
    }
    if (bc->chunk_depth > 0 && ast_is_inline_while(node)) {
        bc_emit_inline_while(bc, node, 0);
        return;
    }
    if (bc->chunk_depth > 0 && ast_inline_count_kind(node)) {
        bc_emit_inline_count(bc, node, 0);
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
    bc_reset_ops(bc);

    /* Every top-level variable is a global from the start, so methods see
     * the ones assigned later in the file too (order doesn't matter) */
    for (int i = 0; i < program->as.program.stmts.count; i++) {
        AstNode *stmt = program->as.program.stmts.items[i];
        if (stmt->type == NODE_ASSIGN && !is_builtin_global(stmt->as.assign.name))
            bc_register_global(bc, stmt->as.assign.name, stmt->file);
    }

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
