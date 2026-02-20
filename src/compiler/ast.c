/*
 * ast.c - AST node creation, destruction, and debug printing
 */
#include "ast.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

AstNode *ast_new(NodeType type, int line) {
    AstNode *node = (AstNode *)calloc(1, sizeof(AstNode));
    node->type = type;
    node->line = line;
    return node;
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
            break;
        case NODE_TRY_CATCH:
            ast_free(node->as.try_catch.try_block);
            if (node->as.try_catch.catch_block) ast_free(node->as.try_catch.catch_block);
            break;
        case NODE_RAISE:
            ast_free(node->as.raise.expr);
            break;
        case NODE_SLOT_ASSIGN:
            ast_free(node->as.slot_assign.receiver);
            free(node->as.slot_assign.slot_name);
            ast_free(node->as.slot_assign.value);
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

void ast_print(AstNode *node, int indent) {
    if (!node) { print_indent(indent); printf("(null)\n"); return; }
    print_indent(indent);
    switch (node->type) {
        case NODE_INT_LIT:
            printf("Int(%ld)\n", node->as.int_lit.value);
            break;
        case NODE_FLOAT_LIT:
            printf("Float(%g)\n", node->as.float_lit.value);
            break;
        case NODE_STRING_LIT:
            printf("String(\"%s\")\n", node->as.string_lit.value);
            break;
        case NODE_IDENT:
            printf("Ident(%s)\n", node->as.ident.name);
            break;
        case NODE_SELF:
            printf("Self\n");
            break;
        case NODE_ASSIGN:
            printf("Assign(%s)\n", node->as.assign.name);
            ast_print(node->as.assign.value, indent + 1);
            break;
        case NODE_SEND:
            printf("Send(.%s)\n", node->as.send.message);
            ast_print(node->as.send.receiver, indent + 1);
            for (int i = 0; i < node->as.send.args.count; i++)
                ast_print(node->as.send.args.items[i], indent + 1);
            break;
        case NODE_BINARY:
            printf("Binary(%s)\n", node->as.binary.op);
            ast_print(node->as.binary.left, indent + 1);
            ast_print(node->as.binary.right, indent + 1);
            break;
        case NODE_BLOCK:
            printf("Block(params=%d)\n", node->as.block.params.count);
            for (int i = 0; i < node->as.block.body.count; i++)
                ast_print(node->as.block.body.items[i], indent + 1);
            break;
        case NODE_METHOD_DEF:
            printf("MethodDef(%s, params=%d)\n", node->as.method_def.name, node->as.method_def.params.count);
            for (int i = 0; i < node->as.method_def.body.count; i++)
                ast_print(node->as.method_def.body.items[i], indent + 1);
            break;
        case NODE_SLOT_DEF:
            printf("SlotDef(%s)\n", node->as.slot_def.name);
            ast_print(node->as.slot_def.value, indent + 1);
            break;
        case NODE_CLONE:
            printf("Clone(members=%d)\n", node->as.clone.members.count);
            ast_print(node->as.clone.proto, indent + 1);
            for (int i = 0; i < node->as.clone.members.count; i++)
                ast_print(node->as.clone.members.items[i], indent + 1);
            break;
        case NODE_ARRAY_LIT:
            printf("Array(count=%d)\n", node->as.array_lit.elements.count);
            for (int i = 0; i < node->as.array_lit.elements.count; i++)
                ast_print(node->as.array_lit.elements.items[i], indent + 1);
            break;
        case NODE_IMPORT:
            printf("Import(\"%s\")\n", node->as.import.path);
            break;
        case NODE_TRY_CATCH:
            printf("TryCatch\n");
            print_indent(indent + 1); printf("try: "); ast_print(node->as.try_catch.try_block, indent + 2);
            if (node->as.try_catch.catch_block) {
                print_indent(indent + 1); printf("catch: "); ast_print(node->as.try_catch.catch_block, indent + 2);
            }
            break;
        case NODE_RAISE:
            printf("Raise\n");
            ast_print(node->as.raise.expr, indent + 1);
            break;
        case NODE_SLOT_ASSIGN:
            printf("SlotAssign(.%s)\n", node->as.slot_assign.slot_name);
            ast_print(node->as.slot_assign.receiver, indent + 1);
            ast_print(node->as.slot_assign.value, indent + 1);
            break;
        case NODE_PROGRAM:
            printf("Program(stmts=%d)\n", node->as.program.stmts.count);
            for (int i = 0; i < node->as.program.stmts.count; i++)
                ast_print(node->as.program.stmts.items[i], indent + 1);
            break;
    }
}
