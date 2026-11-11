#ifndef CLAY_CODEGEN_H
#define CLAY_CODEGEN_H

#include "ast.h"
#include <stdio.h>

typedef struct {
    FILE *out;
    const char *source_file;
    int indent;
    int temp_counter;
    int method_counter;
    int in_method;       /* nonzero when emitting method/block body */
    char **local_names;  /* parameter/local names in current method scope */
    int local_count;
    int local_capacity;
    int method_local_base; /* index where method-specific locals start */
    int toplevel_local_count; /* locals [0, n) are the program's top-level variables */
    AstNode *program;         /* the program being emitted (who assigns which top-level variable) */
    char **boxed_names;  /* variables boxed into cells (by-reference capture) */
    int boxed_count;
    int boxed_capacity;
    const char *super_proto; /* variable name of defining prototype (for super) */
    const char **known_slots; /* slot names from enclosing clone (Io-style assignment) */
    int known_slot_count;
    char **owned_temps;  /* temps holding owned references (need release) */
    int owned_temp_count;
    int owned_temp_capacity;
    /* Ownership in the method/block function being emitted (not main):
     * C variables that hold a reference, and cells created here — all released
     * when the function returns. */
    const char *plugin_exe_path; /* clay binary whose plugins/ native programs load */
    const char **native_plugins; /* native bricks linked in: <name>_plugin_init */
    int native_plugin_count;
    int in_function;
    int fn_has_ret_frame; /* method catching non-local returns (see emit_ret_frame) */
    int fn_is_block;     /* emitting a block function (always returns an owned value) */
    int use_slot_recv;   /* top-level block: self is the receiver it was invoked
                          * as a method on (clay_slot_method_self), held in __recv */
    char **fn_vars;
    int fn_var_count;
    int fn_var_capacity;
    char **fn_cells;
    int fn_cell_count;
    int fn_cell_capacity;
} CodeGen;

void codegen_init(CodeGen *cg, FILE *out, const char *source_file);
void codegen_emit(CodeGen *cg, AstNode *program);

#endif /* CLAY_CODEGEN_H */
