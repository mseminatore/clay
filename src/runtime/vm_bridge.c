/*
 * vm_bridge.c - Runtime side of the Clay bytecode VM
 *
 * Wraps Clay runtime objects in CLAYVM_VAL_OBJECT values and implements every
 * Clay-specific operation (send, clone, blocks, methods, try/catch, ...) as a
 * VM native function. Bytecode comes from the C bytecode compiler
 * (src/compiler/bc_compiler.c) or, through the VM prototype at the end of
 * this file, from Clay programs (the self-hosted compiler in selfhost/).
 */
#include "vm_bridge.h"
#include "clay.h"
#include "range.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

/* --- Global state for trampoline access --- */

static CLAY_THREAD_LOCAL BcCompiler *g_bc = NULL;  /* current compiler (for trampoline) */
static CLAY_THREAD_LOCAL clayvm_vm_t *g_vm = NULL; /* current VM (for trampoline) */
static CLAY_THREAD_LOCAL ClayObject *g_super_proto = NULL; /* defining prototype for super dispatch */
static CLAY_THREAD_LOCAL ClayObject *g_method_self = NULL; /* cached self for fast method access */

/* Forward declarations for retain/release helpers */
static void vm_retain_val(clayvm_value_t v);
static void vm_release_val(clayvm_value_t v);

/* Dispatch table from vm.c for direct chunk execution */
extern int (*clayvm_dispatch_table[])(clayvm_vm_t *vm);

/* --- Block trampoline ---
 * When a block's value() method is called, this trampoline:
 * 1. Reads chunk_id from self->as.block.env[0]
 * 2. Stores block params as temporary globals
 * 3. Runs the bytecode chunk on the VM
 * 4. Restores old global values
 * 5. Returns the result
 */
static ClayValue bc_block_trampoline(ClayObject *self, int argc, ClayValue *argv) {
    if (!g_bc || !g_vm) return CLAY_NIL;

    /* Read chunk_id from env[0] */
    int chunk_id = (int)clay_to_int(self->as.block.env[0]);
    if (chunk_id < 0 || chunk_id >= g_bc->chunk_count) return CLAY_NIL;

    BcChunk *chunk = g_bc->chunks[chunk_id];

    /* If invoked as a slot-method, bind the receiver as 'self' via fast global */
    ClayObject *slot_method_self = clay_slot_method_self;
    ClayObject *saved_method_self = g_method_self;
    if (slot_method_self) {
        clay_slot_method_self = NULL;
        g_method_self = slot_method_self;
    }

    /* Save VM state for re-entrant execution */
    const uint8_t *saved_code = g_vm->code;
    size_t saved_code_len = g_vm->code_len;
    size_t saved_ip = g_vm->ip;
    size_t saved_sp = g_vm->sp;
    int saved_running = g_vm->running;
    int saved_frame_count = g_vm->frame_count;

    /* Set up the chunk's code — keep sp where it is to avoid clobbering
     * the caller's stack.  Push a call frame with base_sp = current sp
     * so GET_LOCAL/SET_LOCAL address our locals correctly. */
    g_vm->code = chunk->code;
    g_vm->code_len = chunk->len;
    g_vm->ip = 0;
    g_vm->running = 1;

    size_t base = g_vm->sp;   /* locals start here */

    if (g_vm->frame_count >= g_vm->frame_capacity) {
        g_vm->frame_capacity *= 2;
        g_vm->frames = (clayvm_frame_t *)realloc(g_vm->frames,
            (size_t)g_vm->frame_capacity * sizeof(clayvm_frame_t));
    }
    g_vm->frames[g_vm->frame_count].return_ip = 0;
    g_vm->frames[g_vm->frame_count].base_sp = base;
    g_vm->frames[g_vm->frame_count].func_index = 0;
    g_vm->frame_count++;

    /* Push params as stack locals (slots 0, 1, ...).
     * Params are borrowed from argv — retain for stack ownership. */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        ClayValue val = (i < argc) ? argv[i] : CLAY_NIL;
        clayvm_value_t vm_val = clay_to_vm(val);
        vm_retain_val(vm_val);
        clayvm_vm_push(g_vm, vm_val);
    }

    /* Push captured values as additional locals (slots param_count, param_count+1, ...).
     * Captures are borrowed from block env — retain for stack ownership. */
    int capture_count = chunk->capture_count;
    for (int i = 0; i < capture_count; i++) {
        ClayValue cv = (1 + i < self->as.block.env_size) ?
                        self->as.block.env[1 + i] : CLAY_NIL;
        clayvm_value_t vm_val;
        if (self->as.block.env_are_cells && CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv)) {
            vm_val = CLAYVM_CELL((void*)(uintptr_t)cv);
        } else {
            vm_val = clay_to_vm(cv);
        }
        vm_retain_val(vm_val);
        clayvm_vm_push(g_vm, vm_val);
    }

    /* Reserve stack space for block-created locals beyond params and captures */
    int reserved = chunk->param_count + capture_count;
    for (int i = reserved; i < chunk->local_count; i++) {
        clayvm_vm_push(g_vm, CLAYVM_NIL_VAL);
    }

    /* Run the dispatch loop manually (can't use vm_run — it resets sp).
     * Wrap in clay_try_push so that exceptions (break/continue/errors)
     * raised from nested calls still get VM state restored before the
     * longjmp propagates to the caller's try handler. */
    ClayValue raised_err = CLAY_NIL;
    int had_exception = 0;

    ClayTryFrame _tf;
    if (clay_try_push(&_tf) == 0) {
        while (g_vm->running && g_vm->ip < g_vm->code_len) {
            if (chunk->line_map && g_vm->ip < chunk->len) {
                clay_source_line = chunk->line_map[g_vm->ip];
            }

            uint8_t opcode = g_vm->code[g_vm->ip++];
            if (opcode >= CLAYVM_OP_COUNT) {
                g_vm->running = 0;
                break;
            }
            if (g_vm->step_callback) g_vm->step_callback(g_vm, g_vm->ip - 1);

            int rc = clayvm_dispatch_table[opcode](g_vm);
            if (rc != 0) break;
        }
        clay_try_pop();
    } else {
        /* Exception caught — save it, restore VM state, then re-raise */
        raised_err = clay_error_stack.current_error;
        had_exception = 1;
    }

    /* Write back captured values from locals to env (by-reference semantics) */
    for (int i = 0; i < capture_count; i++) {
        size_t slot = base + (size_t)chunk->param_count + (size_t)i;
        if (g_vm->sp > 0 && slot < (size_t)g_vm->sp && (1 + i) < self->as.block.env_size) {
            self->as.block.env[1 + i] = vm_to_clay(g_vm->stack[slot]);
        }
    }

    /* Get return value (last value on stack) */
    ClayValue retval = CLAY_NIL;
    clayvm_value_t retval_vm = CLAYVM_NIL_VAL;
    if (!had_exception && g_vm->sp > (int)base) {
        retval_vm = g_vm->stack[g_vm->sp - 1];
        retval = vm_to_clay(retval_vm);
        vm_retain_val(retval_vm); /* keep retval alive through cleanup */
    }

    /* Release all stack slots in this frame (params, captures, locals, temps).
     * Every slot owns a reference (retained on push/SET_LOCAL). */
    for (size_t i = base; i < (size_t)g_vm->sp; i++) {
        vm_release_val(g_vm->stack[i]);
    }

    /* Restore full VM state */
    g_vm->code = saved_code;
    g_vm->code_len = saved_code_len;
    g_vm->ip = saved_ip;
    g_vm->sp = saved_sp;
    g_vm->running = saved_running;
    g_vm->frame_count = saved_frame_count;

    /* Restore 'self' if we bound a slot-method receiver */
    if (slot_method_self) {
        g_method_self = saved_method_self;
    }

    /* Re-raise after VM state is safely restored */
    if (had_exception) {
        clay_raise(raised_err);
        return CLAY_NIL; /* unreachable */
    }

    return retval;
}

/* --- Fiber (coroutine) support ---
 * Fibers themselves (C-stack switching, yield from any depth) live in
 * src/runtime/fiber.c. The VM additionally gives each fiber its own VM stack
 * and frame array, swapped in and out through these hooks, so a fiber's
 * bytecode never interferes with the caller's VM data.
 */

typedef struct {
    /* Fiber's own VM stack and frames (swapped into g_vm while it runs) */
    clayvm_value_t *vm_stack;
    int vm_stack_capacity;
    clayvm_frame_t *vm_frames;
    int vm_frame_capacity;

    /* Fiber's saved VM execution state (for resume after yield) */
    int vm_sp;
    int vm_frame_count;
    const uint8_t *vm_code;
    size_t vm_code_len;
    size_t vm_ip;
    int vm_exception_count;

    /* Caller's VM state, saved by enter and restored by leave */
    clayvm_value_t *caller_stack;
    int caller_stack_cap;
    int caller_sp;
    clayvm_frame_t *caller_frames;
    int caller_frame_cap;
    int caller_fc;
    const uint8_t *caller_code;
    size_t caller_code_len;
    size_t caller_ip;
    int caller_running;
    int caller_exc;
} VmFiberState;

static void *vm_fiber_create(void) {
    VmFiberState *st = (VmFiberState *)calloc(1, sizeof(VmFiberState));
    st->vm_stack_capacity = 4096;
    st->vm_stack = (clayvm_value_t *)calloc((size_t)st->vm_stack_capacity, sizeof(clayvm_value_t));
    st->vm_frame_capacity = 64;
    st->vm_frames = (clayvm_frame_t *)calloc((size_t)st->vm_frame_capacity, sizeof(clayvm_frame_t));
    return st;
}

static void vm_fiber_enter(void *state, int first) {
    VmFiberState *st = (VmFiberState *)state;
    if (!g_vm || !st) return;
    /* Save caller's full VM state */
    st->caller_stack     = g_vm->stack;
    st->caller_stack_cap = g_vm->stack_capacity;
    st->caller_sp        = g_vm->sp;
    st->caller_frames    = g_vm->frames;
    st->caller_frame_cap = g_vm->frame_capacity;
    st->caller_fc        = g_vm->frame_count;
    st->caller_code      = g_vm->code;
    st->caller_code_len  = g_vm->code_len;
    st->caller_ip        = g_vm->ip;
    st->caller_running   = g_vm->running;
    st->caller_exc       = g_vm->exception_count;

    /* Swap in the fiber's VM stack and frames */
    g_vm->stack          = st->vm_stack;
    g_vm->stack_capacity = st->vm_stack_capacity;
    g_vm->frames         = st->vm_frames;
    g_vm->frame_capacity = st->vm_frame_capacity;
    if (first) {
        /* First resume: fresh state — the block trampoline sets up code/ip */
        g_vm->sp              = 0;
        g_vm->frame_count     = 0;
        g_vm->running         = 1;
        g_vm->exception_count = 0;
    } else {
        g_vm->sp              = st->vm_sp;
        g_vm->frame_count     = st->vm_frame_count;
        g_vm->code            = st->vm_code;
        g_vm->code_len        = st->vm_code_len;
        g_vm->ip              = st->vm_ip;
        g_vm->running         = 1;
        g_vm->exception_count = st->vm_exception_count;
    }
}

static void vm_fiber_leave(void *state, int done) {
    VmFiberState *st = (VmFiberState *)state;
    if (!g_vm || !st) return;
    /* Save the fiber's VM state (stack/frames may have been reallocated) */
    st->vm_stack           = g_vm->stack;
    st->vm_stack_capacity  = g_vm->stack_capacity;
    st->vm_sp              = g_vm->sp;
    st->vm_frames          = g_vm->frames;
    st->vm_frame_capacity  = g_vm->frame_capacity;
    st->vm_frame_count     = g_vm->frame_count;
    st->vm_code            = g_vm->code;
    st->vm_code_len        = g_vm->code_len;
    st->vm_ip              = g_vm->ip;
    st->vm_exception_count = g_vm->exception_count;
    if (done) {
        free(st->vm_stack);  st->vm_stack = NULL;
        free(st->vm_frames); st->vm_frames = NULL;
    }
    /* Restore the caller's VM state */
    g_vm->stack           = st->caller_stack;
    g_vm->stack_capacity  = st->caller_stack_cap;
    g_vm->sp              = st->caller_sp;
    g_vm->frames          = st->caller_frames;
    g_vm->frame_capacity  = st->caller_frame_cap;
    g_vm->frame_count     = st->caller_fc;
    g_vm->code            = st->caller_code;
    g_vm->code_len        = st->caller_code_len;
    g_vm->ip              = st->caller_ip;
    g_vm->running         = st->caller_running;
    g_vm->exception_count = st->caller_exc;
}

static void vm_fiber_destroy(void *state) {
    VmFiberState *st = (VmFiberState *)state;
    if (!st) return;
    free(st->vm_stack);
    free(st->vm_frames);
    free(st);
}

/* --- Try-frame hooks: VM state restored when an error unwinds to a frame ---
 * Any C code that catches an error (try/catch, loops catching break/continue,
 * method non-local return) resumes with the VM exactly as it was when the try
 * frame was pushed — not with whatever chunk the error was raised from. */
/* Return id handed from bc_method_dispatch to the method prologue (see
 * native_method_return_id_fn) */
static CLAY_THREAD_LOCAL ClayValue g_pending_return_id = 0;

typedef struct {
    clayvm_vm_t *vm;
    clayvm_value_t *stack;
    const uint8_t *code;
    size_t code_len;
    size_t ip;
    int sp;
    int running;
    int frame_count;
    int exception_count;
    ClayObject *method_self;
    ClayObject *super_proto;
} VmTryState;

typedef char vm_try_state_fits[(sizeof(VmTryState) <= CLAY_TRY_ENGINE_STATE_SIZE) ? 1 : -1];

static void vm_try_save(void *buf) {
    VmTryState *st = (VmTryState *)buf;
    st->vm = g_vm;
    st->method_self = g_method_self;
    st->super_proto = g_super_proto;
    if (!g_vm) return;
    st->stack = g_vm->stack;
    st->code = g_vm->code;
    st->code_len = g_vm->code_len;
    st->ip = g_vm->ip;
    st->sp = g_vm->sp;
    st->running = g_vm->running;
    st->frame_count = g_vm->frame_count;
    st->exception_count = g_vm->exception_count;
}

static void vm_try_restore(void *buf) {
    VmTryState *st = (VmTryState *)buf;
    g_method_self = st->method_self;
    g_super_proto = st->super_proto;
    if (!st->vm || st->vm != g_vm) return;
    g_vm->code = st->code;
    g_vm->code_len = st->code_len;
    g_vm->ip = st->ip;
    /* Values the unwound code left on the stack above the frame's mark are
     * abandoned: release them. (Each VM call in between already released its
     * own frame before re-raising; a fiber switch swaps stacks, so only on
     * the same stack.) */
    if (g_vm->stack == st->stack) {
        while (g_vm->sp > st->sp) vm_release_val(g_vm->stack[--g_vm->sp]);
    }
    g_vm->sp = st->sp;
    g_vm->running = st->running;
    g_vm->frame_count = st->frame_count;
    g_vm->exception_count = st->exception_count;
}

static const ClayTryHooks bc_try_hooks = { vm_try_save, vm_try_restore };

static const ClayFiberHooks bc_fiber_hooks = {
    vm_fiber_create, vm_fiber_enter, vm_fiber_leave, vm_fiber_destroy
};

/* --- Method dispatch via context ---
 * Instead of generating unique trampoline functions per method, we use a
 * single dispatch function. The per-method data (chunk_id, param_count,
 * defining_obj) is stored as a heap-allocated MethodSlot passed via the
 * ClayMethod context pointer (set by dispatch.c before calling).
 */

typedef struct {
    int chunk_id;
    int param_count;
    ClayObject *defining_obj; /* prototype where this method was defined */
    char *name;               /* selector, for error messages */
} MethodSlot;

/* Dynamic array of method slots (heap-allocated, grows as needed) */
static MethodSlot **g_method_slots = NULL;
static int g_method_slot_count = 0;
static int g_method_slot_capacity = 0;

static int alloc_method_slot(void) {
    if (g_method_slot_count >= g_method_slot_capacity) {
        int new_cap = g_method_slot_capacity == 0 ? 64 : g_method_slot_capacity * 2;
        g_method_slots = (MethodSlot **)realloc(g_method_slots, sizeof(MethodSlot *) * (size_t)new_cap);
        g_method_slot_capacity = new_cap;
    }
    MethodSlot *ms = (MethodSlot *)malloc(sizeof(MethodSlot));
    ms->chunk_id = -1;
    ms->param_count = 0;
    ms->defining_obj = NULL;
    ms->name = NULL;
    g_method_slots[g_method_slot_count] = ms;
    return g_method_slot_count++;
}

int bc_method_slot_new(int chunk_id, int param_count, const char *name) {
    int idx = alloc_method_slot();
    g_method_slots[idx]->chunk_id = chunk_id;
    g_method_slots[idx]->param_count = param_count;
    g_method_slots[idx]->name = name ? strdup(name) : NULL;
    return idx;
}

int bc_method_slot_count(void) { return g_method_slot_count; }

const char *bc_method_slot_info(int idx, int *chunk_id, int *param_count) {
    MethodSlot *ms = g_method_slots[idx];
    *chunk_id = ms->chunk_id;
    *param_count = ms->param_count;
    return ms->name;
}

/* Single dispatch function for all BC-compiled methods.
 * Reads the MethodSlot from clay_method_context (set by dispatch.c). */
static ClayValue bc_method_dispatch(ClayObject *self, int argc, ClayValue *argv) {
    MethodSlot *ms = (MethodSlot *)clay_method_context;
    if (!g_bc || !g_vm || !ms) return CLAY_NIL;
    if (ms->chunk_id < 0 || ms->chunk_id >= g_bc->chunk_count) return CLAY_NIL;

    BcChunk *chunk = g_bc->chunks[ms->chunk_id];

    /* Too many arguments for a method written in Clay (before any VM state
     * changes, so raising leaves the VM consistent). Fewer is fine: missing
     * parameters are nil or take their defaults. */
    if (argc > ms->param_count)
        clay_raise_arity(ms->name ? ms->name : "method", ms->param_count, argc);

    /* Non-local return: catch the ReturnSignal a block raises for this
     * activation. The try frame is pushed before any VM state changes, so the
     * try hooks restore the caller's VM state, self and super on the way out. */
    ClayTryFrame nl_frame;
    ClayValue nl_id = CLAY_NIL;
    if (chunk->has_block_return) {
        nl_id = clay_return_id_next();
        if (clay_try_push(&nl_frame) != 0) {
            ClayValue err = clay_error_stack.current_error;
            ClayValue value;
            if (clay_return_matches(err, nl_id, &value))
                return clay_return_value(value, CLAY_VAL(self), argc, argv);
            clay_raise(err);
        }
        nl_frame.return_id = nl_id;
        g_pending_return_id = nl_id;
    }

    /* Save and set super prototype context */
    ClayObject *saved_super_proto = g_super_proto;
    g_super_proto = ms->defining_obj;

    /* Save and set self via fast-access global */
    ClayObject *saved_method_self = g_method_self;
    g_method_self = self;

    /* Save full VM state */
    const uint8_t *saved_code = g_vm->code;
    size_t saved_code_len = g_vm->code_len;
    size_t saved_ip = g_vm->ip;
    size_t saved_sp = g_vm->sp;
    int saved_running = g_vm->running;
    int saved_frame_count = g_vm->frame_count;

    /* Set up chunk's code — keep sp to avoid clobbering caller's stack */
    g_vm->code = chunk->code;
    g_vm->code_len = chunk->len;
    g_vm->ip = 0;
    g_vm->running = 1;

    size_t base = g_vm->sp;

    if (g_vm->frame_count >= g_vm->frame_capacity) {
        g_vm->frame_capacity *= 2;
        g_vm->frames = (clayvm_frame_t *)realloc(g_vm->frames,
            (size_t)g_vm->frame_capacity * sizeof(clayvm_frame_t));
    }
    g_vm->frames[g_vm->frame_count].return_ip = 0;
    g_vm->frames[g_vm->frame_count].base_sp = base;
    g_vm->frames[g_vm->frame_count].func_index = 0;
    g_vm->frame_count++;

    /* Push params as stack locals (slots 0, 1, ...).
     * Params are borrowed from argv — retain for stack ownership. */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        ClayValue val = (i < argc) ? argv[i] : CLAY_NIL;
        clayvm_value_t vm_val = clay_to_vm(val);
        vm_retain_val(vm_val);
        clayvm_vm_push(g_vm, vm_val);
    }

    /* Reserve stack space for method-created locals beyond params */
    for (int i = chunk->param_count; i < chunk->local_count; i++) {
        clayvm_vm_push(g_vm, CLAYVM_NIL_VAL);
    }

    /* Run the dispatch loop */
    while (g_vm->running && g_vm->ip < g_vm->code_len) {
        /* Update source line from chunk's line_map for error reporting and debugging */
        if (chunk->line_map && g_vm->ip < chunk->len) {
            clay_source_line = chunk->line_map[g_vm->ip];
        }
        uint8_t opcode = g_vm->code[g_vm->ip++];
        if (opcode >= CLAYVM_OP_COUNT) { g_vm->running = 0; break; }
        if (g_vm->step_callback) g_vm->step_callback(g_vm, g_vm->ip - 1);
        int rc = clayvm_dispatch_table[opcode](g_vm);
        if (rc != 0) break;
    }

    /* Get return value */
    ClayValue retval = CLAY_NIL;
    if (g_vm->sp > (int)base) {
        clayvm_value_t retval_vm = g_vm->stack[g_vm->sp - 1];
        retval = vm_to_clay(retval_vm);
        vm_retain_val(retval_vm); /* keep retval alive through cleanup */
    }

    /* Release all stack slots in this frame */
    for (size_t i = base; i < (size_t)g_vm->sp; i++) {
        vm_release_val(g_vm->stack[i]);
    }

    /* Restore full VM state */
    g_vm->code = saved_code;
    g_vm->code_len = saved_code_len;
    g_vm->ip = saved_ip;
    g_vm->sp = saved_sp;
    g_vm->running = saved_running;
    g_vm->frame_count = saved_frame_count;

    /* Restore self */
    g_method_self = saved_method_self;

    /* Restore super prototype context */
    g_super_proto = saved_super_proto;

    if (chunk->has_block_return) clay_try_pop();
    /* Like every method: a result that is self or an argument is borrowed */
    return clay_return_value(retval, CLAY_VAL(self), argc, argv);
}

/* --- Conversion helpers --- */

ClayValue vm_to_clay(clayvm_value_t v) {
    switch (v.type) {
        case CLAYVM_VAL_INT64:  return clay_int((intptr_t)v.as.i64);
        case CLAYVM_VAL_BOOL:   return v.as.boolean ? CLAY_TRUE : CLAY_FALSE;
        case CLAYVM_VAL_NIL:    return CLAY_NIL;
        case CLAYVM_VAL_OBJECT: return CLAY_VAL(v.as.obj);
        case CLAYVM_VAL_CELL:   return CLAY_VAL(v.as.obj);
        case CLAYVM_VAL_DOUBLE: return clay_float_tag((float)v.as.f64);
        default: break;
    }
    return CLAY_NIL;
}

clayvm_value_t clay_to_vm(ClayValue cv) {
    if (CLAY_IS_NIL(cv))   return CLAYVM_NIL_VAL;
    if (CLAY_IS_INT(cv))   return CLAYVM_INT64(clay_to_int(cv));
    if (CLAY_IS_TAGGED_FLOAT(cv)) return CLAYVM_DOUBLE((double)clay_float_untag(cv));
    if (cv == CLAY_TRUE)   return CLAYVM_BOOL(1);
    if (cv == CLAY_FALSE)  return CLAYVM_BOOL(0);
    return CLAYVM_OBJ((void*)(uintptr_t)cv);
}

/* --- Native bridge functions --- */

/* VM retain/release callbacks for stack ownership semantics */
static void vm_retain_cb(clayvm_value_t v) {
    if (v.type == CLAYVM_VAL_OBJECT && v.as.obj) {
        clay_retain((ClayObject *)v.as.obj);
    } else if (v.type == CLAYVM_VAL_CELL && v.as.obj) {
        clay_cell_retain((ClayCell *)v.as.obj);
    }
}

static void vm_release_cb(clayvm_value_t v) {
    if (v.type == CLAYVM_VAL_OBJECT && v.as.obj) {
        clay_release((ClayObject *)v.as.obj);
    } else if (v.type == CLAYVM_VAL_CELL && v.as.obj) {
        clay_cell_release((ClayCell *)v.as.obj);
    }
}

/* Gated helpers — no-op when retain/release not wired */
static void vm_retain_val(clayvm_value_t v) {
    if (g_vm && g_vm->retain_fn) g_vm->retain_fn(v);
}

static void vm_release_val(clayvm_value_t v) {
    if (g_vm && g_vm->release_fn) g_vm->release_fn(v);
}

/* Release a ClayValue if it's a heap object (not tagged int/float/nil/bool) */
static void release_clay_val(ClayValue v) {
    if (CLAY_IS_PTR(v) && !CLAY_IS_NIL(v) &&
        v != CLAY_TRUE && v != CLAY_FALSE) {
        clay_release(CLAY_OBJ(v));
    }
}

/* Map VM opcode to Clay operator string */
static const char *opcode_to_selector(int opcode) {
    switch (opcode) {
        case CLAYVM_OP_ADD: return "+";
        case CLAYVM_OP_SUB: return "-";
        case CLAYVM_OP_MUL: return "*";
        case CLAYVM_OP_DIV: return "/";
        case CLAYVM_OP_MOD: return "%";
        case CLAYVM_OP_EQ:  return "==";
        case CLAYVM_OP_NEQ: return "!=";
        case CLAYVM_OP_LT:  return "<";
        case CLAYVM_OP_GT:  return ">";
        case CLAYVM_OP_LTE: return "<=";
        case CLAYVM_OP_GTE: return ">=";
        default: return "+";
    }
}

/* Fallback for binary ops on non-numeric types (e.g., string + string).
 * Converts VM values to Clay values, dispatches via clay_send_val. */
static void arith_fallback_fn(clayvm_vm_t *vm, clayvm_value_t a,
                               clayvm_value_t b, int opcode) {
    /* Update source line for error messages (only for main code, not chunks) */
    if (g_bc && vm->code == g_bc->code && vm->ip < (size_t)g_bc->len) {
        clay_source_line = g_bc->line_map[vm->ip];
    }
    ClayValue receiver = vm_to_clay(a);
    ClayValue arg = vm_to_clay(b);
    const char *sel = opcode_to_selector(opcode);
    ClayValue result = clay_send_val(receiver, sel, 1, &arg);

    /* Alias check: if result is same as an operand, retain before releasing */
    int aliased = 0;
    if (CLAY_IS_PTR(result) && !CLAY_IS_NIL(result) &&
        result != CLAY_TRUE && result != CLAY_FALSE) {
        if (result == receiver || result == arg) aliased = 1;
        if (aliased) clay_retain(CLAY_OBJ(result));
    }

    /* Release consumed operands (they were popped from stack by the caller) */
    vm_release_val(a);
    vm_release_val(b);

    clayvm_vm_push(vm, clay_to_vm(result));
}

/* --- Per-callsite inline cache --- */
#define IC_TABLE_SIZE 4096
#define IC_TABLE_MASK (IC_TABLE_SIZE - 1)

typedef struct {
    size_t      callsite_ip;
    HashTable  *methods_key;
    const char *selector;
    ClayMethod *method;
} InlineCacheEntry;

static CLAY_THREAD_LOCAL InlineCacheEntry g_ic_table[IC_TABLE_SIZE];

/* Separate IC table for slot reads — avoids evicting method IC entries */
typedef struct {
    size_t      callsite_ip;
    HashTable  *methods_key;
    const char *selector;
} SlotICEntry;

static CLAY_THREAD_LOCAL SlotICEntry g_slot_ic[IC_TABLE_SIZE];

/* Invalidate IC entries referencing a freed methods hash table */
static void bc_ic_invalidate(void *methods_ptr) {
    HashTable *target = (HashTable *)methods_ptr;
    for (int i = 0; i < IC_TABLE_SIZE; i++) {
        if (g_ic_table[i].methods_key == target)
            g_ic_table[i].methods_key = NULL;
        if (g_slot_ic[i].methods_key == target)
            g_slot_ic[i].methods_key = NULL;
    }
}

/* native_send(receiver, selector_str, arg1, arg2, ...) */
static int native_send_fn(clayvm_vm_t *vm, int argc) {
    if (argc < 2) { clayvm_vm_push(vm, CLAYVM_NIL_VAL); return 0; }

    /* Update source line for error messages (only for main code, not chunks) */
    if (g_bc && vm->code == g_bc->code && vm->ip < (size_t)g_bc->len) {
        clay_source_line = g_bc->line_map[vm->ip];
    }

    /* The operands stay in the stack array after popping; during the call
     * sp is raised over them again so an error unwinding past this send
     * releases them (vm_try_restore), and lowered afterwards. */
    int send_top = vm->sp, send_bottom = vm->sp - argc;

    /* Pop args in reverse: first popped = last arg */
    int msg_argc = argc - 2;
    ClayValue stack_args[8];
    ClayValue *args = NULL;
    if (msg_argc > 0) {
        args = (msg_argc <= 8) ? stack_args :
               (ClayValue *)malloc(sizeof(ClayValue) * (size_t)msg_argc);
        for (int i = msg_argc - 1; i >= 0; i--)
            args[i] = vm_to_clay(clayvm_vm_pop(vm));
    }

    clayvm_value_t sel_val = clayvm_vm_pop(vm);
    clayvm_value_t recv_val = clayvm_vm_pop(vm);

    ClayValue receiver = vm_to_clay(recv_val);
    const char *selector = clay_string_data((ClayObject *)sel_val.as.obj);

    /* Fast-path: tagged number arithmetic — avoids wrapper clone entirely.
     * Guard: receiver is tagged number, 1 arg that's also tagged, short selector. */
    if ((CLAY_IS_INT(receiver) || CLAY_IS_TAGGED_FLOAT(receiver)) &&
        msg_argc == 1 && (CLAY_IS_INT(args[0]) || CLAY_IS_TAGGED_FLOAT(args[0])) &&
        selector[0] != '\0' && selector[1] <= '=') {
        ClayValue arg = args[0];
        char c0 = selector[0], c1 = selector[1];
        int both_int = CLAY_IS_INT(receiver) && CLAY_IS_INT(arg);
        ClayValue fast_result = CLAY_NIL;
        int handled = 0;
        if (both_int) {
            intptr_t a = clay_to_int(receiver), b = clay_to_int(arg);
            if      (c0 == '+' && c1 == '\0') { fast_result = clay_int(a + b); handled = 1; }
            else if (c0 == '-' && c1 == '\0') { fast_result = clay_int(a - b); handled = 1; }
            else if (c0 == '*' && c1 == '\0') { fast_result = clay_int(clay_int_mul(a, b)); handled = 1; }
            else if (c0 == '/' && c1 == '\0' && b != 0) { fast_result = clay_int(a / b); handled = 1; }
            else if (c0 == '%' && c1 == '\0' && b != 0) { fast_result = clay_int(a % b); handled = 1; }
            else if (c0 == '<' && c1 == '\0') { fast_result = CLAY_BOOL(a < b); handled = 1; }
            else if (c0 == '>' && c1 == '\0') { fast_result = CLAY_BOOL(a > b); handled = 1; }
            else if (c0 == '<' && c1 == '=')  { fast_result = CLAY_BOOL(a <= b); handled = 1; }
            else if (c0 == '>' && c1 == '=')  { fast_result = CLAY_BOOL(a >= b); handled = 1; }
            else if (c0 == '=' && c1 == '=')  { fast_result = CLAY_BOOL(a == b); handled = 1; }
            else if (c0 == '!' && c1 == '=')  { fast_result = CLAY_BOOL(a != b); handled = 1; }
        }
        if (!handled) {
            float fa = CLAY_IS_TAGGED_FLOAT(receiver) ? clay_float_untag(receiver) : (float)clay_to_int(receiver);
            float fb = CLAY_IS_TAGGED_FLOAT(arg) ? clay_float_untag(arg) : (float)clay_to_int(arg);
            if      (c0 == '+' && c1 == '\0') { fast_result = clay_float_tag(fa + fb); handled = 1; }
            else if (c0 == '-' && c1 == '\0') { fast_result = clay_float_tag(fa - fb); handled = 1; }
            else if (c0 == '*' && c1 == '\0') { fast_result = clay_float_tag(fa * fb); handled = 1; }
            else if (c0 == '/' && c1 == '\0') { fast_result = clay_float_tag(fa / fb); handled = 1; }  /* IEEE: x/0.0 → inf */
            else if (c0 == '<' && c1 == '\0') { fast_result = CLAY_BOOL(fa < fb); handled = 1; }
            else if (c0 == '>' && c1 == '\0') { fast_result = CLAY_BOOL(fa > fb); handled = 1; }
            else if (c0 == '<' && c1 == '=')  { fast_result = CLAY_BOOL(fa <= fb); handled = 1; }
            else if (c0 == '>' && c1 == '=')  { fast_result = CLAY_BOOL(fa >= fb); handled = 1; }
            else if (c0 == '=' && c1 == '=')  { fast_result = CLAY_BOOL(fa == fb); handled = 1; }
            else if (c0 == '!' && c1 == '=')  { fast_result = CLAY_BOOL(fa != fb); handled = 1; }
        }
        if (handled) {
            vm_release_val(sel_val);
            if (args && args != stack_args) free(args);
            clayvm_vm_push(vm, clay_to_vm(fast_result));
            return 0;
        }
    }

    /* Fast-path: unary number operations (0-arg sends on tagged numbers) */
    if ((CLAY_IS_INT(receiver) || CLAY_IS_TAGGED_FLOAT(receiver)) && msg_argc == 0) {
        ClayValue fast_result = CLAY_NIL;
        int handled = 0;
        if (strcmp(selector, "negated") == 0) {
            if (CLAY_IS_INT(receiver)) fast_result = clay_int(-clay_to_int(receiver));
            else fast_result = clay_float_tag(-clay_float_untag(receiver));
            handled = 1;
        } else if (strcmp(selector, "abs") == 0) {
            if (CLAY_IS_INT(receiver)) {
                intptr_t v = clay_to_int(receiver);
                fast_result = clay_int(v < 0 ? -v : v);
            } else {
                float v = clay_float_untag(receiver);
                fast_result = clay_float_tag(v < 0.0f ? -v : v);
            }
            handled = 1;
        } else if (strcmp(selector, "toFloat") == 0) {
            if (CLAY_IS_INT(receiver)) fast_result = clay_float_tag((float)clay_to_int(receiver));
            else fast_result = receiver;
            handled = 1;
        } else if (strcmp(selector, "toInt") == 0) {
            if (CLAY_IS_TAGGED_FLOAT(receiver)) fast_result = clay_int(clay_float_to_int(clay_float_untag(receiver)));
            else fast_result = receiver;
            handled = 1;
        }
        if (handled) {
            vm_release_val(sel_val);
            if (args && args != stack_args) free(args);
            clayvm_vm_push(vm, clay_to_vm(fast_result));
            return 0;
        }
    }

    /* Fast-path: Vec3 operations — inline vec3 arithmetic without full dispatch */
    if (CLAY_IS_PTR(receiver) && !CLAY_IS_NIL(receiver) &&
        receiver != CLAY_TRUE && receiver != CLAY_FALSE) {
        ClayObject *rv = CLAY_OBJ(receiver);
        if (rv->type == CLAY_TYPE_VEC3) {
            ClayValue fast_result = CLAY_NIL;
            int handled = 0;
            char c0 = selector[0], c1 = selector[1];

            if (msg_argc == 1) {
                ClayValue arg = args[0];
                /* Vec3 + Vec3, Vec3 - Vec3 */
                if (CLAY_IS_PTR(arg) && !CLAY_IS_NIL(arg) &&
                    arg != CLAY_TRUE && arg != CLAY_FALSE &&
                    CLAY_OBJ(arg)->type == CLAY_TYPE_VEC3) {
                    ClayObject *av = CLAY_OBJ(arg);
                    if (c0 == '+' && c1 == '\0') {
                        fast_result = CLAY_VAL(clay_vec3_new(
                            rv->as.vec3.x + av->as.vec3.x,
                            rv->as.vec3.y + av->as.vec3.y,
                            rv->as.vec3.z + av->as.vec3.z));
                        handled = 1;
                    } else if (c0 == '-' && c1 == '\0') {
                        fast_result = CLAY_VAL(clay_vec3_new(
                            rv->as.vec3.x - av->as.vec3.x,
                            rv->as.vec3.y - av->as.vec3.y,
                            rv->as.vec3.z - av->as.vec3.z));
                        handled = 1;
                    } else if (c0 == 'd' && strcmp(selector, "dot") == 0) {
                        fast_result = clay_float_tag(
                            rv->as.vec3.x * av->as.vec3.x +
                            rv->as.vec3.y * av->as.vec3.y +
                            rv->as.vec3.z * av->as.vec3.z);
                        handled = 1;
                    } else if (c0 == 'c' && strcmp(selector, "cross") == 0) {
                        fast_result = CLAY_VAL(clay_vec3_new(
                            rv->as.vec3.y * av->as.vec3.z - rv->as.vec3.z * av->as.vec3.y,
                            rv->as.vec3.z * av->as.vec3.x - rv->as.vec3.x * av->as.vec3.z,
                            rv->as.vec3.x * av->as.vec3.y - rv->as.vec3.y * av->as.vec3.x));
                        handled = 1;
                    } else if (c0 == 'm' && strcmp(selector, "mul") == 0) {
                        fast_result = CLAY_VAL(clay_vec3_new(
                            rv->as.vec3.x * av->as.vec3.x,
                            rv->as.vec3.y * av->as.vec3.y,
                            rv->as.vec3.z * av->as.vec3.z));
                        handled = 1;
                    } else if (c0 == 'r' && strcmp(selector, "reflect") == 0) {
                        float d = rv->as.vec3.x * av->as.vec3.x +
                                  rv->as.vec3.y * av->as.vec3.y +
                                  rv->as.vec3.z * av->as.vec3.z;
                        float t = 2.0f * d;
                        fast_result = CLAY_VAL(clay_vec3_new(
                            rv->as.vec3.x - av->as.vec3.x * t,
                            rv->as.vec3.y - av->as.vec3.y * t,
                            rv->as.vec3.z - av->as.vec3.z * t));
                        handled = 1;
                    }
                }
                /* Vec3 * scalar, Vec3 / scalar */
                if (!handled && (CLAY_IS_INT(arg) || CLAY_IS_TAGGED_FLOAT(arg))) {
                    float s = CLAY_IS_TAGGED_FLOAT(arg) ? clay_float_untag(arg) : (float)clay_to_int(arg);
                    if (c0 == '*' && c1 == '\0') {
                        fast_result = CLAY_VAL(clay_vec3_new(
                            rv->as.vec3.x * s, rv->as.vec3.y * s, rv->as.vec3.z * s));
                        handled = 1;
                    } else if (c0 == '/' && c1 == '\0' && s != 0.0f) {
                        float inv = 1.0f / s;
                        fast_result = CLAY_VAL(clay_vec3_new(
                            rv->as.vec3.x * inv, rv->as.vec3.y * inv, rv->as.vec3.z * inv));
                        handled = 1;
                    }
                }
            } else if (msg_argc == 0) {
                /* Unary Vec3 operations */
                if (c0 == 'x' && c1 == '\0') {
                    fast_result = clay_float_tag(rv->as.vec3.x); handled = 1;
                } else if (c0 == 'y' && c1 == '\0') {
                    fast_result = clay_float_tag(rv->as.vec3.y); handled = 1;
                } else if (c0 == 'z' && c1 == '\0') {
                    fast_result = clay_float_tag(rv->as.vec3.z); handled = 1;
                } else if (c0 == 'l' && strcmp(selector, "length") == 0) {
                    float x = rv->as.vec3.x, y = rv->as.vec3.y, z = rv->as.vec3.z;
                    fast_result = clay_float_tag(sqrtf(x*x + y*y + z*z));
                    handled = 1;
                } else if (c0 == 'l' && strcmp(selector, "lengthSquared") == 0) {
                    float x = rv->as.vec3.x, y = rv->as.vec3.y, z = rv->as.vec3.z;
                    fast_result = clay_float_tag(x*x + y*y + z*z);
                    handled = 1;
                } else if (c0 == 'n' && strcmp(selector, "normalize") == 0) {
                    float x = rv->as.vec3.x, y = rv->as.vec3.y, z = rv->as.vec3.z;
                    float len = sqrtf(x*x + y*y + z*z);
                    if (len > 0.0f) {
                        float inv = 1.0f / len;
                        fast_result = CLAY_VAL(clay_vec3_new(x*inv, y*inv, z*inv));
                    } else {
                        fast_result = CLAY_VAL(clay_vec3_new(0, 0, 0));
                    }
                    handled = 1;
                } else if (c0 == 'n' && strcmp(selector, "negated") == 0) {
                    fast_result = CLAY_VAL(clay_vec3_new(-rv->as.vec3.x, -rv->as.vec3.y, -rv->as.vec3.z));
                    handled = 1;
                } else if (c0 == 'n' && strcmp(selector, "nearZero") == 0) {
                    float s = 0.00001f;
                    fast_result = CLAY_BOOL(fabsf(rv->as.vec3.x) < s &&
                                            fabsf(rv->as.vec3.y) < s &&
                                            fabsf(rv->as.vec3.z) < s);
                    handled = 1;
                }
            }
            if (handled) {
                vm_release_val(sel_val);
                release_clay_val(receiver);
                for (int i = 0; i < msg_argc; i++)
                    release_clay_val(args[i]);
                if (args && args != stack_args) free(args);
                clayvm_vm_push(vm, clay_to_vm(fast_result));
                return 0;
            }
        }
    }

    /* Determine receiver object for IC keying */
    ClayObject *target_obj = NULL;
    HashTable *target_methods = NULL;

    if (CLAY_IS_INT(receiver) || CLAY_IS_TAGGED_FLOAT(receiver)) {
        target_obj = clay_Number;
        target_methods = clay_Number->methods;
    } else if (!CLAY_IS_NIL(receiver)) {
        target_obj = CLAY_OBJ(receiver);
        /* Use parent methods for IC keying when instance has no own methods
         * (common after lazy methods optimization). This groups instances by
         * prototype type, which is the correct semantic for IC matching. */
        target_methods = target_obj->methods;
        if (!target_methods && target_obj->parent)
            target_methods = target_obj->parent->methods;
    }

    vm->sp = send_top;

    /* Per-callsite inline cache check */
    size_t site_ip = vm->ip;
    unsigned int ic_idx = (unsigned int)(site_ip >> 2) & IC_TABLE_MASK;
    InlineCacheEntry *ic = &g_ic_table[ic_idx];
    ClayValue result;
    int ic_handled = 0;

    if (target_obj &&
        ic->callsite_ip == site_ip &&
        ic->methods_key == target_methods &&
        ic->selector == selector &&
        !clay_selector_shadowed(selector)) {
        /* Method IC hit — invoke directly, skip dispatch */
        if (CLAY_IS_INT(receiver) || CLAY_IS_TAGGED_FLOAT(receiver)) {
            ClayObject *wrapper = clay_clone(clay_Number);
            clay_set_slot(wrapper, "_value", receiver);
            clay_unwind_push(wrapper);
            result = clay_invoke(ic->method, wrapper, msg_argc, args);
            clay_unwind_pop();
            if (result == CLAY_VAL(wrapper)) {
                clay_release(wrapper);
                result = receiver;
            } else {
                clay_release(wrapper);
            }
        } else {
            result = clay_invoke(ic->method, target_obj, msg_argc, args);
        }
        ic_handled = 1;
    }

    /* Slot-read IC check (separate table, only for 0-arg sends on objects) */
    if (!ic_handled && msg_argc == 0 && target_obj &&
        !CLAY_IS_INT(receiver) && !CLAY_IS_TAGGED_FLOAT(receiver) &&
        !clay_selector_shadowed(selector)) {
        SlotICEntry *sic = &g_slot_ic[ic_idx];
        if (sic->callsite_ip == site_ip &&
            sic->methods_key == target_methods &&
            sic->selector == selector) {
            ClayValue slot_val = clay_get_slot(target_obj, selector);
            if (!CLAY_IS_NIL(slot_val) && CLAY_IS_PTR(slot_val) &&
                slot_val != CLAY_TRUE && slot_val != CLAY_FALSE) {
                ClayObject *slot_obj = CLAY_OBJ(slot_val);
                if (slot_obj->type == CLAY_TYPE_BLOCK && slot_obj->as.block.func) {
                    /* Block slot — needs invocation, fall through to full dispatch */
                } else {
                    clay_retain(slot_obj);
                    result = slot_val;
                    ic_handled = 1;
                }
            } else {
                result = slot_val;
                ic_handled = 1;
            }
        }
    }

    if (!ic_handled) {
        /* IC miss — full dispatch */
        result = clay_send_val(receiver, selector, msg_argc, args);
        /* Update method IC or slot IC */
        if (target_obj) {
            ClayMethod *m = clay_cached_lookup(target_obj, selector);
            if (m) {
                ic->callsite_ip = site_ip;
                ic->methods_key = target_methods;
                ic->selector = selector;
                ic->method = m;
            } else if (msg_argc == 0 &&
                       !CLAY_IS_INT(receiver) && !CLAY_IS_TAGGED_FLOAT(receiver) &&
                       !CLAY_IS_NIL(receiver) && clay_has_slot(target_obj, selector)) {
                SlotICEntry *sic = &g_slot_ic[ic_idx];
                sic->callsite_ip = site_ip;
                sic->methods_key = target_methods;
                sic->selector = selector;
            }
        }
    }

    vm->sp = send_bottom;

    /* Alias check: if result is same as an input, retain before releasing */
    int aliased = 0;
    if (CLAY_IS_PTR(result) && !CLAY_IS_NIL(result) &&
        result != CLAY_TRUE && result != CLAY_FALSE) {
        if (result == receiver) aliased = 1;
        for (int i = 0; i < msg_argc && !aliased; i++) {
            if (result == args[i]) aliased = 1;
        }
        if (aliased) clay_retain(CLAY_OBJ(result));
    }

    /* Release consumed values */
    vm_release_val(recv_val);
    vm_release_val(sel_val);
    for (int i = 0; i < msg_argc; i++)
        release_clay_val(args[i]);

    if (args && args != stack_args) free(args);
    clayvm_vm_push(vm, clay_to_vm(result));
    return 0;
}

/* native_super_send(receiver, selector_str, arg1, arg2, ...) */
static int native_super_send_fn(clayvm_vm_t *vm, int argc) {
    if (argc < 2) { clayvm_vm_push(vm, CLAYVM_NIL_VAL); return 0; }

    int msg_argc = argc - 2;
    ClayValue stack_args[8];
    ClayValue *args = NULL;
    if (msg_argc > 0) {
        args = (msg_argc <= 8) ? stack_args :
               (ClayValue *)malloc(sizeof(ClayValue) * (size_t)msg_argc);
        for (int i = msg_argc - 1; i >= 0; i--)
            args[i] = vm_to_clay(clayvm_vm_pop(vm));
    }

    clayvm_value_t sel_val = clayvm_vm_pop(vm);
    clayvm_value_t recv_val = clayvm_vm_pop(vm);

    ClayValue receiver = vm_to_clay(recv_val);
    const char *selector = clay_string_data((ClayObject *)sel_val.as.obj);

    ClayValue result = clay_send_super(receiver, g_super_proto, selector, msg_argc, args);

    /* Alias check */
    int aliased = 0;
    if (CLAY_IS_PTR(result) && !CLAY_IS_NIL(result) &&
        result != CLAY_TRUE && result != CLAY_FALSE) {
        if (result == receiver) aliased = 1;
        for (int i = 0; i < msg_argc && !aliased; i++) {
            if (result == args[i]) aliased = 1;
        }
        if (aliased) clay_retain(CLAY_OBJ(result));
    }

    /* Release consumed values */
    vm_release_val(recv_val);
    vm_release_val(sel_val);
    for (int i = 0; i < msg_argc; i++)
        release_clay_val(args[i]);

    if (args && args != stack_args) free(args);

    clayvm_vm_push(vm, clay_to_vm(result));
    return 0;
}

/* native_clone(proto) -> cloned object */
static int native_clone_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t proto_val = clayvm_vm_pop(vm);
    ClayValue proto = vm_to_clay(proto_val);
    ClayObject *obj = clay_clone(CLAY_OBJ(proto));
    vm_release_val(proto_val);
    clayvm_vm_push(vm, CLAYVM_OBJ(obj));
    return 0;
}

/* native_set_slot(obj, name_str, value) */
static int native_set_slot_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val_vm = clayvm_vm_pop(vm);
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    clayvm_value_t obj_val = clayvm_vm_pop(vm);
    ClayValue val = vm_to_clay(val_vm);
    ClayValue obj = vm_to_clay(obj_val);
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);
    clay_set_slot(CLAY_OBJ(obj), name, val);
    vm_release_val(name_val);
    vm_release_val(obj_val);
    clayvm_vm_push(vm, val_vm); /* return assigned value — transfer pop's ownership */
    return 0;
}

/* native_println(value) */
static int native_println_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val = clayvm_vm_pop(vm);
    ClayValue cv = vm_to_clay(val);
    char buf[256];
    if (CLAY_IS_INT(cv)) {
        snprintf(buf, sizeof(buf), "%lld\n", (long long)clay_to_int(cv));
    } else if (CLAY_IS_TAGGED_FLOAT(cv)) {
        clay_format_float(clay_float_untag(cv), buf, sizeof(buf) - 1);
        strcat(buf, "\n");
    } else if (CLAY_IS_NIL(cv)) {
        snprintf(buf, sizeof(buf), "nil\n");
    } else if (cv == CLAY_TRUE) {
        snprintf(buf, sizeof(buf), "true\n");
    } else if (cv == CLAY_FALSE) {
        snprintf(buf, sizeof(buf), "false\n");
    } else {
        ClayValue sv = clay_send_val(cv, "toString", 0, NULL);
        snprintf(buf, sizeof(buf), "%s\n", clay_string_data(CLAY_OBJ(sv)));
        clay_release(CLAY_OBJ(sv));
    }
    vm_release_val(val);
    if (clay_output_fn) {
        clay_output_fn(buf);
    } else {
        fputs(buf, stdout);
    }
    clayvm_vm_push(vm, CLAYVM_NIL_VAL);
    return 0;
}

/* native_make_block(chunk_id, param_count, capture0, ..., captureN, capture_count) */
static int native_make_block_fn(clayvm_vm_t *vm, int argc) {
    /* Pop capture_count (last arg) */
    clayvm_value_t cc_val = clayvm_vm_pop(vm);
    int capture_count = (int)cc_val.as.i64;

    /* Pop captured values in reverse order */
    clayvm_value_t captured[BC_MAX_CAPTURES];
    if (capture_count > BC_MAX_CAPTURES) capture_count = BC_MAX_CAPTURES;  /* compiler rejects more */
    for (int i = capture_count - 1; i >= 0; i--) {
        captured[i] = clayvm_vm_pop(vm);
    }

    /* Pop param_count and chunk_id */
    clayvm_value_t pc_val = clayvm_vm_pop(vm);
    clayvm_value_t id_val = clayvm_vm_pop(vm);
    int chunk_id = (int)id_val.as.i64;

    (void)pc_val;
    (void)argc;

    if (capture_count > 0) {
        /* Captures are cells — bypass clay_block_new's retain (which uses
         * clay_retain and can't handle ClayCell pointers). Set up env manually. */
        ClayObject *block = clay_block_new(bc_block_trampoline, NULL, 0);
        int env_size = 1 + capture_count;
        block->as.block.env = (ClayValue *)malloc(sizeof(ClayValue) * (size_t)env_size);
        block->as.block.env_size = env_size;
        block->as.block.env[0] = clay_int(chunk_id);
        for (int i = 0; i < capture_count; i++) {
            block->as.block.env[1 + i] = vm_to_clay(captured[i]);
            ClayValue cv = block->as.block.env[1 + i];
            if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) &&
                cv != CLAY_TRUE && cv != CLAY_FALSE) {
                clay_cell_retain((ClayCell *)CLAY_OBJ(cv));
            }
        }
        block->as.block.env_are_cells = 1;
        /* Release popped captured values (env took its own ref via cell_retain) */
        for (int i = 0; i < capture_count; i++)
            vm_release_val(captured[i]);
        clayvm_vm_push(vm, CLAYVM_OBJ(block));
    } else {
        /* No captures — use standard clay_block_new path.
         * clay_block_new copies the env array, so we free ours after. */
        ClayValue env_chunk = clay_int(chunk_id);
        ClayObject *block = clay_block_new(bc_block_trampoline, &env_chunk, 1);
        clayvm_vm_push(vm, CLAYVM_OBJ(block));
    }
    return 0;
}

/* native_cell_new(value) — create a ClayCell wrapping value */
static int native_cell_new_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val_vm = clayvm_vm_pop(vm);
    ClayValue val = vm_to_clay(val_vm);
    ClayCell *cell = clay_cell_new(val);
    vm_release_val(val_vm);
    clayvm_vm_push(vm, CLAYVM_CELL(cell));
    return 0;
}

/* native_cell_get(cell) — read value from cell */
static int native_cell_get_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t cv = clayvm_vm_pop(vm);
    if (cv.type != CLAYVM_VAL_CELL || cv.as.obj == NULL) {
        fprintf(stderr, "cell_get: expected cell object, got type %d\n", cv.type);
        clayvm_vm_push(vm, CLAYVM_NIL_VAL);
        return 0;
    }
    ClayCell *cell = (ClayCell *)cv.as.obj;
    clayvm_value_t result = clay_to_vm(cell->value);
    vm_retain_val(result); /* borrowed ref from cell — retain for stack */
    vm_release_val(cv);
    clayvm_vm_push(vm, result);
    return 0;
}

/* native_cell_set(cell, value) — write value to cell, return value */
static int native_cell_set_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t new_vm = clayvm_vm_pop(vm);
    clayvm_value_t cell_vm = clayvm_vm_pop(vm);
    ClayValue new_val = vm_to_clay(new_vm);
    ClayCell *cell = (ClayCell *)cell_vm.as.obj;
    /* Release old, retain new if needed */
    ClayValue old = cell->value;
    if (CLAY_IS_PTR(new_val) && !CLAY_IS_NIL(new_val) &&
        new_val != CLAY_TRUE && new_val != CLAY_FALSE) {
        clay_retain(CLAY_OBJ(new_val));
    }
    cell->value = new_val;
    if (CLAY_IS_PTR(old) && !CLAY_IS_NIL(old) &&
        old != CLAY_TRUE && old != CLAY_FALSE) {
        clay_release(CLAY_OBJ(old));
    }
    vm_release_val(cell_vm); /* release cell (CELL type) */
    clayvm_vm_push(vm, new_vm); /* transfer pop's ownership */
    return 0;
}

/* native_is_truthy(val) — push 1 or 0 (for JUMP_IF_FALSE) */
static int native_is_truthy_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t v = clayvm_vm_pop(vm);
    ClayValue cv = vm_to_clay(v);
    clayvm_vm_push(vm, CLAYVM_INT64(CLAY_IS_TRUTHY(cv) ? 1 : 0));
    vm_release_val(v);
    return 0;
}

/* native_to_bool(val) — push CLAY_TRUE or CLAY_FALSE */
static int native_to_bool_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t v = clayvm_vm_pop(vm);
    ClayValue cv = vm_to_clay(v);
    clayvm_vm_push(vm, clay_to_vm(CLAY_IS_TRUTHY(cv) ? CLAY_TRUE : CLAY_FALSE));
    vm_release_val(v);
    return 0;
}

/* native_is_nil(val) — push 1 if nil, 0 otherwise (for JUMP_IF_FALSE) */
static int native_is_nil_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t v = clayvm_vm_pop(vm);
    ClayValue cv = vm_to_clay(v);
    clayvm_vm_push(vm, CLAYVM_INT64(cv == CLAY_NIL ? 1 : 0));
    vm_release_val(v);
    return 0;
}

/* native_stop_vm() — stop the dispatch loop (for early return).
 * The top-of-stack value is preserved as the return value. */
static int native_stop_vm_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    vm->running = 0;
    return 1; /* non-zero also breaks the dispatch loop */
}

/* native_try_catch(try_block, catch_block) — setjmp-based exception handling */
static int native_try_catch_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t finally_vm = clayvm_vm_pop(vm);
    clayvm_value_t catch_vm = clayvm_vm_pop(vm);
    clayvm_value_t try_vm = clayvm_vm_pop(vm);
    ClayValue try_block = vm_to_clay(try_vm);
    ClayValue catch_block = vm_to_clay(catch_vm);
    ClayValue finally_block = vm_to_clay(finally_vm);

    /* Save VM state — longjmp from raise() bypasses trampoline restore */
    const uint8_t *saved_code = vm->code;
    size_t saved_code_len = vm->code_len;
    size_t saved_ip = vm->ip;
    size_t saved_sp = vm->sp;
    int saved_running = vm->running;
    int saved_frame_count = vm->frame_count;

    ClayTryFrame _tf;
    if (clay_try_push(&_tf) == 0) {
        ClayValue result = clay_send_val(try_block, "value", 0, NULL);
        clay_try_pop();
        vm->code = saved_code;
        vm->code_len = saved_code_len;
        vm->ip = saved_ip;
        vm->sp = saved_sp;
        vm->running = saved_running;
        vm->frame_count = saved_frame_count;
        if (!CLAY_IS_NIL(finally_block)) {
            clay_release_val(clay_send_val(finally_block, "value", 0, NULL));
        }
        vm_release_val(try_vm);
        vm_release_val(catch_vm);
        vm_release_val(finally_vm);
        clayvm_vm_push(vm, clay_to_vm(result));
    } else {
        /* Error path — trampoline's restore was skipped by longjmp */
        vm->code = saved_code;
        vm->code_len = saved_code_len;
        vm->ip = saved_ip;
        vm->sp = saved_sp;
        vm->running = saved_running;
        vm->frame_count = saved_frame_count;

        ClayValue error = clay_error_stack.current_error;   /* we own it */
        if (clay_is_control_signal(error)) {
            /* break / continue / non-local return are not errors: run finally,
             * then let the signal continue to its loop or method */
            if (!CLAY_IS_NIL(finally_block)) {
                clay_release_val(clay_send_val(finally_block, "value", 0, NULL));
            }
            vm_release_val(try_vm);
            vm_release_val(catch_vm);
            vm_release_val(finally_vm);
            clay_raise(error);
        }
        ClayValue result;
        if (!CLAY_IS_NIL(catch_block)) {
            result = clay_send_val(catch_block, "value", 1, &error);
        } else {
            result = CLAY_NIL;
        }
        clay_release_val(error);   /* handled */
        if (!CLAY_IS_NIL(finally_block)) {
            clay_release_val(clay_send_val(finally_block, "value", 0, NULL));
        }
        vm_release_val(try_vm);
        vm_release_val(catch_vm);
        vm_release_val(finally_vm);
        clayvm_vm_push(vm, clay_to_vm(result));
    }
    return 0;
}

/* native_raise(value) — raise an exception via longjmp */
static int native_raise_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    /* Update source line for error messages (only for main code, not chunks) */
    if (g_bc && vm->code == g_bc->code && vm->ip < (size_t)g_bc->len) {
        clay_source_line = g_bc->line_map[vm->ip];
    }
    clayvm_value_t val_vm = clayvm_vm_pop(vm);
    ClayValue val = vm_to_clay(val_vm);
    if (CLAY_IS_PTR(val) && !CLAY_IS_NIL(val) &&
        CLAY_OBJ(val)->type == CLAY_TYPE_STRING) {
        /* Create error from string — must copy data before releasing */
        ClayValue err = CLAY_VAL(clay_error_new(clay_string_data(CLAY_OBJ(val)), "RuntimeError"));
        vm_release_val(val_vm);
        val = err;
    }
    /* Don't release non-string error objects — they survive through longjmp */
    clay_raise(val);
    /* clay_raise never returns (longjmp) */
    return 0;
}

/* native_make_range(start, end) — create a Range object */
static int native_make_range_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue end = vm_to_clay(clayvm_vm_pop(vm));
    ClayValue start = vm_to_clay(clayvm_vm_pop(vm));
    intptr_t s = CLAY_IS_INT(start) ? clay_to_int(start) : 0;
    intptr_t e = CLAY_IS_INT(end) ? clay_to_int(end) : 0;
    ClayObject *range = clay_range_new(s, e, 1);
    clayvm_vm_push(vm, clay_to_vm(CLAY_VAL(range)));
    return 0;
}

/* native_make_array() — create empty array */
static int native_make_array_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayObject *arr = clay_array_new();
    clayvm_vm_push(vm, CLAYVM_OBJ(arr));
    return 0;
}

/* native_make_dict() — create empty Dict */
static int native_make_dict_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayObject *dict = clay_dict_new();
    clayvm_vm_push(vm, CLAYVM_OBJ(dict));
    return 0;
}


/* native_set_method(obj, name_str, method_slot_index) */
static int native_set_method_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t slot_val = clayvm_vm_pop(vm);
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    clayvm_value_t obj_val = clayvm_vm_pop(vm);

    int slot_idx = (int)slot_val.as.i64;
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);
    ClayObject *obj = (ClayObject *)obj_val.as.obj;

    if (slot_idx >= 0 && slot_idx < g_method_slot_count) {
        MethodSlot *ms = g_method_slots[slot_idx];
        ms->defining_obj = obj;
        clay_set_method_ctx(obj, name, bc_method_dispatch, ms);
    } else {
        fprintf(stderr, "clay: invalid method slot index (%d/%d)\n",
                slot_idx, g_method_slot_count);
    }

    vm_release_val(name_val);
    clayvm_vm_push(vm, obj_val); /* transfer pop's ownership to push */
    return 0;
}

/* native_load_self() — fast self access via cached global, no hash lookup */
/* self_has_slot(name) — 1 if the current method's self (or its prototype
 * chain) has a slot `name`, else 0. Used at method entry for the
 * implicit-slot fallback: a bare assignment in a method writes an existing
 * slot instead of creating a local. */
static int native_self_has_slot_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    int has = 0;
    if (g_method_self && name_val.type == CLAYVM_VAL_OBJECT && name_val.as.obj)
        has = clay_has_slot(g_method_self, clay_string_data((ClayObject *)name_val.as.obj));
    vm_release_val(name_val);
    clayvm_vm_push(vm, CLAYVM_INT64(has));
    return 0;
}

/* Non-local return. bc_method_dispatch gives each activation of a method that
 * contains `return` inside a block an id (and catches the ReturnSignal);
 * the method's prologue reads it into its hidden "?ret" local, which nested
 * blocks capture and pass to raise_return. */

static int native_method_return_id_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_vm_push(vm, clay_to_vm(g_pending_return_id));
    return 0;
}

static int native_raise_return_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t id_vm = clayvm_vm_pop(vm);
    clayvm_value_t val_vm = clayvm_vm_pop(vm);
    ClayValue val = vm_to_clay(val_vm);
    ClayValue id = vm_to_clay(id_vm);
    clay_raise_return(val, id);     /* takes pop's reference to val */
    return 0;
}

/* set_self_slot(value, name) — self.name = value; leaves value on the stack */
static int native_set_self_slot_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    clayvm_value_t val_vm = clayvm_vm_pop(vm);
    if (g_method_self && name_val.type == CLAYVM_VAL_OBJECT && name_val.as.obj) {
        const char *name = clay_string_data((ClayObject *)name_val.as.obj);
        clay_set_slot(g_method_self, name, vm_to_clay(val_vm));
    }
    vm_release_val(name_val);
    clayvm_vm_push(vm, val_vm); /* transfer pop's ownership */
    return 0;
}

/* cell_set_rev(value, cell) — cell_set with the operands swapped */
static int native_cell_set_rev_fn(clayvm_vm_t *vm, int argc) {
    clayvm_value_t cell_vm = clayvm_vm_pop(vm);
    clayvm_value_t new_vm = clayvm_vm_pop(vm);
    clayvm_vm_push(vm, cell_vm);
    clayvm_vm_push(vm, new_vm);
    return native_cell_set_fn(vm, argc);
}

static int native_load_self_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    if (g_method_self) {
        clayvm_vm_push(vm, CLAYVM_OBJ(g_method_self));
        clay_retain(g_method_self);
    } else {
        clayvm_vm_push(vm, CLAYVM_NIL_VAL);
    }
    return 0;
}

/* native_load_global(name_str) */
static int native_load_global_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);

    /* Look up in globals hash table */
    HashTable *globals = (HashTable *)vm->globals;
    if (globals) {
        ht_value_t found = ht_find(globals, (ht_key_t)name);
        if (found) {
            clayvm_value_t *stored = (clayvm_value_t *)found;
            vm_retain_val(*stored); /* borrowed ref from global table */
            vm_release_val(name_val);
            clayvm_vm_push(vm, *stored);
            return 0;
        }
    }
    vm_release_val(name_val);
    clayvm_vm_push(vm, CLAYVM_NIL_VAL);
    return 0;
}

/* native_store_global(name_str, value) */
static int native_store_global_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val = clayvm_vm_pop(vm);
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);

    HashTable *globals = (HashTable *)vm->globals;
    if (!globals) {
        vm_release_val(name_val);
        vm_release_val(val);
        clayvm_vm_push(vm, CLAYVM_NIL_VAL);
        return 0;
    }

    /* Retain new object value for the global table */
    if (val.type == CLAYVM_VAL_OBJECT && val.as.obj) {
        clay_retain((ClayObject *)val.as.obj);
    }

    /* Update in-place if the global already exists (avoids heap churn) */
    ht_value_t old = ht_find(globals, (ht_key_t)name);
    if (old) {
        clayvm_value_t *existing = (clayvm_value_t *)old;
        if (existing->type == CLAYVM_VAL_OBJECT && existing->as.obj) {
            clay_release((ClayObject *)existing->as.obj);
        }
        *existing = val;
    } else {
        clayvm_value_t *stored = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
        *stored = val;
        char *key_copy = strdup(name);
        ht_add(globals, (ht_key_t)key_copy, (ht_value_t)stored);
    }

    vm_release_val(name_val);
    clayvm_vm_push(vm, val); /* transfer pop's ownership to push */
    return 0;
}

/* --- Compiler init --- */

static int globals_str_compare(ht_key_t a, ht_key_t b) {
    return strcmp((const char *)a, (const char *)b) == 0;
}

/* Initialize a worker thread's VM context (called via clay_thread_set_vm_init callback) */
static BcCompiler *g_main_bc = NULL;  /* shared: main thread's compiler (set once) */

void bc_worker_thread_init(void) {
    static CLAY_THREAD_LOCAL BcCompiler worker_bc;
    static CLAY_THREAD_LOCAL clayvm_vm_t worker_vm;
    clayvm_vm_init(&worker_vm);
    bc_compiler_init(&worker_bc, &worker_vm);

    /* Share chunks, constants, and globals from main thread */
    if (g_main_bc) {
        worker_bc.chunks = g_main_bc->chunks;
        worker_bc.chunk_count = g_main_bc->chunk_count;
        worker_bc.chunk_capacity = g_main_bc->chunk_capacity;

        /* Share constant pool (values are immutable after compilation) */
        worker_vm.constants = g_main_bc->vm->constants;

        /* Share globals table (user globals + builtins).
         * Worker globals table was freshly created by bc_compiler_init
         * with only builtins — replace it with the main thread's table
         * which also has user-defined globals like closures. */
        worker_vm.globals = g_main_bc->vm->globals;
    }
}

void bc_set_main_compiler(BcCompiler *bc) {
    g_main_bc = bc;
}

void bc_compiler_init(BcCompiler *bc, clayvm_vm_t *vm) {
    memset(bc, 0, sizeof(*bc));
    bc->vm = vm;

    /* Allocate dynamic chunks array */
    bc->chunk_capacity = BC_INITIAL_CHUNKS;
    bc->chunks = (BcChunk **)calloc((size_t)bc->chunk_capacity, sizeof(BcChunk *));

    /* Set global pointers for trampoline access */
    g_bc = bc;
    g_vm = vm;

    /* Set up globals hash table */
    if (!vm->globals) {
        HashTable *g = ht_create();
        ht_set_hash_func(g, HT_HASH_STRING);
        ht_set_compare_func(g, globals_str_compare);
        vm->globals = g;
    }

    /* Register native functions */
    vm->arith_fallback = arith_fallback_fn;
    clay_ic_invalidate_hook = bc_ic_invalidate;
    clay_fiber_hooks = &bc_fiber_hooks;
    clay_try_hooks = &bc_try_hooks;
    vm->retain_fn = vm_retain_cb;
    vm->release_fn = vm_release_cb;
    bc->native_send = clayvm_native_register(&vm->natives, "send", native_send_fn, -1);
    bc->native_super_send = clayvm_native_register(&vm->natives, "super_send", native_super_send_fn, -1);
    bc->native_clone = clayvm_native_register(&vm->natives, "clone", native_clone_fn, 1);
    bc->native_set_slot = clayvm_native_register(&vm->natives, "set_slot", native_set_slot_fn, 3);
    bc->native_set_method = clayvm_native_register(&vm->natives, "set_method", native_set_method_fn, 3);
    bc->native_println = clayvm_native_register(&vm->natives, "println", native_println_fn, 1);
    bc->native_make_block = clayvm_native_register(&vm->natives, "make_block", native_make_block_fn, 2);
    bc->native_make_array = clayvm_native_register(&vm->natives, "make_array", native_make_array_fn, 0);
    bc->native_make_dict = clayvm_native_register(&vm->natives, "make_dict", native_make_dict_fn, 0);
    bc->native_load_global = clayvm_native_register(&vm->natives, "load_global", native_load_global_fn, 1);
    bc->native_load_self = clayvm_native_register(&vm->natives, "load_self", native_load_self_fn, 0);
    bc->native_store_global = clayvm_native_register(&vm->natives, "store_global", native_store_global_fn, 2);
    bc->native_cell_new = clayvm_native_register(&vm->natives, "cell_new", native_cell_new_fn, 1);
    bc->native_cell_get = clayvm_native_register(&vm->natives, "cell_get", native_cell_get_fn, 1);
    bc->native_cell_set = clayvm_native_register(&vm->natives, "cell_set", native_cell_set_fn, 2);
    bc->native_is_truthy = clayvm_native_register(&vm->natives, "is_truthy", native_is_truthy_fn, 1);
    bc->native_to_bool = clayvm_native_register(&vm->natives, "to_bool", native_to_bool_fn, 1);
    bc->native_is_nil = clayvm_native_register(&vm->natives, "is_nil", native_is_nil_fn, 1);
    bc->native_self_has_slot = clayvm_native_register(&vm->natives, "self_has_slot", native_self_has_slot_fn, 1);
    bc->native_set_self_slot = clayvm_native_register(&vm->natives, "set_self_slot", native_set_self_slot_fn, 2);
    bc->native_cell_set_rev = clayvm_native_register(&vm->natives, "cell_set_rev", native_cell_set_rev_fn, 2);
    bc->native_method_return_id = clayvm_native_register(&vm->natives, "method_return_id", native_method_return_id_fn, 0);
    bc->native_raise_return = clayvm_native_register(&vm->natives, "raise_return", native_raise_return_fn, 2);
    bc->native_stop_vm = clayvm_native_register(&vm->natives, "stop_vm", native_stop_vm_fn, 0);
    bc->native_try_catch = clayvm_native_register(&vm->natives, "try_catch", native_try_catch_fn, 3);
    bc->native_raise = clayvm_native_register(&vm->natives, "raise", native_raise_fn, 1);
    bc->native_make_range = clayvm_native_register(&vm->natives, "make_range", native_make_range_fn, 2);

    /* Pre-register Clay builtin prototypes as globals */
    struct { const char *name; ClayObject *obj; } builtins[] = {
        {"Object", clay_Object}, {"Number", clay_Number}, {"String", clay_String},
        {"Array", clay_Array}, {"Block", clay_Block}, {"Nil", clay_Nil},
        {"Boolean", clay_Boolean}, {"Error", clay_Error}, {"Dict", clay_Dict},
        {"Math", clay_Math}, {"Console", clay_Console}, {"File", clay_File},
        {"System", clay_System},
        {"FFI", clay_FFI},
        {"Date", clay_Date},
        {"Http", clay_Http},
        {"Regex", clay_Regex},
        {"Crypto", clay_Crypto},
        {"Process", clay_Process},
        {"Range", clay_Range},
        {"Fiber", clay_Fiber},
        {"Db", clay_Db},
        {"Graphics", clay_Graphics},
        {"Vec3", clay_Vec3},
        {"Thread", clay_Thread},
        {"BreakSignal", clay_BreakSignal},
        {"ContinueSignal", clay_ContinueSignal},
        {"VM", clay_VM},
        {NULL, NULL}
    };

    HashTable *g = (HashTable *)vm->globals;
    for (int i = 0; builtins[i].name; i++) {
        clayvm_value_t *stored = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
        *stored = CLAYVM_OBJ(builtins[i].obj);
        char *key = strdup(builtins[i].name);
        ht_add(g, (ht_key_t)key, (ht_value_t)stored);
    }

    /* Also store true/false/nil */
    clayvm_value_t *tv = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *tv = CLAYVM_BOOL(1);
    ht_add(g, (ht_key_t)strdup("true"), (ht_value_t)tv);

    clayvm_value_t *fv = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *fv = CLAYVM_BOOL(0);
    ht_add(g, (ht_key_t)strdup("false"), (ht_value_t)fv);

    clayvm_value_t *nv = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
    *nv = CLAYVM_NIL_VAL;
    ht_add(g, (ht_key_t)strdup("nil"), (ht_value_t)nv);

    /* Add plugin-registered globals */
    for (int i = 0; ; i++) {
        ClayObject *pobj = NULL;
        const char *pname = NULL;
        /* Iterate plugin globals via the public API */
        extern int clay_plugin_is_global(const char *name);
        extern ClayObject *clay_plugin_get_global(const char *name);
        /* Use index-based access — need an accessor */
        extern int clay_plugin_global_count(void);
        extern const char *clay_plugin_global_name(int idx);
        extern ClayObject *clay_plugin_global_object(int idx);
        if (i >= clay_plugin_global_count()) break;
        pname = clay_plugin_global_name(i);
        pobj = clay_plugin_global_object(i);
        if (!pname || !pobj) continue;
        clayvm_value_t *pv = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
        *pv = CLAYVM_OBJ(pobj);
        ht_add(g, (ht_key_t)strdup(pname), (ht_value_t)pv);
    }
}


/* --- The VM prototype: bytecode compiled by Clay programs ---
 *
 * The self-hosted bytecode compiler (selfhost/clay-bytecode) mirrors the C
 * compiler's state — constant pool, chunks, method slots, known globals — and
 * hands each compiled program to `VM run(unit)`, which appends its new
 * constants, chunks and methods to the VM and runs its top-level code. One VM
 * per process: it is created on first use and keeps its globals, so a REPL
 * runs one input after another in it. Inside `clay` (whose own VM is already
 * running the program) the prototype is unavailable.
 */

static clayvm_vm_t *g_host_vm = NULL;
static BcCompiler *g_host_bc = NULL;

static void vm_host_check(void) {
    if (g_host_vm) return;
    if (g_bc)
        clay_raise(CLAY_VAL(clay_error_new(
            "VM is not available to programs running in the VM (build the program "
            "with clay --compile)", "RuntimeError")));
    g_host_vm = (clayvm_vm_t *)calloc(1, sizeof(clayvm_vm_t));
    g_host_bc = (BcCompiler *)malloc(sizeof(BcCompiler));
    clayvm_vm_init(g_host_vm);
    bc_compiler_init(g_host_bc, g_host_vm);
    clay_thread_set_vm_init(bc_worker_thread_init);
    bc_set_main_compiler(g_host_bc);
}

static ClayObject *vm_arg_array(ClayValue v, const char *what) {
    if (!CLAY_IS_TYPE(v, CLAY_TYPE_ARRAY)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "VM run: %s must be an Array", what);
        clay_raise(CLAY_VAL(clay_error_new(msg, "TypeError")));
    }
    return CLAY_OBJ(v);
}

static int vm_arg_int(ClayValue v, const char *what) {
    if (!CLAY_IS_INT(v)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "VM run: %s must be an integer", what);
        clay_raise(CLAY_VAL(clay_error_new(msg, "TypeError")));
    }
    return (int)clay_to_int(v);
}

static char *vm_arg_strdup(ClayValue v, const char *what) {
    if (!CLAY_IS_TYPE(v, CLAY_TYPE_STRING)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "VM run: %s must be a String", what);
        clay_raise(CLAY_VAL(clay_error_new(msg, "TypeError")));
    }
    return strdup(clay_string_data(CLAY_OBJ(v)));
}

/* Bytes and line numbers from Arrays of integers (malloc'd) */
static uint8_t *vm_arg_code(ClayValue v, size_t *len) {
    ClayObject *a = vm_arg_array(v, "code");
    size_t n = clay_array_size(a);
    uint8_t *code = (uint8_t *)malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) code[i] = (uint8_t)vm_arg_int(clay_array_at(a, i), "a code byte");
    *len = n;
    return code;
}

static int *vm_arg_lines(ClayValue v, size_t len) {
    ClayObject *a = vm_arg_array(v, "lines");
    if (clay_array_size(a) != len)
        clay_raise(CLAY_VAL(clay_error_new("VM run: lines and code differ in length", "ArgumentError")));
    int *lines = (int *)malloc((len ? len : 1) * sizeof(int));
    for (size_t i = 0; i < len; i++) lines[i] = vm_arg_int(clay_array_at(a, i), "a line");
    return lines;
}

static void vm_check_base(int base, int have, const char *what) {
    if (base != have) {
        char msg[160];
        snprintf(msg, sizeof(msg), "VM run: the program was compiled for %d %s, the VM has %d",
                 base, what, have);
        clay_raise(CLAY_VAL(clay_error_new(msg, "RuntimeError")));
    }
}

/* [code, lines, paramNames, captureNames, captureSlots, localCount, hasBlockReturn] */
static BcChunk *vm_load_chunk(ClayValue v) {
    ClayObject *a = vm_arg_array(v, "a chunk");
    if (clay_array_size(a) < 7)
        clay_raise(CLAY_VAL(clay_error_new("VM run: a chunk has 7 fields", "ArgumentError")));
    BcChunk *chunk = (BcChunk *)calloc(1, sizeof(BcChunk));
    chunk->code = vm_arg_code(clay_array_at(a, 0), &chunk->len);
    chunk->line_map = vm_arg_lines(clay_array_at(a, 1), chunk->len);
    ClayObject *params = vm_arg_array(clay_array_at(a, 2), "paramNames");
    chunk->param_count = (int)clay_array_size(params);
    for (int i = 0; i < chunk->param_count && i < 16; i++)
        chunk->param_names[i] = vm_arg_strdup(clay_array_at(params, (size_t)i), "a parameter name");
    ClayObject *cnames = vm_arg_array(clay_array_at(a, 3), "captureNames");
    ClayObject *cslots = vm_arg_array(clay_array_at(a, 4), "captureSlots");
    chunk->capture_count = (int)clay_array_size(cnames);
    if (chunk->capture_count > BC_MAX_CAPTURES) chunk->capture_count = BC_MAX_CAPTURES;
    for (int i = 0; i < chunk->capture_count; i++) {
        chunk->capture_names[i] = vm_arg_strdup(clay_array_at(cnames, (size_t)i), "a capture name");
        chunk->capture_slots[i] = vm_arg_int(clay_array_at(cslots, (size_t)i), "a capture slot");
    }
    chunk->local_count = vm_arg_int(clay_array_at(a, 5), "localCount");
    chunk->has_block_return = clay_array_at(a, 6) == CLAY_TRUE;
    return chunk;
}

/* VM run(unit) / run(unit, args) — load a compiled program and run its
 * top-level code; returns the value of its last expression. args (an Array of
 * Strings) become the program's System args from then on. unit is
 *   [constBase, constants, chunkBase, chunks, methodBase, methods, code, lines, file]
 * where the bases are the VM's counts the program was compiled against and
 * constants/chunks/methods are the ones it adds. Errors propagate. */
static ClayValue vm_run(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    vm_host_check();
    if (argc < 1) clay_raise(CLAY_VAL(clay_error_new("VM run requires a compiled unit", "ArgumentError")));
    ClayObject *unit = vm_arg_array(argv[0], "the unit");
    if (clay_array_size(unit) < 9)
        clay_raise(CLAY_VAL(clay_error_new("VM run: a unit has 9 fields", "ArgumentError")));
    clayvm_vm_t *vm = g_host_vm;
    BcCompiler *bc = g_host_bc;

    /* Check everything before changing the VM */
    vm_check_base(vm_arg_int(clay_array_at(unit, 0), "constBase"), vm->constants.count, "constants");
    vm_check_base(vm_arg_int(clay_array_at(unit, 2), "chunkBase"), bc->chunk_count, "chunks");
    vm_check_base(vm_arg_int(clay_array_at(unit, 4), "methodBase"), g_method_slot_count, "methods");
    ClayObject *consts = vm_arg_array(clay_array_at(unit, 1), "constants");
    ClayObject *chunks = vm_arg_array(clay_array_at(unit, 3), "chunks");
    ClayObject *methods = vm_arg_array(clay_array_at(unit, 5), "methods");
    size_t code_len;
    uint8_t *code = vm_arg_code(clay_array_at(unit, 6), &code_len);
    if (code_len > BC_MAX_CODE) {
        free(code);
        clay_raise(CLAY_VAL(clay_error_new("VM run: top-level code too long", "ArgumentError")));
    }
    int *lines = vm_arg_lines(clay_array_at(unit, 7), code_len);
    ClayValue file = clay_array_at(unit, 8);
    if (argc > 1 && !CLAY_IS_NIL(argv[1])) {
        ClayObject *args = vm_arg_array(argv[1], "args");
        int n = (int)clay_array_size(args);
        char **v = (char **)malloc(sizeof(char *) * (size_t)(n + 1));   /* kept: System args */
        for (int i = 0; i < n; i++) v[i] = vm_arg_strdup(clay_array_at(args, (size_t)i), "an argument");
        v[n] = NULL;
        clay_set_args(n, v);
    }

    for (size_t i = 0; i < clay_array_size(consts); i++) {
        ClayValue c = clay_array_at(consts, i);
        clayvm_value_t v = clay_to_vm(c);
        if (v.type == CLAYVM_VAL_OBJECT) clay_retain((ClayObject *)v.as.obj);
        clayvm_vm_add_constant(vm, v);
    }
    for (size_t i = 0; i < clay_array_size(chunks); i++) {
        BcChunk *chunk = vm_load_chunk(clay_array_at(chunks, i));
        if (bc->chunk_count >= bc->chunk_capacity) {
            bc->chunk_capacity *= 2;
            bc->chunks = (BcChunk **)realloc(bc->chunks, (size_t)bc->chunk_capacity * sizeof(BcChunk *));
        }
        bc->chunks[bc->chunk_count++] = chunk;
    }
    for (size_t i = 0; i < clay_array_size(methods); i++) {
        ClayObject *m = vm_arg_array(clay_array_at(methods, i), "a method");
        char *name = vm_arg_strdup(clay_array_at(m, 2), "a method name");
        bc_method_slot_new(vm_arg_int(clay_array_at(m, 0), "a method chunk"),
                           vm_arg_int(clay_array_at(m, 1), "a method's parameter count"), name);
        free(name);
    }

    memcpy(bc->code, code, code_len);
    memcpy(bc->line_map, lines, code_len * sizeof(int));
    bc->len = code_len;
    free(code);
    free(lines);

    /* Run with the program's file name for error locations, and a call trace
     * of its own: stack traces show the program, not the host calling VM run */
    const char *saved_file = clay_source_file;
    int saved_line = clay_source_line;
    ClayCallTrace *saved_trace = (ClayCallTrace *)malloc(sizeof(ClayCallTrace));
    *saved_trace = clay_call_trace;
    clay_call_trace.depth = 0;
    clay_call_trace.overflow = 0;
    clay_source_line = 0;
    static char file_buf[1024];
    if (CLAY_IS_TYPE(file, CLAY_TYPE_STRING)) {
        snprintf(file_buf, sizeof(file_buf), "%s", clay_string_data(CLAY_OBJ(file)));
        clay_source_file = file_buf;
        bc->source_file = file_buf;
    }
    ClayTryFrame tf;
    if (clay_try_push(&tf) != 0) {
        ClayValue err = clay_error_stack.current_error;
        clay_source_file = saved_file;
        clay_source_line = saved_line;
        clay_call_trace = *saved_trace;
        free(saved_trace);
        clay_raise(err);
    }
    int rc = clayvm_vm_run(vm, bc->code, bc->len);
    clay_try_pop();
    clay_source_file = saved_file;
    clay_source_line = saved_line;
    clay_call_trace = *saved_trace;
    free(saved_trace);

    /* The value of the last expression (owned); release the rest */
    ClayValue result = CLAY_NIL;
    if (rc == 0 && vm->sp > 0) {
        clayvm_value_t top = vm->stack[vm->sp - 1];
        vm_retain_val(top);
        result = vm_to_clay(top);
    }
    for (int i = 0; i < vm->sp; i++) vm_release_val(vm->stack[i]);
    vm->sp = 0;
    if (rc != 0)
        clay_raise(CLAY_VAL(clay_error_new("VM run: the bytecode failed", "RuntimeError")));
    return result;
}

/* VM hasGlobal(name) — is name a global of the VM (a builtin, plugin
 * prototype or variable a program has set)? */
static ClayValue vm_hasGlobal(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    vm_host_check();
    if (argc < 1 || !CLAY_IS_TYPE(argv[0], CLAY_TYPE_STRING)) return CLAY_FALSE;
    HashTable *globals = (HashTable *)g_host_vm->globals;
    return CLAY_BOOL(ht_find(globals, (ht_key_t)clay_string_data(CLAY_OBJ(argv[0]))) != NULL);
}

/* VM counts — [constants, chunks, methods] the VM holds (the bases of the
 * next program it runs) */
static ClayValue vm_counts(ClayObject *self, int argc, ClayValue *argv) {
    (void)self; (void)argc; (void)argv;
    vm_host_check();
    ClayObject *a = clay_array_new();
    clay_array_append(a, clay_int(g_host_vm->constants.count));
    clay_array_append(a, clay_int(g_host_bc->chunk_count));
    clay_array_append(a, clay_int(g_method_slot_count));
    return CLAY_VAL(a);
}

void clay_vm_init_methods(ClayObject *proto) {
    clay_set_method(proto, "run", vm_run);
    clay_set_method(proto, "hasGlobal", vm_hasGlobal);
    clay_set_method(proto, "counts", vm_counts);
}
