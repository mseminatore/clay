/*
 * error.c - Error object and try/catch mechanism
 */
#include "clay.h"

/* Global error stack */
ClayErrorStack clay_error_stack = { .head = NULL, .current_error = 0 };

/* Source location tracking */
int clay_source_line = 0;
const char *clay_source_file = NULL;

/* Output hook for DAP */
void (*clay_output_fn)(const char *text) = NULL;

/* Shadow call stack for stack traces */
ClayCallTrace clay_call_trace = { .depth = 0 };
int clay_trace_enabled = 1; /* can be disabled for benchmarking */

/* Create an Error object with message and type slots */
ClayObject *clay_error_new(const char *message, const char *type) {
    ClayObject *err = clay_clone(clay_Error);
    clay_set_slot(err, "_message", CLAY_VAL(clay_string_new(message)));
    clay_set_slot(err, "_type", CLAY_VAL(clay_string_new(type)));
    if (clay_source_line > 0) {
        clay_set_slot(err, "_line", clay_int(clay_source_line));
    }
    if (clay_source_file) {
        clay_set_slot(err, "_file", CLAY_VAL(clay_string_new(clay_source_file)));
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
            clay_set_slot(err, "_trace", CLAY_VAL(clay_string_new(trace_buf)));
        }
    }

    return err;
}

/* Raise an error: longjmp to nearest try frame, or abort */
void clay_raise(ClayValue error) {
    if (clay_error_stack.head != NULL) {
        clay_error_stack.current_error = error;
        ClayTryFrame *frame = clay_error_stack.head;
        clay_error_stack.head = frame->prev;
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
            fprintf(stderr, "%s:%ld: ", clay_string_data(CLAY_OBJ(file)), (long)clay_to_int(line));
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
