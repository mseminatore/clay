/*
 * test_runtime.c - Runtime unit tests using testy
 */
#include "test.h"
#include "clay.h"
#include "parser.h"
#include "bc_compiler.h"
#include "vm.h"
#include "hash.h"

/* External test modules */
extern void test_lexer(void);
extern void test_parser(void);

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

    COMMENT("inequality...");
    ClayObject *s4 = clay_string_new("world");
    arg = CLAY_VAL(s4);
    ClayValue neq = clay_send(s, "!=", 1, &arg);
    TEST(CLAY_IS_TRUE(neq));
    arg = CLAY_VAL(s3);
    ClayValue neq2 = clay_send(s, "!=", 1, &arg);
    TEST(CLAY_IS_FALSE(neq2));
    clay_release(s4);

    COMMENT("at...");
    ClayValue idx = clay_int(0);
    ClayValue ch = clay_send(s, "at", 1, &idx);
    TEST(strcmp(clay_string_data(CLAY_OBJ(ch)), "h") == 0);
    clay_release(CLAY_OBJ(ch));
    idx = clay_int(4);
    ch = clay_send(s, "at", 1, &idx);
    TEST(strcmp(clay_string_data(CLAY_OBJ(ch)), "o") == 0);
    clay_release(CLAY_OBJ(ch));
    idx = clay_int(10);
    ch = clay_send(s, "at", 1, &idx);
    TEST(CLAY_IS_NIL(ch));

    COMMENT("contains...");
    ClayObject *sub = clay_string_new("ell");
    arg = CLAY_VAL(sub);
    TEST(CLAY_IS_TRUE(clay_send(s, "contains", 1, &arg)));
    clay_release(sub);
    sub = clay_string_new("xyz");
    arg = CLAY_VAL(sub);
    TEST(CLAY_IS_FALSE(clay_send(s, "contains", 1, &arg)));
    clay_release(sub);

    COMMENT("slice...");
    ClayValue slice_args[2] = { clay_int(1), clay_int(4) };
    ClayValue sliced = clay_send(s, "slice", 2, slice_args);
    TEST(strcmp(clay_string_data(CLAY_OBJ(sliced)), "ell") == 0);
    clay_release(CLAY_OBJ(sliced));

    COMMENT("split...");
    ClayObject *csv = clay_string_new("a,b,c");
    ClayObject *comma = clay_string_new(",");
    arg = CLAY_VAL(comma);
    ClayValue parts = clay_send(csv, "split", 1, &arg);
    TEST(CLAY_OBJ(parts)->as.array.size == 3);
    TEST(strcmp(clay_string_data(CLAY_OBJ(CLAY_OBJ(parts)->as.array.items[0])), "a") == 0);
    TEST(strcmp(clay_string_data(CLAY_OBJ(CLAY_OBJ(parts)->as.array.items[2])), "c") == 0);
    clay_release(CLAY_OBJ(parts));
    clay_release(comma);
    clay_release(csv);

    COMMENT("replace...");
    ClayObject *hw = clay_string_new("hello world");
    ClayObject *old_s = clay_string_new("world");
    ClayObject *new_s = clay_string_new("Clay");
    ClayValue repl_args[2] = { CLAY_VAL(old_s), CLAY_VAL(new_s) };
    ClayValue replaced = clay_send(hw, "replace", 2, repl_args);
    TEST(strcmp(clay_string_data(CLAY_OBJ(replaced)), "hello Clay") == 0);
    clay_release(CLAY_OBJ(replaced));
    clay_release(new_s);
    clay_release(old_s);
    clay_release(hw);

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

    COMMENT("push (alias for append)...");
    ClayObject *arr2 = clay_array_new();
    ClayValue push_arg = clay_int(42);
    clay_send(arr2, "push", 1, &push_arg);
    push_arg = clay_int(99);
    clay_send(arr2, "push", 1, &push_arg);
    TEST(clay_to_int(clay_send(arr2, "size", 0, NULL)) == 2);
    idx = clay_int(0);
    TEST(clay_to_int(clay_send(arr2, "at", 1, &idx)) == 42);
    idx = clay_int(1);
    TEST(clay_to_int(clay_send(arr2, "at", 1, &idx)) == 99);
    clay_release(arr2);

    COMMENT("array equality...");
    ClayObject *eq1 = clay_array_new();
    clay_array_append(eq1, clay_int(1));
    clay_array_append(eq1, clay_int(2));
    clay_array_append(eq1, clay_int(3));
    ClayObject *eq2 = clay_array_new();
    clay_array_append(eq2, clay_int(1));
    clay_array_append(eq2, clay_int(2));
    clay_array_append(eq2, clay_int(3));
    ClayObject *eq3 = clay_array_new();
    clay_array_append(eq3, clay_int(1));
    clay_array_append(eq3, clay_int(2));
    clay_array_append(eq3, clay_int(4));
    ClayValue eq_arg = CLAY_VAL(eq2);
    TEST(clay_send(eq1, "==", 1, &eq_arg) == CLAY_TRUE);
    eq_arg = CLAY_VAL(eq3);
    TEST(clay_send(eq1, "==", 1, &eq_arg) == CLAY_FALSE);
    eq_arg = CLAY_VAL(eq1);
    TEST(clay_send(eq1, "!=", 1, &eq_arg) == CLAY_FALSE);
    eq_arg = CLAY_VAL(eq3);
    TEST(clay_send(eq1, "!=", 1, &eq_arg) == CLAY_TRUE);
    /* Different sizes */
    ClayObject *eq4 = clay_array_new();
    clay_array_append(eq4, clay_int(1));
    eq_arg = CLAY_VAL(eq4);
    TEST(clay_send(eq1, "==", 1, &eq_arg) == CLAY_FALSE);

    COMMENT("array clone copies elements...");
    ClayObject *cloned = clay_clone(arr);
    TEST(clay_array_size(cloned) == 3);
    idx = clay_int(0);
    TEST(clay_to_int(clay_send(cloned, "at", 1, &idx)) == 10);
    /* Cloned array is independent */
    ClayValue push_val = clay_int(99);
    clay_send(cloned, "push", 1, &push_val);
    TEST(clay_array_size(cloned) == 4);
    TEST(clay_array_size(arr) == 3);

    clay_release(cloned);
    clay_release(eq4);
    clay_release(eq3);
    clay_release(eq2);
    clay_release(eq1);

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

/* whileTrue test helpers - use a shared static counter */
static int while_counter = 0;

static ClayValue while_cond_fn(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_BOOL(while_counter < 5);
}

static ClayValue while_body_fn(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    while_counter++;
    return clay_int(while_counter);
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

    COMMENT("whileTrue...");
    while_counter = 0;
    ClayObject *cond_block = clay_block_new(while_cond_fn, NULL, 0);
    ClayObject *body_block = clay_block_new(while_body_fn, NULL, 0);
    arg = CLAY_VAL(body_block);
    result = clay_send(cond_block, "whileTrue", 1, &arg);
    TEST(while_counter == 5);
    TEST(clay_to_int(result) == 5);
    clay_release(cond_block);
    clay_release(body_block);
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

/* Method override helpers */
static ClayValue animal_speak(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_VAL(clay_string_new("..."));
}

static ClayValue dog_speak(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_VAL(clay_string_new("woof"));
}

static ClayValue puppy_speak(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_VAL(clay_string_new("yip"));
}

static ClayValue animal_legs(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return clay_int(4);
}

static void test_deep_prototype_chain(void) {
    SUITE("Deep Prototype Chain");

    /* 3-level chain: Animal -> Dog -> Puppy */
    ClayObject *Animal = clay_clone(clay_Object);
    clay_set_method(Animal, "speak", animal_speak);
    clay_set_method(Animal, "legs", animal_legs);
    clay_set_slot(Animal, "kind", CLAY_VAL(clay_string_new("animal")));

    ClayObject *Dog = clay_clone(Animal);
    clay_set_method(Dog, "speak", dog_speak); /* override */
    clay_set_slot(Dog, "kind", CLAY_VAL(clay_string_new("dog")));

    ClayObject *Puppy = clay_clone(Dog);
    clay_set_method(Puppy, "speak", puppy_speak); /* override again */

    COMMENT("method override at each level...");
    ClayValue r = clay_send(Animal, "speak", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r)), "...") == 0);
    clay_release(CLAY_OBJ(r));
    r = clay_send(Dog, "speak", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r)), "woof") == 0);
    clay_release(CLAY_OBJ(r));
    r = clay_send(Puppy, "speak", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r)), "yip") == 0);
    clay_release(CLAY_OBJ(r));

    COMMENT("inherited method from grandparent...");
    ClayValue legs = clay_send(Puppy, "legs", 0, NULL);
    TEST(clay_to_int(legs) == 4);
    legs = clay_send(Dog, "legs", 0, NULL);
    TEST(clay_to_int(legs) == 4);

    COMMENT("slot inheritance and shadowing...");
    /* Puppy doesn't have 'kind' slot, inherits from Dog */
    ClayValue kind = clay_get_slot(Puppy, "kind");
    TEST(strcmp(clay_string_data(CLAY_OBJ(kind)), "dog") == 0);
    /* Dog shadows Animal's 'kind' */
    kind = clay_get_slot(Dog, "kind");
    TEST(strcmp(clay_string_data(CLAY_OBJ(kind)), "dog") == 0);
    kind = clay_get_slot(Animal, "kind");
    TEST(strcmp(clay_string_data(CLAY_OBJ(kind)), "animal") == 0);

    /* Set slot on Puppy shadows parent */
    clay_set_slot(Puppy, "kind", CLAY_VAL(clay_string_new("puppy")));
    kind = clay_get_slot(Puppy, "kind");
    TEST(strcmp(clay_string_data(CLAY_OBJ(kind)), "puppy") == 0);
    /* Dog's slot unchanged */
    kind = clay_get_slot(Dog, "kind");
    TEST(strcmp(clay_string_data(CLAY_OBJ(kind)), "dog") == 0);

    COMMENT("clone creates independent objects...");
    ClayObject *d1 = clay_clone(Dog);
    ClayObject *d2 = clay_clone(Dog);
    clay_set_slot(d1, "name", CLAY_VAL(clay_string_new("Rex")));
    clay_set_slot(d2, "name", CLAY_VAL(clay_string_new("Fido")));
    TEST(strcmp(clay_string_data(CLAY_OBJ(clay_get_slot(d1, "name"))), "Rex") == 0);
    TEST(strcmp(clay_string_data(CLAY_OBJ(clay_get_slot(d2, "name"))), "Fido") == 0);
    /* Neither has 'name' from parent */
    ClayValue parent_name = clay_get_slot(Dog, "name");
    TEST(CLAY_IS_NIL(parent_name));

    clay_release(d2);
    clay_release(d1);
    clay_release(Puppy);
    clay_release(Dog);
    clay_release(Animal);
}

static void test_error_handling(void) {
    SUITE("Error Handling");

    COMMENT("error object creation...");
    ClayObject *err = clay_error_new("test error", "TestError");
    TEST(err != NULL);
    ClayValue msg = clay_send(err, "message", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(msg)), "test error") == 0);
    ClayValue typ = clay_send(err, "type", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(typ)), "TestError") == 0);
    clay_release(CLAY_OBJ(msg));
    clay_release(CLAY_OBJ(typ));
    clay_release(err);

    COMMENT("try/catch with raise...");
    ClayValue result = CLAY_NIL;
    if (clay_try_push() == 0) {
        clay_raise(CLAY_VAL(clay_error_new("caught me", "RuntimeError")));
        clay_try_pop();
        result = CLAY_VAL(clay_string_new("not reached"));
    } else {
        result = clay_error_stack.current_error;
    }
    TEST(!CLAY_IS_NIL(result));
    ClayValue rmsg = clay_send_val(result, "message", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(rmsg)), "caught me") == 0);
    clay_release(CLAY_OBJ(rmsg));

    COMMENT("try/catch with div by zero...");
    ClayValue result2 = CLAY_NIL;
    if (clay_try_push() == 0) {
        /* Trigger division by zero through dispatch */
        ClayObject *ten = clay_clone(clay_Number);
        clay_set_slot(ten, "_value", clay_int(10));
        ClayValue zero = clay_int(0);
        clay_send(ten, "/", 1, &zero);
        clay_try_pop();
        result2 = CLAY_VAL(clay_string_new("not reached"));
        clay_release(ten);
    } else {
        result2 = clay_error_stack.current_error;
    }
    TEST(!CLAY_IS_NIL(result2));
    ClayValue dmsg = clay_send_val(result2, "message", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(dmsg)), "division by zero") == 0);
    clay_release(CLAY_OBJ(dmsg));

    COMMENT("nested try/catch...");
    ClayValue outer = CLAY_NIL;
    if (clay_try_push() == 0) {
        ClayValue inner = CLAY_NIL;
        if (clay_try_push() == 0) {
            clay_raise(CLAY_VAL(clay_error_new("inner error", "RuntimeError")));
            clay_try_pop();
        } else {
            inner = clay_error_stack.current_error;
        }
        clay_try_pop();
        outer = inner; /* inner catch handles it */
    } else {
        outer = CLAY_VAL(clay_string_new("outer should not catch"));
    }
    ClayValue imsg = clay_send_val(outer, "message", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(imsg)), "inner error") == 0);
    clay_release(CLAY_OBJ(imsg));

    COMMENT("method not found raises error...");
    ClayValue result3 = CLAY_NIL;
    if (clay_try_push() == 0) {
        clay_send_val(clay_int(42), "nonexistent", 0, NULL);
        clay_try_pop();
    } else {
        result3 = clay_error_stack.current_error;
    }
    TEST(!CLAY_IS_NIL(result3));
    ClayValue etype = clay_send_val(result3, "type", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(etype)), "MethodNotFound") == 0);
    clay_release(CLAY_OBJ(etype));
}

static void test_dict(void) {
    SUITE("Dict");

    COMMENT("create and set/get...");
    ClayObject *d = clay_dict_new();
    TEST(d != NULL);
    TEST(clay_dict_size(d) == 0);

    clay_dict_set(d, "name", CLAY_VAL(clay_string_new("Clay")));
    clay_dict_set(d, "version", clay_int(1));
    TEST(clay_dict_size(d) == 2);

    ClayValue name = clay_dict_get(d, "name");
    TEST(!CLAY_IS_NIL(name));
    TEST(strcmp(clay_string_data(CLAY_OBJ(name)), "Clay") == 0);

    ClayValue ver = clay_dict_get(d, "version");
    TEST(CLAY_IS_INT(ver));
    TEST(clay_to_int(ver) == 1);

    COMMENT("has and missing key...");
    TEST(clay_dict_has(d, "name"));
    TEST(!clay_dict_has(d, "missing"));
    ClayValue miss = clay_dict_get(d, "missing");
    TEST(CLAY_IS_NIL(miss));

    COMMENT("overwrite existing key...");
    clay_dict_set(d, "name", CLAY_VAL(clay_string_new("Clay2")));
    ClayValue name2 = clay_dict_get(d, "name");
    TEST(strcmp(clay_string_data(CLAY_OBJ(name2)), "Clay2") == 0);
    TEST(clay_dict_size(d) == 2);

    COMMENT("remove...");
    clay_dict_remove(d, "version");
    TEST(clay_dict_size(d) == 1);
    TEST(!clay_dict_has(d, "version"));

    COMMENT("keys and values via methods...");
    clay_dict_set(d, "a", clay_int(10));
    ClayValue keys = clay_send(d, "keys", 0, NULL);
    TEST(clay_array_size(CLAY_OBJ(keys)) == 2);
    ClayValue vals = clay_send(d, "values", 0, NULL);
    TEST(clay_array_size(CLAY_OBJ(vals)) == 2);

    COMMENT("toString...");
    ClayObject *d2 = clay_dict_new();
    clay_dict_set(d2, "x", clay_int(42));
    ClayValue ts = clay_send(d2, "toString", 0, NULL);
    TEST(!CLAY_IS_NIL(ts));
    const char *s = clay_string_data(CLAY_OBJ(ts));
    TEST(s[0] == '{');
    TEST(s[strlen(s) - 1] == '}');

    clay_release(CLAY_OBJ(ts));
    clay_release(CLAY_OBJ(keys));
    clay_release(CLAY_OBJ(vals));

    COMMENT("dict clone copies entries...");
    ClayObject *d3 = clay_dict_new();
    clay_dict_set(d3, "x", clay_int(1));
    clay_dict_set(d3, "y", clay_int(2));
    ClayObject *d4 = clay_clone(d3);
    TEST(clay_dict_size(d4) == 2);
    TEST(clay_to_int(clay_dict_get(d4, "x")) == 1);
    TEST(clay_to_int(clay_dict_get(d4, "y")) == 2);
    /* Cloned dict is independent */
    clay_dict_set(d4, "z", clay_int(3));
    TEST(clay_dict_size(d4) == 3);
    TEST(clay_dict_size(d3) == 2);

    COMMENT("dict equality...");
    ClayObject *d5 = clay_dict_new();
    clay_dict_set(d5, "x", clay_int(1));
    clay_dict_set(d5, "y", clay_int(2));
    ClayValue deq_arg = CLAY_VAL(d5);
    TEST(clay_send(d3, "==", 1, &deq_arg) == CLAY_TRUE);
    deq_arg = CLAY_VAL(d4);
    TEST(clay_send(d3, "==", 1, &deq_arg) == CLAY_FALSE);
    deq_arg = CLAY_VAL(d5);
    TEST(clay_send(d3, "!=", 1, &deq_arg) == CLAY_FALSE);
    deq_arg = CLAY_VAL(d4);
    TEST(clay_send(d3, "!=", 1, &deq_arg) == CLAY_TRUE);

    clay_release(d5);
    clay_release(d4);
    clay_release(d3);
    clay_release(d2);
    clay_release(d);
}

/* Constructor init helpers */
static ClayValue point_init(ClayObject *self, int argc, ClayValue *argv) {
    if (argc >= 2) {
        clay_set_slot(self, "x", argv[0]);
        clay_set_slot(self, "y", argv[1]);
    }
    return CLAY_NIL;
}

static ClayValue greeter_init(ClayObject *self, int argc, ClayValue *argv) {
    if (argc >= 1)
        clay_set_slot(self, "name", argv[0]);
    return CLAY_NIL;
}

static void test_slot_access(void) {
    SUITE("Slot Access");

    COMMENT("slot read via message dispatch...");
    ClayObject *obj = clay_clone(clay_Object);
    clay_set_slot(obj, "x", clay_int(42));
    ClayValue r = clay_send_val(CLAY_VAL(obj), "x", 0, NULL);
    TEST(CLAY_IS_INT(r));
    TEST(clay_to_int(r) == 42);

    COMMENT("slot read walks prototype chain...");
    ClayObject *child = clay_clone(obj);
    r = clay_send_val(CLAY_VAL(child), "x", 0, NULL);
    TEST(clay_to_int(r) == 42);

    COMMENT("slot write on child doesn't affect parent...");
    clay_set_slot(child, "x", clay_int(99));
    TEST(clay_to_int(clay_send_val(CLAY_VAL(child), "x", 0, NULL)) == 99);
    TEST(clay_to_int(clay_send_val(CLAY_VAL(obj), "x", 0, NULL)) == 42);

    COMMENT("method takes priority over slot...");
    clay_set_method(obj, "x", animal_legs); /* returns 4 */
    r = clay_send_val(CLAY_VAL(obj), "x", 0, NULL);
    TEST(clay_to_int(r) == 4);

    clay_release(child);
    clay_release(obj);
}

static void test_constructors(void) {
    SUITE("Constructors");

    COMMENT("new without init just clones...");
    ClayObject *Proto = clay_clone(clay_Object);
    clay_set_slot(Proto, "val", clay_int(10));
    ClayValue inst = clay_send(Proto, "new", 0, NULL);
    TEST(!CLAY_IS_NIL(inst));
    TEST(CLAY_OBJ(inst)->parent == Proto);
    TEST(clay_to_int(clay_get_slot(CLAY_OBJ(inst), "val")) == 10);
    clay_release(CLAY_OBJ(inst));

    COMMENT("new with init calls init...");
    ClayObject *Point = clay_clone(clay_Object);
    clay_set_slot(Point, "x", clay_int(0));
    clay_set_slot(Point, "y", clay_int(0));
    clay_set_method(Point, "init", point_init);

    ClayValue args[2] = { clay_int(3), clay_int(4) };
    ClayValue p = clay_send(Point, "new", 2, args);
    TEST(!CLAY_IS_NIL(p));
    TEST(clay_to_int(clay_get_slot(CLAY_OBJ(p), "x")) == 3);
    TEST(clay_to_int(clay_get_slot(CLAY_OBJ(p), "y")) == 4);
    TEST(CLAY_OBJ(p)->parent == Point);
    clay_release(CLAY_OBJ(p));

    COMMENT("new passes args to init...");
    ClayObject *Greeter = clay_clone(clay_Object);
    clay_set_slot(Greeter, "name", CLAY_VAL(clay_string_new("world")));
    clay_set_method(Greeter, "init", greeter_init);

    ClayValue name_arg = CLAY_VAL(clay_string_new("Clay"));
    ClayValue g = clay_send(Greeter, "new", 1, &name_arg);
    TEST(strcmp(clay_string_data(CLAY_OBJ(clay_get_slot(CLAY_OBJ(g), "name"))), "Clay") == 0);
    clay_release(CLAY_OBJ(g));

    COMMENT("init inherited from parent prototype...");
    ClayObject *Point3D = clay_clone(Point);
    clay_set_slot(Point3D, "z", clay_int(0));
    /* Point3D inherits init from Point */
    ClayValue args3[2] = { clay_int(7), clay_int(8) };
    ClayValue p3 = clay_send(Point3D, "new", 2, args3);
    TEST(clay_to_int(clay_get_slot(CLAY_OBJ(p3), "x")) == 7);
    TEST(clay_to_int(clay_get_slot(CLAY_OBJ(p3), "y")) == 8);
    TEST(clay_to_int(clay_get_slot(CLAY_OBJ(p3), "z")) == 0);
    clay_release(CLAY_OBJ(p3));

    COMMENT("multiple instances are independent...");
    ClayValue a1[2] = { clay_int(1), clay_int(2) };
    ClayValue a2[2] = { clay_int(10), clay_int(20) };
    ClayValue i1 = clay_send(Point, "new", 2, a1);
    ClayValue i2 = clay_send(Point, "new", 2, a2);
    TEST(clay_to_int(clay_get_slot(CLAY_OBJ(i1), "x")) == 1);
    TEST(clay_to_int(clay_get_slot(CLAY_OBJ(i2), "x")) == 10);
    TEST(CLAY_OBJ(i1) != CLAY_OBJ(i2));
    clay_release(CLAY_OBJ(i1));
    clay_release(CLAY_OBJ(i2));

    clay_release(Point3D);
    clay_release(Greeter);
    clay_release(Point);
    clay_release(Proto);
}

/* Helper: compile and run Clay source via bytecode VM, return last stack value.
 * The VM and compiler are initialized/destroyed per call for isolation. */
static int vm_eval_count = 0;

static ClayValue vm_eval(const char *source) {
    vm_eval_count++;
    
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);
    BcCompiler bc;
    bc_compiler_init(&bc, &vm);

    Parser parser;
    parser_init(&parser, source, "<test>");
    AstNode *program = parser_parse(&parser);
    if (!program || parser.had_error) {
        if (program) ast_free(program);
        return CLAY_NIL;
    }

    bc.len = 0;
    bc.had_error = 0;
    if (bc_compile(&bc, program) != 0) {
        ast_free(program);
        return CLAY_NIL;
    }

    vm.sp = 0;
    int rc = clayvm_vm_run(&vm, bc.code, bc.len);
    ClayValue result = CLAY_NIL;
    if (rc == 0 && vm.sp > 0) {
        result = vm_to_clay(vm.stack[vm.sp - 1]);
    }

    ast_free(program);

    /* Note: vm resources not freed here — g_vm global pointer is used
     * by the bc_compiler trampoline and must remain valid. */

    return result;
}

static void test_vm_captures(void) {
    SUITE("VM Closures & Captures");

    COMMENT("basic capture — inner block reads outer param...");
    {
        ClayValue r = vm_eval(
            "wrap = { |n| { n } }\n"
            "getter = wrap value(42)\n"
            "getter value()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 42);
    }

    COMMENT("makeAdder pattern — capture + arithmetic...");
    {
        ClayValue r = vm_eval(
            "makeAdder = { |n| { |x| n + x } }\n"
            "add5 = makeAdder value(5)\n"
            "add5 value(10)\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 15);
    }

    COMMENT("capture survives scope exit...");
    {
        ClayValue r = vm_eval(
            "maker = { |a| { |b| a * b } }\n"
            "mul7 = maker value(7)\n"
            "mul7 value(6)\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 42);
    }

    COMMENT("3-level nested capture...");
    {
        ClayValue r = vm_eval(
            "f = { |a| { |b| { a * b } } }\n"
            "g = f value(3)\n"
            "h = g value(5)\n"
            "h value()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 15);
    }

    COMMENT("multiple captured variables...");
    {
        ClayValue r = vm_eval(
            "pair = { |a, b| { a + b } }\n"
            "sum = pair value(10, 20)\n"
            "sum value()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 30);
    }

    COMMENT("string capture...");
    {
        ClayValue r = vm_eval(
            "greet = { |g| { |n| g + \" \" + n } }\n"
            "hello = greet value(\"Hello\")\n"
            "hello value(\"World\")\n");
        TEST(!CLAY_IS_NIL(r));
        TEST(!CLAY_IS_INT(r));
        ClayObject *obj = CLAY_OBJ(r);
        TEST(strcmp(clay_string_data(obj), "Hello World") == 0);
    }

    COMMENT("top-level variables remain globals (not captured)...");
    {
        ClayValue r = vm_eval(
            "x = 10\n"
            "getter = { x }\n"
            "getter value()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 10);
    }

    COMMENT("simple block without captures still works...");
    {
        ClayValue r = vm_eval(
            "doubler = { |x| x * 2 }\n"
            "doubler value(21)\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 42);
    }

    COMMENT("capture with write-back...");
    {
        ClayValue r = vm_eval(
            "makeCounter = { |start| { start = start + 1; start } }\n"
            "counter = makeCounter value(0)\n"
            "counter value()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 1);
    }
}

static void test_compound_assignment(void) {
    SUITE("Compound Assignment");

    COMMENT("+=");
    {
        ClayValue r = vm_eval("x = 10\nx += 5\nx");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 15);
    }

    COMMENT("-=");
    {
        ClayValue r = vm_eval("x = 10\nx -= 3\nx");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 7);
    }

    COMMENT("*=");
    {
        ClayValue r = vm_eval("x = 6\nx *= 4\nx");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 24);
    }

    COMMENT("/=");
    {
        ClayValue r = vm_eval("x = 20\nx /= 5\nx");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 4);
    }

    COMMENT("modulo assign %%=");
    {
        ClayValue r = vm_eval("x = 17\nx %= 5\nx");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 2);
    }

    COMMENT("chained compound assignment");
    {
        ClayValue r = vm_eval("x = 1\nx += 2\nx *= 3\nx -= 1\nx");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 8);
    }
}

static void test_implicit_self(void) {
    SUITE("VM Implicit Self");

    COMMENT("bare slot read in method resolves to self slot...");
    {
        ClayValue r = vm_eval(
            "P = Object clone({\n"
            "    x = 10\n"
            "    getX() { x }\n"
            "})\n"
            "p = P clone()\n"
            "p getX()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 10);
    }

    COMMENT("method parameter shadows self slot...");
    {
        ClayValue r = vm_eval(
            "P = Object clone({\n"
            "    x = 10\n"
            "    f(x) { x }\n"
            "})\n"
            "p = P clone()\n"
            "p f(99)\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 99);
    }

    COMMENT("implicit self with arithmetic...");
    {
        ClayValue r = vm_eval(
            "P = Object clone({\n"
            "    x = 3\n"
            "    y = 4\n"
            "    sum() { x + y }\n"
            "})\n"
            "p = P clone()\n"
            "p sum()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 7);
    }

    COMMENT("builtin globals still resolve in methods...");
    {
        ClayValue r = vm_eval(
            "M = Object clone({\n"
            "    make() { Object clone() }\n"
            "})\n"
            "m = M make()\n"
            "m type()\n");
        TEST(CLAY_IS_PTR(r));
        TEST(CLAY_OBJ(r)->type == CLAY_TYPE_STRING);
        TEST(strcmp(clay_string_data(CLAY_OBJ(r)), "Object") == 0);
    }

    COMMENT("global fallback outside methods still works...");
    {
        ClayValue r = vm_eval(
            "x = 42\n"
            "x\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 42);
    }

    COMMENT("bare assignment in method writes to self slot...");
    {
        ClayValue r = vm_eval(
            "P = Object clone({\n"
            "    x = 10\n"
            "    f() {\n"
            "        y = 20\n"
            "        y\n"
            "    }\n"
            "})\n"
            "p = P clone()\n"
            "p f()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 20);
    }
}

static void test_vm_shared_captures(void) {
    SUITE("VM Shared Captures (ClayCell)");

    COMMENT("shared capture — two closures see each other's mutations...");
    {
        ClayValue r = vm_eval(
            "f = { |x|\n"
            "    inc = { x = x + 1; x }\n"
            "    get = { x }\n"
            "    [inc, get]\n"
            "}\n"
            "result = f value(0)\n"
            "inc = result at(0)\n"
            "get = result at(1)\n"
            "inc value()\n"
            "inc value()\n"
            "get value()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 2);
    }

    COMMENT("counter pattern — cell persists across calls...");
    {
        ClayValue r = vm_eval(
            "makeCounter = { |start| { start = start + 1; start } }\n"
            "c = makeCounter value(0)\n"
            "c value()\n"
            "c value()\n"
            "c value()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 3);
    }

    COMMENT("3-level nested capture through cells...");
    {
        ClayValue r = vm_eval(
            "f = { |a| { |b| { a * b } } }\n"
            "g = f value(3)\n"
            "h = g value(5)\n"
            "h value()\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 15);
    }

    COMMENT("capture with block passed to higher-order function...");
    {
        ClayValue r = vm_eval(
            "f = { |x| { |y| x + y } }\n"
            "adder = f value(100)\n"
            "[1, 2, 3] map(adder) at(2)\n");
        TEST(CLAY_IS_INT(r));
        TEST(clay_to_int(r) == 103);
    }
}

static void test_vm_logical_operators(void) {
    SUITE("VM Logical Operators (&&, ||, !)");

    COMMENT("basic && truth table...");
    {
        TEST(vm_eval("true && true") == CLAY_TRUE);
        TEST(vm_eval("true && false") == CLAY_FALSE);
        TEST(vm_eval("false && true") == CLAY_FALSE);
        TEST(vm_eval("false && false") == CLAY_FALSE);
    }

    COMMENT("basic || truth table...");
    {
        TEST(vm_eval("true || true") == CLAY_TRUE);
        TEST(vm_eval("true || false") == CLAY_TRUE);
        TEST(vm_eval("false || true") == CLAY_TRUE);
        TEST(vm_eval("false || false") == CLAY_FALSE);
    }

    COMMENT("! (not) operator...");
    {
        TEST(vm_eval("!true") == CLAY_FALSE);
        TEST(vm_eval("!false") == CLAY_TRUE);
    }

    COMMENT("short-circuit — && skips right when left is false...");
    {
        /* Division by zero would crash if right side were evaluated */
        ClayValue r = vm_eval("false && (1 / 0)");
        TEST(r == CLAY_FALSE);
    }

    COMMENT("short-circuit — || skips right when left is true...");
    {
        ClayValue r = vm_eval("true || (1 / 0)");
        TEST(r == CLAY_TRUE);
    }

    COMMENT("mixed with comparison operators...");
    {
        TEST(vm_eval("(3 > 1) && (5 < 10)") == CLAY_TRUE);
        TEST(vm_eval("(3 > 1) && (5 > 10)") == CLAY_FALSE);
        TEST(vm_eval("(3 > 5) || (5 < 10)") == CLAY_TRUE);
    }

    COMMENT("chained logical operators...");
    {
        TEST(vm_eval("true && true && true") == CLAY_TRUE);
        TEST(vm_eval("true && true && false") == CLAY_FALSE);
        TEST(vm_eval("false || false || true") == CLAY_TRUE);
        TEST(vm_eval("false || false || false") == CLAY_FALSE);
    }
}

/* --- Method cache invalidation --- */

static ClayValue cache_test_hello(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_VAL(clay_string_new("hello"));
}

static ClayValue cache_test_world(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    return CLAY_VAL(clay_string_new("world"));
}

static void test_method_cache(void) {
    SUITE("Method Cache");

    COMMENT("cache returns correct method after warm-up...");
    ClayObject *obj = clay_clone(clay_Object);
    clay_set_method(obj, "greet", cache_test_hello);

    /* Warm the cache with repeated sends */
    ClayValue r1 = clay_send(obj, "greet", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r1)), "hello") == 0);
    clay_release(CLAY_OBJ(r1));
    ClayValue r2 = clay_send(obj, "greet", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r2)), "hello") == 0);
    clay_release(CLAY_OBJ(r2));

    COMMENT("cache invalidated when method is replaced...");
    clay_set_method(obj, "greet", cache_test_world);
    ClayValue r3 = clay_send(obj, "greet", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r3)), "world") == 0);
    clay_release(CLAY_OBJ(r3));

    COMMENT("inherited method cached and invalidated correctly...");
    ClayObject *child = clay_clone(obj);
    ClayValue r4 = clay_send(child, "greet", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r4)), "world") == 0);
    clay_release(CLAY_OBJ(r4));

    /* Override on child — cache should see new method */
    clay_set_method(child, "greet", cache_test_hello);
    ClayValue r5 = clay_send(child, "greet", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r5)), "hello") == 0);
    clay_release(CLAY_OBJ(r5));

    /* Parent unchanged */
    ClayValue r6 = clay_send(obj, "greet", 0, NULL);
    TEST(strcmp(clay_string_data(CLAY_OBJ(r6)), "world") == 0);
    clay_release(CLAY_OBJ(r6));

    clay_release(child);
    clay_release(obj);
}

void test_main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* Compiler frontend tests (no bootstrap needed) */
    test_lexer();
    test_parser();

    /* Runtime tests */
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
    test_deep_prototype_chain();
    test_error_handling();
    test_dict();
    test_slot_access();
    test_constructors();
    test_vm_captures();
    test_compound_assignment();
    test_implicit_self();
    test_vm_shared_captures();
    test_vm_logical_operators();
    test_method_cache();
    /* test_compound_assignment();
    test_implicit_self();
    test_vm_shared_captures(); */

    clay_shutdown();
}
