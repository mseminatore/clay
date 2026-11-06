/*
 * bc_compiler.h - Bytecode compiler for the Clay VM
 */
#ifndef CLAY_BC_COMPILER_H
#define CLAY_BC_COMPILER_H

#include "ast.h"
#include "vm_bridge.h"

int  bc_compile(BcCompiler *bc, AstNode *program);

#endif /* CLAY_BC_COMPILER_H */
