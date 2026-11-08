/*
 * check.c - checks on a parsed program before it is compiled
 *
 * check_undefined_variables: a misspelled name is otherwise nil in the VM
 * (found much later, somewhere else) and a C compiler error in native code.
 */
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int clay_plugin_is_global(const char *name);   /* runtime: loaded plugins */

/* Every prototype either compiler starts with (codegen is_builtin_global,
 * vm_bridge's builtins) */
static const char *k_builtins[] = {
    "Object", "Number", "String", "Array", "Block", "Nil", "Boolean", "Error",
    "Dict", "Math", "Console", "File", "System", "FFI", "Date", "Http",
    "Regex", "Crypto", "Process", "Range", "Fiber", "Db", "Graphics", "Vec3",
    "Thread", "VM", "BreakSignal", "ContinueSignal", "true", "false", "nil",
    NULL
};

typedef struct {
    const char **names;
    int count, cap;
} NameSet;

static void set_add(NameSet *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0) return;
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->names = (const char **)realloc(s->names, (size_t)s->cap * sizeof(char *));
    }
    s->names[s->count++] = name;
}

static int set_has(const NameSet *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0) return 1;
    return 0;
}

#define MAX_SCOPES 256

typedef struct {
    NameSet globals;
    NameSet scopes[MAX_SCOPES];   /* enclosing blocks, innermost last */
    int depth;
    const char *main_file;
    int errors;
} Checker;

static int is_defined(Checker *c, const char *name) {
    for (int i = 0; k_builtins[i]; i++)
        if (strcmp(k_builtins[i], name) == 0) return 1;
    for (int d = c->depth - 1; d >= 0; d--)
        if (set_has(&c->scopes[d], name)) return 1;
    if (set_has(&c->globals, name)) return 1;
    return clay_plugin_is_global(name);
}

/* Names a statement list assigns (a block's or the program's locals) */
static void collect_assigned(NodeList *stmts, NameSet *into) {
    for (int i = 0; i < stmts->count; i++) {
        AstNode *s = stmts->items[i];
        if (!s) continue;
        if (s->type == NODE_ASSIGN) {
            set_add(into, s->as.assign.name);
        } else if (s->type == NODE_DESTRUCTURE) {
            for (int j = 0; j < s->as.destructure.targets.count; j++)
                set_add(into, s->as.destructure.targets.items[j]->as.ident.name);
        }
    }
}

static void walk(Checker *c, AstNode *n);

static void walk_list(Checker *c, NodeList *l) {
    for (int i = 0; i < l->count; i++) walk(c, l->items[i]);
}

static void walk(Checker *c, AstNode *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_IDENT:
            if (!is_defined(c, n->as.ident.name)) {
                fprintf(stderr, "%s:%d: error: undefined variable '%s'\n",
                        n->file ? n->file : c->main_file, n->line, n->as.ident.name);
                c->errors++;
            }
            break;
        case NODE_ASSIGN:      walk(c, n->as.assign.value); break;
        case NODE_SEND:        walk(c, n->as.send.receiver); walk_list(c, &n->as.send.args); break;
        case NODE_BINARY:      walk(c, n->as.binary.left); walk(c, n->as.binary.right); break;
        case NODE_BLOCK: {
            if (c->depth >= MAX_SCOPES) break;
            NameSet *scope = &c->scopes[c->depth++];
            scope->count = 0;
            for (int i = 0; i < n->as.block.params.count; i++) {
                AstNode *p = n->as.block.params.items[i];
                if (p->type == NODE_IDENT) set_add(scope, p->as.ident.name);
                else if (p->type == NODE_ASSIGN) set_add(scope, p->as.assign.name);
            }
            collect_assigned(&n->as.block.body, scope);
            walk_list(c, &n->as.block.body);
            c->depth--;
            break;
        }
        case NODE_METHOD_DEF:  break;   /* bare names may be self's slots */
        case NODE_SLOT_DEF:    walk(c, n->as.slot_def.value); break;
        case NODE_CLONE:
            walk(c, n->as.clone.proto);
            walk_list(c, &n->as.clone.members);
            break;
        case NODE_ARRAY_LIT:   walk_list(c, &n->as.array_lit.elements); break;
        case NODE_DICT_LIT:    walk_list(c, &n->as.dict_lit.values); break;
        case NODE_TRY_CATCH:
            walk(c, n->as.try_catch.try_block);
            walk(c, n->as.try_catch.catch_block);
            walk(c, n->as.try_catch.finally_block);
            break;
        case NODE_RAISE:       walk(c, n->as.raise.expr); break;
        case NODE_SLOT_ASSIGN: walk(c, n->as.slot_assign.receiver); walk(c, n->as.slot_assign.value); break;
        case NODE_RETURN:      walk(c, n->as.return_stmt.expr); break;
        case NODE_DESTRUCTURE: walk(c, n->as.destructure.value); break;
        case NODE_PROGRAM:     walk_list(c, &n->as.program.stmts); break;
        default:               break;   /* literals, self, break, continue, import */
    }
}

int check_undefined_variables(AstNode *program, const char *main_file) {
    if (!program || program->type != NODE_PROGRAM) return 0;
    Checker *c = (Checker *)calloc(1, sizeof(Checker));
    c->main_file = main_file;
    collect_assigned(&program->as.program.stmts, &c->globals);
    walk(c, program);
    int errors = c->errors;
    free(c->globals.names);
    for (int i = 0; i < MAX_SCOPES; i++) free(c->scopes[i].names);
    free(c);
    return errors;
}
