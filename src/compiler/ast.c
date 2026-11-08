/*
 * ast.c - AST node creation, destruction, and debug printing
 */
#include "ast.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

const char *ast_current_file = NULL;

AstNode *ast_new(NodeType type, int line) {
    AstNode *node = (AstNode *)calloc(1, sizeof(AstNode));
    node->type = type;
    node->line = line;
    node->file = ast_current_file;
    return node;
}

static void nodelist_clone(NodeList *dst, const NodeList *src);

AstNode *ast_clone(AstNode *node) {
    if (!node) return NULL;
    AstNode *c = ast_new(node->type, node->line);
    c->file = node->file;
    switch (node->type) {
        case NODE_INT_LIT:
            c->as.int_lit.value = node->as.int_lit.value;
            break;
        case NODE_FLOAT_LIT:
            c->as.float_lit.value = node->as.float_lit.value;
            break;
        case NODE_STRING_LIT:
            c->as.string_lit.value = strdup(node->as.string_lit.value);
            break;
        case NODE_IDENT:
            c->as.ident.name = strdup(node->as.ident.name);
            break;
        case NODE_SELF:
            break;
        case NODE_ASSIGN:
            c->as.assign.name = strdup(node->as.assign.name);
            c->as.assign.value = ast_clone(node->as.assign.value);
            break;
        case NODE_SEND:
            c->as.send.receiver = ast_clone(node->as.send.receiver);
            c->as.send.message = strdup(node->as.send.message);
            c->as.send.is_super = node->as.send.is_super;
            nodelist_clone(&c->as.send.args, &node->as.send.args);
            break;
        case NODE_BINARY:
            c->as.binary.left = ast_clone(node->as.binary.left);
            c->as.binary.op = strdup(node->as.binary.op);
            c->as.binary.right = ast_clone(node->as.binary.right);
            break;
        case NODE_BLOCK:
            nodelist_clone(&c->as.block.params, &node->as.block.params);
            nodelist_clone(&c->as.block.body, &node->as.block.body);
            break;
        case NODE_SLOT_ASSIGN:
            c->as.slot_assign.receiver = ast_clone(node->as.slot_assign.receiver);
            c->as.slot_assign.slot_name = strdup(node->as.slot_assign.slot_name);
            c->as.slot_assign.value = ast_clone(node->as.slot_assign.value);
            break;
        case NODE_RETURN:
            c->as.return_stmt.expr = ast_clone(node->as.return_stmt.expr);
            break;
        case NODE_DICT_LIT:
            nodelist_clone(&c->as.dict_lit.keys, &node->as.dict_lit.keys);
            nodelist_clone(&c->as.dict_lit.values, &node->as.dict_lit.values);
            break;
        case NODE_DESTRUCTURE:
            nodelist_clone(&c->as.destructure.targets, &node->as.destructure.targets);
            c->as.destructure.value = ast_clone(node->as.destructure.value);
            break;
        default:
            /* Remaining node types not expected as compound assignment receivers */
            break;
    }
    return c;
}

static void nodelist_clone(NodeList *dst, const NodeList *src) {
    nodelist_init(dst);
    for (int i = 0; i < src->count; i++)
        nodelist_push(dst, ast_clone(src->items[i]));
}

void nodelist_init(NodeList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nodelist_push(NodeList *list, AstNode *node) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = (AstNode **)realloc(list->items, sizeof(AstNode *) * (size_t)list->capacity);
    }
    list->items[list->count++] = node;
}

void nodelist_free(NodeList *list) {
    for (int i = 0; i < list->count; i++) {
        ast_free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void ast_free(AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case NODE_STRING_LIT: free(node->as.string_lit.value); break;
        case NODE_IDENT:      free(node->as.ident.name); break;
        case NODE_ASSIGN:
            free(node->as.assign.name);
            ast_free(node->as.assign.value);
            break;
        case NODE_SEND:
            ast_free(node->as.send.receiver);
            free(node->as.send.message);
            nodelist_free(&node->as.send.args);
            break;
        case NODE_BINARY:
            ast_free(node->as.binary.left);
            free(node->as.binary.op);
            ast_free(node->as.binary.right);
            break;
        case NODE_BLOCK:
            nodelist_free(&node->as.block.params);
            nodelist_free(&node->as.block.body);
            break;
        case NODE_METHOD_DEF:
            free(node->as.method_def.name);
            nodelist_free(&node->as.method_def.params);
            nodelist_free(&node->as.method_def.body);
            break;
        case NODE_SLOT_DEF:
            free(node->as.slot_def.name);
            ast_free(node->as.slot_def.value);
            break;
        case NODE_CLONE:
            ast_free(node->as.clone.proto);
            nodelist_free(&node->as.clone.members);
            break;
        case NODE_ARRAY_LIT:
            nodelist_free(&node->as.array_lit.elements);
            break;
        case NODE_IMPORT:
            free(node->as.import.path);
            free(node->as.import.alias);
            break;
        case NODE_TRY_CATCH:
            ast_free(node->as.try_catch.try_block);
            if (node->as.try_catch.catch_block) ast_free(node->as.try_catch.catch_block);
            if (node->as.try_catch.finally_block) ast_free(node->as.try_catch.finally_block);
            break;
        case NODE_RAISE:
            ast_free(node->as.raise.expr);
            break;
        case NODE_SLOT_ASSIGN:
            ast_free(node->as.slot_assign.receiver);
            free(node->as.slot_assign.slot_name);
            ast_free(node->as.slot_assign.value);
            break;
        case NODE_RETURN:
            ast_free(node->as.return_stmt.expr);
            break;
        case NODE_DICT_LIT:
            nodelist_free(&node->as.dict_lit.keys);
            nodelist_free(&node->as.dict_lit.values);
            break;
        case NODE_DESTRUCTURE:
            nodelist_free(&node->as.destructure.targets);
            ast_free(node->as.destructure.value);
            break;
        case NODE_BREAK:
        case NODE_CONTINUE:
            break;
        case NODE_PROGRAM:
            nodelist_free(&node->as.program.stmts);
            break;
        default: break;
    }
    free(node);
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

/* String contents with \, ", newline, CR, tab and other control bytes escaped,
 * so each node stays on one line */
static void print_escaped(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\') fputs("\\\\", stdout);
        else if (c == '"') fputs("\\\"", stdout);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
        else putchar(c);
    }
}

void clay_format_float(float f, char *buf, size_t sz);   /* runtime: shortest round-trip */

static void print_node(AstNode *node, int indent, const char *label);

static void print_list(NodeList *list, int indent, const char *label) {
    for (int i = 0; i < list->count; i++) print_node(list->items[i], indent, label);
}

/* One node per line: "<indent>[label]Kind(details) @line", children indented.
 * Lossless enough to compare two parsers (`clay --ast`): parameters, defaults,
 * super sends and line numbers are all shown. */
static void print_node(AstNode *node, int indent, const char *label) {
    print_indent(indent);
    if (label) fputs(label, stdout);
    if (!node) { printf("(null)\n"); return; }
    switch (node->type) {
        case NODE_INT_LIT:
            printf("Int(%lld)", node->as.int_lit.value);
            break;
        case NODE_FLOAT_LIT: {
            char buf[64];
            clay_format_float(node->as.float_lit.value, buf, sizeof(buf));
            printf("Float(%s)", buf);
            break;
        }
        case NODE_STRING_LIT:
            printf("String(\"");
            print_escaped(node->as.string_lit.value);
            printf("\")");
            break;
        case NODE_IDENT:      printf("Ident(%s)", node->as.ident.name); break;
        case NODE_SELF:       printf("Self"); break;
        case NODE_ASSIGN:     printf("Assign(%s)", node->as.assign.name); break;
        case NODE_SEND:
            printf("Send(%s.%s)", node->as.send.is_super ? "super" : "", node->as.send.message);
            break;
        case NODE_BINARY:     printf("Binary(%s)", node->as.binary.op); break;
        case NODE_BLOCK:      printf("Block(params=%d)", node->as.block.params.count); break;
        case NODE_METHOD_DEF:
            printf("MethodDef(%s, params=%d)", node->as.method_def.name, node->as.method_def.params.count);
            break;
        case NODE_SLOT_DEF:   printf("SlotDef(%s)", node->as.slot_def.name); break;
        case NODE_CLONE:      printf("Clone(members=%d)", node->as.clone.members.count); break;
        case NODE_ARRAY_LIT:  printf("Array(count=%d)", node->as.array_lit.elements.count); break;
        case NODE_IMPORT:
            printf("Import(\"%s\"", node->as.import.path);
            if (node->as.import.alias) printf(" as %s", node->as.import.alias);
            printf(")");
            break;
        case NODE_TRY_CATCH:  printf("TryCatch"); break;
        case NODE_RAISE:      printf("Raise"); break;
        case NODE_SLOT_ASSIGN: printf("SlotAssign(.%s)", node->as.slot_assign.slot_name); break;
        case NODE_RETURN:     printf("Return"); break;
        case NODE_BREAK:      printf("Break"); break;
        case NODE_CONTINUE:   printf("Continue"); break;
        case NODE_DICT_LIT:   printf("DictLit(count=%d)", node->as.dict_lit.keys.count); break;
        case NODE_DESTRUCTURE: printf("Destructure(targets=%d)", node->as.destructure.targets.count); break;
        case NODE_PROGRAM:    printf("Program(stmts=%d)", node->as.program.stmts.count); break;
    }
    printf(" @%d\n", node->line);

    int in = indent + 1;
    switch (node->type) {
        case NODE_ASSIGN:     print_node(node->as.assign.value, in, NULL); break;
        case NODE_SEND:
            print_node(node->as.send.receiver, in, NULL);
            print_list(&node->as.send.args, in, NULL);
            break;
        case NODE_BINARY:
            print_node(node->as.binary.left, in, NULL);
            print_node(node->as.binary.right, in, NULL);
            break;
        case NODE_BLOCK:
            print_list(&node->as.block.params, in, "param: ");
            print_list(&node->as.block.body, in, NULL);
            break;
        case NODE_METHOD_DEF:
            print_list(&node->as.method_def.params, in, "param: ");
            print_list(&node->as.method_def.body, in, NULL);
            break;
        case NODE_SLOT_DEF:   print_node(node->as.slot_def.value, in, NULL); break;
        case NODE_CLONE:
            print_node(node->as.clone.proto, in, "proto: ");
            print_list(&node->as.clone.members, in, NULL);
            break;
        case NODE_ARRAY_LIT:  print_list(&node->as.array_lit.elements, in, NULL); break;
        case NODE_TRY_CATCH:
            print_node(node->as.try_catch.try_block, in, "try: ");
            if (node->as.try_catch.catch_block) print_node(node->as.try_catch.catch_block, in, "catch: ");
            if (node->as.try_catch.finally_block) print_node(node->as.try_catch.finally_block, in, "finally: ");
            break;
        case NODE_RAISE:      print_node(node->as.raise.expr, in, NULL); break;
        case NODE_SLOT_ASSIGN:
            print_node(node->as.slot_assign.receiver, in, NULL);
            print_node(node->as.slot_assign.value, in, NULL);
            break;
        case NODE_RETURN:
            if (node->as.return_stmt.expr) print_node(node->as.return_stmt.expr, in, NULL);
            break;
        case NODE_DICT_LIT:
            for (int i = 0; i < node->as.dict_lit.keys.count; i++) {
                print_node(node->as.dict_lit.keys.items[i], in, "key: ");
                print_node(node->as.dict_lit.values.items[i], in, "value: ");
            }
            break;
        case NODE_DESTRUCTURE:
            print_list(&node->as.destructure.targets, in, "target: ");
            print_node(node->as.destructure.value, in, "value: ");
            break;
        case NODE_PROGRAM:    print_list(&node->as.program.stmts, in, NULL); break;
        default: break;
    }
}

void ast_print(AstNode *node, int indent) {
    print_node(node, indent, NULL);
}

/* --- Non-local return analysis ---
 * Walk expressions without descending into nested clone bodies (their methods
 * have their own returns). in_block: are we inside a block literal? */
static int ast_return_walk(AstNode *n, int in_block, int want_in_block);

static int ast_return_walk_list(NodeList *l, int in_block, int want_in_block) {
    for (int i = 0; i < l->count; i++)
        if (ast_return_walk(l->items[i], in_block, want_in_block)) return 1;
    return 0;
}

static int ast_return_walk(AstNode *n, int in_block, int want_in_block) {
    if (!n) return 0;
    switch (n->type) {
        case NODE_RETURN:
            if (!want_in_block || in_block) return 1;
            return ast_return_walk(n->as.return_stmt.expr, in_block, want_in_block);
        case NODE_BLOCK:
            return ast_return_walk_list(&n->as.block.body, 1, want_in_block);
        case NODE_ASSIGN:
            return ast_return_walk(n->as.assign.value, in_block, want_in_block);
        case NODE_SEND:
            return ast_return_walk(n->as.send.receiver, in_block, want_in_block) ||
                   ast_return_walk_list(&n->as.send.args, in_block, want_in_block);
        case NODE_BINARY:
            return ast_return_walk(n->as.binary.left, in_block, want_in_block) ||
                   ast_return_walk(n->as.binary.right, in_block, want_in_block);
        case NODE_ARRAY_LIT:
            return ast_return_walk_list(&n->as.array_lit.elements, in_block, want_in_block);
        case NODE_DICT_LIT:
            return ast_return_walk_list(&n->as.dict_lit.values, in_block, want_in_block);
        case NODE_TRY_CATCH:
            return ast_return_walk(n->as.try_catch.try_block, in_block, want_in_block) ||
                   ast_return_walk(n->as.try_catch.catch_block, in_block, want_in_block) ||
                   ast_return_walk(n->as.try_catch.finally_block, in_block, want_in_block);
        case NODE_RAISE:
            return ast_return_walk(n->as.raise.expr, in_block, want_in_block);
        case NODE_SLOT_ASSIGN:
            return ast_return_walk(n->as.slot_assign.receiver, in_block, want_in_block) ||
                   ast_return_walk(n->as.slot_assign.value, in_block, want_in_block);
        case NODE_DESTRUCTURE:
            return ast_return_walk(n->as.destructure.value, in_block, want_in_block);
        case NODE_CLONE:
            return ast_return_walk(n->as.clone.proto, in_block, want_in_block);
        default:
            return 0;   /* literals, identifiers, method/slot defs, break, ... */
    }
}

int ast_has_block_return(NodeList *method_body) {
    return ast_return_walk_list(method_body, 0, 1);
}

int ast_block_contains_return(AstNode *block) {
    return block && block->type == NODE_BLOCK &&
           ast_return_walk_list(&block->as.block.body, 1, 0);
}
