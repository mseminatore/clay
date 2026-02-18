/*
 * dispatch.c - Message sending and method dispatch
 */
#include "clay.h"

ClayValue clay_send_val(ClayValue receiver, const char *selector, int argc, ClayValue *argv) {
    /* Handle tagged integer receivers */
    if (CLAY_IS_INT(receiver)) {
        /* Create a temporary wrapper object for dispatch */
        ClayObject *wrapper = clay_clone(clay_Number);
        clay_set_slot(wrapper, "_value", receiver);
        ClayMethodFunc func = clay_lookup_method(wrapper, selector);
        if (func) {
            ClayValue result = func(wrapper, argc, argv);
            clay_release(wrapper);
            return result;
        }
        clay_release(wrapper);
        fprintf(stderr, "clay: integer does not understand '%s'\n", selector);
        exit(1);
        return CLAY_NIL;
    }

    /* Handle nil */
    if (CLAY_IS_NIL(receiver)) {
        /* Dispatch to Nil prototype */
        if (clay_Nil) {
            ClayMethodFunc func = clay_lookup_method(clay_Nil, selector);
            if (func) {
                return func(clay_Nil, argc, argv);
            }
        }
        fprintf(stderr, "clay: message '%s' sent to nil\n", selector);
        exit(1);
        return CLAY_NIL;
    }

    /* Handle boolean singletons and regular objects */
    ClayObject *obj = CLAY_OBJ(receiver);

    ClayMethodFunc func = clay_lookup_method(obj, selector);
    if (func) {
        return func(obj, argc, argv);
    }

    /* Try doesNotUnderstand */
    ClayMethodFunc dnu = clay_lookup_method(obj, "doesNotUnderstand");
    if (dnu) {
        ClayObject *sel_str = clay_string_new(selector);
        ClayObject *args_arr = clay_array_new();
        for (int i = 0; i < argc; i++) {
            clay_array_append(args_arr, argv[i]);
        }

        ClayValue dnu_argv[2] = { CLAY_VAL(sel_str), CLAY_VAL(args_arr) };
        ClayValue result = dnu(obj, 2, dnu_argv);

        clay_release(sel_str);
        clay_release(args_arr);
        return result;
    }

    fprintf(stderr, "clay: object does not understand '%s'\n", selector);
    exit(1);
    return CLAY_NIL;
}
