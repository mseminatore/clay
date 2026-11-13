/*
 * test_lexer.c - Lexer unit tests using testy
 */
#include "test.h"
#include "lexer.h"
#include <string.h>

/* Helper: lex a single token from source */
static Token lex_one(const char *source) {
    Lexer lexer;
    lexer_init(&lexer, source);
    return lexer_next(&lexer);
}

/* Helper: collect all non-EOF tokens, return count */
static int lex_all(const char *source, Token *out, int max) {
    Lexer lexer;
    lexer_init(&lexer, source);
    int count = 0;
    while (count < max) {
        Token t = lexer_next(&lexer);
        if (t.type == TOK_EOF) break;
        out[count++] = t;
    }
    return count;
}

static void test_lexer_literals(void) {
    SUITE("Lexer Literals");

    COMMENT("integer literals...");
    {
        Token t = lex_one("42");
        TEST(t.type == TOK_INT);
        TEST(t.length == 2);
        TEST(strncmp(t.start, "42", 2) == 0);
    }
    {
        Token t = lex_one("0");
        TEST(t.type == TOK_INT);
    }
    {
        Token t = lex_one("999999");
        TEST(t.type == TOK_INT);
        TEST(t.length == 6);
    }

    COMMENT("float literals...");
    {
        Token t = lex_one("3.14");
        TEST(t.type == TOK_FLOAT);
        TEST(t.length == 4);
    }
    {
        Token t = lex_one("0.5");
        TEST(t.type == TOK_FLOAT);
    }

    COMMENT("string literals...");
    {
        Token t = lex_one("\"hello\"");
        TEST(t.type == TOK_STRING);
    }
    {
        Token t = lex_one("\"\"");
        TEST(t.type == TOK_STRING);
    }

    COMMENT("identifiers...");
    {
        Token t = lex_one("foo");
        TEST(t.type == TOK_IDENT);
        TEST(t.length == 3);
        TEST(strncmp(t.start, "foo", 3) == 0);
    }
    {
        Token t = lex_one("Object");
        TEST(t.type == TOK_IDENT);
    }
    {
        Token t = lex_one("my_var123");
        TEST(t.type == TOK_IDENT);
    }
}

static void test_lexer_keywords(void) {
    SUITE("Lexer Keywords");

    TEST(lex_one("self").type == TOK_SELF);
    TEST(lex_one("import").type == TOK_IMPORT);
    TEST(lex_one("try").type == TOK_TRY);
    TEST(lex_one("catch").type == TOK_CATCH);
    TEST(lex_one("raise").type == TOK_RAISE);

    COMMENT("keywords vs identifiers...");
    TEST(lex_one("self_ref").type == TOK_IDENT);
    TEST(lex_one("import_file").type == TOK_IDENT);
    TEST(lex_one("trying").type == TOK_IDENT);
}

static void test_lexer_operators(void) {
    SUITE("Lexer Operators");

    COMMENT("single-char operators...");
    TEST(lex_one("+").type == TOK_PLUS);
    TEST(lex_one("-").type == TOK_MINUS);
    TEST(lex_one("*").type == TOK_STAR);
    TEST(lex_one("/").type == TOK_SLASH);
    /* (testy's TEST() puts the expression text in a printf format string, so
     * keep a literal percent sign out of it) */
    const char *percent = "%", *percent_assign = "%=";
    TEST(lex_one(percent).type == TOK_PERCENT);
    TEST(lex_one("=").type == TOK_ASSIGN);
    TEST(lex_one("<").type == TOK_LESS);
    TEST(lex_one(">").type == TOK_GREATER);
    TEST(lex_one("!").type == TOK_BANG);

    COMMENT("two-char operators...");
    TEST(lex_one("==").type == TOK_EQUAL);
    TEST(lex_one("!=").type == TOK_NOT_EQUAL);
    TEST(lex_one("<=").type == TOK_LESS_EQ);
    TEST(lex_one(">=").type == TOK_GREATER_EQ);
    TEST(lex_one("&&").type == TOK_AND);
    TEST(lex_one("||").type == TOK_OR);

    COMMENT("compound assignment...");
    TEST(lex_one("+=").type == TOK_PLUS_ASSIGN);
    TEST(lex_one("-=").type == TOK_MINUS_ASSIGN);
    TEST(lex_one("*=").type == TOK_STAR_ASSIGN);
    TEST(lex_one("/=").type == TOK_SLASH_ASSIGN);
    TEST(lex_one(percent_assign).type == TOK_PERCENT_ASSIGN);
}

static void test_lexer_punctuation(void) {
    SUITE("Lexer Punctuation");

    TEST(lex_one("(").type == TOK_LPAREN);
    TEST(lex_one(")").type == TOK_RPAREN);
    TEST(lex_one("{").type == TOK_LBRACE);
    TEST(lex_one("}").type == TOK_RBRACE);
    TEST(lex_one("[").type == TOK_LBRACKET);
    TEST(lex_one("]").type == TOK_RBRACKET);
    TEST(lex_one(",").type == TOK_COMMA);
    TEST(lex_one(".").type == TOK_DOT);
    TEST(lex_one("|").type == TOK_PIPE);
    TEST(lex_one(";").type == TOK_SEMICOLON);
}

static void test_lexer_sequences(void) {
    SUITE("Lexer Token Sequences");

    COMMENT("simple expression...");
    {
        Token tokens[10];
        int n = lex_all("x = 42", tokens, 10);
        TEST(n == 3);
        TEST(tokens[0].type == TOK_IDENT);
        TEST(tokens[1].type == TOK_ASSIGN);
        TEST(tokens[2].type == TOK_INT);
    }

    COMMENT("binary expression...");
    {
        Token tokens[10];
        int n = lex_all("3 + 4", tokens, 10);
        TEST(n == 3);
        TEST(tokens[0].type == TOK_INT);
        TEST(tokens[1].type == TOK_PLUS);
        TEST(tokens[2].type == TOK_INT);
    }

    COMMENT("message send...");
    {
        Token tokens[20];
        int n = lex_all("obj foo(1, 2)", tokens, 20);
        TEST(n >= 6);
        TEST(tokens[0].type == TOK_IDENT);
        TEST(tokens[1].type == TOK_IDENT);
        TEST(tokens[2].type == TOK_LPAREN);
        TEST(tokens[3].type == TOK_INT);
        TEST(tokens[4].type == TOK_COMMA);
        TEST(tokens[5].type == TOK_INT);
    }

    COMMENT("array literal...");
    {
        Token tokens[10];
        int n = lex_all("[1, 2, 3]", tokens, 10);
        TEST(n == 7);
        TEST(tokens[0].type == TOK_LBRACKET);
        TEST(tokens[6].type == TOK_RBRACKET);
    }
}

static void test_lexer_comments_whitespace(void) {
    SUITE("Lexer Comments & Whitespace");

    COMMENT("line comments...");
    {
        Token tokens[10];
        int n = lex_all("x // this is a comment", tokens, 10);
        TEST(n == 1);
        TEST(tokens[0].type == TOK_IDENT);
    }

    COMMENT("whitespace handling...");
    {
        Token t = lex_one("   42   ");
        TEST(t.type == TOK_INT);
    }

    COMMENT("newlines as separators...");
    {
        Token tokens[10];
        int n = lex_all("x\ny", tokens, 10);
        TEST(n == 3);
        TEST(tokens[0].type == TOK_IDENT);
        TEST(tokens[1].type == TOK_NEWLINE);
        TEST(tokens[2].type == TOK_IDENT);
    }

    COMMENT("line tracking...");
    {
        Lexer lexer;
        lexer_init(&lexer, "a\nb\nc");
        Token t1 = lexer_next(&lexer);
        TEST(t1.line == 1);
        lexer_next(&lexer); /* newline */
        Token t2 = lexer_next(&lexer);
        TEST(t2.line == 2);
        lexer_next(&lexer); /* newline */
        Token t3 = lexer_next(&lexer);
        TEST(t3.line == 3);
    }
}

static void test_lexer_edge_cases(void) {
    SUITE("Lexer Edge Cases");

    COMMENT("empty input...");
    {
        Token t = lex_one("");
        TEST(t.type == TOK_EOF);
    }

    COMMENT("only whitespace...");
    {
        Token t = lex_one("   \t  ");
        TEST(t.type == TOK_EOF);
    }

    COMMENT("negative number (minus then int)...");
    {
        Token tokens[10];
        int n = lex_all("-5", tokens, 10);
        TEST(n == 2);
        TEST(tokens[0].type == TOK_MINUS);
        TEST(tokens[1].type == TOK_INT);
    }
}

static void test_lexer_bitwise_literals(void) {
    SUITE("Lexer Bitwise and Radix Literals");

    COMMENT("hex and binary literals are INT tokens...");
    {
        Token t = lex_one("0xFF");
        TEST(t.type == TOK_INT);
        TEST(t.length == 4);
        t = lex_one("0b1010");
        TEST(t.type == TOK_INT);
        TEST(t.length == 6);
        t = lex_one("0XaB");
        TEST(t.type == TOK_INT);
    }

    COMMENT("malformed radix literals are errors...");
    {
        TEST(lex_one("0x").type == TOK_ERROR);
        TEST(lex_one("0xg").type == TOK_ERROR);
        TEST(lex_one("0b").type == TOK_ERROR);
        TEST(lex_one("0b102").type == TOK_ERROR);
    }

    COMMENT("character literals keep the raw text between the quotes...");
    {
        Token t = lex_one("'a'");
        TEST(t.type == TOK_CHAR);
        TEST(t.length == 1 && t.start[0] == 'a');
        t = lex_one("'ab'");
        TEST(t.type == TOK_CHAR);
        TEST(t.length == 2);
        TEST(lex_one("'a").type == TOK_ERROR);
        TEST(lex_one("'").type == TOK_ERROR);
    }

    COMMENT("bitwise operators...");
    {
        TEST(lex_one("&").type == TOK_AMP);
        TEST(lex_one("^").type == TOK_CARET);
        TEST(lex_one("~").type == TOK_TILDE);
        TEST(lex_one("<<").type == TOK_SHL);
        TEST(lex_one(">>").type == TOK_SHR);
        TEST(lex_one("&=").type == TOK_AMP_ASSIGN);
        TEST(lex_one("|=").type == TOK_PIPE_ASSIGN);
        TEST(lex_one("^=").type == TOK_CARET_ASSIGN);
        TEST(lex_one("<<=").type == TOK_SHL_ASSIGN);
        TEST(lex_one(">>=").type == TOK_SHR_ASSIGN);
    }

    COMMENT("existing operators are unchanged...");
    {
        TEST(lex_one("&&").type == TOK_AND);
        TEST(lex_one("||").type == TOK_OR);
        TEST(lex_one("|").type == TOK_PIPE);
        TEST(lex_one("<=").type == TOK_LESS_EQ);
        TEST(lex_one(">=").type == TOK_GREATER_EQ);
        TEST(lex_one("<").type == TOK_LESS);
    }
}

void test_lexer(void) {
    MODULE("Lexer");

    test_lexer_literals();
    test_lexer_keywords();
    test_lexer_operators();
    test_lexer_punctuation();
    test_lexer_sequences();
    test_lexer_comments_whitespace();
    test_lexer_edge_cases();
    test_lexer_bitwise_literals();
}
