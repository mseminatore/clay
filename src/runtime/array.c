/*
 * array.c - Array object implementation
 */
#include "clay.h"
#include "range.h"

#define CLAY_ARRAY_INITIAL_CAP 8

ClayObject *clay_array_new(void) {
    ClayObject *obj = clay_object_new();
    obj->type = CLAY_TYPE_ARRAY;
    obj->parent = clay_Array;
    if (clay_Array) clay_retain(clay_Array);
    obj->as.array.items = (ClayValue *)malloc(sizeof(ClayValue) * CLAY_ARRAY_INITIAL_CAP);
    obj->as.array.size = 0;
    obj->as.array.capacity = CLAY_ARRAY_INITIAL_CAP;
    return obj;
}

/* Append retains val; ownership shared with array until removal. */
void clay_array_append(ClayObject *arr, ClayValue val) {
    if (arr->as.array.size >= arr->as.array.capacity) {
        arr->as.array.capacity *= 2;
        arr->as.array.items = (ClayValue *)realloc(arr->as.array.items,
            sizeof(ClayValue) * arr->as.array.capacity);
    }
    if (CLAY_IS_PTR(val) && !CLAY_IS_NIL(val) && val != CLAY_TRUE && val != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(val));
    }
    arr->as.array.items[arr->as.array.size++] = val;
}

ClayValue clay_array_at(ClayObject *arr, size_t index) {
    if (index >= arr->as.array.size) return CLAY_NIL;
    return arr->as.array.items[index];
}

size_t clay_array_size(ClayObject *arr) {
    return arr->as.array.size;
}

/* --- Array methods --- */

static ClayValue array_append(ClayObject *self, int argc, ClayValue *argv) {
    if (argc >= 1) {
        clay_array_append(self, argv[0]);
    }
    return CLAY_VAL(self);
}

static ClayValue array_at(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) clay_raise_argument_error("Array at", "missing argument");

    /* Range-based slicing: arr at(1..4) → sub-array */
    if (CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0])) {
        ClayObject *arg = CLAY_OBJ(argv[0]);
        if (arg->parent == clay_Range || arg == clay_Range) {
            ClayValue start_v = clay_get_slot(arg, "_start");
            ClayValue end_v = clay_get_slot(arg, "_end");
            intptr_t start = CLAY_IS_INT(start_v) ? clay_to_int(start_v) : 0;
            intptr_t end = CLAY_IS_INT(end_v) ? clay_to_int(end_v) : 0;
            ClayObject *result = clay_array_new();
            intptr_t len = (intptr_t)self->as.array.size;
            if (start < 0) start = 0;
            if (start > len) start = len;
            if (end > len) end = len;
            for (intptr_t i = start; i < end; i++) {
                ClayValue elem = clay_array_at(self, (size_t)i);
                clay_array_append(result, elem);
            }
            return CLAY_VAL(result);
        }
    }

    /* Integer indexing (a non-number index is a TypeError, not nil) */
    intptr_t idx = clay_arg_integer(argc, argv, 0, "Array at");
    if (idx < 0) return CLAY_NIL;
    ClayValue result = clay_array_at(self, (size_t)idx);
    clay_retain_val(result);
    return result;
}

static ClayValue array_size(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_int((intptr_t)self->as.array.size);
}

static ClayValue array_each(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;

    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        ClayTryFrame _tf;
        if (clay_try_push(&_tf) == 0) {
            ClayValue ret = clay_send(CLAY_OBJ(argv[0]), "value", 1, &item);
            clay_release_val(ret);
            clay_try_pop();
        } else {
            ClayValue err = clay_error_stack.current_error;
            int sig = clay_loop_signal(err);
            if (sig == CLAY_SIGNAL_BREAK) return CLAY_VAL(self);
            if (sig == CLAY_SIGNAL_CONTINUE) continue;
            clay_raise(err);
        }
    }
    return CLAY_VAL(self);
}

static ClayValue array_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    /* Build "[item1, item2, ...]" */
    char *buf = (char *)malloc(256);
    size_t cap = 256, len = 0;
    buf[len++] = '[';

    for (size_t i = 0; i < self->as.array.size; i++) {
        if (i > 0) {
            buf[len++] = ',';
            buf[len++] = ' ';
        }
        ClayValue item = self->as.array.items[i];
        const char *s;
        char numbuf[32];
        ClayValue sv = CLAY_NIL;
        if (CLAY_IS_INT(item)) {
            snprintf(numbuf, sizeof(numbuf), "%lld", (long long)clay_to_int(item));
            s = numbuf;
        } else if (CLAY_IS_NIL(item)) {
            s = "nil";
        } else {
            sv = clay_send(CLAY_OBJ(item), "toString", 0, NULL);
            s = clay_string_data(CLAY_OBJ(sv));
        }
        size_t slen = strlen(s);
        while (len + slen + 4 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, s, slen);
        len += slen;
        if (!CLAY_IS_NIL(sv)) clay_release(CLAY_OBJ(sv));
    }
    buf[len++] = ']';
    buf[len] = '\0';

    ClayObject *result = clay_string_new(buf);
    free(buf);
    return CLAY_VAL(result);
}

static ClayValue array_map(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        ClayValue mapped = clay_send(CLAY_OBJ(argv[0]), "value", 1, &item);
        clay_array_append(result, mapped);
        clay_release_val(mapped);
    }
    return CLAY_VAL(result);
}

static ClayValue array_filter(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        ClayValue test = clay_send(CLAY_OBJ(argv[0]), "value", 1, &item);
        if (CLAY_IS_TRUTHY(test)) {
            clay_array_append(result, item);
        }
        clay_release_val(test);
    }
    return CLAY_VAL(result);
}

static ClayValue array_reduce(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 2) return CLAY_NIL;
    ClayValue acc = argv[0];
    clay_retain_val(acc);
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue args[2] = { acc, self->as.array.items[i] };
        ClayValue new_acc = clay_send(CLAY_OBJ(argv[1]), "value", 2, args);
        clay_release_val(acc);
        acc = new_acc;
    }
    return acc;
}

static ClayValue array_find(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        ClayValue test = clay_send(CLAY_OBJ(argv[0]), "value", 1, &item);
        if (CLAY_IS_TRUTHY(test)) return item;
    }
    return CLAY_NIL;
}

static ClayValue array_contains(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_FALSE;
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        if (item == argv[0]) return CLAY_TRUE;
        /* Compare by value for ints and strings */
        if (CLAY_IS_INT(item) && CLAY_IS_INT(argv[0]) && item == argv[0]) return CLAY_TRUE;
        if (CLAY_IS_PTR(item) && !CLAY_IS_NIL(item) && CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0])) {
            ClayValue eq = clay_send_val(item, "==", 1, &argv[0]);
            if (eq == CLAY_TRUE) return CLAY_TRUE;
        }
    }
    return CLAY_FALSE;
}

static ClayValue array_indexOf(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return clay_int(-1);
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        if (item == argv[0]) return clay_int((intptr_t)i);
        if (CLAY_IS_PTR(item) && !CLAY_IS_NIL(item) && CLAY_IS_PTR(argv[0]) && !CLAY_IS_NIL(argv[0])) {
            ClayValue eq = clay_send_val(item, "==", 1, &argv[0]);
            if (eq == CLAY_TRUE) return clay_int((intptr_t)i);
        }
    }
    return clay_int(-1);
}

static ClayValue array_first(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    ClayValue result = self->as.array.items[0];
    clay_retain_val(result);
    return result;
}

static ClayValue array_last(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    ClayValue result = self->as.array.items[self->as.array.size - 1];
    clay_retain_val(result);
    return result;
}

static ClayValue array_reverse(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = clay_array_new();
    for (size_t i = self->as.array.size; i > 0; i--) {
        clay_array_append(result, self->as.array.items[i - 1]);
    }
    return CLAY_VAL(result);
}

static ClayValue array_join(ClayObject *self, int argc, ClayValue *argv) {
    /* No argument (or nil) joins with no separator; otherwise it must be a String */
    const char *sep = (argc > 0 && !CLAY_IS_NIL(argv[0])) ? clay_arg_string(argc, argv, 0, "Array join") : "";
    size_t sep_len = strlen(sep);
    char *buf = (char *)malloc(256);
    size_t cap = 256, len = 0;

    for (size_t i = 0; i < self->as.array.size; i++) {
        if (i > 0) {
            while (len + sep_len + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
            memcpy(buf + len, sep, sep_len);
            len += sep_len;
        }
        ClayValue item = self->as.array.items[i];
        const char *s;
        char numbuf[32];
        if (CLAY_IS_INT(item)) {
            snprintf(numbuf, sizeof(numbuf), "%lld", (long long)clay_to_int(item));
            s = numbuf;
        } else if (CLAY_IS_NIL(item)) {
            s = "nil";
        } else {
            ClayValue sv = clay_send(CLAY_OBJ(item), "toString", 0, NULL);
            s = clay_string_data(CLAY_OBJ(sv));
            /* Copy s before releasing sv */
            size_t slen = strlen(s);
            while (len + slen + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
            memcpy(buf + len, s, slen);
            len += slen;
            clay_release(CLAY_OBJ(sv));
            continue;
        }
        size_t slen = strlen(s);
        while (len + slen + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        memcpy(buf + len, s, slen);
        len += slen;
    }
    buf[len] = '\0';
    ClayObject *result = clay_string_new(buf);
    free(buf);
    return CLAY_VAL(result);
}

static int clay_compare_values(const void *a, const void *b) {
    ClayValue va = *(const ClayValue *)a;
    ClayValue vb = *(const ClayValue *)b;
    if (CLAY_IS_INT(va) && CLAY_IS_INT(vb)) {
        intptr_t ia = clay_to_int(va), ib = clay_to_int(vb);
        return (ia > ib) - (ia < ib);
    }
    /* Fall back to string comparison */
    ClayValue sa = clay_send_val(va, "toString", 0, NULL);
    ClayValue sb = clay_send_val(vb, "toString", 0, NULL);
    int cmp = strcmp(clay_string_data(CLAY_OBJ(sa)), clay_string_data(CLAY_OBJ(sb)));
    clay_release(CLAY_OBJ(sa));
    clay_release(CLAY_OBJ(sb));
    return cmp;
}

static ClayValue array_sort(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < self->as.array.size; i++) {
        clay_array_append(result, self->as.array.items[i]);
    }
    if (result->as.array.size > 1) {
        qsort(result->as.array.items, result->as.array.size, sizeof(ClayValue), clay_compare_values);
    }
    return CLAY_VAL(result);
}

/* Ownership of removed item transfers to caller (array's retain not released). */
static ClayValue array_removeAt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_INT(argv[0])) return CLAY_NIL;
    intptr_t idx = clay_to_int(argv[0]);
    if (idx < 0 || (size_t)idx >= self->as.array.size) return CLAY_NIL;
    ClayValue removed = self->as.array.items[idx];
    memmove(&self->as.array.items[idx], &self->as.array.items[idx + 1],
            (self->as.array.size - (size_t)idx - 1) * sizeof(ClayValue));
    self->as.array.size--;
    return removed;
}

static ClayValue array_equals(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_ARRAY) return CLAY_FALSE;
    if (self == other) return CLAY_TRUE;
    if (self->as.array.size != other->as.array.size) return CLAY_FALSE;
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue a = self->as.array.items[i];
        ClayValue b = other->as.array.items[i];
        ClayValue eq = clay_send_val(a, "==", 1, &b);
        if (eq != CLAY_TRUE) return CLAY_FALSE;
    }
    return CLAY_TRUE;
}

static ClayValue array_neq(ClayObject *self, int argc, ClayValue *argv) {
    return array_equals(self, argc, argv) == CLAY_TRUE ? CLAY_FALSE : CLAY_TRUE;
}

static ClayValue array_flatten(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        if (CLAY_IS_PTR(item) && !CLAY_IS_NIL(item) &&
            CLAY_OBJ(item)->type == CLAY_TYPE_ARRAY) {
            ClayObject *inner = CLAY_OBJ(item);
            for (size_t j = 0; j < inner->as.array.size; j++) {
                clay_array_append(result, inner->as.array.items[j]);
            }
        } else {
            clay_array_append(result, item);
        }
    }
    return CLAY_VAL(result);
}

static ClayValue array_sum(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return clay_int(0);
    int use_float = 0;
    intptr_t isum = 0;
    double fsum = 0.0;
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue v = self->as.array.items[i];
        if (CLAY_IS_INT(v)) {
            if (use_float) fsum += (double)clay_to_int(v);
            else isum += clay_to_int(v);
        } else if (CLAY_IS_FLOAT(v)) {
            if (!use_float) { fsum = (double)isum; use_float = 1; }
            fsum += clay_to_float(v);
        }
    }
    if (use_float) return clay_float_tag((float)fsum);
    return clay_int(isum);
}

static ClayValue array_min(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    ClayValue best = self->as.array.items[0];
    for (size_t i = 1; i < self->as.array.size; i++) {
        ClayValue v = self->as.array.items[i];
        ClayValue lt = clay_send_val(v, "<", 1, &best);
        if (lt == CLAY_TRUE) best = v;
    }
    return best;
}

static ClayValue array_max(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    ClayValue best = self->as.array.items[0];
    for (size_t i = 1; i < self->as.array.size; i++) {
        ClayValue v = self->as.array.items[i];
        ClayValue gt = clay_send_val(v, ">", 1, &best);
        if (gt == CLAY_TRUE) best = v;
    }
    return best;
}

static ClayValue array_avg(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    double sum = 0.0;
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue v = self->as.array.items[i];
        if (CLAY_IS_INT(v)) sum += (double)clay_to_int(v);
        else if (CLAY_IS_FLOAT(v)) sum += (double)clay_to_float(v);
    }
    return clay_float_tag((float)(sum / (double)self->as.array.size));
}

static ClayValue array_unique(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayObject *result = clay_array_new();
    for (size_t i = 0; i < self->as.array.size; i++) {
        ClayValue item = self->as.array.items[i];
        int found = 0;
        for (size_t j = 0; j < result->as.array.size; j++) {
            ClayValue existing = result->as.array.items[j];
            ClayValue eq = clay_send_val(item, "==", 1, &existing);
            if (eq == CLAY_TRUE) { found = 1; break; }
        }
        if (!found) clay_array_append(result, item);
    }
    return CLAY_VAL(result);
}

static ClayValue array_pop(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    ClayValue val = self->as.array.items[--self->as.array.size];
    return val;
}

static ClayValue array_shift(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (self->as.array.size == 0) return CLAY_NIL;
    ClayValue val = self->as.array.items[0];
    memmove(&self->as.array.items[0], &self->as.array.items[1],
            (self->as.array.size - 1) * sizeof(ClayValue));
    self->as.array.size--;
    return val;
}

static ClayValue array_slice(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_VAL(clay_array_new());
    intptr_t start = CLAY_IS_INT(argv[0]) ? clay_to_int(argv[0]) : 0;
    intptr_t end = (argc > 1 && CLAY_IS_INT(argv[1])) ? clay_to_int(argv[1])
                 : (intptr_t)self->as.array.size;
    intptr_t len = (intptr_t)self->as.array.size;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return CLAY_VAL(clay_array_new());
    ClayObject *result = clay_array_new();
    for (intptr_t i = start; i < end; i++) {
        clay_array_append(result, self->as.array.items[i]);
    }
    return CLAY_VAL(result);
}

static ClayValue array_insert(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 2 || !CLAY_IS_INT(argv[0])) return CLAY_VAL(self);
    intptr_t idx = clay_to_int(argv[0]);
    if (idx < 0) idx = 0;
    if ((size_t)idx > self->as.array.size) idx = (intptr_t)self->as.array.size;
    /* Ensure capacity */
    if (self->as.array.size >= self->as.array.capacity) {
        self->as.array.capacity *= 2;
        self->as.array.items = (ClayValue *)realloc(self->as.array.items,
            sizeof(ClayValue) * self->as.array.capacity);
    }
    /* Shift elements right */
    memmove(&self->as.array.items[idx + 1], &self->as.array.items[idx],
            (self->as.array.size - (size_t)idx) * sizeof(ClayValue));
    ClayValue val = argv[1];
    if (CLAY_IS_PTR(val) && !CLAY_IS_NIL(val) && val != CLAY_TRUE && val != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(val));
    }
    self->as.array.items[idx] = val;
    self->as.array.size++;
    return CLAY_VAL(self);
}

static ClayValue array_setAt(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 2 || !CLAY_IS_INT(argv[0])) return CLAY_VAL(self);
    intptr_t idx = clay_to_int(argv[0]);
    if (idx < 0 || (size_t)idx >= self->as.array.size) return CLAY_VAL(self);
    /* Release old value */
    ClayValue old = self->as.array.items[idx];
    if (CLAY_IS_PTR(old) && !CLAY_IS_NIL(old) && old != CLAY_TRUE && old != CLAY_FALSE) {
        clay_release(CLAY_OBJ(old));
    }
    ClayValue val = argv[1];
    if (CLAY_IS_PTR(val) && !CLAY_IS_NIL(val) && val != CLAY_TRUE && val != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(val));
    }
    self->as.array.items[idx] = val;
    return CLAY_VAL(self);
}

/* parallelMap: uses clay_parallel_map from thread.c */
extern ClayValue clay_parallel_map(ClayObject *arr, ClayObject *block);

static ClayValue array_parallel_map(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    if (self->as.array.size == 0) return CLAY_VAL(clay_array_new());
    return clay_parallel_map(self, CLAY_OBJ(argv[0]));
}

void clay_array_init_methods(ClayObject *proto) {
    clay_set_method(proto, "append", array_append);
    clay_set_method(proto, "push", array_append);
    clay_set_method(proto, "at", array_at);
    clay_set_method(proto, "size", array_size);
    clay_set_method(proto, "length", array_size);
    clay_set_method(proto, "each", array_each);
    clay_set_method(proto, "map", array_map);
    clay_set_method(proto, "filter", array_filter);
    clay_set_method(proto, "reduce", array_reduce);
    clay_set_method(proto, "find", array_find);
    clay_set_method(proto, "contains", array_contains);
    clay_set_method(proto, "indexOf", array_indexOf);
    clay_set_method(proto, "first", array_first);
    clay_set_method(proto, "last", array_last);
    clay_set_method(proto, "reverse", array_reverse);
    clay_set_method(proto, "sort", array_sort);
    clay_set_method(proto, "join", array_join);
    clay_set_method(proto, "removeAt", array_removeAt);
    clay_set_method(proto, "toString", array_toString);
    clay_set_method(proto, "==", array_equals);
    clay_set_method(proto, "!=", array_neq);
    clay_set_method(proto, "flatten", array_flatten);
    clay_set_method(proto, "sum", array_sum);
    clay_set_method(proto, "min", array_min);
    clay_set_method(proto, "max", array_max);
    clay_set_method(proto, "avg", array_avg);
    clay_set_method(proto, "unique", array_unique);
    clay_set_method(proto, "pop", array_pop);
    clay_set_method(proto, "shift", array_shift);
    clay_set_method(proto, "slice", array_slice);
    clay_set_method(proto, "insert", array_insert);
    clay_set_method(proto, "setAt", array_setAt);
    clay_set_method(proto, "parallelMap", array_parallel_map);
}
