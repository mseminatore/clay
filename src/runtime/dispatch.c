/*
 * dispatch.c - Message sending and method dispatch
 */
#include "clay.h"

ClayValue clay_send(ClayObject *receiver, const char *selector, int argc, ClayValue *argv) {
    if (!receiver) {
        fprintf(stderr, "clay: message '%s' sent to nil\n", selector);
        exit(1);
    }

    /* Look up method in receiver and parent chain */
    ClayMethodFunc func = clay_lookup_method(receiver, selector);
    if (func) {
        return func(receiver, argc, argv);
    }

    /* Try doesNotUnderstand */
    ClayMethodFunc dnu = clay_lookup_method(receiver, "doesNotUnderstand");
    if (dnu) {
        /* Build args array: [selectorString, originalArgs...] */
        ClayObject *sel_str = clay_string_new(selector);
        ClayObject *args_arr = clay_array_new();
        for (int i = 0; i < argc; i++) {
            clay_array_append(args_arr, argv[i]);
        }

        ClayValue dnu_argv[2] = { CLAY_VAL(sel_str), CLAY_VAL(args_arr) };
        ClayValue result = dnu(receiver, 2, dnu_argv);

        clay_release(sel_str);
        clay_release(args_arr);
        return result;
    }

    /* No handler at all - abort */
    fprintf(stderr, "clay: object does not understand '%s'\n", selector);
    exit(1);
    return CLAY_NIL;
}
