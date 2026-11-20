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

#ifdef _MSC_VER
#ifndef strdup
#define strdup _strdup
#endif
#endif

/* ------------------------------------------------------------------ */
/*  Thread-safety support (opt-in via -DCLAY_THREAD_SAFE)              */
/* ------------------------------------------------------------------ */

#ifdef CLAY_THREAD_SAFE
  /* Atomic refcount operations */
  #ifdef _MSC_VER
    #include <intrin.h>
    #define CLAY_ATOMIC_INC(p) _InterlockedIncrement((volatile long *)(p))
    #define CLAY_ATOMIC_DEC(p) _InterlockedDecrement((volatile long *)(p))
  #else
    #define CLAY_ATOMIC_INC(p) (__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
    #define CLAY_ATOMIC_DEC(p) (__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST))
  #endif

  /* Thread-local storage */
  #ifdef _MSC_VER
    #define CLAY_THREAD_LOCAL __declspec(thread)
  #else
    #define CLAY_THREAD_LOCAL __thread
  #endif
#else
  /* No threading: plain increment/decrement, no TLS */
  #define CLAY_ATOMIC_INC(p) (++(*(p)))
  #define CLAY_ATOMIC_DEC(p) (--(*(p)))
  #define CLAY_THREAD_LOCAL   /* nothing */
#endif

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

ClayValue clay_int(intptr_t n);          /* raises OverflowError outside the range */
void      clay_format_float(float f, char *buf, size_t sz);  /* shortest round-trip text */
intptr_t  clay_int_mul(intptr_t a, intptr_t b);   /* product; raises on overflow */
intptr_t  clay_float_to_int(float f);  /* truncate; raises OverflowError if out of range */
void      clay_raise_overflow(void);

/* C stack guard: the lowest address a call may reach before the stack is too
 * full to go on (NULL: unknown, no check). Set per thread and per fiber, so a
 * runaway recursion raises a RuntimeError instead of crashing. */
extern CLAY_THREAD_LOCAL char *clay_stack_limit;
void      clay_stack_init(void);        /* the current thread's limit (bootstrap, thread start) */
/* Run fn(argc, argv) on a thread with a big stack (CLAY_STACK_MB megabytes, default
 * 256; 0 = the main thread's own) and return its result. Both `clay` and compiled
 * programs start through this. */
int       clay_run_with_stack(int (*fn)(int, char **), int argc, char **argv);
size_t    clay_stack_bytes(void);       /* the size that would be used */
void      clay_stack_overflow(void);    /* raises RuntimeError; does not return */
#if defined(_MSC_VER)
#include <intrin.h>
#define CLAY_STACK_POINTER() ((char *)_AddressOfReturnAddress())
#else
/* (not the address of a local: AddressSanitizer puts locals on a fake stack) */
#define CLAY_STACK_POINTER() ((char *)__builtin_frame_address(0))
#endif
/* Cycle collector (memory.c). Reference counting frees everything but
 * objects in reference cycles; the collector finds those among objects whose
 * count went down without reaching zero (trial deletion, Bacon & Rajan 2001)
 * and frees them. It runs at a safe point, the start of a method or block
 * (CLAY_STACK_CHECK), once enough candidates have built up, or when asked
 * (System collect). Safe there because every holder of a reference holds a
 * count: the collector never looks at stacks or globals, it only takes the
 * counts that objects hold on each other away and sees what is left. */
extern volatile int clay_gc_pending;
void      clay_gc_safepoint(void);
long      clay_gc_collect(void);        /* runs it now; the number of objects freed */
void      clay_gc_init(void);           /* bootstrap: this thread runs the program */
extern volatile long clay_gc_threads_running;   /* worker threads (no collection while > 0) */

/* The start of every generated or VM method and block: the C stack guard and
 * the collector's safe point */
#define CLAY_STACK_CHECK() \
    do { if (clay_stack_limit && CLAY_STACK_POINTER() < clay_stack_limit) clay_stack_overflow(); \
         if (clay_gc_pending) clay_gc_safepoint(); } while (0)

/* Argument helpers for native methods: return argv[i] converted, or raise —
 * ArgumentError if the argument is missing, TypeError if it has the wrong
 * type. `who` names the method in the message, e.g. "Math sqrt". */
float       clay_arg_float(int argc, ClayValue *argv, int i, const char *who);
intptr_t    clay_arg_integer(int argc, ClayValue *argv, int i, const char *who); /* floats truncate */
const char *clay_arg_string(int argc, ClayValue *argv, int i, const char *who);
void        clay_raise_argument_error(const char *who, const char *problem);
/* A method written in Clay was called with more arguments than it declares */
void        clay_raise_arity(const char *method, int expected, int got);
/* A hint to append to "does not understand" for a likely precedence slip
 * (ifTrue / whileTrue sent to a non-Boolean), or "" */
const char *clay_dnu_hint(const char *selector);
intptr_t  clay_to_int(ClayValue v);
int       clay_is_int(ClayValue v);

/* Forward declaration */
/* Objects alive for the whole program (the core prototypes, true, false):
 * retain/release ignore them */
#define CLAY_REFCOUNT_IMMORTAL 0x40000000

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
    CLAY_TYPE_FLOAT,
    CLAY_TYPE_PLUGIN,
    CLAY_TYPE_VEC3,
    CLAY_TYPE_COND      /* what ifTrue/ifFalse return (bootstrap.c) */
} ClayType;

/* gc_flags bit: the object has a _ctx (fiber) or _sock (socket) slot whose
 * resource clay_dealloc frees; other objects skip looking for them */
#define CLAY_OBJ_RESOURCE 0x80

struct ClayObject {
    ClayObject *parent;
    HashTable  *slots;
    HashTable  *methods;
    int         refcount;
    uint8_t     type;       /* a ClayType */
    uint8_t     gc_color;   /* cycle collector state (memory.c) */
    uint8_t     gc_flags;   /* the collector's flags, and CLAY_OBJ_RESOURCE */

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
            ClayObject    *outer_self;    /* VM blocks: self of the method that made
                                           * them (retained; NULL at top level) */
        } block;

        struct {
            HashTable *map;
            struct ClayDictOrder *order;   /* keys in insertion order (clay_dict.c) */
        } dict;

        struct {
            float value;
        } number_float;

        struct {
            void *data;
            void (*free_fn)(void *data);
        } plugin;

        struct {
            float x, y, z;
        } vec3;

        struct {
            ClayValue value;   /* the block's result (retained) */
            int       matched; /* 1 = a block ran: a chained ifTrue/ifFalse gives value */
        } cond;
    } as;
};

/* Convert between ClayValue and ClayObject* */
#define CLAY_OBJ(v)     ((ClayObject*)(v))
#define CLAY_VAL(obj)   ((ClayValue)(uintptr_t)(obj))

/* Nil value (clay_plugin.h defines the same value for plugins) */
#ifndef CLAY_NIL
#define CLAY_NIL        ((ClayValue)0)
#endif
#define CLAY_IS_NIL(v)  ((v) == CLAY_NIL)

/* True only for a real heap object (not nil, not a tagged int/float) — check
 * this before dereferencing CLAY_OBJ(v) on an argument of unknown type. */
#define CLAY_IS_OBJ(v)      (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v))
#define CLAY_IS_TYPE(v, t)  (CLAY_IS_OBJ(v) && CLAY_OBJ(v)->type == (t))

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
void        clay_object_pool_return(ClayObject *obj); /* Return to free-list pool */
ClayObject *clay_clone(ClayObject *proto);
void        clay_set_slot(ClayObject *obj, const char *name, ClayValue value);
/* Slots shadowing inherited methods: the nearest definition (slot or method)
 * along the prototype chain wins. Only selectors some object actually shadows
 * take the careful lookup; see object.c. */
extern volatile long clay_shadow_count;
int         clay_selector_shadowed_slow(const char *selector);
static inline int clay_selector_shadowed(const char *selector) {
    return clay_shadow_count != 0 && clay_selector_shadowed_slow(selector);
}
/* Goes up whenever a lookup's answer may change: a method is defined anywhere
 * or a selector becomes shadowed. Method caches (dispatch.c, the VM's inline
 * caches) record it and only use an entry while it is unchanged. */
extern volatile long clay_dispatch_epoch;
/* clay_set_slot for a value the caller owns: the slot takes that reference
 * (use for fresh objects, e.g. clay_set_slot_take(o, "k", CLAY_VAL(clay_string_new(s)))) */
void        clay_set_slot_take(ClayObject *obj, const char *name, ClayValue value);
ClayValue   clay_get_slot(ClayObject *obj, const char *name);
int         clay_has_slot(ClayObject *obj, const char *name);
/* Both at once: 1 and the value in *out if obj or a prototype has the slot,
 * else 0 and nil */
int         clay_find_slot(ClayObject *obj, const char *name, ClayValue *out);
void        clay_set_method(ClayObject *obj, const char *name, ClayMethodFunc func);
void        clay_set_method_ctx(ClayObject *obj, const char *name, ClayMethodFunc func, void *context);
ClayMethodFunc clay_lookup_method(ClayObject *obj, const char *name);

/* Full method lookup (returns ClayMethod* with context, uses global cache) */
ClayMethod *clay_cached_lookup(ClayObject *obj, const char *selector);
ClayMethod *clay_lookup_nearest(ClayObject *obj, const char *name);   /* NULL if a slot is nearer */

/* Invoke a method, setting context before call */
ClayValue clay_invoke(ClayMethod *m, ClayObject *obj, int argc, ClayValue *argv);

/* Context pointer set by dispatch before calling a method (for BC compiler) */
extern CLAY_THREAD_LOCAL void *clay_method_context;

/* When non-NULL, block trampolines bind this as 'self' (for slot-as-method dispatch) */
extern CLAY_THREAD_LOCAL ClayObject *clay_slot_method_self;

/* --- String interning --- */
const char *clay_intern(const char *name);

/* --- Method cache --- */
void clay_method_cache_clear(void);
void clay_method_cache_invalidate(void *methods_ptr);

/* --- Inline cache invalidation hook ---
 * Set by the bytecode VM (bc_compiler.c) so its per-callsite inline caches drop
 * entries for a methods table that is being freed. NULL when no VM is linked
 * in, e.g. in natively compiled programs. */
extern void (*clay_ic_invalidate_hook)(void *methods_ptr);

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

/* Memory diagnostics counters (volatile long for atomic increment in MT builds) */
extern volatile long clay_alloc_count;
extern volatile long clay_dealloc_count;

/* --- Message dispatch --- */
ClayValue   clay_send_val(ClayValue receiver, const char *selector, int argc, ClayValue *argv);
ClayValue   clay_send_super(ClayValue receiver, ClayObject *defining_obj,
                            const char *selector, int argc, ClayValue *argv);

/* Legacy API - still works for object pointers */
#define clay_send(recv, sel, ac, av) clay_send_val(CLAY_VAL(recv), sel, ac, av)

/* Ownership-normalizing sends, used by generated C (and matching the VM's
 * native_send). A method's result is a new reference unless it *is* the
 * receiver or one of the arguments (e.g. `println` returns self), in which case
 * it is borrowed. These retain such aliased results, so the caller always owns
 * exactly one reference to the returned value. */
ClayValue   clay_send_owned(ClayValue receiver, const char *selector, int argc, ClayValue *argv);
ClayValue   clay_self_get(ClayValue self, const char *name);  /* bare `name` in a method, owned */
ClayValue   clay_send_super_owned(ClayValue receiver, ClayObject *defining_obj,
                                  const char *selector, int argc, ClayValue *argv);

/* Used by generated functions on return: `rv` holds a reference owned by the
 * function; if it is the receiver (or, for methods, an argument), give that
 * reference up so the result is borrowed — the convention above. Blocks pass
 * argc = 0: callers of blocks (each, map, ifTrue, ...) always own the result. */
ClayValue   clay_return_value(ClayValue rv, ClayValue self, int argc, ClayValue *argv);

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
extern ClayObject *clay_Crypto;
extern ClayObject *clay_Process;
extern ClayObject *clay_Fiber;

/* Fiber engine hooks: the bytecode VM gives each fiber its own VM stack and
 * frames and swaps them on resume/yield. NULL for natively compiled programs. */
typedef struct {
    void *(*create)(void);                  /* per-fiber engine state */
    void  (*enter)(void *state, int first); /* save caller's state, switch to the fiber's */
    void  (*leave)(void *state, int done);  /* save the fiber's state, restore the caller's */
    void  (*destroy)(void *state);
} ClayFiberHooks;
extern const ClayFiberHooks *clay_fiber_hooks;
extern ClayObject *clay_Db;
extern ClayObject *clay_Range;

extern ClayObject *clay_Graphics;

extern ClayObject *clay_Vec3;

extern ClayObject *clay_Thread;
extern ClayObject *clay_Socket;
void clay_socket_free(intptr_t fd);   /* closes a socket descriptor (memory.c, when its object is freed) */
extern ClayObject *clay_VM;

/* Thread VM initialization callback (set by compiler) */
void clay_thread_set_vm_init(void (*fn)(void));

/* Loop control signal singletons(Error clones used by break/continue) */
extern ClayObject *clay_BreakSignal;
extern ClayObject *clay_ContinueSignal;
extern ClayObject *clay_ReturnSignal;   /* internal: prototype of non-local return signals */

/* Fiber context cleanup (called from memory.c when fiber objects are freed) */
void clay_fiber_context_free(intptr_t ctx_ptr);

/* Regex search — public API for String convenience methods */
int clay_regex_search(const char *pat, const char *text, int *match_start, int *match_len);

void clay_bootstrap(void);
void clay_shutdown(void);
void clay_set_args(int argc, char **argv);

/* --- Plugin system --- */
/* Check if a name is a plugin-registered global */
int clay_plugin_is_global(const char *name);
extern const char *clay_executable_path;   /* set by the clay CLI (System executable) */
/* Get a plugin-registered prototype by name (or NULL) */
ClayObject *clay_plugin_get_global(const char *name);
void        clay_plugin_auto_load(const char *exe_path);   /* load every plugin in <exe dir>/plugins */

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
/* Entries in insertion order: pos starts at 0; returns 0 at the end */
int         clay_dict_next(ClayObject *dict, size_t *pos, const char **key, ClayValue *val);
void        clay_dict_copy_into(ClayObject *dst, ClayObject *src);  /* clone */
void        clay_dict_free_contents(ClayObject *dict);              /* dealloc */

/* --- Vec3 helpers --- */
ClayObject *clay_vec3_new(float x, float y, float z);
void        clay_vec3_init_methods(ClayObject *proto);

/* --- Debug --- */
void        clay_inspect(ClayObject *obj);

/* --- Error handling (setjmp/longjmp) --- */
#include <setjmp.h>
#if defined(__APPLE__)
/* setjmp/longjmp there save and restore the signal mask: two system calls
 * per try frame. Clay doesn't change signal masks, so skip that. */
int  _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define CLAY_SETJMP(env)       _setjmp(env)
#define CLAY_LONGJMP(env, val) _longjmp(env, val)
#else
#define CLAY_SETJMP(env)       setjmp(env)
#define CLAY_LONGJMP(env, val) longjmp(env, val)
#endif

/* Try/catch frames — linked list on the C stack (no fixed limit) */
/* Execution-engine state saved with each try frame (see ClayTryHooks) */
#define CLAY_TRY_ENGINE_STATE_SIZE 128

/* What a C function owns only while a call it makes is running: objects
 * (the temporary Number wrapper dispatch creates for an int receiver, say),
 * and in generated code the addresses of the variables, temporaries and cells
 * holding a reference. An error, break, continue or non-local return
 * unwinding past that call releases them: each try frame records the depth
 * and restoring it releases what is above (an address's value, set to nil).
 * One list per fiber. Entries are tagged pointers (CLAY_UW_*). */
typedef struct ClayUnwindList {
    uintptr_t *items;
    int count;
    int capacity;
} ClayUnwindList;
#define CLAY_UW_OBJECT 0     /* a ClayObject * */
#define CLAY_UW_TEMP   1     /* a ClayValue * holding a statement's temporary */
#define CLAY_UW_VAR    2     /* a ClayValue * holding a variable, parameter or __rv */
#define CLAY_UW_CELL   3     /* a ClayCell ** */
extern CLAY_THREAD_LOCAL ClayUnwindList *clay_unwind_list;   /* NULL: the thread's own */
void clay_unwind_push(ClayObject *obj);
void clay_unwind_pop(void);              /* normal path: the caller releases it */
void clay_unwind_list_free(ClayUnwindList *list);   /* releases what remains */
/* Generated code (codegen.c) registers what it owns as it goes, and drops the
 * entries without releasing them on the normal path. Inline: a generated
 * function does this for every temporary. */
ClayUnwindList *clay_uw_thread_list(void);           /* the thread's own, made current */
void clay_uw_grow(ClayUnwindList *l);
void clay_uw_drop_slow(ClayUnwindList *l, int n);
static inline ClayUnwindList *clay_uw_list(void) {
    ClayUnwindList *l = clay_unwind_list;
    return l ? l : clay_uw_thread_list();
}
static inline void clay_uw_add(uintptr_t entry) {
    ClayUnwindList *l = clay_uw_list();
    if (l->count == l->capacity) clay_uw_grow(l);
    l->items[l->count++] = entry;
}
static inline int  clay_uw_mark(void) { return clay_uw_list()->count; }
static inline void clay_uw_reset(int mark) { clay_uw_list()->count = mark; }   /* drop all above */
static inline void clay_uw_temp(ClayValue *slot) { clay_uw_add((uintptr_t)slot | CLAY_UW_TEMP); }
static inline void clay_uw_var(ClayValue *slot) { clay_uw_add((uintptr_t)slot | CLAY_UW_VAR); }
/* drop the n most recent temporaries (usually the top n entries) */
static inline void clay_uw_drop(int n) {
    ClayUnwindList *l = clay_uw_list();
    for (int i = 1; i <= n; i++)
        if ((l->items[l->count - i] & 3) != CLAY_UW_TEMP) { clay_uw_drop_slow(l, n); return; }
    l->count -= n;
}

typedef struct ClayTryFrame {
    jmp_buf buf;
    struct ClayTryFrame *prev;
    /* Snapshot taken by clay_try_push and restored by clay_raise before it
     * jumps here: a longjmp skips the code that would normally restore these */
    ClayObject *slot_method_self;
    int trace_depth;
    int trace_overflow;
    ClayValue return_id;    /* nonzero: catches non-local returns for this method call */
    int unwind_depth;       /* clay_unwind_list count when pushed */
    unsigned char engine_state[CLAY_TRY_ENGINE_STATE_SIZE];
} ClayTryFrame;

/* Hooks for an execution engine (the bytecode VM) to save and restore its own
 * state — code pointer, ip, stack depth, current self — with each try frame.
 * NULL for natively compiled programs. */
typedef struct {
    void (*save)(void *state);      /* at clay_try_push */
    void (*restore)(void *state);   /* in clay_raise, before jumping */
} ClayTryHooks;
extern const ClayTryHooks *clay_try_hooks;

void clay_try_save_state(ClayTryFrame *frame);

typedef struct {
    ClayTryFrame *head;          /* linked list of active try frames */
    ClayValue current_error;     /* the error being raised */
} ClayErrorStack;

extern CLAY_THREAD_LOCAL ClayErrorStack clay_error_stack;

/* Push/pop try frames — caller must declare a local ClayTryFrame variable */
#define clay_try_push(frame) \
    ((frame)->prev = clay_error_stack.head, \
     clay_error_stack.head = (frame), \
     clay_try_save_state(frame), \
     CLAY_SETJMP((frame)->buf))
#define clay_try_pop() \
    (clay_error_stack.head = clay_error_stack.head->prev)

/* Raise an error — longjmps to nearest try frame, or aborts */
void clay_raise(ClayValue error);

/* break / continue / non-local return travel as errors; user try/catch lets
 * them through (running finally) instead of catching them */
int  clay_is_control_signal(ClayValue error);
/* Loop methods (each, times, whileTrue, ...): classify a caught error. For
 * break/continue the signal is consumed (released); otherwise the caller
 * re-raises it. */
#define CLAY_SIGNAL_BREAK    1
#define CLAY_SIGNAL_CONTINUE 2
int  clay_loop_signal(ClayValue error);

/* Non-local return: `return v` inside a block returns from the method the
 * block was written in. Each such method activation gets an id; the block
 * raises a ReturnSignal carrying (value, id) and the method catches it. */
ClayValue clay_return_id_next(void);
void      clay_raise_return(ClayValue value, ClayValue id);   /* takes a reference to value; does not return */
int       clay_return_matches(ClayValue error, ClayValue id, ClayValue *value_out); /* consumes a matched signal */

/* Source location tracking for error messages */
extern CLAY_THREAD_LOCAL int clay_source_line;
extern CLAY_THREAD_LOCAL const char *clay_source_file;

/* A permanent copy of a source path, the same pointer for equal paths: what
 * clay_source_file, trace frames and compiled code point at, since they
 * outlive the parser's strings. */
const char *clay_intern_file(const char *path);
/* Bundled-brick files shown relative to lib/ in errors and tracebacks */
void clay_set_lib_dir(const char *dir);
const char *clay_display_file(const char *path);
const char *clay_path_under(const char *dir, const char *path);

/* Output hook — when set, all println/print calls route through this
 * instead of writing to stdout. Used by DAP to send output events. */
extern void (*clay_output_fn)(const char *text);

/* HTTP GET for C callers (http.c): the body as a new string, or NULL on failure */
char *clay_http_get_text(const char *url, int *status);

/* Create an Error object with message and type */
ClayObject *clay_error_new(const char *message, const char *type);

/* What `type` says about an object: "String", "Array", ..., a prototype's _name, else "Object".
 * Valid while the object is alive. Used to say what did not understand a message. */
const char *clay_type_name(ClayObject *obj);

/* The traceback printed when an error ends a program: an Error's stack trace
 * with repeats counted and long ones cut in the middle. Both return a
 * malloc'd string of whole lines, or NULL. */
char *clay_traceback_format(const char *trace);
char *clay_error_traceback(ClayValue err);
/* What an uncaught error's report calls it: its type ("RuntimeError",
 * "MethodNotFound", ...), or "Error". Valid while err is alive. */
const char *clay_error_kind(ClayValue err);

/* --- Call trace (shadow call stack for stack traces) --- */
#define CLAY_MAX_TRACE_DEPTH 64

/* A frame is a call: where it was made and the message sent. kind says what
 * the call ran, so a traceback can name the code each line is in: a method
 * or block body marks the frame that called it when it starts. */
#define CLAY_FRAME_CALL   0   /* a built-in (or not entered): no Clay code */
#define CLAY_FRAME_METHOD 1   /* a method written in Clay */
#define CLAY_FRAME_BLOCK  2   /* a block (value, each, ifTrue, ...) */

typedef struct {
    const char *file;
    int line;
    const char *selector;
    int kind;       /* CLAY_FRAME_* */
} ClayTraceFrame;

typedef struct {
    ClayTraceFrame frames[CLAY_MAX_TRACE_DEPTH];
    int depth;
    int overflow;   /* pushes beyond CLAY_MAX_TRACE_DEPTH (not recorded) */
} ClayCallTrace;

extern CLAY_THREAD_LOCAL ClayCallTrace clay_call_trace;
extern CLAY_THREAD_LOCAL int clay_trace_enabled; /* 0 = skip tracing for performance */

static inline void clay_trace_push(const char *file, int line, const char *sel) {
    if (!clay_trace_enabled) return;
    if (clay_call_trace.depth < CLAY_MAX_TRACE_DEPTH) {
        ClayTraceFrame *f = &clay_call_trace.frames[clay_call_trace.depth++];
        f->file = file;
        f->line = line;
        f->selector = sel;
        f->kind = CLAY_FRAME_CALL;
    } else {
        clay_call_trace.overflow++;   /* keep pushes and pops balanced */
    }
}

/* A method or block body starting: mark the call that ran it. A block only
 * marks a call nothing has claimed, so a block run directly by its method
 * (no call of its own) leaves the method's frame a method. */
static inline void clay_trace_enter(int kind) {
    if (!clay_trace_enabled || clay_call_trace.overflow > 0 || clay_call_trace.depth == 0) return;
    ClayTraceFrame *f = &clay_call_trace.frames[clay_call_trace.depth - 1];
    if (kind == CLAY_FRAME_METHOD || f->kind == CLAY_FRAME_CALL) f->kind = kind;
}

static inline void clay_trace_pop(void) {
    if (!clay_trace_enabled) return;
    if (clay_call_trace.overflow > 0) clay_call_trace.overflow--;
    else if (clay_call_trace.depth > 0) clay_call_trace.depth--;
}

/* --- Cell (heap-allocated box for by-reference capture in C codegen) --- */
typedef struct {
    int       refcount;
    uint8_t   gc_color;     /* cycle collector state, as in ClayObject */
    uint8_t   gc_flags;
    ClayValue value;
} ClayCell;

ClayCell   *clay_cell_new(ClayValue initial);
void        clay_cell_retain(ClayCell *cell);
void        clay_cell_release(ClayCell *cell);
static inline void clay_uw_cell(ClayCell **slot) { clay_uw_add((uintptr_t)slot | CLAY_UW_CELL); }

#ifdef __cplusplus
}
#endif

#endif /* CLAY_H */
