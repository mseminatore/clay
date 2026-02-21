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
#define BC_MAX_CHUNKS  256
#define BC_CHUNK_CODE  4096
#define BC_MAX_METHOD_SLOTS 128

/* A compiled bytecode chunk (block or method body) */
typedef struct {
    uint8_t *code;       /* dynamically allocated */
    size_t  len;
    int     param_count;
    char   *param_names[16]; /* up to 16 params */
    int     capture_count;
    char   *capture_names[16]; /* names of captured variables */
    int     capture_slots[16]; /* enclosing scope slot index for each capture */
    int     local_count;       /* total locals (params + temps) in this chunk */
} BcChunk;

/* Local variable tracking for compile-time scope resolution */
typedef struct {
    char *name;
    int   slot;    /* stack slot relative to frame base */
    int   depth;   /* scope depth (0 = chunk-level) */
    int   is_captured; /* set if an inner block captures this local */
} BcLocal;

#define BC_MAX_LOCALS  256
#define BC_MAX_SCOPE_DEPTH 16

/* Bytecode compilation context */
typedef struct {
    uint8_t code[BC_MAX_CODE];
    size_t  len;

    clayvm_vm_t *vm;  /* VM to add constants to */

    /* Global variable name tracking */
    char *global_names[BC_MAX_GLOBALS];
    int   global_count;

    /* Bytecode chunks for blocks/methods (heap-allocated) */
    BcChunk *chunks[BC_MAX_CHUNKS];
    int     chunk_count;

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
    int native_cell_new;
    int native_cell_get;
    int native_cell_set;
    int native_is_truthy;
    int native_to_bool;

    int had_error;

    /* Source line map: line_map[offset] = source line number */
    int line_map[BC_MAX_CODE];
    int current_line;

    /* Local variable tracking for current compilation scope */
    BcLocal locals[BC_MAX_LOCALS];
    int     local_count;
    int     scope_depth;

    /* Enclosing scope locals (for capture scanning) */
    BcLocal *enclosing_locals;  /* pointer to outer scope's locals */
    int      enclosing_local_count;

    /* Method context flag — 1 when compiling inside a method body */
    int      in_method;
} BcCompiler;

void bc_compiler_init(BcCompiler *bc, clayvm_vm_t *vm);
int  bc_compile(BcCompiler *bc, AstNode *program);

/* Conversion between VM and Clay value systems */
ClayValue vm_to_clay(clayvm_value_t v);
clayvm_value_t clay_to_vm(ClayValue cv);

#endif /* CLAY_BC_COMPILER_H */
