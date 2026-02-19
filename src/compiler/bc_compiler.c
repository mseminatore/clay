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

/* --- Bytecode emission helpers --- */

static void emit_byte(BcCompiler *bc, uint8_t b) {
    if (bc->len >= BC_MAX_CODE) { bc->had_error = 1; return; }
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

    /* Allocate storage for the value */
    clayvm_value_t *stored = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *stored = val;

    /* Remove old if exists */
    ht_value_t old = ht_find(globals, (ht_key_t)name);
    if (old) {
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
    bc->native_println = clayvm_native_register(&vm->natives, "println", native_println_fn, 1);
    bc->native_load_global = clayvm_native_register(&vm->natives, "load_global", native_load_global_fn, 1);
    bc->native_store_global = clayvm_native_register(&vm->natives, "store_global", native_store_global_fn, 2);

    /* Pre-register Clay builtin prototypes as globals */
    struct { const char *name; ClayObject *obj; } builtins[] = {
        {"Object", clay_Object}, {"Number", clay_Number}, {"String", clay_String},
        {"Array", clay_Array}, {"Block", clay_Block}, {"Nil", clay_Nil},
        {"Boolean", clay_Boolean}, {"Error", clay_Error}, {"Dict", clay_Dict},
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

static void bc_emit_expr(BcCompiler *bc, AstNode *node) {
    if (!node || bc->had_error) return;

    switch (node->type) {
        case NODE_INT_LIT: {
            int idx = add_constant_int(bc, node->as.int_lit.value);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_STRING_LIT: {
            int idx = add_constant_string(bc, node->as.string_lit.value);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_IDENT: {
            /* Load global variable via native call */
            int name_idx = add_constant_string(bc, node->as.ident.name);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_load_global);
            emit_byte(bc, 1);
            break;
        }

        case NODE_SELF: {
            /* In REPL context, self is rarely used; emit nil */
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
            break;
        }

        case NODE_ASSIGN: {
            /* Push name first, compile value, call store_global */
            int name_idx = add_constant_string(bc, node->as.assign.name);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
            bc_emit_expr(bc, node->as.assign.value);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_store_global);
            emit_byte(bc, 2);
            /* Result left on stack for expression use */
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
            /* Blocks in the REPL are complex — for now, fall through to send */
            /* TODO: proper block compilation */
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
            break;
        }

        case NODE_ARRAY_LIT: {
            /* TODO: array literal compilation */
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
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
                }
                /* METHOD_DEF skipped for now — needs block compilation */
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

static void bc_emit_stmt(BcCompiler *bc, AstNode *node) {
    if (!node || bc->had_error) return;

    if (node->type == NODE_ASSIGN) {
        /* Push name first, then compile value */
        int name_idx = add_constant_string(bc, node->as.assign.name);
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
        bc_emit_expr(bc, node->as.assign.value);
        /* Stack: name, value. store_global pops val then name. Correct! */
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_store_global);
        emit_byte(bc, 2);
        emit_op(bc, CLAYVM_OP_POP); /* discard result */
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

        if (is_last && stmt->type != NODE_ASSIGN) {
            /* Last expression: leave on stack for REPL auto-print */
            bc_emit_expr(bc, stmt);
        } else {
            bc_emit_stmt(bc, stmt);
        }
    }

    emit_op(bc, CLAYVM_OP_HALT);
    return bc->had_error ? -1 : 0;
}
