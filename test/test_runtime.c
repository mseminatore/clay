/*
 * test_runtime.c - Runtime unit tests using testy
 */
#include "test.h"
#include "clay.h"

static void test_tagged_values(void) {
    SUITE("Tagged Values");

    COMMENT("integer tagging...");
    TEST(clay_is_int(clay_int(0)));
    TEST(clay_is_int(clay_int(42)));
    TEST(clay_is_int(clay_int(-1)));
    TEST(clay_to_int(clay_int(0)) == 0);
    TEST(clay_to_int(clay_int(42)) == 42);
    TEST(clay_to_int(clay_int(-1)) == -1);
    TEST(clay_to_int(clay_int(1000000)) == 1000000);

    COMMENT("nil and booleans...");
    TEST(CLAY_IS_NIL(CLAY_NIL));
    TEST(!CLAY_IS_INT(CLAY_NIL));
    TEST(!CLAY_IS_NIL(CLAY_TRUE));
    TEST(!CLAY_IS_NIL(CLAY_FALSE));
    TEST(CLAY_IS_TRUE(CLAY_TRUE));
    TEST(CLAY_IS_FALSE(CLAY_FALSE));
}

static void test_object_model(void) {
    SUITE("Object Model");

    COMMENT("create and clone...");
    ClayObject *obj = clay_clone(clay_Object);
    TEST(obj != NULL);
    TEST(obj->parent == clay_Object);
    TEST(obj->refcount == 1);

    COMMENT("set and get slots...");
    clay_set_slot(obj, "x", clay_int(10));
    clay_set_slot(obj, "y", clay_int(20));
    TEST(clay_to_int(clay_get_slot(obj, "x")) == 10);
    TEST(clay_to_int(clay_get_slot(obj, "y")) == 20);
    TEST(CLAY_IS_NIL(clay_get_slot(obj, "z")));

    COMMENT("slot overwrite...");
    clay_set_slot(obj, "x", clay_int(99));
    TEST(clay_to_int(clay_get_slot(obj, "x")) == 99);

    COMMENT("clone inherits slots...");
    ClayObject *child = clay_clone(obj);
    TEST(clay_to_int(clay_get_slot(child, "x")) == 99);
    TEST(clay_to_int(clay_get_slot(child, "y")) == 20);

    COMMENT("child slot doesn't affect parent...");
    clay_set_slot(child, "x", clay_int(1));
    TEST(clay_to_int(clay_get_slot(child, "x")) == 1);
    TEST(clay_to_int(clay_get_slot(obj, "x")) == 99);

    clay_release(child);
    clay_release(obj);
}

static void test_message_dispatch(void) {
    SUITE("Message Dispatch");

    COMMENT("clone via message...");
    ClayObject *obj = clay_clone(clay_Object);
    ClayValue cloned = clay_send(obj, "clone", 0, NULL);
    TEST(!CLAY_IS_NIL(cloned));
    TEST(CLAY_OBJ(cloned)->parent == obj);

    clay_release(CLAY_OBJ(cloned));
    clay_release(obj);
}

static void test_numbers(void) {
    SUITE("Numbers");

    COMMENT("arithmetic via dispatch...");
    /* Create a temporary number wrapper for dispatch */
    ClayObject *num = clay_clone(clay_Number);
    clay_set_slot(num, "_value", clay_int(10));

    ClayValue arg = clay_int(3);
    ClayValue result;

    result = clay_send(num, "+", 1, &arg);
    TEST(clay_to_int(result) == 13);

    result = clay_send(num, "-", 1, &arg);
    TEST(clay_to_int(result) == 7);

    result = clay_send(num, "*", 1, &arg);
    TEST(clay_to_int(result) == 30);

    result = clay_send(num, "/", 1, &arg);
    TEST(clay_to_int(result) == 3);

    result = clay_send(num, "%", 1, &arg);
    TEST(clay_to_int(result) == 1);

    COMMENT("comparison...");
    result = clay_send(num, ">", 1, &arg);
    TEST(CLAY_IS_TRUE(result));

    result = clay_send(num, "<", 1, &arg);
    TEST(CLAY_IS_FALSE(result));

    arg = clay_int(10);
    result = clay_send(num, "==", 1, &arg);
    TEST(CLAY_IS_TRUE(result));

    COMMENT("toString...");
    result = clay_send(num, "toString", 0, NULL);
    TEST(!CLAY_IS_NIL(result));
    TEST(strcmp(clay_string_data(CLAY_OBJ(result)), "10") == 0);
    clay_release(CLAY_OBJ(result));

    clay_release(num);
}

static void test_strings(void) {
    SUITE("Strings");

    COMMENT("create and access...");
    ClayObject *s = clay_string_new("hello");
    TEST(s != NULL);
    TEST(strcmp(clay_string_data(s), "hello") == 0);

    ClayValue len = clay_send(s, "length", 0, NULL);
    TEST(clay_to_int(len) == 5);

    COMMENT("concatenation...");
    ClayObject *s2 = clay_string_new(" world");
    ClayValue arg = CLAY_VAL(s2);
    ClayValue concat = clay_send(s, "+", 1, &arg);
    TEST(strcmp(clay_string_data(CLAY_OBJ(concat)), "hello world") == 0);

    COMMENT("equality...");
    ClayObject *s3 = clay_string_new("hello");
    arg = CLAY_VAL(s3);
    ClayValue eq = clay_send(s, "==", 1, &arg);
    TEST(CLAY_IS_TRUE(eq));

    clay_release(CLAY_OBJ(concat));
    clay_release(s3);
    clay_release(s2);
    clay_release(s);
}

static void test_arrays(void) {
    SUITE("Arrays");

    COMMENT("create and append...");
    ClayObject *arr = clay_array_new();
    TEST(arr != NULL);

    clay_array_append(arr, clay_int(10));
    clay_array_append(arr, clay_int(20));
    clay_array_append(arr, clay_int(30));

    ClayValue size = clay_send(arr, "size", 0, NULL);
    TEST(clay_to_int(size) == 3);

    COMMENT("access by index...");
    ClayValue idx = clay_int(0);
    ClayValue item = clay_send(arr, "at", 1, &idx);
    TEST(clay_to_int(item) == 10);

    idx = clay_int(2);
    item = clay_send(arr, "at", 1, &idx);
    TEST(clay_to_int(item) == 30);

    COMMENT("out of bounds...");
    idx = clay_int(99);
    item = clay_send(arr, "at", 1, &idx);
    TEST(CLAY_IS_NIL(item));

    clay_release(arr);
}

static ClayValue add_one_fn(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1) return CLAY_NIL;
    return clay_int(clay_to_int(argv[0]) + 1);
}

static ClayValue return_yes(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int(1);
}

static ClayValue return_no(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int(0);
}

static void test_blocks(void) {
    SUITE("Blocks");

    COMMENT("create and call...");
    ClayObject *block = clay_block_new(add_one_fn, NULL, 0);
    TEST(block != NULL);
    TEST(block->type == CLAY_TYPE_BLOCK);

    ClayValue arg = clay_int(41);
    ClayValue result = clay_send(block, "value", 1, &arg);
    TEST(clay_to_int(result) == 42);

    clay_release(block);
}

static void test_boolean_control(void) {
    SUITE("Boolean Control Flow");

    COMMENT("ifTrue on true...");
    ClayObject *yes_block = clay_block_new(return_yes, NULL, 0);
    ClayObject *no_block = clay_block_new(return_no, NULL, 0);

    /* ifTrue on true evaluates block, returns cond result wrapping 1 */
    ClayValue arg = CLAY_VAL(yes_block);
    ClayValue result = clay_send(&clay_true_obj, "ifTrue", 1, &arg);
    TEST(CLAY_IS_PTR(result)); /* returns a condition result object */

    /* ifTrue on false returns cond result with _matched=false */
    arg = CLAY_VAL(yes_block);
    result = clay_send(&clay_false_obj, "ifTrue", 1, &arg);
    TEST(CLAY_IS_PTR(result));

    /* ifFalse on false evaluates block, returns cond result wrapping 0 */
    arg = CLAY_VAL(no_block);
    result = clay_send(&clay_false_obj, "ifFalse", 1, &arg);
    TEST(CLAY_IS_PTR(result));

    /* Test chaining: ifTrue on false -> ifFalse evaluates fallback */
    arg = CLAY_VAL(yes_block);
    result = clay_send(&clay_false_obj, "ifTrue", 1, &arg);
    arg = CLAY_VAL(no_block);
    result = clay_send_val(result, "ifFalse", 1, &arg);
    TEST(clay_to_int(result) == 0);

    clay_release(yes_block);
    clay_release(no_block);
}

static void test_prototype_chain(void) {
    SUITE("Prototype Chain");

    COMMENT("method inherited from grandparent...");
    ClayObject *a = clay_clone(clay_Object);
    ClayObject *b = clay_clone(a);
    ClayObject *c = clay_clone(b);

    /* All should inherit Object's toString */
    ClayValue sa = clay_send(a, "toString", 0, NULL);
    ClayValue sb = clay_send(b, "toString", 0, NULL);
    ClayValue sc = clay_send(c, "toString", 0, NULL);
    TEST(!CLAY_IS_NIL(sa));
    TEST(!CLAY_IS_NIL(sb));
    TEST(!CLAY_IS_NIL(sc));

    clay_release(CLAY_OBJ(sa));
    clay_release(CLAY_OBJ(sb));
    clay_release(CLAY_OBJ(sc));

    COMMENT("slot lookup walks parent chain...");
    clay_set_slot(a, "name", CLAY_VAL(clay_string_new("proto_a")));
    ClayValue name = clay_get_slot(c, "name");
    TEST(!CLAY_IS_NIL(name));
    TEST(strcmp(clay_string_data(CLAY_OBJ(name)), "proto_a") == 0);

    clay_release(c);
    clay_release(b);
    clay_release(a);
}

void test_main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    clay_bootstrap();

    MODULE("Clay Runtime");

    test_tagged_values();
    test_object_model();
    test_message_dispatch();
    test_numbers();
    test_strings();
    test_arrays();
    test_blocks();
    test_boolean_control();
    test_prototype_chain();

    clay_shutdown();
}
