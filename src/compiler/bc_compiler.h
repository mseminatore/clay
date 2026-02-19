/*
 * bc_compiler.h - Bytecode compiler for Clay VM REPL
 */
#ifndef CLAY_BC_COMPILER_H
#define CLAY_BC_COMPILER_H

#include "ast.h"
#include "clay.h"
#include "vm.h"
#include "opcodes.h"
#include "native.h"

#define BC_MAX_CODE   65536
#define BC_MAX_GLOBALS 256

/* Bytecode compilation context */
typedef struct {
    uint8_t code[BC_MAX_CODE];
    size_t  len;

    clayvm_vm_t *vm;  /* VM to add constants to */

    /* Global variable name tracking */
    char *global_names[BC_MAX_GLOBALS];
    int   global_count;

    /* Native function indices (pre-registered) */
    int native_send;      /* clay_send bridge */
    int native_clone;     /* clay_clone bridge */
    int native_set_slot;  /* clay_set_slot bridge */
    int native_set_method;
    int native_println;
    int native_print;
    int native_make_string;
    int native_make_array;
    int native_make_block;
    int native_load_global;
    int native_store_global;
    int native_make_dict;

    int had_error;
} BcCompiler;

void bc_compiler_init(BcCompiler *bc, clayvm_vm_t *vm);
int  bc_compile(BcCompiler *bc, AstNode *program);

/* Conversion between VM and Clay value systems */
ClayValue vm_to_clay(clayvm_value_t v);
clayvm_value_t clay_to_vm(ClayValue cv);

#endif /* CLAY_BC_COMPILER_H */
