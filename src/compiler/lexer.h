#ifndef CLAY_LEXER_H
#define CLAY_LEXER_H

typedef enum {
    /* Literals */
    TOK_INT,            /* 42 */
    TOK_FLOAT,          /* 3.14 */
    TOK_STRING,         /* "hello" */
    TOK_INTERP_STRING,  /* "hello ${name}" - contains interpolation */
    TOK_IDENT,          /* foo, Bar, clone */

    /* Keywords */
    TOK_SELF,           /* self */
    TOK_SUPER,          /* super */
    TOK_IMPORT,         /* import */
    TOK_TRY,            /* try */
    TOK_CATCH,          /* catch */
    TOK_FINALLY,        /* finally */
    TOK_RAISE,          /* raise */
    TOK_RETURN,         /* return */
    TOK_BREAK,          /* break */
    TOK_CONTINUE,       /* continue */

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
    TOK_HASH_LBRACE,    /* #{ (dict literal) */
    TOK_COLON,          /* : */
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
    TOK_AND,            /* && */
    TOK_OR,             /* || */
    TOK_NULLCOALESCE,   /* ?? */
    TOK_RANGE,          /* .. */

    /* Compound assignment */
    TOK_PLUS_ASSIGN,    /* += */
    TOK_MINUS_ASSIGN,   /* -= */
    TOK_STAR_ASSIGN,    /* *= */
    TOK_SLASH_ASSIGN,   /* /= */
    TOK_PERCENT_ASSIGN, /* %= */

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
