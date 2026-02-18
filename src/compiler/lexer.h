#ifndef CLAY_LEXER_H
#define CLAY_LEXER_H

typedef enum {
    /* Literals */
    TOK_INT,            /* 42 */
    TOK_STRING,         /* "hello" */
    TOK_IDENT,          /* foo, Bar, clone */

    /* Keywords */
    TOK_SELF,           /* self */
    TOK_IMPORT,         /* import */

    /* Punctuation */
    TOK_LPAREN,         /* ( */
    TOK_RPAREN,         /* ) */
    TOK_LBRACE,         /* { */
    TOK_RBRACE,         /* } */
    TOK_LBRACKET,       /* [ */
    TOK_RBRACKET,       /* ] */
    TOK_COMMA,          /* , */
    TOK_DOT,            /* . */
    TOK_PIPE,           /* | */
    TOK_NEWLINE,        /* newline (statement separator) */
    TOK_SEMICOLON,      /* ; (optional statement separator) */

    /* Operators */
    TOK_ASSIGN,         /* = */
    TOK_PLUS,           /* + */
    TOK_MINUS,          /* - */
    TOK_STAR,           /* * */
    TOK_SLASH,          /* / */
    TOK_PERCENT,        /* % */
    TOK_EQUAL,          /* == */
    TOK_NOT_EQUAL,      /* != */
    TOK_LESS,           /* < */
    TOK_GREATER,        /* > */
    TOK_LESS_EQ,        /* <= */
    TOK_GREATER_EQ,     /* >= */
    TOK_BANG,           /* ! */

    /* Special */
    TOK_EOF,
    TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char *start;   /* pointer into source */
    int length;
    int line;
} Token;

typedef struct {
    const char *source;
    const char *current;
    int line;
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next(Lexer *lexer);
const char *token_type_name(TokenType type);

#endif /* CLAY_LEXER_H */
