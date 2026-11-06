/*
 * vm_bridge.h - Runtime side of the Clay bytecode VM: compiled chunks, the
 * compiler/VM context shared with the native bridge, value conversion
 */
#ifndef CLAY_VM_BRIDGE_H
#define CLAY_VM_BRIDGE_H

#include "clay.h"
#include "vm.h"
#include "opcodes.h"
#include "native.h"

#define BC_MAX_CODE   65536
#define BC_INITIAL_CHUNKS 64

/* A compiled bytecode chunk (block or method body) */
/* Most variables one block can capture from enclosing scopes (each name that
 * may be an implicit slot also captures its hidden "?name" flag) */
#define BC_MAX_CAPTURES 64

typedef struct {
    uint8_t *code;       /* dynamically allocated */
    size_t  len;
    int     param_count;
    char   *param_names[16]; /* up to 16 params */
    int     capture_count;
    char   *capture_names[BC_MAX_CAPTURES]; /* names of captured variables */
    int     capture_slots[BC_MAX_CAPTURES]; /* enclosing scope slot index for each capture */
    int     local_count;       /* total locals (params + temps) in this chunk */
    int     has_block_return;  /* method whose blocks `return` from it (non-local return) */
    int    *line_map;    /* source line map: line_map[offset] = source line */
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

    /* Bytecode chunks for blocks/methods (dynamically allocated) */
    BcChunk **chunks;
    int     chunk_count;
    int     chunk_capacity;

    /* Compile-time global name tracking (for resolving user globals in methods) */
    char **known_globals;
    int    known_global_count;
    int    known_global_capacity;

    /* Native function indices (pre-registered) */
    int native_send;      /* clay_send bridge */
    int native_super_send; /* clay_send_super bridge */
    int native_clone;     /* clay_clone bridge */
    int native_set_slot;  /* clay_set_slot bridge */
    int native_set_method;
    int native_println;
    int native_print;
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
    int native_is_nil;
    int native_stop_vm;
    int native_try_catch;
    int native_raise;
    int native_make_range;
    int native_load_self;
    int native_self_has_slot;   /* self_has_slot(name) → 1/0 (implicit-slot fallback) */
    int native_set_self_slot;   /* set_self_slot(value, name) → value */
    int native_cell_set_rev;    /* cell_set_rev(value, cell) → value */
    int native_method_return_id; /* method_return_id() → id of the method call being entered */
    int native_raise_return;    /* raise_return(value, id) — non-local return from a block */

    int had_error;

    /* Source line map: line_map[offset] = source line number */
    int line_map[BC_MAX_CODE];
    int current_line;
    const char *source_file; /* filename for error messages */

    /* Local variable tracking for current compilation scope */
    BcLocal locals[BC_MAX_LOCALS];
    int     local_count;
    int     scope_depth;

    /* Enclosing scope locals (for capture scanning) */
    BcLocal *enclosing_locals;  /* pointer to outer scope's locals */
    int      enclosing_local_count;

    /* Method context flag — 1 when compiling inside a method body */
    int      in_method;
    int      chunk_is_method;  /* compiling a method body (not a block) */

    /* Known slot names from the enclosing clone block (for Io-style assignment) */
    const char **known_slots;
    int          known_slot_count;
} BcCompiler;

void bc_compiler_init(BcCompiler *bc, clayvm_vm_t *vm);

/* Register a compiled method: returns its method-slot index (the constant
 * set_method takes) */
int bc_method_slot_new(int chunk_id, int param_count, const char *name);
int bc_method_slot_count(void);
const char *bc_method_slot_info(int idx, int *chunk_id, int *param_count);

/* Threads: give each worker its own VM sharing the main compiler's chunks */
void bc_worker_thread_init(void);
void bc_set_main_compiler(BcCompiler *bc);

/* Conversion between VM and Clay value systems */
ClayValue vm_to_clay(clayvm_value_t v);
clayvm_value_t clay_to_vm(ClayValue cv);

/* The VM prototype (bytecode from Clay programs) */
void clay_vm_init_methods(ClayObject *proto);

#endif /* CLAY_VM_BRIDGE_H */
