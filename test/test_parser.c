/*
 * test_parser.c - Parser unit tests using testy
 */
#include "test.h"
#include "parser.h"
#include "ast.h"
#include <string.h>

/* Helper: parse source and return the AST program node */
static AstNode *parse(const char *source) {
    Parser parser;
    parser_init(&parser, source, "<test>");
    return parser_parse(&parser);
}

/* Helper: parse and return the first statement */
static AstNode *parse_stmt(const char *source) {
    AstNode *prog = parse(source);
    if (!prog || prog->as.program.stmts.count == 0) return NULL;
    return prog->as.program.stmts.items[0];
}

static void test_parser_literals(void) {
    SUITE("Parser Literals");

    COMMENT("integer literal...");
    {
        AstNode *s = parse_stmt("42");
        TEST(s != NULL);
        TEST(s->type == NODE_INT_LIT);
        TEST(s->as.int_lit.value == 42);
    }

    COMMENT("zero...");
    {
        AstNode *s = parse_stmt("0");
        TEST(s != NULL);
        TEST(s->type == NODE_INT_LIT);
        TEST(s->as.int_lit.value == 0);
    }

    COMMENT("float literal...");
    {
        AstNode *s = parse_stmt("3.14");
        TEST(s != NULL);
        TEST(s->type == NODE_FLOAT_LIT);
    }

    COMMENT("string literal...");
    {
        AstNode *s = parse_stmt("\"hello\"");
        TEST(s != NULL);
        TEST(s->type == NODE_STRING_LIT);
        TEST(strcmp(s->as.string_lit.value, "hello") == 0);
    }

    COMMENT("empty string...");
    {
        AstNode *s = parse_stmt("\"\"");
        TEST(s != NULL);
        TEST(s->type == NODE_STRING_LIT);
        TEST(strcmp(s->as.string_lit.value, "") == 0);
    }

    COMMENT("identifier...");
    {
        AstNode *s = parse_stmt("foo");
        TEST(s != NULL);
        TEST(s->type == NODE_IDENT);
        TEST(strcmp(s->as.ident.name, "foo") == 0);
    }

    COMMENT("self...");
    {
        AstNode *s = parse_stmt("self");
        TEST(s != NULL);
        TEST(s->type == NODE_SELF);
    }
}

static void test_parser_assignment(void) {
    SUITE("Parser Assignment");

    COMMENT("simple assignment...");
    {
        AstNode *s = parse_stmt("x = 42");
        TEST(s != NULL);
        TEST(s->type == NODE_ASSIGN);
        TEST(strcmp(s->as.assign.name, "x") == 0);
        TEST(s->as.assign.value->type == NODE_INT_LIT);
        TEST(s->as.assign.value->as.int_lit.value == 42);
    }

    COMMENT("assignment with expression...");
    {
        AstNode *s = parse_stmt("y = 3 + 4");
        TEST(s != NULL);
        TEST(s->type == NODE_ASSIGN);
        TEST(s->as.assign.value->type == NODE_BINARY);
    }
}

static void test_parser_binary(void) {
    SUITE("Parser Binary Expressions");

    COMMENT("addition...");
    {
        AstNode *s = parse_stmt("3 + 4");
        TEST(s != NULL);
        TEST(s->type == NODE_BINARY);
        TEST(strcmp(s->as.binary.op, "+") == 0);
        TEST(s->as.binary.left->type == NODE_INT_LIT);
        TEST(s->as.binary.left->as.int_lit.value == 3);
        TEST(s->as.binary.right->type == NODE_INT_LIT);
        TEST(s->as.binary.right->as.int_lit.value == 4);
    }

    COMMENT("subtraction...");
    {
        AstNode *s = parse_stmt("10 - 5");
        TEST(s != NULL);
        TEST(s->type == NODE_BINARY);
        TEST(strcmp(s->as.binary.op, "-") == 0);
    }

    COMMENT("multiplication...");
    {
        AstNode *s = parse_stmt("6 * 7");
        TEST(s != NULL);
        TEST(s->type == NODE_BINARY);
        TEST(strcmp(s->as.binary.op, "*") == 0);
    }

    COMMENT("comparison operators...");
    {
        AstNode *s = parse_stmt("a == b");
        TEST(s != NULL);
        TEST(s->type == NODE_BINARY);
        TEST(strcmp(s->as.binary.op, "==") == 0);
    }
    {
        AstNode *s = parse_stmt("a != b");
        TEST(s != NULL);
        TEST(s->type == NODE_BINARY);
        TEST(strcmp(s->as.binary.op, "!=") == 0);
    }
    {
        AstNode *s = parse_stmt("a < b");
        TEST(s != NULL);
        TEST(s->type == NODE_BINARY);
        TEST(strcmp(s->as.binary.op, "<") == 0);
    }
}

static void test_parser_send(void) {
    SUITE("Parser Message Send");

    COMMENT("no-arg send...");
    {
        AstNode *s = parse_stmt("obj foo");
        TEST(s != NULL);
        TEST(s->type == NODE_SEND);
        TEST(strcmp(s->as.send.message, "foo") == 0);
        TEST(s->as.send.args.count == 0);
    }

    COMMENT("send with args...");
    {
        AstNode *s = parse_stmt("obj foo(1, 2)");
        TEST(s != NULL);
        TEST(s->type == NODE_SEND);
        TEST(strcmp(s->as.send.message, "foo") == 0);
        TEST(s->as.send.args.count == 2);
    }
}

static void test_parser_blocks(void) {
    SUITE("Parser Blocks");

    COMMENT("empty block...");
    {
        AstNode *s = parse_stmt("{ }");
        TEST(s != NULL);
        TEST(s->type == NODE_BLOCK);
        TEST(s->as.block.params.count == 0);
    }

    COMMENT("block with params...");
    {
        AstNode *s = parse_stmt("{ |x, y| x + y }");
        TEST(s != NULL);
        TEST(s->type == NODE_BLOCK);
        TEST(s->as.block.params.count == 2);
    }

    COMMENT("block with body...");
    {
        AstNode *s = parse_stmt("{ 42 }");
        TEST(s != NULL);
        TEST(s->type == NODE_BLOCK);
        TEST(s->as.block.body.count == 1);
    }
}

static void test_parser_clone(void) {
    SUITE("Parser Clone");

    COMMENT("clone with slot...");
    {
        AstNode *s = parse_stmt("Object clone({ x = 10 })");
        TEST(s != NULL);
        TEST(s->type == NODE_CLONE);
        TEST(s->as.clone.members.count >= 1);
        TEST(s->as.clone.members.items[0]->type == NODE_SLOT_DEF);
    }

    COMMENT("clone with method...");
    {
        AstNode *s = parse_stmt("Object clone({ foo() { 42 } })");
        TEST(s != NULL);
        TEST(s->type == NODE_CLONE);
        TEST(s->as.clone.members.count >= 1);
        TEST(s->as.clone.members.items[0]->type == NODE_METHOD_DEF);
    }
}

static void test_parser_arrays(void) {
    SUITE("Parser Arrays");

    COMMENT("empty array...");
    {
        AstNode *s = parse_stmt("[]");
        TEST(s != NULL);
        TEST(s->type == NODE_ARRAY_LIT);
        TEST(s->as.array_lit.elements.count == 0);
    }

    COMMENT("array with elements...");
    {
        AstNode *s = parse_stmt("[1, 2, 3]");
        TEST(s != NULL);
        TEST(s->type == NODE_ARRAY_LIT);
        TEST(s->as.array_lit.elements.count == 3);
    }
}

static void test_parser_error_handling(void) {
    SUITE("Parser Try/Catch/Raise");

    COMMENT("raise expression...");
    {
        AstNode *s = parse_stmt("raise(42)");
        TEST(s != NULL);
        TEST(s->type == NODE_RAISE);
    }
}

static void test_parser_multiple_stmts(void) {
    SUITE("Parser Multiple Statements");

    COMMENT("two statements...");
    {
        AstNode *prog = parse("x = 1\ny = 2");
        TEST(prog != NULL);
        TEST(prog->type == NODE_PROGRAM);
        TEST(prog->as.program.stmts.count == 2);
        TEST(prog->as.program.stmts.items[0]->type == NODE_ASSIGN);
        TEST(prog->as.program.stmts.items[1]->type == NODE_ASSIGN);
    }

    COMMENT("empty program...");
    {
        AstNode *prog = parse("");
        TEST(prog != NULL);
        TEST(prog->type == NODE_PROGRAM);
        TEST(prog->as.program.stmts.count == 0);
    }

    COMMENT("semicolon separator...");
    {
        AstNode *prog = parse("x = 1; y = 2");
        TEST(prog != NULL);
        TEST(prog->as.program.stmts.count == 2);
    }
}

static void test_parser_errors(void) {
    SUITE("Parser Error Recovery");

    COMMENT("parse error sets had_error...");
    {
        Parser parser;
        parser_init(&parser, "= = =", "<test>");
        AstNode *prog = parser_parse(&parser);
        TEST(parser.had_error == 1);
        if (prog) ast_free(prog);
    }
}

static void test_parser_slot_assign(void) {
    SUITE("Parser Slot Assignment");

    COMMENT("self slot assign...");
    {
        AstNode *s = parse_stmt("self x = 10");
        TEST(s != NULL);
        TEST(s->type == NODE_SLOT_ASSIGN);
        TEST(strcmp(s->as.slot_assign.slot_name, "x") == 0);
        TEST(s->as.slot_assign.receiver->type == NODE_SELF);
    }
}

void test_parser(void) {
    MODULE("Parser");

    test_parser_literals();
    test_parser_assignment();
    test_parser_binary();
    test_parser_send();
    test_parser_blocks();
    test_parser_clone();
    test_parser_arrays();
    test_parser_error_handling();
    test_parser_multiple_stmts();
    test_parser_errors();
    test_parser_slot_assign();
}
