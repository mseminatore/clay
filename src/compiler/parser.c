/*
 * parser.c - Recursive descent parser for Clay
 *
 * Grammar (simplified):
 *   program     = { statement NEWLINE }
 *   statement   = assignment | expression
 *   assignment  = IDENT "=" expression
 *   expression  = binary
 *   binary      = unary { BINOP unary }
 *   unary       = postfix
 *   postfix     = primary { message_send }
 *   message_send = IDENT [ "(" arglist ")" ]
 *   primary     = INT | STRING | IDENT | SELF | "(" expression ")"
 *                | block | "[" arglist "]"
 *   block       = "{" [ "|" params "|" ] { statement NEWLINE } "}"
 *   clone_body  = "{" { slot_or_method NEWLINE } "}"
 */
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Utilities --- */

static void parser_error(Parser *p, const char *msg) {
    if (p->had_error) return;
    p->had_error = 1;
    fprintf(stderr, "%s:%d: error: %s (got '%.*s')\n",
            p->filename, p->current.line, msg,
            p->current.length, p->current.start);
}

static void parser_advance(Parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = lexer_next(&p->lexer);
        if (p->current.type != TOK_ERROR) break;
        parser_error(p, p->current.start);
    }
}

static int check(Parser *p, TokenType type) {
    return p->current.type == type;
}

static int match(Parser *p, TokenType type) {
    if (!check(p, type)) return 0;
    parser_advance(p);
    return 1;
}

static void expect(Parser *p, TokenType type, const char *msg) {
    if (check(p, type)) {
        parser_advance(p);
        return;
    }
    parser_error(p, msg);
}

static void skip_newlines(Parser *p) {
    while (check(p, TOK_NEWLINE) || check(p, TOK_SEMICOLON)) {
        parser_advance(p);
    }
}

static char *token_strdup(Token *tok) {
    char *s = (char *)malloc((size_t)tok->length + 1);
    memcpy(s, tok->start, (size_t)tok->length);
    s[tok->length] = '\0';
    return s;
}

/* --- Forward declarations --- */
static AstNode *parse_expression(Parser *p);
static AstNode *parse_statement(Parser *p);
static AstNode *parse_block(Parser *p);

/* --- Primary --- */

static AstNode *parse_primary(Parser *p) {
    if (p->had_error) return NULL;

    /* Integer literal */
    if (match(p, TOK_INT)) {
        AstNode *node = ast_new(NODE_INT_LIT, p->previous.line);
        char *s = token_strdup(&p->previous);
        node->as.int_lit.value = strtol(s, NULL, 10);
        free(s);
        return node;
    }

    /* String literal */
    if (match(p, TOK_STRING)) {
        AstNode *node = ast_new(NODE_STRING_LIT, p->previous.line);
        node->as.string_lit.value = token_strdup(&p->previous);
        return node;
    }

    /* Identifier */
    if (match(p, TOK_IDENT)) {
        AstNode *node = ast_new(NODE_IDENT, p->previous.line);
        node->as.ident.name = token_strdup(&p->previous);
        return node;
    }

    /* Self */
    if (match(p, TOK_SELF)) {
        return ast_new(NODE_SELF, p->previous.line);
    }

    /* Parenthesized expression */
    if (match(p, TOK_LPAREN)) {
        AstNode *expr = parse_expression(p);
        expect(p, TOK_RPAREN, "Expected ')'");
        return expr;
    }

    /* Block */
    if (check(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    /* Array literal */
    if (match(p, TOK_LBRACKET)) {
        AstNode *node = ast_new(NODE_ARRAY_LIT, p->previous.line);
        nodelist_init(&node->as.array_lit.elements);
        if (!check(p, TOK_RBRACKET)) {
            nodelist_push(&node->as.array_lit.elements, parse_expression(p));
            while (match(p, TOK_COMMA)) {
                skip_newlines(p);
                nodelist_push(&node->as.array_lit.elements, parse_expression(p));
            }
        }
        expect(p, TOK_RBRACKET, "Expected ']'");
        return node;
    }

    parser_error(p, "Expected expression");
    return NULL;
}

/* --- Block --- */

static AstNode *parse_block(Parser *p) {
    expect(p, TOK_LBRACE, "Expected '{'");
    int line = p->previous.line;
    AstNode *node = ast_new(NODE_BLOCK, line);
    nodelist_init(&node->as.block.params);
    nodelist_init(&node->as.block.body);

    skip_newlines(p);

    /* Check for |param1, param2| */
    if (check(p, TOK_PIPE)) {
        parser_advance(p);
        if (!check(p, TOK_PIPE)) {
            /* Parse parameter names */
            expect(p, TOK_IDENT, "Expected parameter name");
            AstNode *param = ast_new(NODE_IDENT, p->previous.line);
            param->as.ident.name = token_strdup(&p->previous);
            nodelist_push(&node->as.block.params, param);

            while (match(p, TOK_COMMA)) {
                expect(p, TOK_IDENT, "Expected parameter name");
                param = ast_new(NODE_IDENT, p->previous.line);
                param->as.ident.name = token_strdup(&p->previous);
                nodelist_push(&node->as.block.params, param);
            }
        }
        expect(p, TOK_PIPE, "Expected '|'");
        skip_newlines(p);
    }

    /* Parse body */
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode *stmt = parse_statement(p);
        if (stmt) nodelist_push(&node->as.block.body, stmt);
        skip_newlines(p);
    }

    expect(p, TOK_RBRACE, "Expected '}'");
    return node;
}

/* --- Clone body: { slot = val \n method(params) { body } } --- */

static AstNode *parse_clone_body(Parser *p) {
    expect(p, TOK_LBRACE, "Expected '{'");
    int line = p->previous.line;
    AstNode *node = ast_new(NODE_CLONE, line);
    node->as.clone.proto = NULL; /* set by caller */
    nodelist_init(&node->as.clone.members);

    skip_newlines(p);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (!check(p, TOK_IDENT)) {
            parser_error(p, "Expected slot or method name in clone body");
            break;
        }

        parser_advance(p); /* consume the identifier */
        char *name = token_strdup(&p->previous);
        int member_line = p->previous.line;

        if (match(p, TOK_ASSIGN)) {
            /* Slot definition: name = expr */
            AstNode *slot = ast_new(NODE_SLOT_DEF, member_line);
            slot->as.slot_def.name = name;
            slot->as.slot_def.value = parse_expression(p);
            nodelist_push(&node->as.clone.members, slot);
        } else if (match(p, TOK_LPAREN)) {
            /* Method definition: name(params) { body } */
            AstNode *method = ast_new(NODE_METHOD_DEF, member_line);
            method->as.method_def.name = name;
            nodelist_init(&method->as.method_def.params);
            nodelist_init(&method->as.method_def.body);

            /* Parse params */
            if (!check(p, TOK_RPAREN)) {
                expect(p, TOK_IDENT, "Expected parameter name");
                AstNode *param = ast_new(NODE_IDENT, p->previous.line);
                param->as.ident.name = token_strdup(&p->previous);
                nodelist_push(&method->as.method_def.params, param);
                while (match(p, TOK_COMMA)) {
                    expect(p, TOK_IDENT, "Expected parameter name");
                    param = ast_new(NODE_IDENT, p->previous.line);
                    param->as.ident.name = token_strdup(&p->previous);
                    nodelist_push(&method->as.method_def.params, param);
                }
            }
            expect(p, TOK_RPAREN, "Expected ')'");
            skip_newlines(p);

            /* Parse method body */
            expect(p, TOK_LBRACE, "Expected '{' for method body");
            skip_newlines(p);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                AstNode *stmt = parse_statement(p);
                if (stmt) nodelist_push(&method->as.method_def.body, stmt);
                skip_newlines(p);
            }
            expect(p, TOK_RBRACE, "Expected '}'");
            nodelist_push(&node->as.clone.members, method);
        } else {
            parser_error(p, "Expected '=' or '(' after name in clone body");
            free(name);
            break;
        }

        skip_newlines(p);
    }

    expect(p, TOK_RBRACE, "Expected '}'");
    return node;
}

/* --- Postfix (message sends) --- */

static AstNode *parse_postfix(Parser *p) {
    AstNode *expr = parse_primary(p);
    if (!expr) return NULL;

    for (;;) {
        if (check(p, TOK_IDENT)) {
            /* message send: expr ident or expr ident(args) */
            parser_advance(p);
            char *msg_name = token_strdup(&p->previous);
            int send_line = p->previous.line;

            /* Check for clone with body: expr clone({ ... }) */
            if (strcmp(msg_name, "clone") == 0 && check(p, TOK_LPAREN)) {
                parser_advance(p); /* consume ( */
                if (check(p, TOK_LBRACE)) {
                    AstNode *clone = parse_clone_body(p);
                    clone->as.clone.proto = expr;
                    clone->line = send_line;
                    expect(p, TOK_RPAREN, "Expected ')' after clone body");
                    free(msg_name);
                    expr = clone;
                    continue;
                }
                /* Normal clone() with no body */
                AstNode *send = ast_new(NODE_SEND, send_line);
                send->as.send.receiver = expr;
                send->as.send.message = msg_name;
                nodelist_init(&send->as.send.args);
                if (!check(p, TOK_RPAREN)) {
                    nodelist_push(&send->as.send.args, parse_expression(p));
                    while (match(p, TOK_COMMA)) {
                        nodelist_push(&send->as.send.args, parse_expression(p));
                    }
                }
                expect(p, TOK_RPAREN, "Expected ')'");
                expr = send;
                continue;
            }

            if (match(p, TOK_LPAREN)) {
                /* Message with args */
                AstNode *send = ast_new(NODE_SEND, send_line);
                send->as.send.receiver = expr;
                send->as.send.message = msg_name;
                nodelist_init(&send->as.send.args);
                if (!check(p, TOK_RPAREN)) {
                    nodelist_push(&send->as.send.args, parse_expression(p));
                    while (match(p, TOK_COMMA)) {
                        skip_newlines(p);
                        nodelist_push(&send->as.send.args, parse_expression(p));
                    }
                }
                expect(p, TOK_RPAREN, "Expected ')'");
                expr = send;
            } else {
                /* Unary message (no args) */
                AstNode *send = ast_new(NODE_SEND, send_line);
                send->as.send.receiver = expr;
                send->as.send.message = msg_name;
                nodelist_init(&send->as.send.args);
                expr = send;
            }
        } else {
            break;
        }
    }

    return expr;
}

/* --- Binary operators --- */

static int is_binary_op(TokenType type) {
    return type == TOK_PLUS || type == TOK_MINUS ||
           type == TOK_STAR || type == TOK_SLASH || type == TOK_PERCENT ||
           type == TOK_EQUAL || type == TOK_NOT_EQUAL ||
           type == TOK_LESS || type == TOK_GREATER ||
           type == TOK_LESS_EQ || type == TOK_GREATER_EQ;
}

static int precedence(TokenType type) {
    switch (type) {
        case TOK_EQUAL: case TOK_NOT_EQUAL:
            return 1;
        case TOK_LESS: case TOK_GREATER: case TOK_LESS_EQ: case TOK_GREATER_EQ:
            return 2;
        case TOK_PLUS: case TOK_MINUS:
            return 3;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
            return 4;
        default: return 0;
    }
}

static AstNode *parse_binary(Parser *p, int min_prec) {
    AstNode *left = parse_postfix(p);
    if (!left) return NULL;

    while (is_binary_op(p->current.type) && precedence(p->current.type) >= min_prec) {
        int op_prec = precedence(p->current.type);
        parser_advance(p);
        char *op = token_strdup(&p->previous);
        int line = p->previous.line;

        AstNode *right = parse_binary(p, op_prec + 1);

        AstNode *bin = ast_new(NODE_BINARY, line);
        bin->as.binary.left = left;
        bin->as.binary.op = op;
        bin->as.binary.right = right;
        left = bin;
    }

    return left;
}

static AstNode *parse_expression(Parser *p) {
    return parse_binary(p, 1);
}

/* --- Statement --- */

static AstNode *parse_statement(Parser *p) {
    if (p->had_error) return NULL;

    /* Check for assignment: IDENT = expr */
    if (check(p, TOK_IDENT)) {
        /* Peek ahead to see if next is '=' (but not '==') */
        Lexer saved_lexer = p->lexer;
        Token saved_current = p->current;

        parser_advance(p);
        Token name_tok = p->previous;

        if (check(p, TOK_ASSIGN)) {
            parser_advance(p);
            AstNode *node = ast_new(NODE_ASSIGN, name_tok.line);
            node->as.assign.name = token_strdup(&name_tok);
            node->as.assign.value = parse_expression(p);
            return node;
        }

        /* Not an assignment — backtrack and parse as expression */
        p->lexer = saved_lexer;
        p->current = saved_current;
    }

    return parse_expression(p);
}

/* --- Public API --- */

void parser_init(Parser *p, const char *source, const char *filename) {
    lexer_init(&p->lexer, source);
    p->had_error = 0;
    p->filename = filename;
    parser_advance(p); /* prime the parser */
}

AstNode *parser_parse(Parser *p) {
    AstNode *program = ast_new(NODE_PROGRAM, 1);
    nodelist_init(&program->as.program.stmts);

    skip_newlines(p);

    while (!check(p, TOK_EOF) && !p->had_error) {
        AstNode *stmt = parse_statement(p);
        if (stmt) {
            nodelist_push(&program->as.program.stmts, stmt);
        }
        /* Expect newline or EOF after statement */
        if (!check(p, TOK_EOF) && !check(p, TOK_RBRACE)) {
            if (!check(p, TOK_NEWLINE) && !check(p, TOK_SEMICOLON)) {
                parser_error(p, "Expected newline or ';' after statement");
                break;
            }
            skip_newlines(p);
        }
    }

    return program;
}
