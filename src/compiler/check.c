/*
 * check.c - checks on a parsed program before it is compiled
 *
 * check_undefined_variables: a misspelled name is otherwise nil in the VM
 * (found much later, somewhere else) and a C compiler error in native code.
 * check_loop_control: a break or continue with no loop to go to.
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
    "Thread", "Socket", "VM", "BreakSignal", "ContinueSignal", "true", "false", "nil",
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

/* --- break / continue --- */

/* A break or continue belongs to the loop it is written in, as in most
 * languages: the body of a whileTrue/whileFalse, or a block literal passed
 * to a message (each, times, a user iterator...). The blocks of ifTrue and
 * ifFalse, try/catch/finally and a block a message is sent to (a loop's
 * condition, `{ ... } value`) run where they are written, so they are part
 * of the code around them. Any other block (stored, returned, a slot's
 * value), a method body and top-level code have no loop. */

typedef struct {
    const char *main_file;
    int errors;
} LoopCheck;

static void loop_walk(LoopCheck *c, AstNode *n, int in_loop);

static void loop_walk_list(LoopCheck *c, NodeList *l, int in_loop) {
    for (int i = 0; i < l->count; i++) loop_walk(c, l->items[i], in_loop);
}

/* A block's statements; its parameters' default values run where it's called */
static void loop_walk_block(LoopCheck *c, AstNode *b, int in_loop) {
    for (int i = 0; i < b->as.block.params.count; i++) {
        AstNode *p = b->as.block.params.items[i];
        if (p && p->type == NODE_ASSIGN) loop_walk(c, p->as.assign.value, 0);
    }
    loop_walk_list(c, &b->as.block.body, in_loop);
}

static int is_cond_message(const char *m) {
    return m && (strcmp(m, "ifTrue") == 0 || strcmp(m, "ifFalse") == 0);
}

static void loop_walk(LoopCheck *c, AstNode *n, int in_loop) {
    if (!n) return;
    switch (n->type) {
        case NODE_BREAK:
        case NODE_CONTINUE:
            if (!in_loop) {
                fprintf(stderr, "%s:%d: error: '%s' must be inside a loop or a block "
                        "passed to a message (like each or times)\n",
                        n->file ? n->file : c->main_file, n->line,
                        n->type == NODE_BREAK ? "break" : "continue");
                c->errors++;
            }
            break;
        case NODE_BLOCK:       loop_walk_block(c, n, 0); break;
        case NODE_SEND: {
            AstNode *r = n->as.send.receiver;
            if (r && r->type == NODE_BLOCK) loop_walk_block(c, r, in_loop);
            else loop_walk(c, r, in_loop);
            int cond = is_cond_message(n->as.send.message);
            for (int i = 0; i < n->as.send.args.count; i++) {
                AstNode *a = n->as.send.args.items[i];
                if (a && a->type == NODE_BLOCK) loop_walk_block(c, a, cond ? in_loop : 1);
                else loop_walk(c, a, in_loop);
            }
            break;
        }
        case NODE_TRY_CATCH: {
            AstNode *parts[3] = { n->as.try_catch.try_block, n->as.try_catch.catch_block,
                                  n->as.try_catch.finally_block };
            for (int i = 0; i < 3; i++) {
                if (parts[i] && parts[i]->type == NODE_BLOCK) loop_walk_block(c, parts[i], in_loop);
                else loop_walk(c, parts[i], in_loop);
            }
            break;
        }
        case NODE_METHOD_DEF:
            for (int i = 0; i < n->as.method_def.params.count; i++) {
                AstNode *p = n->as.method_def.params.items[i];
                if (p && p->type == NODE_ASSIGN) loop_walk(c, p->as.assign.value, 0);
            }
            loop_walk_list(c, &n->as.method_def.body, 0);
            break;
        case NODE_ASSIGN:      loop_walk(c, n->as.assign.value, in_loop); break;
        case NODE_BINARY:
            loop_walk(c, n->as.binary.left, in_loop);
            loop_walk(c, n->as.binary.right, in_loop);
            break;
        case NODE_SLOT_DEF:    loop_walk(c, n->as.slot_def.value, in_loop); break;
        case NODE_CLONE:
            loop_walk(c, n->as.clone.proto, in_loop);
            loop_walk_list(c, &n->as.clone.members, in_loop);
            break;
        case NODE_ARRAY_LIT:   loop_walk_list(c, &n->as.array_lit.elements, in_loop); break;
        case NODE_DICT_LIT:    loop_walk_list(c, &n->as.dict_lit.values, in_loop); break;
        case NODE_RAISE:       loop_walk(c, n->as.raise.expr, in_loop); break;
        case NODE_SLOT_ASSIGN:
            loop_walk(c, n->as.slot_assign.receiver, in_loop);
            loop_walk(c, n->as.slot_assign.value, in_loop);
            break;
        case NODE_RETURN:      loop_walk(c, n->as.return_stmt.expr, in_loop); break;
        case NODE_DESTRUCTURE: loop_walk(c, n->as.destructure.value, in_loop); break;
        case NODE_PROGRAM:     loop_walk_list(c, &n->as.program.stmts, 0); break;
        default:               break;
    }
}

int check_loop_control(AstNode *program, const char *main_file) {
    if (!program) return 0;
    LoopCheck c = { main_file, 0 };
    loop_walk(&c, program, 0);
    return c.errors;
}

int check_program(AstNode *program, const char *main_file) {
    int errors = check_undefined_variables(program, main_file);
    return errors + check_loop_control(program, main_file);
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
