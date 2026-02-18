#ifndef CLAY_PARSER_H
#define CLAY_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    int had_error;
    const char *filename;
} Parser;

void    parser_init(Parser *parser, const char *source, const char *filename);
AstNode *parser_parse(Parser *parser);

#endif /* CLAY_PARSER_H */
