#ifndef CLAY_PARSER_H
#define CLAY_PARSER_H

#include "lexer.h"
#include "ast.h"

/* Track imported files to prevent double-import */
typedef struct ImportEntry {
    char *path;
    struct ImportEntry *next;
} ImportEntry;

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    int had_error;
    const char *filename;
    ImportEntry *imported;  /* linked list of already-imported paths */
} Parser;

void    parser_init(Parser *parser, const char *source, const char *filename);
AstNode *parser_parse(Parser *parser);
void    parser_free_imports(Parser *parser);

#endif /* CLAY_PARSER_H */
