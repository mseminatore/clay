#ifndef CLAY_H
#define CLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hash.h"

/*
 * ClayValue - tagged union type
 *
 * Low bit of pointer == 1 means small integer (value >> 1).
 * Otherwise it's a pointer to a ClayObject.
 * Special singletons: clay_nil, clay_true, clay_false.
 */
typedef uintptr_t ClayValue;

/* Tag checks — 2-bit tagging scheme:
 *   (v & 3) == 0  →  pointer or nil
 *   (v & 3) == 1  →  tagged integer
 *   (v & 3) == 3  →  tagged float (32-bit IEEE 754 bits in upper word)
 */
#define CLAY_TAG_INT    1
#define CLAY_TAG_FLOAT  3
#define CLAY_IS_INT(v)  (((v) & 3) == CLAY_TAG_INT)
#define CLAY_IS_PTR(v)  (((v) & 1) == 0)
#define CLAY_IS_TAGGED_FLOAT(v) (((v) & 3) == CLAY_TAG_FLOAT)

/* Integer tagging */
#define CLAY_INT_MAX  (INTPTR_MAX >> 2)
#define CLAY_INT_MIN  (INTPTR_MIN >> 2)

ClayValue clay_int(intptr_t n);
intptr_t  clay_to_int(ClayValue v);
int       clay_is_int(ClayValue v);

/* Forward declaration */
typedef struct ClayObject ClayObject;

/* Float helpers (CLAY_IS_FLOAT macro defined after struct ClayObject) */
ClayObject *clay_float_new(float v);
float      clay_to_float(ClayValue v);
ClayValue  clay_float_tag(float v);
float      clay_float_untag(ClayValue v);

/* Method function signature: receiver, argc, argv -> return value */
typedef ClayValue (*ClayMethodFunc)(ClayObject *self, int argc, ClayValue *argv);

/* Method entry stored in the methods table */
typedef struct {
    ClayMethodFunc func;
    void *context;       /* optional data for the method (used by BC compiler) */
} ClayMethod;

/*
 * ClayObject - the core object structure
 *
 * Every object has:
 *   - parent: prototype chain (NULL for root Object)
 *   - slots: hash table of named ClayValue slots
 *   - methods: hash table of named ClayMethod entries
 *   - refcount: reference counting for memory management
 *   - type: internal type tag for special objects (string, array, block)
 */
typedef enum {
    CLAY_TYPE_OBJECT = 0,
    CLAY_TYPE_STRING,
    CLAY_TYPE_ARRAY,
    CLAY_TYPE_BLOCK,
    CLAY_TYPE_DICT,
    CLAY_TYPE_FLOAT
} ClayType;

struct ClayObject {
    ClayObject *parent;
    HashTable  *slots;
    HashTable  *methods;
    int         refcount;
    ClayType    type;

    /* Type-specific data */
    union {
        struct {
            char  *data;
            size_t length;
        } string;

        struct {
            ClayValue *items;
            size_t     size;
            size_t     capacity;
        } array;

        struct {
            ClayMethodFunc func;
            ClayValue     *env;
            int            env_size;
            int            env_are_cells; /* 1 = env entries are ClayCell pointers */
        } block;

        struct {
            HashTable *map;
        } dict;

        struct {
            float value;
        } number_float;
    } as;
};

/* Convert between ClayValue and ClayObject* */
#define CLAY_OBJ(v)     ((ClayObject*)(v))
#define CLAY_VAL(obj)   ((ClayValue)(uintptr_t)(obj))

/* Nil value */
#define CLAY_NIL        ((ClayValue)0)
#define CLAY_IS_NIL(v)  ((v) == CLAY_NIL)

/* Float type check: tagged float OR boxed float object */
#define CLAY_IS_FLOAT(v) (CLAY_IS_TAGGED_FLOAT(v) || \
    (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v) && CLAY_OBJ(v)->type == CLAY_TYPE_FLOAT))

/* Boolean singletons */
extern ClayObject clay_true_obj;
extern ClayObject clay_false_obj;
#define CLAY_TRUE   CLAY_VAL(&clay_true_obj)
#define CLAY_FALSE  CLAY_VAL(&clay_false_obj)
#define CLAY_BOOL(c) ((c) ? CLAY_TRUE : CLAY_FALSE)
#define CLAY_IS_TRUE(v) ((v) == CLAY_TRUE)
#define CLAY_IS_FALSE(v) ((v) == CLAY_FALSE)

/* Truthy: everything except nil, false, integer 0, and float 0.0 */
#define CLAY_IS_TRUTHY(v) ((v) != CLAY_NIL && (v) != CLAY_FALSE && (v) != clay_int(0) && (v) != CLAY_TAG_FLOAT)

/* --- Object model --- */
ClayObject *clay_object_new(void);
ClayObject *clay_clone(ClayObject *proto);
void        clay_set_slot(ClayObject *obj, const char *name, ClayValue value);
ClayValue   clay_get_slot(ClayObject *obj, const char *name);
int         clay_has_slot(ClayObject *obj, const char *name);
void        clay_set_method(ClayObject *obj, const char *name, ClayMethodFunc func);
void        clay_set_method_ctx(ClayObject *obj, const char *name, ClayMethodFunc func, void *context);
ClayMethodFunc clay_lookup_method(ClayObject *obj, const char *name);

/* Full method lookup (returns ClayMethod* with context, uses global cache) */
ClayMethod *clay_cached_lookup(ClayObject *obj, const char *selector);

/* Invoke a method, setting context before call */
ClayValue clay_invoke(ClayMethod *m, ClayObject *obj, int argc, ClayValue *argv);

/* Context pointer set by dispatch before calling a method (for BC compiler) */
extern void *clay_method_context;

/* When non-NULL, block trampolines bind this as 'self' (for slot-as-method dispatch) */
extern ClayObject *clay_slot_method_self;

/* --- String interning --- */
const char *clay_intern(const char *name);

/* --- Method cache --- */
void clay_method_cache_clear(void);
void clay_method_cache_invalidate(void *methods_ptr);

/* --- Memory management --- */
ClayObject *clay_retain(ClayObject *obj);
void        clay_release(ClayObject *obj);

/* Retain a ClayValue if it's a heap-allocated object (safe no-op for ints, floats, nil, booleans) */
static inline void clay_retain_val(ClayValue v) {
    if (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v) && v != CLAY_TRUE && v != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(v));
    }
}

/* Release a ClayValue if it's a heap-allocated object (safe no-op for ints, floats, nil, booleans) */
static inline void clay_release_val(ClayValue v) {
    if (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v) && v != CLAY_TRUE && v != CLAY_FALSE) {
        clay_release(CLAY_OBJ(v));
    }
}

/* Memory diagnostics counters */
extern size_t clay_alloc_count;
extern size_t clay_dealloc_count;

/* --- Message dispatch --- */
ClayValue   clay_send_val(ClayValue receiver, const char *selector, int argc, ClayValue *argv);
ClayValue   clay_send_super(ClayValue receiver, ClayObject *defining_obj,
                            const char *selector, int argc, ClayValue *argv);

/* Legacy API - still works for object pointers */
#define clay_send(recv, sel, ac, av) clay_send_val(CLAY_VAL(recv), sel, ac, av)

/* --- Bootstrap / core prototypes --- */
extern ClayObject *clay_Object;
extern ClayObject *clay_Number;
extern ClayObject *clay_String;
extern ClayObject *clay_Array;
extern ClayObject *clay_Block;
extern ClayObject *clay_Nil;
extern ClayObject *clay_Boolean;
extern ClayObject *clay_Error;
extern ClayObject *clay_Dict;
extern ClayObject *clay_Math;
extern ClayObject *clay_Console;
extern ClayObject *clay_File;
extern ClayObject *clay_System;
extern ClayObject *clay_FFI;
extern ClayObject *clay_Date;
extern ClayObject *clay_Http;
extern ClayObject *clay_Regex;
extern ClayObject *clay_Fiber;
extern ClayObject *clay_Db;
extern ClayObject *clay_Range;

/* Loop control signal singletons(Error clones used by break/continue) */
extern ClayObject *clay_BreakSignal;
extern ClayObject *clay_ContinueSignal;

/* Fiber context cleanup (called from memory.c when fiber objects are freed) */
void clay_fiber_context_free(intptr_t ctx_ptr);

/* Regex search — public API for String convenience methods */
int clay_regex_search(const char *pat, const char *text, int *match_start, int *match_len);

void clay_bootstrap(void);
void clay_shutdown(void);
void clay_set_args(int argc, char **argv);

/* --- String helpers --- */
ClayObject *clay_string_new(const char *str);
ClayObject *clay_string_new_len(const char *str, size_t len);
const char *clay_string_data(ClayObject *obj);

/* --- Array helpers --- */
ClayObject *clay_array_new(void);
void        clay_array_append(ClayObject *arr, ClayValue val);
ClayValue   clay_array_at(ClayObject *arr, size_t index);
size_t      clay_array_size(ClayObject *arr);

/* --- Block helpers --- */
ClayObject *clay_block_new(ClayMethodFunc func, ClayValue *env, int env_size);
ClayObject *clay_block_new_cells(ClayMethodFunc func, ClayValue *env, int env_size);

/* --- Dict helpers --- */
ClayObject *clay_dict_new(void);
void        clay_dict_set(ClayObject *dict, const char *key, ClayValue val);
ClayValue   clay_dict_get(ClayObject *dict, const char *key);
int         clay_dict_has(ClayObject *dict, const char *key);
void        clay_dict_remove(ClayObject *dict, const char *key);
size_t      clay_dict_size(ClayObject *dict);

/* --- Debug --- */
void        clay_inspect(ClayObject *obj);

/* --- Error handling (setjmp/longjmp) --- */
#include <setjmp.h>

/* Try/catch frames — linked list on the C stack (no fixed limit) */
typedef struct ClayTryFrame {
    jmp_buf buf;
    struct ClayTryFrame *prev;
} ClayTryFrame;

typedef struct {
    ClayTryFrame *head;          /* linked list of active try frames */
    ClayValue current_error;     /* the error being raised */
} ClayErrorStack;

extern ClayErrorStack clay_error_stack;

/* Push/pop try frames — caller must declare a local ClayTryFrame variable */
#define clay_try_push(frame) \
    ((frame)->prev = clay_error_stack.head, \
     clay_error_stack.head = (frame), \
     setjmp((frame)->buf))
#define clay_try_pop() \
    (clay_error_stack.head = clay_error_stack.head->prev)

/* Raise an error — longjmps to nearest try frame, or aborts */
void clay_raise(ClayValue error);

/* Source location tracking for error messages */
extern int clay_source_line;
extern const char *clay_source_file;

/* Output hook — when set, all println/print calls route through this
 * instead of writing to stdout. Used by DAP to send output events. */
extern void (*clay_output_fn)(const char *text);

/* Create an Error object with message and type */
ClayObject *clay_error_new(const char *message, const char *type);

/* --- Call trace (shadow call stack for stack traces) --- */
#define CLAY_MAX_TRACE_DEPTH 64

typedef struct {
    const char *file;
    int line;
    const char *selector;
} ClayTraceFrame;

typedef struct {
    ClayTraceFrame frames[CLAY_MAX_TRACE_DEPTH];
    int depth;
} ClayCallTrace;

extern ClayCallTrace clay_call_trace;

static inline void clay_trace_push(const char *file, int line, const char *sel) {
    if (clay_call_trace.depth < CLAY_MAX_TRACE_DEPTH) {
        ClayTraceFrame *f = &clay_call_trace.frames[clay_call_trace.depth++];
        f->file = file;
        f->line = line;
        f->selector = sel;
    }
}

static inline void clay_trace_pop(void) {
    if (clay_call_trace.depth > 0) clay_call_trace.depth--;
}

/* --- Cell (heap-allocated box for by-reference capture in C codegen) --- */
typedef struct {
    int       refcount;
    ClayValue value;
} ClayCell;

ClayCell   *clay_cell_new(ClayValue initial);
void        clay_cell_retain(ClayCell *cell);
void        clay_cell_release(ClayCell *cell);

#ifdef __cplusplus
}
#endif

#endif /* CLAY_H */
