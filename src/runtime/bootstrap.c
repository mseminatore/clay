/*
 * bootstrap.c - Initialize core prototype objects
 */
#include "clay.h"

/* Core prototypes - global singletons */
ClayObject *clay_Object  = NULL;
ClayObject *clay_Number  = NULL;
ClayObject *clay_String  = NULL;
ClayObject *clay_Array   = NULL;
ClayObject *clay_Block   = NULL;
ClayObject *clay_Nil     = NULL;
ClayObject *clay_Boolean = NULL;

/* External init functions from each type module */
extern void clay_number_init_methods(ClayObject *proto);
extern void clay_string_init_methods(ClayObject *proto);
extern void clay_array_init_methods(ClayObject *proto);
extern void clay_block_init_methods(ClayObject *proto);

/* Object base methods */

static ClayValue object_clone(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return CLAY_VAL(clay_clone(self));
}

static ClayValue object_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char buf[64];
    snprintf(buf, sizeof(buf), "<object@%p>", (void *)self);
    return CLAY_VAL(clay_string_new(buf));
}

static ClayValue object_println(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue s = clay_send(self, "toString", 0, NULL);
    clay_send(CLAY_OBJ(s), "println", 0, NULL);
    clay_release(CLAY_OBJ(s));
    return CLAY_VAL(self);
}

static ClayValue object_doesNotUnderstand(ClayObject *self, int argc, ClayValue *argv) {
    const char *sel = (argc > 0 && CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0]))
        ? clay_string_data(CLAY_OBJ(argv[0]))
        : "???";
    fprintf(stderr, "clay: object %p does not understand '%s'\n", (void *)self, sel);
    exit(1);
    return CLAY_NIL;
}

/* Boolean methods */

/*
 * ifTrue/ifFalse return a "condition result" object that supports chaining.
 * The result stores the value in a "_val" slot and the "_matched" flag.
 * ifFalse on a result where _matched==true returns _val unchanged.
 * ifFalse on a result where _matched==false evaluates the block.
 */

static ClayObject *make_cond_result(ClayValue val, int matched) {
    ClayObject *obj = clay_clone(clay_Object);
    clay_set_slot(obj, "_val", val);
    clay_set_slot(obj, "_matched", matched ? CLAY_TRUE : CLAY_FALSE);
    return obj;
}

static ClayValue cond_ifFalse(ClayObject *self, int argc, ClayValue *argv) {
    ClayValue matched = clay_get_slot(self, "_matched");
    if (matched == CLAY_TRUE) {
        return clay_get_slot(self, "_val");
    }
    if (argc < 1) return CLAY_NIL;
    return clay_send(CLAY_OBJ(argv[0]), "value", 0, NULL);
}

static ClayValue cond_ifTrue(ClayObject *self, int argc, ClayValue *argv) {
    ClayValue matched = clay_get_slot(self, "_matched");
    if (matched == CLAY_TRUE) {
        return clay_get_slot(self, "_val");
    }
    if (argc < 1) return CLAY_VAL(self);
    ClayValue result = clay_send(CLAY_OBJ(argv[0]), "value", 0, NULL);
    ClayObject *cr = make_cond_result(result, 1);
    clay_set_method(cr, "ifFalse", cond_ifFalse);
    clay_set_method(cr, "ifTrue", cond_ifTrue);
    return CLAY_VAL(cr);
}

static ClayValue bool_ifTrue(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_VAL(self);
    if (self == &clay_true_obj) {
        ClayValue result = clay_send(CLAY_OBJ(argv[0]), "value", 0, NULL);
        ClayObject *cr = make_cond_result(result, 1);
        clay_set_method(cr, "ifFalse", cond_ifFalse);
        clay_set_method(cr, "ifTrue", cond_ifTrue);
        return CLAY_VAL(cr);
    }
    /* false: return a condition result with _matched=false */
    ClayObject *cr = make_cond_result(CLAY_NIL, 0);
    clay_set_method(cr, "ifFalse", cond_ifFalse);
    clay_set_method(cr, "ifTrue", cond_ifTrue);
    return CLAY_VAL(cr);
}

static ClayValue bool_ifFalse(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_VAL(self);
    if (self == &clay_false_obj) {
        ClayValue result = clay_send(CLAY_OBJ(argv[0]), "value", 0, NULL);
        ClayObject *cr = make_cond_result(result, 1);
        clay_set_method(cr, "ifTrue", cond_ifTrue);
        clay_set_method(cr, "ifFalse", cond_ifFalse);
        return CLAY_VAL(cr);
    }
    /* true: return a condition result with _matched=false */
    ClayObject *cr = make_cond_result(CLAY_NIL, 0);
    clay_set_method(cr, "ifTrue", cond_ifTrue);
    clay_set_method(cr, "ifFalse", cond_ifFalse);
    return CLAY_VAL(cr);
}

static ClayValue bool_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self == &clay_true_obj) return CLAY_VAL(clay_string_new("true"));
    return CLAY_VAL(clay_string_new("false"));
}

static ClayValue bool_not(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return (self == &clay_true_obj) ? CLAY_FALSE : CLAY_TRUE;
}

static ClayValue nil_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv; (void)self;
    return CLAY_VAL(clay_string_new("nil"));
}

void clay_bootstrap(void) {
    /* Create root Object prototype */
    clay_Object = clay_object_new();
    clay_Object->refcount = 1000000; /* never freed */

    clay_set_method(clay_Object, "clone", object_clone);
    clay_set_method(clay_Object, "toString", object_toString);
    clay_set_method(clay_Object, "println", object_println);
    clay_set_method(clay_Object, "doesNotUnderstand", object_doesNotUnderstand);

    /* Number prototype */
    clay_Number = clay_object_new();
    clay_Number->parent = clay_Object;
    clay_Number->refcount = 1000000;
    clay_number_init_methods(clay_Number);

    /* String prototype */
    clay_String = clay_object_new();
    clay_String->parent = clay_Object;
    clay_String->type = CLAY_TYPE_STRING;
    clay_String->refcount = 1000000;
    clay_string_init_methods(clay_String);

    /* Array prototype */
    clay_Array = clay_object_new();
    clay_Array->parent = clay_Object;
    clay_Array->type = CLAY_TYPE_ARRAY;
    clay_Array->refcount = 1000000;
    clay_array_init_methods(clay_Array);

    /* Block prototype */
    clay_Block = clay_object_new();
    clay_Block->parent = clay_Object;
    clay_Block->type = CLAY_TYPE_BLOCK;
    clay_Block->refcount = 1000000;
    clay_block_init_methods(clay_Block);

    /* Boolean prototype */
    clay_Boolean = clay_object_new();
    clay_Boolean->parent = clay_Object;
    clay_Boolean->refcount = 1000000;
    clay_set_method(clay_Boolean, "ifTrue", bool_ifTrue);
    clay_set_method(clay_Boolean, "ifFalse", bool_ifFalse);
    clay_set_method(clay_Boolean, "toString", bool_toString);
    clay_set_method(clay_Boolean, "not", bool_not);

    /* Wire up boolean singletons */
    clay_true_obj.parent = clay_Boolean;
    clay_false_obj.parent = clay_Boolean;
    /* Boolean singletons need slot/method tables for lookups */
    clay_true_obj.slots = NULL;
    clay_true_obj.methods = NULL;
    clay_false_obj.slots = NULL;
    clay_false_obj.methods = NULL;

    /* Nil prototype */
    clay_Nil = clay_object_new();
    clay_Nil->parent = clay_Object;
    clay_Nil->refcount = 1000000;
    clay_set_method(clay_Nil, "toString", nil_toString);
}

void clay_shutdown(void) {
    /* In a real implementation we'd free all prototypes.
     * For now, they live for the program's lifetime. */
}

/* Debug helper: print object details to stderr */
void clay_inspect(ClayObject *obj) {
    if (!obj) {
        fprintf(stderr, "[nil]\n");
        return;
    }
    const char *type_name;
    switch (obj->type) {
        case CLAY_TYPE_STRING: type_name = "String"; break;
        case CLAY_TYPE_ARRAY:  type_name = "Array"; break;
        case CLAY_TYPE_BLOCK:  type_name = "Block"; break;
        default:               type_name = "Object"; break;
    }
    fprintf(stderr, "[%s @%p refcount=%d parent=%p", type_name, (void *)obj, obj->refcount, (void *)obj->parent);

    if (obj->type == CLAY_TYPE_STRING) {
        fprintf(stderr, " data=\"%s\" len=%zu", obj->as.string.data, obj->as.string.length);
    } else if (obj->type == CLAY_TYPE_ARRAY) {
        fprintf(stderr, " size=%zu cap=%zu", obj->as.array.size, obj->as.array.capacity);
    }

    /* Print slots */
    if (obj->slots) {
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        fprintf(stderr, " slots={");
        int first = 1;
        while (ht_next(obj->slots, &pos, &key, &val)) {
            ClayValue cv = (ClayValue)(uintptr_t)val;
            if (!first) fprintf(stderr, ", ");
            first = 0;
            fprintf(stderr, "%s=", (const char *)key);
            if (CLAY_IS_INT(cv)) {
                fprintf(stderr, "%ld", (long)clay_to_int(cv));
            } else if (CLAY_IS_NIL(cv)) {
                fprintf(stderr, "nil");
            } else {
                fprintf(stderr, "@%p", (void *)(uintptr_t)cv);
            }
        }
        fprintf(stderr, "}");
    }
    fprintf(stderr, "]\n");
}
