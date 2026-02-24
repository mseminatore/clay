#ifndef CLAY_AST_H
#define CLAY_AST_H

typedef enum {
    NODE_PROGRAM,       /* top-level list of statements */
    NODE_INT_LIT,       /* integer literal: 42 */
    NODE_FLOAT_LIT,     /* float literal: 3.14 */
    NODE_STRING_LIT,    /* string literal: "hello" */
    NODE_IDENT,         /* identifier: foo */
    NODE_SELF,          /* self keyword */
    NODE_ASSIGN,        /* name = expr */
    NODE_SEND,          /* receiver message(args) */
    NODE_BINARY,        /* a + b  (sugar for send) */
    NODE_BLOCK,         /* { |params| body } */
    NODE_METHOD_DEF,    /* name(params) { body } inside clone */
    NODE_SLOT_DEF,      /* name = expr inside clone */
    NODE_CLONE,         /* expr clone({ slots/methods }) */
    NODE_ARRAY_LIT,     /* [a, b, c] */
    NODE_IMPORT,        /* import("path") */
    NODE_TRY_CATCH,     /* try({ ... }) catch({ |e| ... }) */
    NODE_RAISE,         /* raise(expr) */
    NODE_SLOT_ASSIGN    /* receiver slot = expr */
} NodeType;

typedef struct AstNode AstNode;

/* Dynamic array of AstNode pointers */
typedef struct {
    AstNode **items;
    int count;
    int capacity;
} NodeList;

struct AstNode {
    NodeType type;
    int line;

    union {
        /* NODE_INT_LIT */
        struct { long value; } int_lit;

        /* NODE_FLOAT_LIT */
        struct { float value; } float_lit;

        /* NODE_STRING_LIT */
        struct { char *value; } string_lit;

        /* NODE_IDENT */
        struct { char *name; } ident;

        /* NODE_ASSIGN */
        struct {
            char *name;
            AstNode *value;
        } assign;

        /* NODE_SEND */
        struct {
            AstNode *receiver;
            char *message;
            NodeList args;
            int is_super;   /* 1 if receiver is 'super' */
        } send;

        /* NODE_BINARY */
        struct {
            AstNode *left;
            char *op;
            AstNode *right;
        } binary;

        /* NODE_BLOCK */
        struct {
            NodeList params;  /* list of NODE_IDENT for param names */
            NodeList body;
        } block;

        /* NODE_METHOD_DEF */
        struct {
            char *name;
            NodeList params;
            NodeList body;
        } method_def;

        /* NODE_SLOT_DEF */
        struct {
            char *name;
            AstNode *value;
        } slot_def;

        /* NODE_CLONE */
        struct {
            AstNode *proto;
            NodeList members; /* NODE_SLOT_DEF and NODE_METHOD_DEF */
        } clone;

        /* NODE_ARRAY_LIT */
        struct { NodeList elements; } array_lit;

        /* NODE_IMPORT */
        struct { char *path; } import;

        /* NODE_TRY_CATCH */
        struct {
            AstNode *try_block;
            AstNode *catch_block;   /* block with 1 param (error) */
            AstNode *finally_block; /* optional finally block */
        } try_catch;

        /* NODE_RAISE */
        struct { AstNode *expr; } raise;

        /* NODE_SLOT_ASSIGN: receiver slot = value */
        struct {
            AstNode *receiver;
            char *slot_name;
            AstNode *value;
        } slot_assign;

        /* NODE_PROGRAM */
        struct { NodeList stmts; } program;
    } as;
};

/* --- API --- */
AstNode *ast_new(NodeType type, int line);
AstNode *ast_clone(AstNode *node);
void     ast_free(AstNode *node);

void nodelist_init(NodeList *list);
void nodelist_push(NodeList *list, AstNode *node);
void nodelist_free(NodeList *list);

void ast_print(AstNode *node, int indent);

#endif /* CLAY_AST_H */
