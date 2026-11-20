/*
 * check.h - checks on a parsed program before it is compiled
 */
#ifndef CLAY_CHECK_H
#define CLAY_CHECK_H

#include "ast.h"

/* Report every read of a variable that is never defined: not assigned at
 * the top level of the program (or its imports), not a block parameter or
 * a name assigned in an enclosing block, and not a builtin or plugin
 * prototype. Method bodies aren't checked: a bare name there may be one of
 * self's slots. Prints "file:line: error: undefined variable 'x'" for each
 * and returns how many there were. selfhost/clay-parser's GlobalCheck
 * mirrors this. */
int check_undefined_variables(AstNode *program, const char *main_file);

/* Report every break or continue that has no loop to go to: one outside
 * any loop body or block passed to a message (in a method body, a stored
 * block, top-level code). The blocks of ifTrue/ifFalse and try/catch are
 * part of the code around them. Returns how many there were; GlobalCheck
 * mirrors this too. */
int check_loop_control(AstNode *program, const char *main_file);

/* Both checks; the number of errors reported */
int check_program(AstNode *program, const char *main_file);

#endif /* CLAY_CHECK_H */
