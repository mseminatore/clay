/*
 * test_vm.c
 * Generated smoke tests for VM 'clayvm' v1.0
 * DO NOT EDIT — regenerate from spec instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm.h"
#include "opcodes.h"
#include "assembler.h"
#include "disasm.h"
#include "bytecode.h"
#include "strings.h"

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        tests_failed++; \
    } \
} while(0)

#define TEST_ASSERT_INT(actual, expected, msg) do { \
    tests_run++; \
    if ((actual) != (expected)) { \
        fprintf(stderr, "FAIL: %s — expected %lld, got %lld (%s:%d)\n", \
            msg, (long long)(expected), (long long)(actual), __FILE__, __LINE__); \
        tests_failed++; \
    } \
} while(0)

static void test_vm_init(void) {
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    TEST_ASSERT(vm.sp == 0, "vm init: sp should be 0");
    TEST_ASSERT(vm.ip == 0, "vm init: ip should be 0");
    clayvm_vm_free(&vm);
}

static void test_push_pop(void) {
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    clayvm_vm_push(&vm, CLAYVM_INT64(42));
    TEST_ASSERT(vm.sp == 1, "push: sp should be 1");
    clayvm_value_t val = clayvm_vm_pop(&vm);
    TEST_ASSERT(vm.sp == 0, "pop: sp should be 0");
    TEST_ASSERT_INT(val.as.i64, 42, "pop: value should be 42");
    clayvm_vm_free(&vm);
}

static void test_constants(void) {
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    int idx = clayvm_vm_add_constant(&vm, CLAYVM_INT64(100));
    TEST_ASSERT(idx == 0, "first constant index should be 0");
    int idx2 = clayvm_vm_add_constant(&vm, CLAYVM_INT64(200));
    TEST_ASSERT(idx2 == 1, "second constant index should be 1");
    TEST_ASSERT_INT(vm.constants.values[0].as.i64, 100, "constant 0 should be 100");
    TEST_ASSERT_INT(vm.constants.values[1].as.i64, 200, "constant 1 should be 200");
}

static void test_halt(void) {
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    uint8_t code[] = { CLAYVM_OP_HALT };
    int rc = clayvm_vm_run(&vm, code, sizeof(code));
    TEST_ASSERT(rc == 0, "HALT should return 0");
}

static void test_push_const(void) {
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    clayvm_vm_add_constant(&vm, CLAYVM_INT64(99));
    uint8_t code[] = {
        CLAYVM_OP_PUSH_CONST, 0, 0,  /* push constant 0 */
        CLAYVM_OP_HALT
    };
    int rc = clayvm_vm_run(&vm, code, sizeof(code));
    TEST_ASSERT(rc == 0, "push_const+halt should return 0");
    TEST_ASSERT(vm.sp == 1, "sp should be 1 after push_const");
    TEST_ASSERT_INT(vm.stack[0].as.i64, 99, "stack[0] should be 99");
}

static void test_add(void) {
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    clayvm_vm_add_constant(&vm, CLAYVM_INT64(10));
    clayvm_vm_add_constant(&vm, CLAYVM_INT64(20));
    uint8_t code[] = {
        CLAYVM_OP_PUSH_CONST, 0, 0,
        CLAYVM_OP_PUSH_CONST, 1, 0,
        CLAYVM_OP_ADD,
        CLAYVM_OP_HALT
    };
    int rc = clayvm_vm_run(&vm, code, sizeof(code));
    TEST_ASSERT(rc == 0, "add should return 0");
    TEST_ASSERT(vm.sp == 1, "add: sp should be 1");
    TEST_ASSERT_INT(vm.stack[0].as.i64, 30, "10 + 20 should be 30");
}

static void test_sub(void) {
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    clayvm_vm_add_constant(&vm, CLAYVM_INT64(50));
    clayvm_vm_add_constant(&vm, CLAYVM_INT64(30));
    uint8_t code[] = {
        CLAYVM_OP_PUSH_CONST, 0, 0,
        CLAYVM_OP_PUSH_CONST, 1, 0,
        CLAYVM_OP_SUB,
        CLAYVM_OP_HALT
    };
    int rc = clayvm_vm_run(&vm, code, sizeof(code));
    TEST_ASSERT(rc == 0, "sub should return 0");
    TEST_ASSERT_INT(vm.stack[0].as.i64, 20, "50 - 30 should be 20");
}

static void test_assembler(void) {
    clayvm_assembler_t as;
    clayvm_asm_init(&as);
    int rc = clayvm_asm_assemble(&as, "PUSH_CONST 0\nHALT\n");
    TEST_ASSERT(rc == 0, "assembler should succeed");
    TEST_ASSERT(as.len > 0, "assembler should produce bytecode");
    TEST_ASSERT(as.code[0] == CLAYVM_OP_PUSH_CONST, "first opcode should be PUSH_CONST");
}

static void test_disassembler(void) {
    uint8_t code[] = { CLAYVM_OP_HALT };
    char out[256];
    int len = clayvm_disasm_buf(code, sizeof(code), out, sizeof(out));
    TEST_ASSERT(len > 0, "disasm_buf should produce output");
    TEST_ASSERT(strstr(out, "HALT") != NULL, "disasm should contain HALT");
}

static void test_string_intern(void) {
    clayvm_string_table_t table;
    clayvm_string_table_init(&table);
    const clayvm_string_t *s1 = clayvm_string_intern(&table, "hello", 5);
    const clayvm_string_t *s2 = clayvm_string_intern(&table, "hello", 5);
    TEST_ASSERT(s1 == s2, "interning same string should return same pointer");
    const clayvm_string_t *s3 = clayvm_string_intern(&table, "world", 5);
    TEST_ASSERT(s1 != s3, "different strings should have different pointers");
    clayvm_string_table_free(&table);
}

static void test_bytecode_roundtrip(void) {
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    clayvm_vm_add_constant(&vm, CLAYVM_INT64(42));
    uint8_t code[] = {
        CLAYVM_OP_PUSH_CONST, 0, 0,
        CLAYVM_OP_HALT
    };
    int rc = clayvm_bc_save("_test_roundtrip.bc", &vm, code, sizeof(code));
    TEST_ASSERT(rc == 0, "bc_save should succeed");
    clayvm_vm_t vm2;
    clayvm_vm_init(&vm2);
    uint8_t *loaded = NULL;
    size_t loaded_len = 0;
    rc = clayvm_bc_load("_test_roundtrip.bc", &vm2, &loaded, &loaded_len);
    TEST_ASSERT(rc == 0, "bc_load should succeed");
    TEST_ASSERT(loaded_len == sizeof(code), "loaded code length should match");
    TEST_ASSERT(loaded[0] == CLAYVM_OP_PUSH_CONST, "loaded first opcode should match");
    clayvm_bc_free_code(loaded);
    remove("_test_roundtrip.bc");
}

int main(void) {
    printf("Running clayvm smoke tests...\n");

    test_vm_init();
    test_push_pop();
    test_constants();
    test_halt();
    test_push_const();
    test_add();
    test_sub();
    test_assembler();
    test_disassembler();
    test_string_intern();
    test_bytecode_roundtrip();

    printf("%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed;
}
