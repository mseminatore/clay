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
} CodeGen;

void codegen_init(CodeGen *cg, FILE *out, const char *source_file);
void codegen_emit(CodeGen *cg, AstNode *program);

#endif /* CLAY_CODEGEN_H */
