/*
 * range.c - Range object implementation
 */
#include "clay.h"
#include "range.h"

ClayObject *clay_range_new(intptr_t start, intptr_t end, intptr_t step) {
    ClayObject *obj = clay_object_new();
    obj->parent = clay_Range;
    if (clay_Range) clay_retain(clay_Range);
    clay_set_slot(obj, "_start", clay_int(start));
    clay_set_slot(obj, "_end", clay_int(end));
    clay_set_slot(obj, "_step", clay_int(step));
    return obj;
}

static void range_read_slots(ClayObject *self, intptr_t *start, intptr_t *end, intptr_t *step) {
    ClayValue sv = clay_get_slot(self, "_start");
    *start = CLAY_IS_INT(sv) ? clay_to_int(sv) : 0;
    ClayValue ev = clay_get_slot(self, "_end");
    *end = CLAY_IS_INT(ev) ? clay_to_int(ev) : 0;
    ClayValue tv = clay_get_slot(self, "_step");
    *step = CLAY_IS_INT(tv) ? clay_to_int(tv) : 1;
}

static ClayValue range_each(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    if (step == 0) return CLAY_VAL(self);

    for (intptr_t i = start; step > 0 ? i <= end : i >= end; i += step) {
        ClayValue arg = clay_int(i);
        ClayTryFrame _tf;
        if (clay_try_push(&_tf) == 0) {
            ClayValue ret = clay_send(CLAY_OBJ(argv[0]), "value", 1, &arg);
            clay_release_val(ret);
            clay_try_pop();
        } else {
            ClayValue err = clay_error_stack.current_error;
            if (CLAY_IS_PTR(err) && !CLAY_IS_NIL(err)) {
                ClayObject *eobj = CLAY_OBJ(err);
                ClayValue type_val = clay_get_slot(eobj, "_type");
                if (CLAY_IS_PTR(type_val) && !CLAY_IS_NIL(type_val) &&
                    CLAY_OBJ(type_val)->type == CLAY_TYPE_STRING) {
                    const char *tstr = clay_string_data(CLAY_OBJ(type_val));
                    if (strcmp(tstr, "BreakSignal") == 0) return CLAY_VAL(self);
                    if (strcmp(tstr, "ContinueSignal") == 0) continue;
                }
            }
            clay_raise(err);
        }
    }
    return CLAY_VAL(self);
}

static ClayValue range_map(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    ClayObject *result = clay_array_new();
    if (step == 0) return CLAY_VAL(result);

    for (intptr_t i = start; step > 0 ? i <= end : i >= end; i += step) {
        ClayValue arg = clay_int(i);
        ClayValue mapped = clay_send(CLAY_OBJ(argv[0]), "value", 1, &arg);
        clay_array_append(result, mapped);
        clay_release_val(mapped);
    }
    return CLAY_VAL(result);
}

static ClayValue range_filter(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    ClayObject *result = clay_array_new();
    if (step == 0) return CLAY_VAL(result);

    for (intptr_t i = start; step > 0 ? i <= end : i >= end; i += step) {
        ClayValue arg = clay_int(i);
        ClayValue test = clay_send(CLAY_OBJ(argv[0]), "value", 1, &arg);
        if (CLAY_IS_TRUTHY(test)) {
            clay_array_append(result, arg);
        }
        clay_release_val(test);
    }
    return CLAY_VAL(result);
}

static ClayValue range_contains(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_INT(argv[0])) return CLAY_FALSE;
    intptr_t val = clay_to_int(argv[0]);
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    if (step == 0) return CLAY_FALSE;

    if (step > 0) {
        if (val < start || val > end) return CLAY_FALSE;
    } else {
        if (val > start || val < end) return CLAY_FALSE;
    }
    if ((val - start) % step != 0) return CLAY_FALSE;
    return CLAY_TRUE;
}

static ClayValue range_toArray(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    ClayObject *result = clay_array_new();
    if (step == 0) return CLAY_VAL(result);

    for (intptr_t i = start; step > 0 ? i <= end : i >= end; i += step) {
        clay_array_append(result, clay_int(i));
    }
    return CLAY_VAL(result);
}

static ClayValue range_size(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    if (step == 0) return clay_int(0);
    intptr_t diff = end - start;
    if ((step > 0 && diff < 0) || (step < 0 && diff > 0)) return clay_int(0);
    return clay_int(diff / step + 1);
}

static ClayValue range_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    char buf[128];
    if (step == 1) {
        snprintf(buf, sizeof(buf), "%td..%td", start, end);
    } else {
        snprintf(buf, sizeof(buf), "%td..%td step %td", start, end, step);
    }
    return CLAY_VAL(clay_string_new(buf));
}

static ClayValue range_step(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_INT(argv[0])) return CLAY_VAL(self);
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    (void)step;
    intptr_t new_step = clay_to_int(argv[0]);
    return CLAY_VAL(clay_range_new(start, end, new_step));
}

static ClayValue range_reverse(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    intptr_t start, end, step;
    range_read_slots(self, &start, &end, &step);
    return CLAY_VAL(clay_range_new(end, start, -step));
}

static ClayValue range_start(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue sv = clay_get_slot(self, "_start");
    return CLAY_IS_INT(sv) ? sv : clay_int(0);
}

static ClayValue range_end(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue ev = clay_get_slot(self, "_end");
    return CLAY_IS_INT(ev) ? ev : clay_int(0);
}

void clay_range_init_methods(ClayObject *proto) {
    clay_set_method(proto, "each", range_each);
    clay_set_method(proto, "map", range_map);
    clay_set_method(proto, "filter", range_filter);
    clay_set_method(proto, "contains", range_contains);
    clay_set_method(proto, "toArray", range_toArray);
    clay_set_method(proto, "size", range_size);
    clay_set_method(proto, "toString", range_toString);
    clay_set_method(proto, "step", range_step);
    clay_set_method(proto, "reverse", range_reverse);
    clay_set_method(proto, "start", range_start);
    clay_set_method(proto, "end", range_end);
}
