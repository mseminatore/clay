/*
 * error.c - Error object and try/catch mechanism
 */
#include "clay.h"
#include <string.h>

/* Global error stack */
CLAY_THREAD_LOCAL ClayErrorStack clay_error_stack = { .head = NULL, .current_error = 0 };

/* Source location tracking */
CLAY_THREAD_LOCAL int clay_source_line = 0;
CLAY_THREAD_LOCAL const char *clay_source_file = NULL;

/* Interned source paths (a handful per program; never freed). Interning
 * happens when code is compiled or loaded, which is on the main thread. */
static const char **g_files = NULL;
static int g_file_count = 0, g_file_capacity = 0;

const char *clay_intern_file(const char *path) {
    if (!path) return NULL;
    const char *found = NULL;
    for (int i = 0; i < g_file_count && !found; i++)
        if (g_files[i] == path || strcmp(g_files[i], path) == 0) found = g_files[i];
    if (!found) {
        if (g_file_count == g_file_capacity) {
            int cap = g_file_capacity ? g_file_capacity * 2 : 16;
            const char **grown = (const char **)realloc((void *)g_files, sizeof(char *) * (size_t)cap);
            if (grown) { g_files = grown; g_file_capacity = cap; }
        }
        char *copy = strdup(path);
        if (copy && g_file_count < g_file_capacity) g_files[g_file_count++] = copy;
        found = copy;
    }
    return found;
}

/* Where the bundled bricks live (lib/), so tracebacks can show their files
 * as "stdlib/test/src/main.clay" instead of a long absolute path. Set by
 * clay's driver, and by a compiled program that uses a bundled brick. */
static char *g_lib_dir = NULL;

void clay_set_lib_dir(const char *dir) {
    free(g_lib_dir);
    g_lib_dir = NULL;
    if (!dir || !dir[0]) return;
    g_lib_dir = strdup(dir);
}

/* The rest of path after dir and its separator, or NULL if path is not
 * inside dir ('/' and '\\' count as the same) */
const char *clay_path_under(const char *dir, const char *path) {
    if (!dir || !dir[0] || !path) return NULL;
    size_t n = strlen(dir);
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) n--;
    const char *p = path;
    for (size_t i = 0; i < n; i++, p++) {
        int ds = (dir[i] == '/' || dir[i] == '\\'), ps = (*p == '/' || *p == '\\');
        if (ds ? !ps : dir[i] != *p) return NULL;
    }
    if (*p != '/' && *p != '\\') return NULL;
    while (*p == '/' || *p == '\\') p++;
    return *p ? p : NULL;
}

const char *clay_display_file(const char *path) {
    const char *rest = clay_path_under(g_lib_dir, path);
    return rest ? rest : path;
}

/* Output hook for DAP */
void (*clay_output_fn)(const char *text) = NULL;

/* Shadow call stack for stack traces */
CLAY_THREAD_LOCAL ClayCallTrace clay_call_trace = { .depth = 0 };
CLAY_THREAD_LOCAL int clay_trace_enabled = 1; /* can be disabled for benchmarking */

/* One traceback line: a place in the code and what it is in (the frame that
 * ran that code, or NULL for the top level). A place "in" a built-in is where
 * the built-in called back into Clay (each running its block, sort calling
 * <): that is the built-in's own line, already the next line, so it is left
 * out. */
static size_t trace_line(char *buf, size_t cap, size_t pos, const char *file, int line,
                         const ClayTraceFrame *in) {
    if (in && in->kind == CLAY_FRAME_CALL) return pos;
    char what[160];
    if (!in) snprintf(what, sizeof(what), "at top level");
    else if (in->kind == CLAY_FRAME_BLOCK) snprintf(what, sizeof(what), "in block");
    else snprintf(what, sizeof(what), "in %s", in->selector ? in->selector : "?");
    if (file && line > 0)
        pos += (size_t)snprintf(buf + pos, cap - pos, "  at %s:%d %s\n", clay_display_file(file), line, what);
    else if (line > 0)
        pos += (size_t)snprintf(buf + pos, cap - pos, "  at line %d %s\n", line, what);
    else
        pos += (size_t)snprintf(buf + pos, cap - pos, "  %s\n", what);
    return pos;
}

/* Create an Error object with message and type slots */
ClayObject *clay_error_new(const char *message, const char *type) {
    ClayObject *err = clay_clone(clay_Error);
    clay_set_slot_take(err, "_message", CLAY_VAL(clay_string_new(message)));
    clay_set_slot_take(err, "_type", CLAY_VAL(clay_string_new(type)));
    if (clay_source_line > 0) {
        clay_set_slot(err, "_line", clay_int(clay_source_line));
    }
    if (clay_source_file) {
        clay_set_slot_take(err, "_file", CLAY_VAL(clay_string_new(clay_display_file(clay_source_file))));
    }

    /* Capture the stack trace from the shadow call stack, newest first: one
     * line per place in the code, "at <file>:<line> in <what>", where <what>
     * is the code that place is in: the method or block that the next older
     * frame ran (its kind), or "top level". The
     * first line is where the error was made, when the newest call ran Clay
     * code (otherwise that call made the error, and its own line says where).
     * The shadow stack keeps the first CLAY_MAX_TRACE_DEPTH calls; deeper
     * ones (the newest) are only counted, which the first line says. */
    if (clay_call_trace.depth > 0) {
        int depth = clay_call_trace.depth;
        size_t cap = 128 + (clay_source_file ? strlen(clay_source_file) : 0);
        for (int i = 0; i < depth; i++) {
            ClayTraceFrame *f = &clay_call_trace.frames[i];
            cap += (f->file ? strlen(f->file) : 0) + (f->selector ? strlen(f->selector) : 1) + 48;
        }
        char *trace_buf = (char *)malloc(cap);
        if (trace_buf) {
            size_t pos = 0;
            if (clay_call_trace.overflow > 0)
                pos += (size_t)snprintf(trace_buf + pos, cap - pos,
                    "  ... %d more calls not recorded\n", clay_call_trace.overflow);
            else if (clay_call_trace.frames[depth - 1].kind != CLAY_FRAME_CALL && clay_source_line > 0)
                pos = trace_line(trace_buf, cap, pos, clay_source_file, clay_source_line,
                                 &clay_call_trace.frames[depth - 1]);
            for (int i = depth - 1; i >= 0; i--) {
                ClayTraceFrame *f = &clay_call_trace.frames[i];
                pos = trace_line(trace_buf, cap, pos, f->file, f->line,
                                 i > 0 ? &clay_call_trace.frames[i - 1] : NULL);
            }
            if (pos > 0) {
                if (trace_buf[pos - 1] == '\n') trace_buf[--pos] = '\0';     /* no trailing newline */
                clay_set_slot_take(err, "_trace", CLAY_VAL(clay_string_new(trace_buf)));
            }
            free(trace_buf);
        }
    }

    return err;
}

/* --- Tracebacks for people ---
 *
 * An error's _trace (Error stackTrace) is every recorded call, newest first.
 * What is printed when an error ends a program is that list made readable:
 *   - a block's line is left out when the next line is the same place (a
 *     block written on the line that runs it: `(x > 1) ifTrue { raise(...) }`
 *     would say line 4 twice, "in block" and "in check");
 *   - the same line repeated (a recursion) is shown once with a count;
 *   - a long trace keeps its first and last lines, the newest, where it went
 *     wrong, and the oldest, where it started, and says how many it left out. */
#define TB_KEEP_HEAD 12
#define TB_KEEP_TAIL 12

typedef struct { const char *p; size_t len; } TbLine;

/* "  at <place> in block" followed by a line at the same place */
static int tb_is_redundant(const TbLine *l, const TbLine *next) {
    static const char tail[] = " in block";
    size_t tl = sizeof(tail) - 1;
    if (!next || l->len <= tl || memcmp(l->p + l->len - tl, tail, tl) != 0) return 0;
    size_t place = l->len - tl;                  /* "  at <file>:<line>" */
    return next->len > place && memcmp(next->p, l->p, place) == 0 && next->p[place] == ' ';
}

/* The trace as printed (every line ends in a newline), or NULL if empty.
 * The caller frees it. */
char *clay_traceback_format(const char *trace) {
    if (!trace || !*trace) return NULL;
    size_t total = strlen(trace);
    int nlines = 1;
    for (const char *c = trace; *c; c++) if (*c == '\n') nlines++;
    TbLine *all = (TbLine *)malloc(sizeof(TbLine) * (size_t)nlines * 2 + sizeof(TbLine));
    if (!all) return NULL;
    TbLine *kept = all + nlines;

    int n = 0;
    const char *start = trace;
    for (const char *c = trace; ; c++) {
        if (*c == '\n' || *c == '\0') {
            if (c > start) { all[n].p = start; all[n].len = (size_t)(c - start); n++; }
            start = c + 1;
            if (*c == '\0') break;
        }
    }
    int k = 0;
    for (int i = 0; i < n; i++)
        if (!tb_is_redundant(&all[i], i + 1 < n ? &all[i + 1] : NULL)) kept[k++] = all[i];

    /* runs of the same line, then the cap: build the output in one pass */
    size_t cap = total + 256 + (size_t)k * 48;
    char *out = (char *)malloc(cap);
    if (!out) { free(all); return NULL; }
    size_t pos = 0;
    int shown = 0, omitted = 0;
    /* count the lines the output would have (runs collapsed) to place the cap */
    int outlines = 0;
    for (int i = 0; i < k; ) {
        int j = i + 1;
        while (j < k && kept[j].len == kept[i].len && memcmp(kept[j].p, kept[i].p, kept[i].len) == 0) j++;
        outlines += (j - i > 2) ? 2 : (j - i);
        i = j;
    }
    int line_no = 0;
    for (int i = 0; i < k; ) {
        int j = i + 1;
        while (j < k && kept[j].len == kept[i].len && memcmp(kept[j].p, kept[i].p, kept[i].len) == 0) j++;
        int run = j - i;
        int emit = run > 2 ? 2 : run;       /* the line, then (for a longer run) the count */
        for (int e = 0; e < emit; e++, line_no++) {
            int skip = outlines > TB_KEEP_HEAD + TB_KEEP_TAIL &&
                       line_no >= TB_KEEP_HEAD && line_no < outlines - TB_KEEP_TAIL;
            if (skip) { omitted++; continue; }     /* the "not shown" line goes in where they end */
            if (omitted > 0 && shown == TB_KEEP_HEAD) {
                pos += (size_t)snprintf(out + pos, cap - pos, "  ... %d more lines not shown ...\n", omitted);
                omitted = 0;
            }
            if (run > 2 && e == 1)
                pos += (size_t)snprintf(out + pos, cap - pos, "  [previous line repeated %d more times]\n", run - 1);
            else
                pos += (size_t)snprintf(out + pos, cap - pos, "%.*s\n", (int)kept[i].len, kept[i].p);
            shown++;
        }
        i = j;
    }
    free(all);
    out[pos] = '\0';
    return out;
}

const char *clay_error_kind(ClayValue err) {
    if (CLAY_IS_PTR(err) && !CLAY_IS_NIL(err)) {
        ClayValue t = clay_get_slot(CLAY_OBJ(err), "_type");
        if (CLAY_IS_TYPE(t, CLAY_TYPE_STRING) && clay_string_data(CLAY_OBJ(t))[0])
            return clay_string_data(CLAY_OBJ(t));
    }
    return "Error";
}

/* The traceback of an Error object as printed (see clay_traceback_format), or
 * NULL if it has none or is not an Error. The caller frees it. */
char *clay_error_traceback(ClayValue err) {
    if (CLAY_IS_NIL(err) || !CLAY_IS_PTR(err)) return NULL;
    ClayValue trace = clay_get_slot(CLAY_OBJ(err), "_trace");
    if (!CLAY_IS_TYPE(trace, CLAY_TYPE_STRING)) return NULL;
    return clay_traceback_format(clay_string_data(CLAY_OBJ(trace)));
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
    /* No VM yet: a frame left zeroed restores "no VM state" should one be
     * created before it unwinds (the VM prototype starts one on demand) */
    if (clay_try_hooks) clay_try_hooks->save(frame->engine_state);
    else memset(frame->engine_state, 0, sizeof(frame->engine_state));
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
        CLAY_LONGJMP(frame->buf, 1);
    }
    /* No try frame — print and abort */
    if (CLAY_IS_PTR(error) && !CLAY_IS_NIL(error)) {
        ClayObject *err = CLAY_OBJ(error);
        ClayValue msg = clay_get_slot(err, "_message");
        ClayValue file = clay_get_slot(err, "_file");
        ClayValue line = clay_get_slot(err, "_line");
        if (!CLAY_IS_NIL(file) && !CLAY_IS_NIL(line)) {
            fprintf(stderr, "%s:%lld: ", clay_string_data(CLAY_OBJ(file)), (long long)clay_to_int(line));
        }
        /* the same report `clay` makes for an error that ends a program */
        if (!CLAY_IS_NIL(msg)) {
            fprintf(stderr, "%s: %s\n", clay_error_kind(error), clay_string_data(CLAY_OBJ(msg)));
        } else {
            fprintf(stderr, "%s\n", clay_error_kind(error));
        }
        char *tb = clay_error_traceback(error);
        if (tb) { fputs(tb, stderr); free(tb); }
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
    const char *f = CLAY_IS_NIL(file) ? "" : clay_string_data(CLAY_OBJ(file));
    const char *tr = CLAY_IS_NIL(trace) ? NULL : clay_string_data(CLAY_OBJ(trace));
    size_t cap = strlen(f) + strlen(t) + strlen(m) + (tr ? strlen(tr) : 0) + 64;
    char *buf = (char *)malloc(cap);
    if (!buf) return CLAY_VAL(clay_string_new(m));
    int pos;
    if (!CLAY_IS_NIL(file) && !CLAY_IS_NIL(line)) {
        pos = snprintf(buf, cap, "%s:%ld: %s: %s", f, (long)clay_to_int(line), t, m);
    } else {
        pos = snprintf(buf, cap, "%s: %s", t, m);
    }
    if (tr) snprintf(buf + pos, cap - (size_t)pos, "\n%s", tr);
    ClayValue result = CLAY_VAL(clay_string_new(buf));
    free(buf);
    return result;
}

static ClayValue error_stackTrace(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    ClayValue trace = clay_get_slot(self, "_trace");
    if (CLAY_IS_NIL(trace)) return CLAY_VAL(clay_string_new(""));
    clay_retain_val(trace);
    return trace;
}

/* Error traceback() — the stack trace as printed when an error ends a program:
 * repeats counted, a long trace cut in the middle. "" if there is none.
 * (stackTrace() is every recorded call.) */
static ClayValue error_traceback(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char *tb = clay_error_traceback(CLAY_VAL(self));
    ClayValue result = CLAY_VAL(clay_string_new(tb ? tb : ""));
    free(tb);
    return result;
}

void clay_error_init_methods(ClayObject *proto) {
    clay_set_method(proto, "message", error_message);
    clay_set_method(proto, "type", error_type);
    clay_set_method(proto, "line", error_line);
    clay_set_method(proto, "file", error_file);
    clay_set_method(proto, "stackTrace", error_stackTrace);
    clay_set_method(proto, "traceback", error_traceback);
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
    /* Only its method's frame sees the signal (catch handlers re-raise it):
     * no stack trace or location to record */
    ClayObject *sig = clay_clone(clay_Error);
    clay_set_slot_take(sig, "_message", CLAY_VAL(clay_string_new("non-local return")));
    clay_set_slot_take(sig, "_type", CLAY_VAL(clay_string_new("ReturnSignal")));
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
