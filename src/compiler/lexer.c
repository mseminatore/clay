/*
 * lexer.c - Tokenizer for Clay source files
 */
#include "lexer.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

void lexer_init(Lexer *lexer, const char *source) {
    lexer->source = source;
    lexer->current = source;
    lexer->line = 1;
}

static Token make_token(Lexer *lexer, TokenType type, const char *start, int length) {
    Token t;
    t.type = type;
    t.start = start;
    t.length = length;
    t.line = lexer->line;
    return t;
}

static Token error_token(Lexer *lexer, const char *msg) {
    Token t;
    t.type = TOK_ERROR;
    t.start = msg;
    t.length = (int)strlen(msg);
    t.line = lexer->line;
    return t;
}

static char peek(Lexer *lexer) {
    return *lexer->current;
}

static char advance(Lexer *lexer) {
    char c = *lexer->current;
    lexer->current++;
    return c;
}

static int match(Lexer *lexer, char expected) {
    if (*lexer->current == expected) {
        lexer->current++;
        return 1;
    }
    return 0;
}

static void skip_whitespace(Lexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else if (c == '/' && lexer->current[1] == '/') {
            /* Line comment */
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
        } else if (c == '/' && lexer->current[1] == '*') {
            /* Block comment */
            advance(lexer); advance(lexer);
            while (!(peek(lexer) == '*' && lexer->current[1] == '/')) {
                if (peek(lexer) == '\0') return;
                if (peek(lexer) == '\n') lexer->line++;
                advance(lexer);
            }
            advance(lexer); advance(lexer);
        } else {
            return;
        }
    }
}

static Token lex_triple_string(Lexer *lexer) {
    const char *start = lexer->current; /* after opening """ */
    int has_interp = 0;
    while (!(lexer->current[0] == '"' && lexer->current[1] == '"' && lexer->current[2] == '"')) {
        if (peek(lexer) == '\0') {
            return error_token(lexer, "Unterminated triple-quoted string");
        }
        if (peek(lexer) == '\n') lexer->line++;
        if (peek(lexer) == '$' && lexer->current[1] == '{') has_interp = 1;
        advance(lexer);
    }
    int length = (int)(lexer->current - start);
    advance(lexer); /* skip first closing " */
    advance(lexer); /* skip second closing " */
    advance(lexer); /* skip third closing " */
    return make_token(lexer, has_interp ? TOK_INTERP_STRING : TOK_STRING, start, length);
}

static Token lex_string(Lexer *lexer) {
    const char *start = lexer->current; /* after opening quote */
    int has_interp = 0;
    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        if (peek(lexer) == '\n') lexer->line++;
        if (peek(lexer) == '\\') {
            /* skip the escaped character (an escaped newline is still a line) */
            advance(lexer);
            if (peek(lexer) == '\0') break;
            if (peek(lexer) == '\n') lexer->line++;
            advance(lexer);
            continue;
        }
        if (peek(lexer) == '$' && lexer->current[1] == '{') has_interp = 1;
        advance(lexer);
    }
    if (peek(lexer) == '\0') {
        return error_token(lexer, "Unterminated string");
    }
    int length = (int)(lexer->current - start);
    advance(lexer); /* closing quote */
    return make_token(lexer, has_interp ? TOK_INTERP_STRING : TOK_STRING, start, length);
}

/* 'c' -- the token text is the raw text between the quotes (escapes and
 * multi-byte characters are decoded by the parser) */
static Token lex_char(Lexer *lexer) {
    const char *start = lexer->current; /* after opening quote */
    while (peek(lexer) != '\'') {
        if (peek(lexer) == '\0' || peek(lexer) == '\n') {
            return error_token(lexer, "Unterminated character literal");
        }
        if (peek(lexer) == '\\') {
            advance(lexer);
            if (peek(lexer) == '\0' || peek(lexer) == '\n') {
                return error_token(lexer, "Unterminated character literal");
            }
        }
        advance(lexer);
    }
    int length = (int)(lexer->current - start);
    advance(lexer); /* closing quote */
    return make_token(lexer, TOK_CHAR, start, length);
}

static Token lex_number(Lexer *lexer, const char *start) {
    /* 0xFF / 0b1010 */
    if (start[0] == '0' && (peek(lexer) == 'x' || peek(lexer) == 'X')) {
        advance(lexer);
        if (!isxdigit((unsigned char)peek(lexer))) {
            return error_token(lexer, "Invalid hexadecimal literal");
        }
        while (isxdigit((unsigned char)peek(lexer))) advance(lexer);
        return make_token(lexer, TOK_INT, start, (int)(lexer->current - start));
    }
    if (start[0] == '0' && (peek(lexer) == 'b' || peek(lexer) == 'B')) {
        advance(lexer);
        if (!isdigit((unsigned char)peek(lexer))) {
            return error_token(lexer, "Invalid binary literal");
        }
        int bad = 0;
        while (isdigit((unsigned char)peek(lexer))) {
            if (peek(lexer) > '1') bad = 1;
            advance(lexer);
        }
        if (bad) return error_token(lexer, "Invalid binary literal");
        return make_token(lexer, TOK_INT, start, (int)(lexer->current - start));
    }
    while (isdigit((unsigned char)peek(lexer))) {
        advance(lexer);
    }
    int is_float = 0;
    /* Check for decimal point followed by digit */
    if (peek(lexer) == '.' && isdigit((unsigned char)lexer->current[1])) {
        advance(lexer); /* consume '.' */
        while (isdigit((unsigned char)peek(lexer))) {
            advance(lexer);
        }
        is_float = 1;
    }
    /* Check for scientific notation: e/E optionally followed by +/- and digits */
    if ((peek(lexer) == 'e' || peek(lexer) == 'E') &&
        (isdigit((unsigned char)lexer->current[1]) ||
         ((lexer->current[1] == '+' || lexer->current[1] == '-') &&
          isdigit((unsigned char)lexer->current[2])))) {
        advance(lexer); /* consume 'e'/'E' */
        if (peek(lexer) == '+' || peek(lexer) == '-') advance(lexer);
        while (isdigit((unsigned char)peek(lexer))) {
            advance(lexer);
        }
        is_float = 1;
    }
    return make_token(lexer, is_float ? TOK_FLOAT : TOK_INT, start,
                      (int)(lexer->current - start));
}

static TokenType check_keyword(const char *start, int length) {
    if (length == 4 && memcmp(start, "self", 4) == 0) return TOK_SELF;
    if (length == 5 && memcmp(start, "super", 5) == 0) return TOK_SUPER;
    if (length == 6 && memcmp(start, "import", 6) == 0) return TOK_IMPORT;
    if (length == 3 && memcmp(start, "try", 3) == 0) return TOK_TRY;
    if (length == 5 && memcmp(start, "catch", 5) == 0) return TOK_CATCH;
    if (length == 7 && memcmp(start, "finally", 7) == 0) return TOK_FINALLY;
    if (length == 5 && memcmp(start, "raise", 5) == 0) return TOK_RAISE;
    if (length == 6 && memcmp(start, "return", 6) == 0) return TOK_RETURN;
    if (length == 5 && memcmp(start, "break", 5) == 0) return TOK_BREAK;
    if (length == 8 && memcmp(start, "continue", 8) == 0) return TOK_CONTINUE;
    if (length == 2 && memcmp(start, "as", 2) == 0) return TOK_AS;
    return TOK_IDENT;
}

static Token lex_identifier(Lexer *lexer, const char *start) {
    while (isalnum((unsigned char)peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }
    int length = (int)(lexer->current - start);
    TokenType type = check_keyword(start, length);
    return make_token(lexer, type, start, length);
}

Token lexer_next(Lexer *lexer) {
    skip_whitespace(lexer);

    if (peek(lexer) == '\0') {
        return make_token(lexer, TOK_EOF, lexer->current, 0);
    }

    const char *start = lexer->current;
    char c = advance(lexer);

    /* Newline */
    if (c == '\n') {
        lexer->line++;
        return make_token(lexer, TOK_NEWLINE, start, 1);
    }

    /* String literal */
    if (c == '"') {
        if (lexer->current[0] == '"' && lexer->current[1] == '"') {
            advance(lexer); /* skip second " */
            advance(lexer); /* skip third " */
            return lex_triple_string(lexer);
        }
        return lex_string(lexer);
    }

    /* Character literal */
    if (c == '\'') {
        return lex_char(lexer);
    }

    /* Number */
    if (isdigit((unsigned char)c)) {
        return lex_number(lexer, start);
    }

    /* Identifier or keyword */
    if (isalpha((unsigned char)c) || c == '_') {
        return lex_identifier(lexer, start);
    }

    /* Operators and punctuation */
    switch (c) {
        case '(': return make_token(lexer, TOK_LPAREN, start, 1);
        case ')': return make_token(lexer, TOK_RPAREN, start, 1);
        case '{': return make_token(lexer, TOK_LBRACE, start, 1);
        case '}': return make_token(lexer, TOK_RBRACE, start, 1);
        case '#':
            if (match(lexer, '{')) return make_token(lexer, TOK_HASH_LBRACE, start, 2);
            return error_token(lexer, "Unexpected character '#' (did you mean '#{' for a dict literal?)");
        case ':': return make_token(lexer, TOK_COLON, start, 1);
        case '[': return make_token(lexer, TOK_LBRACKET, start, 1);
        case ']': return make_token(lexer, TOK_RBRACKET, start, 1);
        case ',': return make_token(lexer, TOK_COMMA, start, 1);
        case '.':
            if (match(lexer, '.')) return make_token(lexer, TOK_RANGE, start, 2);
            return make_token(lexer, TOK_DOT, start, 1);
        case '|':
            if (match(lexer, '|')) return make_token(lexer, TOK_OR, start, 2);
            if (match(lexer, '=')) return make_token(lexer, TOK_PIPE_ASSIGN, start, 2);
            return make_token(lexer, TOK_PIPE, start, 1);
        case ';': return make_token(lexer, TOK_SEMICOLON, start, 1);
        case '+':
            if (match(lexer, '=')) return make_token(lexer, TOK_PLUS_ASSIGN, start, 2);
            return make_token(lexer, TOK_PLUS, start, 1);
        case '-':
            if (match(lexer, '=')) return make_token(lexer, TOK_MINUS_ASSIGN, start, 2);
            return make_token(lexer, TOK_MINUS, start, 1);
        case '*':
            if (match(lexer, '=')) return make_token(lexer, TOK_STAR_ASSIGN, start, 2);
            return make_token(lexer, TOK_STAR, start, 1);
        case '/':
            if (match(lexer, '=')) return make_token(lexer, TOK_SLASH_ASSIGN, start, 2);
            return make_token(lexer, TOK_SLASH, start, 1);
        case '%':
            if (match(lexer, '=')) return make_token(lexer, TOK_PERCENT_ASSIGN, start, 2);
            return make_token(lexer, TOK_PERCENT, start, 1);
        case '=':
            if (match(lexer, '=')) return make_token(lexer, TOK_EQUAL, start, 2);
            return make_token(lexer, TOK_ASSIGN, start, 1);
        case '!':
            if (match(lexer, '=')) return make_token(lexer, TOK_NOT_EQUAL, start, 2);
            return make_token(lexer, TOK_BANG, start, 1);
        case '<':
            if (match(lexer, '<')) {
                if (match(lexer, '=')) return make_token(lexer, TOK_SHL_ASSIGN, start, 3);
                return make_token(lexer, TOK_SHL, start, 2);
            }
            if (match(lexer, '=')) return make_token(lexer, TOK_LESS_EQ, start, 2);
            return make_token(lexer, TOK_LESS, start, 1);
        case '>':
            if (match(lexer, '>')) {
                if (match(lexer, '=')) return make_token(lexer, TOK_SHR_ASSIGN, start, 3);
                return make_token(lexer, TOK_SHR, start, 2);
            }
            if (match(lexer, '=')) return make_token(lexer, TOK_GREATER_EQ, start, 2);
            return make_token(lexer, TOK_GREATER, start, 1);
        case '&':
            if (match(lexer, '&')) return make_token(lexer, TOK_AND, start, 2);
            if (match(lexer, '=')) return make_token(lexer, TOK_AMP_ASSIGN, start, 2);
            return make_token(lexer, TOK_AMP, start, 1);
        case '^':
            if (match(lexer, '=')) return make_token(lexer, TOK_CARET_ASSIGN, start, 2);
            return make_token(lexer, TOK_CARET, start, 1);
        case '~': return make_token(lexer, TOK_TILDE, start, 1);
        case '?':
            if (match(lexer, '?')) return make_token(lexer, TOK_NULLCOALESCE, start, 2);
            return error_token(lexer, "Unexpected character '?' (did you mean '?\?'?)");
        default:
            return error_token(lexer, "Unexpected character");
    }
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_INT:        return "INT";
        case TOK_FLOAT:      return "FLOAT";
        case TOK_STRING:     return "STRING";
        case TOK_CHAR:       return "CHAR";
        case TOK_INTERP_STRING: return "INTERP_STRING";
        case TOK_IDENT:      return "IDENT";
        case TOK_SELF:       return "SELF";
        case TOK_SUPER:      return "SUPER";
        case TOK_IMPORT:     return "IMPORT";
        case TOK_TRY:        return "TRY";
        case TOK_CATCH:      return "CATCH";
        case TOK_FINALLY:    return "FINALLY";
        case TOK_RAISE:      return "RAISE";
        case TOK_RETURN:     return "RETURN";
        case TOK_BREAK:      return "BREAK";
        case TOK_CONTINUE:   return "CONTINUE";
        case TOK_AS:         return "AS";
        case TOK_LPAREN:     return "(";
        case TOK_RPAREN:     return ")";
        case TOK_LBRACE:     return "{";
        case TOK_RBRACE:     return "}";
        case TOK_LBRACKET:   return "[";
        case TOK_RBRACKET:   return "]";
        case TOK_COMMA:      return ",";
        case TOK_DOT:        return ".";
        case TOK_RANGE:      return "..";
        case TOK_PIPE:       return "|";
        case TOK_HASH_LBRACE: return "#{";
        case TOK_COLON:      return ":";
        case TOK_NEWLINE:    return "NEWLINE";
        case TOK_SEMICOLON:  return ";";
        case TOK_ASSIGN:     return "=";
        case TOK_PLUS:       return "+";
        case TOK_MINUS:      return "-";
        case TOK_STAR:       return "*";
        case TOK_SLASH:      return "/";
        case TOK_PERCENT:    return "%";
        case TOK_EQUAL:      return "==";
        case TOK_NOT_EQUAL:  return "!=";
        case TOK_LESS:       return "<";
        case TOK_GREATER:    return ">";
        case TOK_LESS_EQ:    return "<=";
        case TOK_GREATER_EQ: return ">=";
        case TOK_BANG:       return "!";
        case TOK_AND:        return "&&";
        case TOK_OR:         return "||";
        case TOK_NULLCOALESCE: return "??";
        case TOK_AMP:        return "&";
        case TOK_CARET:      return "^";
        case TOK_TILDE:      return "~";
        case TOK_SHL:        return "<<";
        case TOK_SHR:        return ">>";
        case TOK_PLUS_ASSIGN:    return "+=";
        case TOK_MINUS_ASSIGN:   return "-=";
        case TOK_STAR_ASSIGN:    return "*=";
        case TOK_SLASH_ASSIGN:   return "/=";
        case TOK_PERCENT_ASSIGN: return "%=";
        case TOK_AMP_ASSIGN:   return "&=";
        case TOK_PIPE_ASSIGN:  return "|=";
        case TOK_CARET_ASSIGN: return "^=";
        case TOK_SHL_ASSIGN:   return "<<=";
        case TOK_SHR_ASSIGN:   return ">>=";
        case TOK_EOF:        return "EOF";
        case TOK_ERROR:      return "ERROR";
        default:             return "???";
    }
}
