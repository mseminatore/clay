/*
 * ast.c - AST node creation, destruction, and debug printing
 */
#include "ast.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

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
            c->as.send.is_call = node->as.send.is_call;
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
            if (node->as.send.is_call)
                printf("Call(%s)", node->as.send.receiver->as.ident.name);
            else
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
            if (!node->as.send.is_call) print_node(node->as.send.receiver, in, NULL);
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

/* Is n `c ifTrue { ... }` or `c ifFalse { ... }` with a block literal that
 * takes no parameters? The compilers run such a block in place (inline). */
int ast_is_inline_if(AstNode *n) {
    if (!n || n->type != NODE_SEND || n->as.send.is_super || n->as.send.is_call) return 0;
    if (n->as.send.args.count != 1) return 0;
    const char *m = n->as.send.message;
    if (!m || (strcmp(m, "ifTrue") != 0 && strcmp(m, "ifFalse") != 0)) return 0;
    AstNode *b = n->as.send.args.items[0];
    return b && b->type == NODE_BLOCK && b->as.block.params.count == 0;
}

/* --- Inlined whileTrue/whileFalse ---
 * `{ c } whileTrue { ... }` (or whileFalse) with parameterless block literals
 * runs in place when each break and continue that stops or restarts it can
 * be a jump: the ones among the body's statements, also through the
 * statements of inlined ifTrue/ifFalse blocks that are statements themselves
 * or give the body's value. One anywhere else (in an expression, a try
 * block, `{ ... } value`, ...) is raised as usual and must reach the loop's
 * runtime method, so that loop is sent. A block literal passed to a message
 * (a loop's body among them) keeps its own break and continue. */

/* x whileTrue { ... } / whileFalse with a block literal argument */
static int is_loop_send(AstNode *n) {
    if (!n || n->type != NODE_SEND || n->as.send.is_super || n->as.send.is_call) return 0;
    if (n->as.send.args.count != 1) return 0;
    const char *m = n->as.send.message;
    if (!m || (strcmp(m, "whileTrue") != 0 && strcmp(m, "whileFalse") != 0)) return 0;
    AstNode *b = n->as.send.args.items[0];
    return b && b->type == NODE_BLOCK;
}

static int is_cond_message(const char *m) {
    return m && (strcmp(m, "ifTrue") == 0 || strcmp(m, "ifFalse") == 0);
}

static int loop_ctl_escapes(AstNode *n);

static int loop_ctl_escapes_list(NodeList *l) {
    for (int i = 0; i < l->count; i++)
        if (loop_ctl_escapes(l->items[i])) return 1;
    return 0;
}

/* Is there a break or continue in n whose signal could leave it? */
static int loop_ctl_escapes(AstNode *n) {
    if (!n) return 0;
    switch (n->type) {
        case NODE_BREAK:
        case NODE_CONTINUE:
            return 1;
        case NODE_BLOCK:
            return loop_ctl_escapes_list(&n->as.block.body);
        case NODE_SEND: {
            /* a block literal passed to a message keeps its own: the send
             * ends on its break, its run ends on its continue */
            if (loop_ctl_escapes(n->as.send.receiver)) return 1;
            int cond = is_cond_message(n->as.send.message);
            for (int i = 0; i < n->as.send.args.count; i++) {
                AstNode *a = n->as.send.args.items[i];
                if (a && a->type == NODE_BLOCK && !cond) continue;
                if (loop_ctl_escapes(a)) return 1;
            }
            return 0;
        }
        case NODE_BINARY:
            return loop_ctl_escapes(n->as.binary.left) || loop_ctl_escapes(n->as.binary.right);
        case NODE_ASSIGN:
            return loop_ctl_escapes(n->as.assign.value);
        case NODE_RETURN:
            return loop_ctl_escapes(n->as.return_stmt.expr);
        case NODE_ARRAY_LIT:
            return loop_ctl_escapes_list(&n->as.array_lit.elements);
        case NODE_DICT_LIT:
            return loop_ctl_escapes_list(&n->as.dict_lit.values);
        case NODE_TRY_CATCH:
            return loop_ctl_escapes(n->as.try_catch.try_block) ||
                   loop_ctl_escapes(n->as.try_catch.catch_block) ||
                   loop_ctl_escapes(n->as.try_catch.finally_block);
        case NODE_RAISE:
            return loop_ctl_escapes(n->as.raise.expr);
        case NODE_SLOT_ASSIGN:
            return loop_ctl_escapes(n->as.slot_assign.receiver) ||
                   loop_ctl_escapes(n->as.slot_assign.value);
        case NODE_DESTRUCTURE:
            return loop_ctl_escapes(n->as.destructure.value);
        case NODE_CLONE:
            if (loop_ctl_escapes(n->as.clone.proto)) return 1;
            for (int i = 0; i < n->as.clone.members.count; i++) {
                AstNode *m = n->as.clone.members.items[i];
                if (m && m->type == NODE_SLOT_DEF && loop_ctl_escapes(m->as.slot_def.value)) return 1;
            }
            return 0;
        default:
            return 0;
    }
}

/* Can every break/continue in these loop-body statements be a jump? */
static int loop_jumps_list(NodeList *l) {
    for (int i = 0; i < l->count; i++) {
        AstNode *n = l->items[i];
        if (!n || n->type == NODE_BREAK || n->type == NODE_CONTINUE) continue;
        if (ast_is_inline_if(n)) {
            AstNode *recv = n->as.send.receiver;
            if (ast_is_inline_if(recv)) {
                if (loop_ctl_escapes(recv->as.send.receiver) ||
                    !loop_jumps_list(&recv->as.send.args.items[0]->as.block.body))
                    return 0;
            } else if (loop_ctl_escapes(recv)) {
                return 0;
            }
            if (!loop_jumps_list(&n->as.send.args.items[0]->as.block.body)) return 0;
            continue;
        }
        if (loop_ctl_escapes(n)) return 0;
    }
    return 1;
}

int ast_is_inline_while(AstNode *n) {
    if (!is_loop_send(n)) return 0;
    AstNode *cond = n->as.send.receiver;
    AstNode *body = n->as.send.args.items[0];
    return cond && cond->type == NODE_BLOCK && cond->as.block.params.count == 0 &&
           body->as.block.params.count == 0 && loop_jumps_list(&body->as.block.body);
}

/* --- break / continue in a block passed to a message ---
 * Each break and continue belongs to the innermost loop around it: an
 * inlined whileTrue/whileFalse (a jump), or a block literal passed to a
 * message other than ifTrue/ifFalse. A break ends that message's call (nil,
 * unless the method catches it itself, as each, times and whileTrue do); a
 * continue ends that run of the block (nil). The blocks of ifTrue/ifFalse
 * and try/catch and a block a message is sent to run where they are, so
 * theirs belong to the code around them. Called on such a block's body:
 * the bodies of inlined ifTrue/ifFalse and whileTrue conditions are its own
 * code; any other block run from it is a sub-block (sub_only counts only
 * the ones in those, which have to be raised). */
static int owned_ctl(AstNode *n, int kind, int sub_only, int in_sub);

static int owned_ctl_list(NodeList *l, int kind, int sub_only, int in_sub) {
    for (int i = 0; i < l->count; i++)
        if (owned_ctl(l->items[i], kind, sub_only, in_sub)) return 1;
    return 0;
}

/* a block run where it is written: its statements, in a sub-block */
static int owned_ctl_sub(AstNode *b, int kind, int sub_only) {
    if (!b) return 0;
    if (b->type != NODE_BLOCK) return owned_ctl(b, kind, sub_only, 1);
    return owned_ctl_list(&b->as.block.body, kind, sub_only, 1);
}

static int owned_ctl(AstNode *n, int kind, int sub_only, int in_sub) {
    if (!n) return 0;
    switch (n->type) {
        case NODE_BREAK:
        case NODE_CONTINUE:
            return (int)n->type == kind && (in_sub || !sub_only);
        case NODE_SEND: {
            AstNode *r = n->as.send.receiver;
            if (ast_is_inline_if(n))
                return owned_ctl(r, kind, sub_only, in_sub) ||
                       owned_ctl_list(&n->as.send.args.items[0]->as.block.body, kind, sub_only, in_sub);
            if (ast_is_inline_while(n))   /* the body is the loop's */
                return owned_ctl_list(&r->as.block.body, kind, sub_only, in_sub);
            if (r && r->type == NODE_BLOCK) {
                if (owned_ctl_sub(r, kind, sub_only)) return 1;
            } else if (owned_ctl(r, kind, sub_only, in_sub)) {
                return 1;
            }
            int cond = is_cond_message(n->as.send.message);
            for (int i = 0; i < n->as.send.args.count; i++) {
                AstNode *a = n->as.send.args.items[i];
                if (a && a->type == NODE_BLOCK) {
                    if (cond && owned_ctl_sub(a, kind, sub_only)) return 1;
                } else if (owned_ctl(a, kind, sub_only, in_sub)) {
                    return 1;
                }
            }
            return 0;
        }
        case NODE_TRY_CATCH:
            return owned_ctl_sub(n->as.try_catch.try_block, kind, sub_only) ||
                   owned_ctl_sub(n->as.try_catch.catch_block, kind, sub_only) ||
                   owned_ctl_sub(n->as.try_catch.finally_block, kind, sub_only);
        case NODE_ASSIGN:
            return owned_ctl(n->as.assign.value, kind, sub_only, in_sub);
        case NODE_BINARY:
            return owned_ctl(n->as.binary.left, kind, sub_only, in_sub) ||
                   owned_ctl(n->as.binary.right, kind, sub_only, in_sub);
        case NODE_RETURN:
            return owned_ctl(n->as.return_stmt.expr, kind, sub_only, in_sub);
        case NODE_ARRAY_LIT:
            return owned_ctl_list(&n->as.array_lit.elements, kind, sub_only, in_sub);
        case NODE_DICT_LIT:
            return owned_ctl_list(&n->as.dict_lit.values, kind, sub_only, in_sub);
        case NODE_RAISE:
            return owned_ctl(n->as.raise.expr, kind, sub_only, in_sub);
        case NODE_SLOT_ASSIGN:
            return owned_ctl(n->as.slot_assign.receiver, kind, sub_only, in_sub) ||
                   owned_ctl(n->as.slot_assign.value, kind, sub_only, in_sub);
        case NODE_DESTRUCTURE:
            return owned_ctl(n->as.destructure.value, kind, sub_only, in_sub);
        case NODE_CLONE:
            return owned_ctl(n->as.clone.proto, kind, sub_only, in_sub);
        default:
            return 0;   /* a stored block, method bodies, literals, ... */
    }
}

/* The body of a whileTrue/whileFalse sent as a message keeps the plain
 * signals, which Block's whileTrue catches (a break gives the last body
 * value, as an inlined loop does) */
int ast_block_is_loop_arg(AstNode *send, AstNode *arg) {
    return send && send->type == NODE_SEND && arg && arg->type == NODE_BLOCK &&
           !is_cond_message(send->as.send.message) && !is_loop_send(send);
}

int ast_send_catches_break(AstNode *n) {
    if (!n || n->type != NODE_SEND) return 0;
    for (int i = 0; i < n->as.send.args.count; i++) {
        AstNode *a = n->as.send.args.items[i];
        if (ast_block_is_loop_arg(n, a) &&
            owned_ctl_list(&a->as.block.body, NODE_BREAK, 0, 0)) return 1;
    }
    return 0;
}

int ast_block_sub_raises(AstNode *block, int kind) {
    return block && block->type == NODE_BLOCK &&
           owned_ctl_list(&block->as.block.body, kind, 1, 0);
}

/* --- Non-local return analysis ---
 * Walk expressions without descending into nested clone bodies (their methods
 * have their own returns). in_block: are we inside a block literal?
 * inline_ifs: the blocks of inlined ifTrue/ifFalse (ast_is_inline_if) are part
 * of the code around them, not blocks. */
static int ast_return_walk(AstNode *n, int in_block, int want_in_block, int inline_ifs);

static int ast_return_walk_list(NodeList *l, int in_block, int want_in_block, int inline_ifs) {
    for (int i = 0; i < l->count; i++)
        if (ast_return_walk(l->items[i], in_block, want_in_block, inline_ifs)) return 1;
    return 0;
}

static int ast_return_walk(AstNode *n, int in_block, int want_in_block, int inline_ifs) {
    if (!n) return 0;
    switch (n->type) {
        case NODE_RETURN:
            if (!want_in_block || in_block) return 1;
            return ast_return_walk(n->as.return_stmt.expr, in_block, want_in_block, inline_ifs);
        case NODE_BLOCK:
            return ast_return_walk_list(&n->as.block.body, 1, want_in_block, inline_ifs);
        case NODE_ASSIGN:
            return ast_return_walk(n->as.assign.value, in_block, want_in_block, inline_ifs);
        case NODE_SEND:
            if (inline_ifs && ast_is_inline_if(n))
                return ast_return_walk(n->as.send.receiver, in_block, want_in_block, inline_ifs) ||
                       ast_return_walk_list(&n->as.send.args.items[0]->as.block.body,
                                            in_block, want_in_block, inline_ifs);
            if (inline_ifs && ast_is_inline_while(n))
                return ast_return_walk_list(&n->as.send.receiver->as.block.body,
                                            in_block, want_in_block, inline_ifs) ||
                       ast_return_walk_list(&n->as.send.args.items[0]->as.block.body,
                                            in_block, want_in_block, inline_ifs);
            return ast_return_walk(n->as.send.receiver, in_block, want_in_block, inline_ifs) ||
                   ast_return_walk_list(&n->as.send.args, in_block, want_in_block, inline_ifs);
        case NODE_BINARY:
            return ast_return_walk(n->as.binary.left, in_block, want_in_block, inline_ifs) ||
                   ast_return_walk(n->as.binary.right, in_block, want_in_block, inline_ifs);
        case NODE_ARRAY_LIT:
            return ast_return_walk_list(&n->as.array_lit.elements, in_block, want_in_block, inline_ifs);
        case NODE_DICT_LIT:
            return ast_return_walk_list(&n->as.dict_lit.values, in_block, want_in_block, inline_ifs);
        case NODE_TRY_CATCH:
            return ast_return_walk(n->as.try_catch.try_block, in_block, want_in_block, inline_ifs) ||
                   ast_return_walk(n->as.try_catch.catch_block, in_block, want_in_block, inline_ifs) ||
                   ast_return_walk(n->as.try_catch.finally_block, in_block, want_in_block, inline_ifs);
        case NODE_RAISE:
            return ast_return_walk(n->as.raise.expr, in_block, want_in_block, inline_ifs);
        case NODE_SLOT_ASSIGN:
            return ast_return_walk(n->as.slot_assign.receiver, in_block, want_in_block, inline_ifs) ||
                   ast_return_walk(n->as.slot_assign.value, in_block, want_in_block, inline_ifs);
        case NODE_DESTRUCTURE:
            return ast_return_walk(n->as.destructure.value, in_block, want_in_block, inline_ifs);
        case NODE_CLONE:
            return ast_return_walk(n->as.clone.proto, in_block, want_in_block, inline_ifs);
        default:
            return 0;   /* literals, identifiers, method/slot defs, break, ... */
    }
}

int ast_has_block_return(NodeList *method_body) {
    return ast_return_walk_list(method_body, 0, 1, 0);
}

int ast_has_block_return_inlined(NodeList *method_body) {
    return ast_return_walk_list(method_body, 0, 1, 1);
}

int ast_block_contains_return(AstNode *block) {
    return block && block->type == NODE_BLOCK &&
           ast_return_walk_list(&block->as.block.body, 1, 0, 0);
}
