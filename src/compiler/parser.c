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
#include <sys/stat.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

#include "bricks.h"

/* --- File reading helper --- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Resolve a relative import path based on the importing file's directory */
static char *resolve_import_path(const char *base_file, const char *import_path) {
    /* Find directory of base_file */
    const char *last_sep = strrchr(base_file, '/');
    const char *last_bsep = strrchr(base_file, '\\');
    if (last_bsep && (!last_sep || last_bsep > last_sep)) last_sep = last_bsep;

    if (!last_sep) {
        /* base_file has no directory component */
        return strdup(import_path);
    }

    size_t dir_len = (size_t)(last_sep - base_file + 1);
    size_t imp_len = strlen(import_path);
    char *result = (char *)malloc(dir_len + imp_len + 1);
    memcpy(result, base_file, dir_len);
    memcpy(result + dir_len, import_path, imp_len);
    result[dir_len + imp_len] = '\0';
    return result;
}

static int already_imported(Parser *p, const char *path) {
    for (ImportEntry *e = p->imported; e; e = e->next) {
        if (strcmp(e->path, path) == 0) return 1;
    }
    return 0;
}

static void mark_imported(Parser *p, const char *path) {
    ImportEntry *e = (ImportEntry *)malloc(sizeof(ImportEntry));
    e->path = strdup(path);
    e->next = p->imported;
    p->imported = e;
}

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

/* Process C-style escape sequences in a string literal */
static char *process_escapes(const char *raw, int len) {
    char *out = (char *)malloc((size_t)len + 1);
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (raw[i] == '\\' && i + 1 < len) {
            switch (raw[i + 1]) {
                case 'n':  out[j++] = '\n'; i++; break;
                case 't':  out[j++] = '\t'; i++; break;
                case 'r':  out[j++] = '\r'; i++; break;
                case '\\': out[j++] = '\\'; i++; break;
                case '"':  out[j++] = '"';  i++; break;
                case '0':  out[j++] = '\0'; i++; break;
                case 'x': {
                    /* \xHH — hex escape */
                    if (i + 3 < len) {
                        char hex[3] = { raw[i+2], raw[i+3], '\0' };
                        char *end;
                        long val = strtol(hex, &end, 16);
                        if (end == hex + 2) {
                            out[j++] = (char)val;
                            i += 3;
                            break;
                        }
                    }
                    out[j++] = raw[i];
                    break;
                }
                default:   out[j++] = raw[i]; break;
            }
        } else {
            out[j++] = raw[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* --- Forward declarations --- */
static AstNode *parse_expression(Parser *p);
static AstNode *parse_statement(Parser *p);
static AstNode *parse_block(Parser *p);

/* --- Primary --- */

static AstNode *parse_primary(Parser *p) {
    if (p->had_error) return NULL;

    /* Unary minus */
    if (match(p, TOK_MINUS)) {
        int line = p->previous.line;
        AstNode *operand = parse_primary(p);
        if (!operand) return NULL;
        /* Optimize: fold negative integer/float literals */
        if (operand->type == NODE_INT_LIT) {
            operand->as.int_lit.value = -operand->as.int_lit.value;
            return operand;
        }
        if (operand->type == NODE_FLOAT_LIT) {
            operand->as.float_lit.value = -operand->as.float_lit.value;
            return operand;
        }
        /* General case: 0 - operand */
        AstNode *zero = ast_new(NODE_INT_LIT, line);
        zero->as.int_lit.value = 0;
        AstNode *bin = ast_new(NODE_BINARY, line);
        bin->as.binary.left = zero;
        bin->as.binary.op = strdup("-");
        bin->as.binary.right = operand;
        return bin;
    }

    /* Unary not: !expr becomes a NODE_BINARY with op "!" */
    if (match(p, TOK_BANG)) {
        int line = p->previous.line;
        AstNode *operand = parse_primary(p);
        if (!operand) return NULL;
        AstNode *node = ast_new(NODE_SEND, line);
        node->as.send.receiver = operand;
        node->as.send.message = strdup("not");
        nodelist_init(&node->as.send.args);
        return node;
    }

    /* Integer literal */
    if (match(p, TOK_INT)) {
        AstNode *node = ast_new(NODE_INT_LIT, p->previous.line);
        char *s = token_strdup(&p->previous);
        node->as.int_lit.value = strtol(s, NULL, 10);
        free(s);
        return node;
    }

    /* Float literal */
    if (match(p, TOK_FLOAT)) {
        AstNode *node = ast_new(NODE_FLOAT_LIT, p->previous.line);
        char *s = token_strdup(&p->previous);
        node->as.float_lit.value = (float)strtod(s, NULL);
        free(s);
        return node;
    }

    /* String literal */
    if (match(p, TOK_STRING)) {
        AstNode *node = ast_new(NODE_STRING_LIT, p->previous.line);
        node->as.string_lit.value = process_escapes(p->previous.start, p->previous.length);
        return node;
    }

    /* Interpolated string: "text ${expr} more text" */
    if (match(p, TOK_INTERP_STRING)) {
        int line = p->previous.line;
        char *raw = token_strdup(&p->previous);
        int len = (int)strlen(raw);
        AstNode *result = NULL;

        int pos = 0;
        while (pos < len) {
            /* Find next ${ */
            int start = pos;
            while (pos < len) {
                if (pos < len - 1 && raw[pos] == '$' && raw[pos + 1] == '{') break;
                pos++;
            }

            /* Emit literal fragment if any */
            if (pos > start) {
                AstNode *lit = ast_new(NODE_STRING_LIT, line);
                lit->as.string_lit.value = process_escapes(raw + start, pos - start);
                if (!result) {
                    result = lit;
                } else {
                    AstNode *concat = ast_new(NODE_BINARY, line);
                    concat->as.binary.left = result;
                    concat->as.binary.op = strdup("+");
                    concat->as.binary.right = lit;
                    result = concat;
                }
            }

            if (pos >= len) break; /* no more interpolations */

            /* Skip ${ */
            pos += 2;
            /* Find matching } with brace counting */
            int depth = 1;
            int expr_start = pos;
            while (pos < len && depth > 0) {
                if (raw[pos] == '{') depth++;
                else if (raw[pos] == '}') depth--;
                if (depth > 0) pos++;
            }

            /* Extract expression text and parse it */
            int expr_len = pos - expr_start;
            char *expr_text = (char *)malloc((size_t)(expr_len + 1));
            memcpy(expr_text, raw + expr_start, (size_t)expr_len);
            expr_text[expr_len] = '\0';

            /* Parse the expression */
            Parser sub;
            lexer_init(&sub.lexer, expr_text);
            sub.had_error = 0;
            sub.filename = p->filename;
            sub.imported = NULL;
            parser_advance(&sub);
            AstNode *expr = parse_expression(&sub);
            if (sub.had_error) p->had_error = 1;
            free(expr_text);

            if (expr) {
                /* Wrap in toString send */
                AstNode *toStr = ast_new(NODE_SEND, line);
                toStr->as.send.receiver = expr;
                toStr->as.send.message = strdup("toString");
                nodelist_init(&toStr->as.send.args);

                if (!result) {
                    /* Start with empty string so result is always a string */
                    AstNode *empty = ast_new(NODE_STRING_LIT, line);
                    empty->as.string_lit.value = strdup("");
                    AstNode *concat = ast_new(NODE_BINARY, line);
                    concat->as.binary.left = empty;
                    concat->as.binary.op = strdup("+");
                    concat->as.binary.right = toStr;
                    result = concat;
                } else {
                    AstNode *concat = ast_new(NODE_BINARY, line);
                    concat->as.binary.left = result;
                    concat->as.binary.op = strdup("+");
                    concat->as.binary.right = toStr;
                    result = concat;
                }
            }

            if (pos < len) pos++; /* skip closing } */
        }

        free(raw);
        if (!result) {
            result = ast_new(NODE_STRING_LIT, line);
            result->as.string_lit.value = strdup("");
        }
        return result;
    }

    /* try/catch expression */
    if (match(p, TOK_TRY)) {
        int line = p->previous.line;
        expect(p, TOK_LPAREN, "Expected '(' after try");
        AstNode *try_block = parse_block(p);
        expect(p, TOK_RPAREN, "Expected ')' after try block");
        skip_newlines(p);
        AstNode *catch_block = NULL;
        if (match(p, TOK_CATCH)) {
            expect(p, TOK_LPAREN, "Expected '(' after catch");
            catch_block = parse_block(p);
            expect(p, TOK_RPAREN, "Expected ')' after catch block");
        }
        /* Peek for finally — save full parser state, skip newlines, check for TOK_FINALLY */
        AstNode *finally_block = NULL;
        {
            Token saved_current = p->current;
            Token saved_previous = p->previous;
            Lexer saved_lexer = p->lexer;
            skip_newlines(p);
            if (match(p, TOK_FINALLY)) {
                expect(p, TOK_LPAREN, "Expected '(' after finally");
                finally_block = parse_block(p);
                expect(p, TOK_RPAREN, "Expected ')' after finally block");
            } else {
                /* No finally — restore full parser state */
                p->current = saved_current;
                p->previous = saved_previous;
                p->lexer = saved_lexer;
            }
        }
        AstNode *node = ast_new(NODE_TRY_CATCH, line);
        node->as.try_catch.try_block = try_block;
        node->as.try_catch.catch_block = catch_block;
        node->as.try_catch.finally_block = finally_block;
        return node;
    }

    /* raise expression */
    if (match(p, TOK_RAISE)) {
        int line = p->previous.line;
        expect(p, TOK_LPAREN, "Expected '(' after raise");
        AstNode *expr = parse_expression(p);
        expect(p, TOK_RPAREN, "Expected ')' after raise argument");
        AstNode *node = ast_new(NODE_RAISE, line);
        node->as.raise.expr = expr;
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

    /* Super — returns NODE_SELF; parse_postfix marks the send as is_super */
    if (match(p, TOK_SUPER)) {
        if (!check(p, TOK_IDENT)) {
            parser_error(p, "Expected method name after 'super'");
            return ast_new(NODE_SELF, p->previous.line);
        }
        /* We return NODE_SELF but tag it so parse_postfix can detect it.
         * Reuse the int_lit.value field (NODE_SELF doesn't use the union). */
        AstNode *node = ast_new(NODE_SELF, p->previous.line);
        node->as.int_lit.value = 1; /* marker for super */
        return node;
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

    /* Detect super marker (NODE_SELF with int_lit.value == 1) */
    int next_is_super = (expr->type == NODE_SELF && expr->as.int_lit.value == 1);
    if (next_is_super) expr->as.int_lit.value = 0; /* clear marker */

    for (;;) {
        if (check(p, TOK_IDENT)) {
            /* message send: expr ident or expr ident(args) */
            parser_advance(p);
            char *msg_name = token_strdup(&p->previous);
            int send_line = p->previous.line;

            /* Check for clone with body: expr clone { ... } or expr clone({ ... }) */
            if (strcmp(msg_name, "clone") == 0 && check(p, TOK_LBRACE)) {
                /* Short form: clone { ... } (no parens) */
                AstNode *clone = parse_clone_body(p);
                clone->as.clone.proto = expr;
                clone->line = send_line;
                free(msg_name);
                expr = clone;
                continue;
            }
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
                send->as.send.is_super = next_is_super;
                next_is_super = 0;
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
                send->as.send.is_super = next_is_super;
                next_is_super = 0;
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
                send->as.send.is_super = next_is_super;
                next_is_super = 0;
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
           type == TOK_LESS_EQ || type == TOK_GREATER_EQ ||
           type == TOK_AND || type == TOK_OR;
}

static int precedence(TokenType type) {
    switch (type) {
        case TOK_OR:
            return 1;
        case TOK_AND:
            return 2;
        case TOK_EQUAL: case TOK_NOT_EQUAL:
            return 3;
        case TOK_LESS: case TOK_GREATER: case TOK_LESS_EQ: case TOK_GREATER_EQ:
            return 4;
        case TOK_PLUS: case TOK_MINUS:
            return 5;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
            return 6;
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

/* Returns the binary operator string for a compound assignment token, or NULL */
static const char *compound_assign_op(TokenType type) {
    switch (type) {
        case TOK_PLUS_ASSIGN:    return "+";
        case TOK_MINUS_ASSIGN:   return "-";
        case TOK_STAR_ASSIGN:    return "*";
        case TOK_SLASH_ASSIGN:   return "/";
        case TOK_PERCENT_ASSIGN: return "%";
        default:                 return NULL;
    }
}

static AstNode *parse_statement(Parser *p) {
    if (p->had_error) return NULL;

    /* Check for import("path") */
    if (check(p, TOK_IMPORT)) {
        parser_advance(p);
        expect(p, TOK_LPAREN, "Expected '(' after import");
        expect(p, TOK_STRING, "Expected string path in import()");
        char *import_path = token_strdup(&p->previous);
        expect(p, TOK_RPAREN, "Expected ')' after import path");

        AstNode *node = ast_new(NODE_IMPORT, p->previous.line);
        node->as.import.path = import_path;
        return node;
    }

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

        /* Check for compound assignment: IDENT += expr → IDENT = IDENT + expr */
        const char *cop = compound_assign_op(p->current.type);
        if (cop) {
            parser_advance(p); /* consume += etc. */
            AstNode *lhs = ast_new(NODE_IDENT, name_tok.line);
            lhs->as.ident.name = token_strdup(&name_tok);

            AstNode *rhs = parse_expression(p);
            AstNode *binop = ast_new(NODE_BINARY, name_tok.line);
            binop->as.binary.left = lhs;
            binop->as.binary.op = strdup(cop);
            binop->as.binary.right = rhs;

            AstNode *node = ast_new(NODE_ASSIGN, name_tok.line);
            node->as.assign.name = token_strdup(&name_tok);
            node->as.assign.value = binop;
            return node;
        }

        /* Not an assignment — backtrack and parse as expression */
        p->lexer = saved_lexer;
        p->current = saved_current;
    }

    /* Parse as expression, then check for slot assignment pattern:
     * If result is a zero-arg send (e.g. p x) followed by =, it's slot assignment */
    AstNode *expr = parse_expression(p);
    if (expr && expr->type == NODE_SEND && expr->as.send.args.count == 0 &&
        check(p, TOK_ASSIGN)) {
        parser_advance(p); /* consume = */
        AstNode *node = ast_new(NODE_SLOT_ASSIGN, expr->line);
        node->as.slot_assign.receiver = expr->as.send.receiver;
        node->as.slot_assign.slot_name = expr->as.send.message; /* steal the string */
        node->as.slot_assign.value = parse_expression(p);
        /* Free the send node shell without freeing receiver/message */
        expr->as.send.receiver = NULL;
        expr->as.send.message = NULL;
        free(expr);
        return node;
    }

    /* Check for compound slot assignment: obj slot += expr → obj slot = obj slot + expr */
    if (expr && expr->type == NODE_SEND && expr->as.send.args.count == 0) {
        const char *cop = compound_assign_op(p->current.type);
        if (cop) {
            parser_advance(p); /* consume += etc. */

            /* Build RHS: obj slot + expr → NODE_BINARY(NODE_SEND(recv, slot), op, expr) */
            AstNode *read_slot = ast_new(NODE_SEND, expr->line);
            read_slot->as.send.receiver = ast_clone(expr->as.send.receiver);
            read_slot->as.send.message = strdup(expr->as.send.message);
            nodelist_init(&read_slot->as.send.args);

            AstNode *rhs = parse_expression(p);
            AstNode *binop = ast_new(NODE_BINARY, expr->line);
            binop->as.binary.left = read_slot;
            binop->as.binary.op = strdup(cop);
            binop->as.binary.right = rhs;

            AstNode *node = ast_new(NODE_SLOT_ASSIGN, expr->line);
            node->as.slot_assign.receiver = expr->as.send.receiver;
            node->as.slot_assign.slot_name = expr->as.send.message;
            node->as.slot_assign.value = binop;
            /* Detach from expr so free doesn't recurse into reused pointers */
            expr->as.send.receiver = NULL;
            expr->as.send.message = NULL;
            free(expr);
            return node;
        }
    }

    return expr;
}

/* --- Public API --- */

void parser_init(Parser *p, const char *source, const char *filename) {
    lexer_init(&p->lexer, source);
    p->had_error = 0;
    p->filename = filename;
    p->imported = NULL;
    parser_advance(p); /* prime the parser */
}

/* Parse an imported file and return its statements */
static AstNode *parse_import_file(Parser *parent, const char *import_path) {
    char *resolved = NULL;

    /* Check if this is a brick import.
     * Bricks: no .clay extension, no backslash, doesn't start with "." (relative path).
     * Simple brick: "clay-match" (no /)
     * Monorepo brick: "clay-stdlib/match" (has /) */
    int is_brick = !strstr(import_path, ".clay") &&
                   !strchr(import_path, '\\') &&
                   import_path[0] != '.';
    if (is_brick) {
        /* Try bricks/<path>/brick.json for main entry point */
        char manifest_path[BRICK_MAX_PATH];
        snprintf(manifest_path, sizeof(manifest_path), "bricks/%s/brick.json", import_path);
        BrickManifest bm;
        if (brick_parse(manifest_path, &bm) == 0 && bm.main_file[0]) {
            char brick_main[BRICK_MAX_PATH];
            snprintf(brick_main, sizeof(brick_main), "bricks/%s/%s", import_path, bm.main_file);
            struct stat st;
            if (stat(brick_main, &st) == 0) {
                resolved = strdup(brick_main);
            }
        }
        /* Fallback: bricks/<path>/src/main.clay */
        if (!resolved) {
            char fallback[BRICK_MAX_PATH];
            snprintf(fallback, sizeof(fallback), "bricks/%s/src/main.clay", import_path);
            struct stat st;
            if (stat(fallback, &st) == 0) {
                resolved = strdup(fallback);
            }
        }
    }

    if (!resolved) {
        resolved = resolve_import_path(parent->filename, import_path);
    }

    if (already_imported(parent, resolved)) {
        free(resolved);
        return NULL; /* skip double import */
    }
    mark_imported(parent, resolved);

    char *source = read_file(resolved);
    if (!source) {
        fprintf(stderr, "%s: cannot open import '%s'\n", parent->filename, resolved);
        parent->had_error = 1;
        free(resolved);
        return NULL;
    }

    Parser import_parser;
    lexer_init(&import_parser.lexer, source);
    import_parser.had_error = 0;
    import_parser.filename = resolved;
    import_parser.imported = parent->imported; /* share import tracking */
    parser_advance(&import_parser);

    AstNode *program = parser_parse(&import_parser);

    if (import_parser.had_error) {
        parent->had_error = 1;
    }
    /* Update parent's import list with any new imports from the child */
    parent->imported = import_parser.imported;

    /* Note: source and resolved are leaked intentionally —
     * they're needed while the AST references them.
     * A real compiler would use an arena allocator. */
    return program;
}

AstNode *parser_parse(Parser *p) {
    AstNode *program = ast_new(NODE_PROGRAM, 1);
    nodelist_init(&program->as.program.stmts);

    skip_newlines(p);

    while (!check(p, TOK_EOF) && !p->had_error) {
        AstNode *stmt = parse_statement(p);
        if (stmt) {
            if (stmt->type == NODE_IMPORT) {
                /* Resolve import: parse the file and splice its statements */
                AstNode *imported = parse_import_file(p, stmt->as.import.path);
                if (imported && imported->type == NODE_PROGRAM) {
                    for (int i = 0; i < imported->as.program.stmts.count; i++) {
                        nodelist_push(&program->as.program.stmts,
                                      imported->as.program.stmts.items[i]);
                    }
                    /* Free the program wrapper but not the statements (they're now ours) */
                    imported->as.program.stmts.items = NULL;
                    imported->as.program.stmts.count = 0;
                    ast_free(imported);
                }
                ast_free(stmt); /* free the import node itself */
            } else {
                nodelist_push(&program->as.program.stmts, stmt);
            }
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

void parser_free_imports(Parser *p) {
    ImportEntry *e = p->imported;
    while (e) {
        ImportEntry *next = e->next;
        free(e->path);
        free(e);
        e = next;
    }
    p->imported = NULL;
}
