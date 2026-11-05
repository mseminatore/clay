/*
 * error.c - Error object and try/catch mechanism
 */
#include "clay.h"

/* Global error stack */
CLAY_THREAD_LOCAL ClayErrorStack clay_error_stack = { .head = NULL, .current_error = 0 };

/* Source location tracking */
CLAY_THREAD_LOCAL int clay_source_line = 0;
CLAY_THREAD_LOCAL const char *clay_source_file = NULL;

/* Output hook for DAP */
void (*clay_output_fn)(const char *text) = NULL;

/* Shadow call stack for stack traces */
CLAY_THREAD_LOCAL ClayCallTrace clay_call_trace = { .depth = 0 };
CLAY_THREAD_LOCAL int clay_trace_enabled = 1; /* can be disabled for benchmarking */

/* Create an Error object with message and type slots */
ClayObject *clay_error_new(const char *message, const char *type) {
    ClayObject *err = clay_clone(clay_Error);
    clay_set_slot_take(err, "_message", CLAY_VAL(clay_string_new(message)));
    clay_set_slot_take(err, "_type", CLAY_VAL(clay_string_new(type)));
    if (clay_source_line > 0) {
        clay_set_slot(err, "_line", clay_int(clay_source_line));
    }
    if (clay_source_file) {
        clay_set_slot_take(err, "_file", CLAY_VAL(clay_string_new(clay_source_file)));
    }

    /* Capture stack trace from shadow call stack */
    if (clay_call_trace.depth > 0) {
        char trace_buf[2048];
        int pos = 0;
        /* Walk frames from most recent to oldest */
        for (int i = clay_call_trace.depth - 1; i >= 0 && pos < 1900; i--) {
            ClayTraceFrame *f = &clay_call_trace.frames[i];
            if (f->file && f->line > 0) {
                pos += snprintf(trace_buf + pos, sizeof(trace_buf) - (size_t)pos,
                    "  at %s:%d in %s()\n", f->file, f->line, f->selector ? f->selector : "?");
            } else if (f->line > 0) {
                pos += snprintf(trace_buf + pos, sizeof(trace_buf) - (size_t)pos,
                    "  at line %d in %s()\n", f->line, f->selector ? f->selector : "?");
            } else if (f->selector) {
                pos += snprintf(trace_buf + pos, sizeof(trace_buf) - (size_t)pos,
                    "  in %s()\n", f->selector);
            }
        }
        if (pos > 0) {
            /* Remove trailing newline */
            if (pos > 0 && trace_buf[pos - 1] == '\n') trace_buf[--pos] = '\0';
            clay_set_slot_take(err, "_trace", CLAY_VAL(clay_string_new(trace_buf)));
        }
    }

    return err;
}

/* Raise an error: longjmp to nearest try frame, or abort */
const ClayTryHooks *clay_try_hooks = NULL;

CLAY_THREAD_LOCAL ClayUnwindList *clay_unwind_list = NULL;
static CLAY_THREAD_LOCAL ClayUnwindList g_thread_unwind = { NULL, 0, 0 };

static ClayUnwindList *unwind_list(void) {
    return clay_unwind_list ? clay_unwind_list : &g_thread_unwind;
}

void clay_unwind_push(ClayObject *obj) {
    ClayUnwindList *l = unwind_list();
    if (l->count == l->capacity) {
        int cap = l->capacity ? l->capacity * 2 : 64;
        ClayObject **items = (ClayObject **)realloc(l->items, sizeof(ClayObject *) * (size_t)cap);
        if (!items) { fprintf(stderr, "clay: out of memory\n"); exit(1); }
        l->items = items;
        l->capacity = cap;
    }
    l->items[l->count++] = obj;
}

void clay_unwind_pop(void) {
    unwind_list()->count--;
}

static void unwind_to(ClayUnwindList *l, int depth) {
    while (l->count > depth) clay_release(l->items[--l->count]);
}

void clay_unwind_list_free(ClayUnwindList *list) {
    unwind_to(list, 0);
    free(list->items);
    list->items = NULL;
    list->capacity = 0;
}

void clay_try_save_state(ClayTryFrame *frame) {
    frame->slot_method_self = clay_slot_method_self;
    frame->trace_depth = clay_call_trace.depth;
    frame->trace_overflow = clay_call_trace.overflow;
    frame->return_id = 0;
    frame->unwind_depth = unwind_list()->count;
    if (clay_try_hooks) clay_try_hooks->save(frame->engine_state);
}

static void clay_try_restore_state(ClayTryFrame *frame) {
    clay_slot_method_self = frame->slot_method_self;
    clay_call_trace.depth = frame->trace_depth;
    clay_call_trace.overflow = frame->trace_overflow;
    if (clay_try_hooks) clay_try_hooks->restore(frame->engine_state);
    unwind_to(unwind_list(), frame->unwind_depth);
}

void clay_raise(ClayValue error) {
    if (clay_error_stack.head != NULL) {
        clay_error_stack.current_error = error;
        ClayTryFrame *frame = clay_error_stack.head;
        clay_error_stack.head = frame->prev;
        /* Undo everything the frames we are about to skip had changed */
        clay_try_restore_state(frame);
        longjmp(frame->buf, 1);
    }
    /* No try frame — print and abort */
    if (CLAY_IS_PTR(error) && !CLAY_IS_NIL(error)) {
        ClayObject *err = CLAY_OBJ(error);
        ClayValue msg = clay_get_slot(err, "_message");
        ClayValue file = clay_get_slot(err, "_file");
        ClayValue line = clay_get_slot(err, "_line");
        ClayValue trace = clay_get_slot(err, "_trace");
        if (!CLAY_IS_NIL(file) && !CLAY_IS_NIL(line)) {
            fprintf(stderr, "%s:%lld: ", clay_string_data(CLAY_OBJ(file)), (long long)clay_to_int(line));
        }
        if (!CLAY_IS_NIL(msg)) {
            fprintf(stderr, "error: %s\n", clay_string_data(CLAY_OBJ(msg)));
        } else {
            fprintf(stderr, "error\n");
        }
        if (!CLAY_IS_NIL(trace)) {
            fprintf(stderr, "%s\n", clay_string_data(CLAY_OBJ(trace)));
        }
    } else {
        fprintf(stderr, "clay: unhandled error\n");
    }
    exit(1);
}

/* Error methods */
static ClayValue error_message(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue result = clay_get_slot(self, "_message");
    clay_retain_val(result);
    return result;
}

static ClayValue error_type(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue result = clay_get_slot(self, "_type");
    clay_retain_val(result);
    return result;
}

static ClayValue error_line(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue result = clay_get_slot(self, "_line");
    clay_retain_val(result);
    return result;
}

static ClayValue error_file(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue result = clay_get_slot(self, "_file");
    clay_retain_val(result);
    return result;
}

static ClayValue error_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue type = clay_get_slot(self, "_type");
    ClayValue msg = clay_get_slot(self, "_message");
    ClayValue file = clay_get_slot(self, "_file");
    ClayValue line = clay_get_slot(self, "_line");
    ClayValue trace = clay_get_slot(self, "_trace");
    const char *t = CLAY_IS_NIL(type) ? "Error" : clay_string_data(CLAY_OBJ(type));
    const char *m = CLAY_IS_NIL(msg) ? "" : clay_string_data(CLAY_OBJ(msg));
    char buf[2560];
    int pos;
    if (!CLAY_IS_NIL(file) && !CLAY_IS_NIL(line)) {
        pos = snprintf(buf, sizeof(buf), "%s:%ld: %s: %s",
                 clay_string_data(CLAY_OBJ(file)), (long)clay_to_int(line), t, m);
    } else {
        pos = snprintf(buf, sizeof(buf), "%s: %s", t, m);
    }
    if (!CLAY_IS_NIL(trace) && pos < (int)sizeof(buf) - 2) {
        snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\n%s",
                 clay_string_data(CLAY_OBJ(trace)));
    }
    return CLAY_VAL(clay_string_new(buf));
}

static ClayValue error_stackTrace(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue trace = clay_get_slot(self, "_trace");
    if (CLAY_IS_NIL(trace)) return CLAY_VAL(clay_string_new(""));
    clay_retain_val(trace);
    return trace;
}

void clay_error_init_methods(ClayObject *proto) {
    clay_set_method(proto, "message", error_message);
    clay_set_method(proto, "type", error_type);
    clay_set_method(proto, "line", error_line);
    clay_set_method(proto, "file", error_file);
    clay_set_method(proto, "stackTrace", error_stackTrace);
    clay_set_method(proto, "toString", error_toString);
}

/* --- Argument helpers (see clay.h) --- */

void clay_raise_argument_error(const char *who, const char *problem) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%s: %s", who, problem);
    clay_raise(CLAY_VAL(clay_error_new(msg, "ArgumentError")));
}

static void raise_arg_type(const char *who, int i, const char *expected) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%s: argument %d must be %s", who, i + 1, expected);
    clay_raise(CLAY_VAL(clay_error_new(msg, "TypeError")));
}

static int arg_is_number(ClayValue v) {
    return CLAY_IS_INT(v) || CLAY_IS_FLOAT(v);
}

float clay_arg_float(int argc, ClayValue *argv, int i, const char *who) {
    if (i >= argc) { clay_raise_argument_error(who, "missing argument"); return 0.0f; }
    if (!arg_is_number(argv[i])) { raise_arg_type(who, i, "a Number"); return 0.0f; }
    return clay_to_float(argv[i]);
}

intptr_t clay_arg_integer(int argc, ClayValue *argv, int i, const char *who) {
    if (i >= argc) { clay_raise_argument_error(who, "missing argument"); return 0; }
    if (CLAY_IS_INT(argv[i])) return clay_to_int(argv[i]);
    if (CLAY_IS_FLOAT(argv[i])) return clay_float_to_int(clay_to_float(argv[i]));
    raise_arg_type(who, i, "an integer");
    return 0;
}

const char *clay_arg_string(int argc, ClayValue *argv, int i, const char *who) {
    if (i >= argc) { clay_raise_argument_error(who, "missing argument"); return ""; }
    if (!CLAY_IS_TYPE(argv[i], CLAY_TYPE_STRING)) { raise_arg_type(who, i, "a String"); return ""; }
    return clay_string_data(CLAY_OBJ(argv[i]));
}

void clay_raise_arity(const char *method, int expected, int got) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%s takes %d argument%s, but %d %s given",
             method, expected, expected == 1 ? "" : "s", got, got == 1 ? "was" : "were");
    clay_raise(CLAY_VAL(clay_error_new(msg, "ArgumentError")));
}

/* --- Control signals and non-local return --- */

static const char *error_type_name(ClayValue error) {
    if (!CLAY_IS_OBJ(error)) return NULL;
    ClayValue t = clay_get_slot(CLAY_OBJ(error), "_type");
    return CLAY_IS_TYPE(t, CLAY_TYPE_STRING) ? clay_string_data(CLAY_OBJ(t)) : NULL;
}

int clay_is_control_signal(ClayValue error) {
    const char *t = error_type_name(error);
    return t && (strcmp(t, "BreakSignal") == 0 || strcmp(t, "ContinueSignal") == 0 ||
                 strcmp(t, "ReturnSignal") == 0);
}

int clay_loop_signal(ClayValue error) {
    const char *t = error_type_name(error);
    int sig = 0;
    if (t && strcmp(t, "BreakSignal") == 0) sig = CLAY_SIGNAL_BREAK;
    else if (t && strcmp(t, "ContinueSignal") == 0) sig = CLAY_SIGNAL_CONTINUE;
    if (sig) clay_release_val(error);
    return sig;
}

static CLAY_THREAD_LOCAL intptr_t g_return_id = 0;

ClayValue clay_return_id_next(void) {
    if (++g_return_id > CLAY_INT_MAX) g_return_id = 1;
    return clay_int(g_return_id);
}

void clay_raise_return(ClayValue value, ClayValue id) {
    /* The method call is live only while its try frame is on the stack. If
     * the block outlived its method, raise an ordinary (catchable) error. */
    ClayTryFrame *f = clay_error_stack.head;
    while (f && f->return_id != id) f = f->prev;
    if (!f) {
        clay_release_val(value);
        ClayObject *err = clay_error_new("return from a method that has already returned",
                                         "RuntimeError");
        clay_raise(CLAY_VAL(err));
    }
    ClayObject *sig = clay_error_new("non-local return", "ReturnSignal");
    clay_set_slot(sig, "_value", value);
    clay_release_val(value);          /* the signal now holds it */
    clay_set_slot(sig, "_id", id);
    clay_raise(CLAY_VAL(sig));
}

int clay_return_matches(ClayValue error, ClayValue id, ClayValue *value_out) {
    const char *t = error_type_name(error);
    if (!t || strcmp(t, "ReturnSignal") != 0) return 0;
    if (clay_get_slot(CLAY_OBJ(error), "_id") != id) return 0;
    ClayValue v = clay_get_slot(CLAY_OBJ(error), "_value");
    clay_retain_val(v);      /* the caller owns the returned value */
    *value_out = v;
    clay_release_val(error); /* the signal is consumed */
    return 1;
}
