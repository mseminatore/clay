/*
 * bc_compiler.c - Bytecode compiler and native bridge for Clay VM REPL
 *
 * Compiles Clay AST to bytecode for the vmgen-generated VM.
 * Clay runtime objects are wrapped in CLAYVM_VAL_OBJECT values.
 * All Clay-specific operations (send, clone, etc.) go through native functions.
 */
#include "bc_compiler.h"
#include "clay.h"
#include "range.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

/* --- Platform fiber support --- */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "fiber_ctx.h"
#endif

#define FIBER_STACK_SIZE (256 * 1024)

/* --- Global state for trampoline access --- */

static BcCompiler *g_bc = NULL;  /* current compiler (for trampoline) */
static clayvm_vm_t *g_vm = NULL; /* current VM (for trampoline) */
static ClayObject *g_super_proto = NULL; /* defining prototype for super dispatch */

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

    /* If invoked as a slot-method, bind the receiver as 'self' in VM globals */
    ClayObject *method_self = clay_slot_method_self;
    ClayValue saved_self = CLAY_NIL;
    int self_existed = 0;
    if (method_self) {
        clay_slot_method_self = NULL;
        HashTable *globals = (HashTable *)g_vm->globals;
        if (globals) {
            ht_value_t old_self = ht_find(globals, (ht_key_t)"self");
            if (old_self) {
                clayvm_value_t *existing = (clayvm_value_t *)old_self;
                saved_self = vm_to_clay(*existing);
                self_existed = 1;
                *existing = CLAYVM_OBJ(method_self);
            } else {
                clayvm_value_t *self_stored = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
                *self_stored = CLAYVM_OBJ(method_self);
                ht_add(globals, (ht_key_t)strdup("self"), (ht_value_t)self_stored);
            }
        }
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

    /* Push params as stack locals (slots 0, 1, ...) */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        ClayValue val = (i < argc) ? argv[i] : CLAY_NIL;
        clayvm_vm_push(g_vm, clay_to_vm(val));
    }

    /* Push captured values as additional locals (slots param_count, param_count+1, ...) */
    int capture_count = chunk->capture_count;
    for (int i = 0; i < capture_count; i++) {
        ClayValue cv = (1 + i < self->as.block.env_size) ?
                        self->as.block.env[1 + i] : CLAY_NIL;
        clayvm_vm_push(g_vm, clay_to_vm(cv));
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
        if (slot < g_vm->sp && (1 + i) < self->as.block.env_size) {
            self->as.block.env[1 + i] = vm_to_clay(g_vm->stack[slot]);
        }
    }

    /* Get return value (last value on stack) */
    ClayValue retval = CLAY_NIL;
    if (!had_exception && g_vm->sp > base) {
        retval = vm_to_clay(g_vm->stack[g_vm->sp - 1]);
    }

    /* Restore full VM state */
    g_vm->code = saved_code;
    g_vm->code_len = saved_code_len;
    g_vm->ip = saved_ip;
    g_vm->sp = saved_sp;
    g_vm->running = saved_running;
    g_vm->frame_count = saved_frame_count;

    /* Restore 'self' if we bound a slot-method receiver */
    if (method_self) {
        HashTable *globals = (HashTable *)g_vm->globals;
        if (globals && self_existed) {
            ht_value_t cur = ht_find(globals, (ht_key_t)"self");
            if (cur) {
                clayvm_value_t *existing = (clayvm_value_t *)cur;
                *existing = clay_to_vm(saved_self);
            }
        }
    }

    /* Re-raise after VM state is safely restored */
    if (had_exception) {
        clay_raise(raised_err);
        return CLAY_NIL; /* unreachable */
    }

    return retval;
}

/* --- Fiber (coroutine) support ---
 * Uses platform fibers (Windows Fibers / POSIX ucontext) to give each
 * Clay fiber its own C stack.  This allows yield() from ANY depth —
 * including inside C-level loops like whileTrue, each, map, etc.
 * Each fiber also gets its own VM stack and frame array so there is
 * no interference with the caller's VM data.
 */

typedef struct {
    ClayObject *block;          /* retained block object */
    ClayValue yield_value;      /* last yielded value */
    ClayValue result;           /* final return value (when done) */
    int status;                 /* 0=created, 1=suspended, 2=done */

    /* Own VM stack and frames (swapped into g_vm during execution) */
    clayvm_value_t *vm_stack;
    int vm_stack_capacity;
    clayvm_frame_t *vm_frames;
    int vm_frame_capacity;

    /* Saved VM execution state (for resume after yield) */
    int vm_sp;
    int vm_frame_count;
    const uint8_t *vm_code;
    size_t vm_code_len;
    size_t vm_ip;
    int vm_exception_count;

    /* Saved error stack head — isolate try frames per fiber */
    ClayTryFrame *saved_error_head;

#ifdef _WIN32
    LPVOID os_fiber;            /* Windows fiber handle */
    LPVOID caller_fiber;        /* caller's fiber handle (set by resume) */
#else
    clay_fiber_regs_t fiber_regs;
    clay_fiber_regs_t caller_regs;
    char *c_stack;              /* allocated C stack */
#endif
} FiberContext;

static FiberContext *g_current_fiber = NULL;
#ifdef _WIN32
static int g_main_thread_is_fiber = 0;
#else
/* No g_fiber_starting needed — entry function receives ctx as arg */
#endif

/* Helper: get FiberContext from a fiber object */
static FiberContext *get_fiber_ctx(ClayObject *self) {
    ClayValue v = clay_get_slot(self, "_ctx");
    if (CLAY_IS_NIL(v)) return NULL;
    return (FiberContext *)(intptr_t)clay_to_int(v);
}

/* --- Fiber entry functions (run on fiber's own C stack) --- */
#ifdef _WIN32
static VOID CALLBACK fiber_entry_win(LPVOID param) {
    FiberContext *ctx = (FiberContext *)param;
    clay_error_stack.head = NULL;  /* fiber starts with clean error stack */
    ctx->result = ctx->block->as.block.func(ctx->block, 0, NULL);
    ctx->status = 2;
    ctx->saved_error_head = clay_error_stack.head;
    SwitchToFiber(ctx->caller_fiber);
}
#else
static void fiber_entry_unix(void *param) {
    FiberContext *ctx = (FiberContext *)param;
    clay_error_stack.head = NULL;  /* fiber starts with clean error stack */
    ctx->result = ctx->block->as.block.func(ctx->block, 0, NULL);
    ctx->status = 2;
    ctx->saved_error_head = clay_error_stack.head;
    clay_fiber_swap(&ctx->fiber_regs, &ctx->caller_regs);
}
#endif

/* Fiber yield(value) — switch back to caller's C stack */
static ClayValue fiber_yield(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_current_fiber) return CLAY_NIL;
    FiberContext *ctx = g_current_fiber;
    ctx->yield_value = (argc > 0) ? argv[0] : CLAY_NIL;
    ctx->status = 1;
    ctx->saved_error_head = clay_error_stack.head;
#ifdef _WIN32
    SwitchToFiber(ctx->caller_fiber);
#else
    clay_fiber_swap(&ctx->fiber_regs, &ctx->caller_regs);
#endif
    return CLAY_NIL;
}

/* Fiber new(block) — create a fiber with its own C stack and VM resources */
static ClayValue fiber_new(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_PTR(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    ClayObject *block = CLAY_OBJ(argv[0]);

    extern ClayObject *clay_Fiber;
    ClayObject *fiber = clay_clone(clay_Fiber);
    clay_set_slot(fiber, "_name", CLAY_VAL(clay_string_new("Fiber")));

    FiberContext *ctx = (FiberContext *)calloc(1, sizeof(FiberContext));
    ctx->block = block;
    clay_retain(block);
    ctx->status = 0;
    ctx->yield_value = CLAY_NIL;
    ctx->result = CLAY_NIL;

    /* Allocate fiber's own VM stack and frames */
    ctx->vm_stack_capacity = 4096;
    ctx->vm_stack = (clayvm_value_t *)calloc((size_t)ctx->vm_stack_capacity, sizeof(clayvm_value_t));
    ctx->vm_frame_capacity = 64;
    ctx->vm_frames = (clayvm_frame_t *)calloc((size_t)ctx->vm_frame_capacity, sizeof(clayvm_frame_t));

    /* Create platform fiber with its own C stack */
#ifdef _WIN32
    if (!g_main_thread_is_fiber) {
        ConvertThreadToFiber(NULL);
        g_main_thread_is_fiber = 1;
    }
    ctx->os_fiber = CreateFiber(FIBER_STACK_SIZE, fiber_entry_win, ctx);
#else
    ctx->c_stack = (char *)malloc(FIBER_STACK_SIZE);
    clay_fiber_init_stack(&ctx->fiber_regs, ctx->c_stack, FIBER_STACK_SIZE,
                          fiber_entry_unix, ctx);
#endif

    clay_set_slot(fiber, "_ctx", clay_int((intptr_t)ctx));
    return CLAY_VAL(fiber);
}

/* Fiber resume() — switch to fiber's C stack, run until yield or completion */
static ClayValue fiber_resume(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    if (!g_vm) return CLAY_NIL;

    FiberContext *ctx = get_fiber_ctx(self);
    if (!ctx || ctx->status == 2) return CLAY_NIL;

    /* Save caller's full VM state */
    clayvm_value_t *caller_stack     = g_vm->stack;
    int             caller_stack_cap = g_vm->stack_capacity;
    int             caller_sp        = g_vm->sp;
    clayvm_frame_t *caller_frames    = g_vm->frames;
    int             caller_frame_cap = g_vm->frame_capacity;
    int             caller_fc        = g_vm->frame_count;
    const uint8_t  *caller_code      = g_vm->code;
    size_t          caller_code_len  = g_vm->code_len;
    size_t          caller_ip        = g_vm->ip;
    int             caller_running   = g_vm->running;
    int             caller_exc       = g_vm->exception_count;

    /* Swap in fiber's VM stack and frames */
    g_vm->stack          = ctx->vm_stack;
    g_vm->stack_capacity = ctx->vm_stack_capacity;
    g_vm->frames         = ctx->vm_frames;
    g_vm->frame_capacity = ctx->vm_frame_capacity;

    if (ctx->status == 0) {
        /* First resume: fresh state — block trampoline will set up code/ip */
        g_vm->sp              = 0;
        g_vm->frame_count     = 0;
        g_vm->running         = 1;
        g_vm->exception_count = 0;
    } else {
        /* Resume suspended: restore execution state */
        g_vm->sp              = ctx->vm_sp;
        g_vm->frame_count     = ctx->vm_frame_count;
        g_vm->code            = ctx->vm_code;
        g_vm->code_len        = ctx->vm_code_len;
        g_vm->ip              = ctx->vm_ip;
        g_vm->running         = 1;
        g_vm->exception_count = ctx->vm_exception_count;
    }

    FiberContext *prev = g_current_fiber;
    g_current_fiber = ctx;

    /* Save caller's error stack head and swap in fiber's */
    ClayTryFrame *caller_error_head = clay_error_stack.head;
    clay_error_stack.head = ctx->saved_error_head;

    /* Switch to fiber's C stack */
#ifdef _WIN32
    ctx->caller_fiber = GetCurrentFiber();
    SwitchToFiber(ctx->os_fiber);
#else
    clay_fiber_swap(&ctx->caller_regs, &ctx->fiber_regs);
#endif

    /* Back here after yield() or completion */
    g_current_fiber = prev;
    clay_error_stack.head = caller_error_head;

    /* Save fiber's VM state (stack/frames may have been reallocated) */
    ctx->vm_stack          = g_vm->stack;
    ctx->vm_stack_capacity = g_vm->stack_capacity;
    ctx->vm_sp             = g_vm->sp;
    ctx->vm_frames         = g_vm->frames;
    ctx->vm_frame_capacity = g_vm->frame_capacity;
    ctx->vm_frame_count    = g_vm->frame_count;
    ctx->vm_code           = g_vm->code;
    ctx->vm_code_len       = g_vm->code_len;
    ctx->vm_ip             = g_vm->ip;
    ctx->vm_exception_count = g_vm->exception_count;

    ClayValue retval = (ctx->status == 2) ? ctx->result : ctx->yield_value;

    /* Free platform resources when fiber is done */
    if (ctx->status == 2) {
#ifdef _WIN32
        if (ctx->os_fiber) { DeleteFiber(ctx->os_fiber); ctx->os_fiber = NULL; }
#else
        free(ctx->c_stack); ctx->c_stack = NULL;
#endif
        free(ctx->vm_stack);  ctx->vm_stack = NULL;
        free(ctx->vm_frames); ctx->vm_frames = NULL;
    }

    /* Restore caller's VM state */
    g_vm->stack          = caller_stack;
    g_vm->stack_capacity = caller_stack_cap;
    g_vm->sp             = caller_sp;
    g_vm->frames         = caller_frames;
    g_vm->frame_capacity = caller_frame_cap;
    g_vm->frame_count    = caller_fc;
    g_vm->code           = caller_code;
    g_vm->code_len       = caller_code_len;
    g_vm->ip             = caller_ip;
    g_vm->running        = caller_running;
    g_vm->exception_count = caller_exc;

    return retval;
}

/* Fiber isDone() — check if fiber has completed */
static ClayValue fiber_isDone(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    FiberContext *ctx = get_fiber_ctx(self);
    if (!ctx) return CLAY_TRUE;
    return CLAY_BOOL(ctx->status == 2);
}

/* Register Fiber methods on prototype */
void clay_fiber_init_methods(ClayObject *proto) {
    clay_set_method(proto, "new", fiber_new);
    clay_set_method(proto, "resume", fiber_resume);
    clay_set_method(proto, "isDone", fiber_isDone);
    clay_set_method(proto, "yield", fiber_yield);
}

/* Free a FiberContext and all its resources (called from memory.c on dealloc) */
void clay_fiber_context_free(intptr_t ctx_ptr) {
    if (!ctx_ptr) return;
    FiberContext *ctx = (FiberContext *)ctx_ptr;
    if (ctx->block) {
        clay_release(ctx->block);
        ctx->block = NULL;
    }
#ifdef _WIN32
    if (ctx->os_fiber) { DeleteFiber(ctx->os_fiber); ctx->os_fiber = NULL; }
#else
    free(ctx->c_stack); ctx->c_stack = NULL;
#endif
    free(ctx->vm_stack);  ctx->vm_stack = NULL;
    free(ctx->vm_frames); ctx->vm_frames = NULL;
    free(ctx);
}

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
    g_method_slots[g_method_slot_count] = ms;
    return g_method_slot_count++;
}

/* Single dispatch function for all BC-compiled methods.
 * Reads the MethodSlot from clay_method_context (set by dispatch.c). */
static ClayValue bc_method_dispatch(ClayObject *self, int argc, ClayValue *argv) {
    MethodSlot *ms = (MethodSlot *)clay_method_context;
    if (!g_bc || !g_vm || !ms) return CLAY_NIL;
    if (ms->chunk_id < 0 || ms->chunk_id >= g_bc->chunk_count) return CLAY_NIL;

    BcChunk *chunk = g_bc->chunks[ms->chunk_id];

    /* Save and set super prototype context */
    ClayObject *saved_super_proto = g_super_proto;
    g_super_proto = ms->defining_obj;

    /* Save and set self as global (methods access self via global).
     * Reuse existing hash entry when possible to avoid heap churn. */
    HashTable *globals = (HashTable *)g_vm->globals;
    ClayValue saved_self = CLAY_NIL;
    int self_existed = 0;
    if (globals) {
        ht_value_t old_self = ht_find(globals, (ht_key_t)"self");
        if (old_self) {
            clayvm_value_t *existing = (clayvm_value_t *)old_self;
            saved_self = vm_to_clay(*existing);
            self_existed = 1;
            *existing = CLAYVM_OBJ(self);  /* update in place */
        } else {
            clayvm_value_t *self_stored = (clayvm_value_t *)malloc(sizeof(clayvm_value_t));
            *self_stored = CLAYVM_OBJ(self);
            ht_add(globals, (ht_key_t)strdup("self"), (ht_value_t)self_stored);
        }
    }

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

    /* Push params as stack locals (slots 0, 1, ...) */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        ClayValue val = (i < argc) ? argv[i] : CLAY_NIL;
        clayvm_vm_push(g_vm, clay_to_vm(val));
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

    ClayValue retval = CLAY_NIL;
    if (g_vm->sp > base)
        retval = vm_to_clay(g_vm->stack[g_vm->sp - 1]);

    /* Restore full VM state */
    g_vm->code = saved_code;
    g_vm->code_len = saved_code_len;
    g_vm->ip = saved_ip;
    g_vm->sp = saved_sp;
    g_vm->running = saved_running;
    g_vm->frame_count = saved_frame_count;

    /* Restore self — update in place when possible */
    if (self_existed) {
        ht_value_t cur_self = ht_find(globals, (ht_key_t)"self");
        if (cur_self) {
            *(clayvm_value_t *)cur_self = clay_to_vm(saved_self);
        }
    } else {
        ht_value_t cur_self = ht_find(globals, (ht_key_t)"self");
        if (cur_self) { free((void *)cur_self); ht_remove(globals, (ht_key_t)"self"); }
    }

    /* Restore super prototype context */
    g_super_proto = saved_super_proto;

    return retval;
}

/* --- Bytecode emission helpers --- */

static void emit_byte(BcCompiler *bc, uint8_t b) {
    if (bc->len >= BC_MAX_CODE) { bc->had_error = 1; return; }
    bc->line_map[bc->len] = bc->current_line;
    bc->code[bc->len++] = b;
}

static void emit_u16(BcCompiler *bc, uint16_t v) {
    emit_byte(bc, (uint8_t)(v & 0xFF));
    emit_byte(bc, (uint8_t)(v >> 8));
}

static void emit_op(BcCompiler *bc, uint8_t op) {
    emit_byte(bc, op);
}

static void emit_op_u16(BcCompiler *bc, uint8_t op, uint16_t arg) {
    emit_op(bc, op);
    emit_u16(bc, arg);
}

/* Emit a jump instruction with a placeholder offset, return patch position */
static int emit_jump(BcCompiler *bc, uint8_t jump_op) {
    emit_op(bc, jump_op);
    int patch_pos = bc->len;
    emit_u16(bc, 0); /* placeholder */
    return patch_pos;
}

/* Patch a previously emitted jump to target the current position */
static void patch_jump(BcCompiler *bc, int patch_pos) {
    int offset = bc->len - (patch_pos + 2);
    bc->code[patch_pos]     = (uint8_t)(offset & 0xFF);
    bc->code[patch_pos + 1] = (uint8_t)((offset >> 8) & 0xFF);
}

static int add_constant_int(BcCompiler *bc, int64_t val) {
    return clayvm_vm_add_constant(bc->vm, CLAYVM_INT64(val));
}

static int add_constant_obj(BcCompiler *bc, void *obj) {
    return clayvm_vm_add_constant(bc->vm, CLAYVM_OBJ(obj));
}

static int add_constant_string(BcCompiler *bc, const char *s) {
    /* Deduplicate: reuse existing constant with the same string value */
    for (int i = 0; i < bc->vm->constants.count; i++) {
        clayvm_value_t v = bc->vm->constants.values[i];
        if (v.type == CLAYVM_VAL_OBJECT && v.as.obj) {
            ClayObject *obj = (ClayObject *)v.as.obj;
            if (obj->type == CLAY_TYPE_STRING &&
                strcmp(clay_string_data(obj), s) == 0) {
                return i;
            }
        }
    }
    ClayObject *str = clay_string_new(s);
    clay_retain(str);
    return clayvm_vm_add_constant(bc->vm, CLAYVM_OBJ(str));
}

/* --- Conversion helpers --- */

ClayValue vm_to_clay(clayvm_value_t v) {
    switch (v.type) {
        case CLAYVM_VAL_INT64:  return clay_int((intptr_t)v.as.i64);
        case CLAYVM_VAL_BOOL:   return v.as.boolean ? CLAY_TRUE : CLAY_FALSE;
        case CLAYVM_VAL_NIL:    return CLAY_NIL;
        case CLAYVM_VAL_OBJECT: return CLAY_VAL(v.as.obj);
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
    clayvm_vm_push(vm, clay_to_vm(result));
}

/* --- Per-callsite inline cache --- */
#define IC_TABLE_SIZE 512
#define IC_TABLE_MASK (IC_TABLE_SIZE - 1)

typedef struct {
    size_t      callsite_ip;
    HashTable  *methods_key;
    const char *selector;
    ClayMethod *method;
} InlineCacheEntry;

static InlineCacheEntry g_ic_table[IC_TABLE_SIZE];

/* native_send(receiver, selector_str, arg1, arg2, ...) */
static int native_send_fn(clayvm_vm_t *vm, int argc) {
    if (argc < 2) { clayvm_vm_push(vm, CLAYVM_NIL_VAL); return 0; }

    /* Update source line for error messages (only for main code, not chunks) */
    if (g_bc && vm->code == g_bc->code && vm->ip < (size_t)g_bc->len) {
        clay_source_line = g_bc->line_map[vm->ip];
    }

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

    /* Determine receiver object for IC keying */
    ClayObject *target_obj = NULL;
    HashTable *target_methods = NULL;

    if (CLAY_IS_INT(receiver) || CLAY_IS_TAGGED_FLOAT(receiver)) {
        target_obj = clay_Number;
        target_methods = clay_Number->methods;
    } else if (!CLAY_IS_NIL(receiver)) {
        target_obj = CLAY_OBJ(receiver);
        target_methods = target_obj->methods;
    }

    /* Per-callsite inline cache check */
    size_t site_ip = vm->ip;
    unsigned int ic_idx = (unsigned int)(site_ip >> 2) & IC_TABLE_MASK;
    InlineCacheEntry *ic = &g_ic_table[ic_idx];
    ClayValue result;

    if (target_obj &&
        ic->callsite_ip == site_ip &&
        ic->methods_key == target_methods &&
        ic->selector == selector) {
        /* IC hit — invoke directly, skip dispatch */
        if (CLAY_IS_INT(receiver) || CLAY_IS_TAGGED_FLOAT(receiver)) {
            ClayObject *wrapper = clay_clone(clay_Number);
            clay_set_slot(wrapper, "_value", receiver);
            result = clay_invoke(ic->method, wrapper, msg_argc, args);
            if (result == CLAY_VAL(wrapper)) {
                clay_release(wrapper);
                result = receiver;
            } else {
                clay_release(wrapper);
            }
        } else {
            result = clay_invoke(ic->method, target_obj, msg_argc, args);
        }
    } else {
        /* IC miss — full dispatch */
        result = clay_send_val(receiver, selector, msg_argc, args);
        /* Update IC if we found a method */
        if (target_obj) {
            ClayMethod *m = clay_cached_lookup(target_obj, selector);
            if (m) {
                ic->callsite_ip = site_ip;
                ic->methods_key = target_methods;
                ic->selector = selector;
                ic->method = m;
            }
        }
    }

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
    if (args && args != stack_args) free(args);

    clayvm_vm_push(vm, clay_to_vm(result));
    return 0;
}

/* native_clone(proto) -> cloned object */
static int native_clone_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue proto = vm_to_clay(clayvm_vm_pop(vm));
    ClayObject *obj = clay_clone(CLAY_OBJ(proto));
    clayvm_vm_push(vm, CLAYVM_OBJ(obj));
    return 0;
}

/* native_set_slot(obj, name_str, value) */
static int native_set_slot_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue val = vm_to_clay(clayvm_vm_pop(vm));
    clayvm_value_t name_val = clayvm_vm_pop(vm);
    ClayValue obj = vm_to_clay(clayvm_vm_pop(vm));
    const char *name = clay_string_data((ClayObject *)name_val.as.obj);
    clay_set_slot(CLAY_OBJ(obj), name, val);
    clayvm_vm_push(vm, clay_to_vm(val)); /* return assigned value */
    return 0;
}

/* native_println(value) */
static int native_println_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val = clayvm_vm_pop(vm);
    ClayValue cv = vm_to_clay(val);
    char buf[256];
    if (CLAY_IS_INT(cv)) {
        snprintf(buf, sizeof(buf), "%ld\n", (long)clay_to_int(cv));
    } else if (CLAY_IS_TAGGED_FLOAT(cv)) {
        snprintf(buf, sizeof(buf), "%g\n", clay_float_untag(cv));
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
    clayvm_value_t captured[16];
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
    ClayValue val = vm_to_clay(clayvm_vm_pop(vm));
    ClayCell *cell = clay_cell_new(val);
    clayvm_vm_push(vm, CLAYVM_OBJ(cell));
    return 0;
}

/* native_cell_get(cell) — read value from cell */
static int native_cell_get_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t cv = clayvm_vm_pop(vm);
    if (cv.type != CLAYVM_VAL_OBJECT || cv.as.obj == NULL) {
        fprintf(stderr, "cell_get: expected cell object, got type %d\n", cv.type);
        clayvm_vm_push(vm, CLAYVM_NIL_VAL);
        return 0;
    }
    ClayCell *cell = (ClayCell *)cv.as.obj;
    clayvm_vm_push(vm, clay_to_vm(cell->value));
    return 0;
}

/* native_cell_set(cell, value) — write value to cell, return value */
static int native_cell_set_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue new_val = vm_to_clay(clayvm_vm_pop(vm));
    clayvm_value_t cv = clayvm_vm_pop(vm);
    ClayCell *cell = (ClayCell *)cv.as.obj;
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
    clayvm_vm_push(vm, clay_to_vm(new_val));
    return 0;
}

/* native_is_truthy(val) — push 1 or 0 (for JUMP_IF_FALSE) */
static int native_is_truthy_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue v = vm_to_clay(clayvm_vm_pop(vm));
    clayvm_vm_push(vm, CLAYVM_INT64(CLAY_IS_TRUTHY(v) ? 1 : 0));
    return 0;
}

/* native_to_bool(val) — push CLAY_TRUE or CLAY_FALSE */
static int native_to_bool_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue v = vm_to_clay(clayvm_vm_pop(vm));
    clayvm_vm_push(vm, clay_to_vm(CLAY_IS_TRUTHY(v) ? CLAY_TRUE : CLAY_FALSE));
    return 0;
}

/* native_is_nil(val) — push 1 if nil, 0 otherwise (for JUMP_IF_FALSE) */
static int native_is_nil_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    ClayValue v = vm_to_clay(clayvm_vm_pop(vm));
    clayvm_vm_push(vm, CLAYVM_INT64(v == CLAY_NIL ? 1 : 0));
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
    clayvm_value_t finally_val = clayvm_vm_pop(vm);
    clayvm_value_t catch_val = clayvm_vm_pop(vm);
    clayvm_value_t try_val = clayvm_vm_pop(vm);
    ClayValue try_block = vm_to_clay(try_val);
    ClayValue catch_block = vm_to_clay(catch_val);
    ClayValue finally_block = vm_to_clay(finally_val);

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
            clay_send_val(finally_block, "value", 0, NULL);
        }
        clayvm_vm_push(vm, clay_to_vm(result));
    } else {
        /* Error path — trampoline's restore was skipped by longjmp */
        vm->code = saved_code;
        vm->code_len = saved_code_len;
        vm->ip = saved_ip;
        vm->sp = saved_sp;
        vm->running = saved_running;
        vm->frame_count = saved_frame_count;

        ClayValue error = clay_error_stack.current_error;
        ClayValue result;
        if (!CLAY_IS_NIL(catch_block)) {
            result = clay_send_val(catch_block, "value", 1, &error);
        } else {
            result = CLAY_NIL;
        }
        if (!CLAY_IS_NIL(finally_block)) {
            clay_send_val(finally_block, "value", 0, NULL);
        }
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
    ClayValue val = vm_to_clay(clayvm_vm_pop(vm));
    if (CLAY_IS_PTR(val) && !CLAY_IS_NIL(val) &&
        CLAY_OBJ(val)->type == CLAY_TYPE_STRING) {
        val = CLAY_VAL(clay_error_new(clay_string_data(CLAY_OBJ(val)), "RuntimeError"));
    }
    clay_raise(val);
    /* clay_raise never returns (longjmp) */
    return 0;
}

/* native_make_range(start, end) — create a Range object */
static int native_make_range_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t end_v = clayvm_vm_pop(vm);
    clayvm_value_t start_v = clayvm_vm_pop(vm);
    ClayValue start = vm_to_clay(start_v);
    ClayValue end = vm_to_clay(end_v);
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

/* native_array_append(arr, val) — append to array, return array */
static int native_array_append_fn(clayvm_vm_t *vm, int argc) {
    (void)argc;
    clayvm_value_t val = clayvm_vm_pop(vm);
    clayvm_value_t arr_val = clayvm_vm_pop(vm);
    ClayObject *arr = (ClayObject *)arr_val.as.obj;
    clay_array_append(arr, vm_to_clay(val));
    clayvm_vm_push(vm, arr_val); /* return array for chaining */
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

    clayvm_vm_push(vm, obj_val);
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
            clayvm_vm_push(vm, *stored);
            return 0;
        }
    }
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
    if (!globals) return 0;

    /* Retain new object value */
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

    clayvm_vm_push(vm, val);
    return 0;
}

/* --- Compiler init --- */

static int globals_str_compare(ht_key_t a, ht_key_t b) {
    return strcmp((const char *)a, (const char *)b) == 0;
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
    bc->native_store_global = clayvm_native_register(&vm->natives, "store_global", native_store_global_fn, 2);
    bc->native_cell_new = clayvm_native_register(&vm->natives, "cell_new", native_cell_new_fn, 1);
    bc->native_cell_get = clayvm_native_register(&vm->natives, "cell_get", native_cell_get_fn, 1);
    bc->native_cell_set = clayvm_native_register(&vm->natives, "cell_set", native_cell_set_fn, 2);
    bc->native_is_truthy = clayvm_native_register(&vm->natives, "is_truthy", native_is_truthy_fn, 1);
    bc->native_to_bool = clayvm_native_register(&vm->natives, "to_bool", native_to_bool_fn, 1);
    bc->native_is_nil = clayvm_native_register(&vm->natives, "is_nil", native_is_nil_fn, 1);
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
        {"Range", clay_Range},
        {"Fiber", clay_Fiber},
        {"Db", clay_Db},
        {"BreakSignal", clay_BreakSignal},
        {"ContinueSignal", clay_ContinueSignal},
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
}

/* --- AST → Bytecode compilation --- */

static void bc_emit_expr(BcCompiler *bc, AstNode *node);
static void bc_emit_stmt(BcCompiler *bc, AstNode *node);

/* --- Local variable helpers --- */

static int is_builtin_global(const char *name) {
    return strcmp(name, "Object") == 0 ||
           strcmp(name, "Number") == 0 ||
           strcmp(name, "String") == 0 ||
           strcmp(name, "Array") == 0 ||
           strcmp(name, "Block") == 0 ||
           strcmp(name, "Nil") == 0 ||
           strcmp(name, "Boolean") == 0 ||
           strcmp(name, "Error") == 0 ||
           strcmp(name, "Dict") == 0 ||
           strcmp(name, "Math") == 0 ||
           strcmp(name, "Console") == 0 ||
           strcmp(name, "File") == 0 ||
           strcmp(name, "System") == 0 ||
           strcmp(name, "FFI") == 0 ||
           strcmp(name, "Date") == 0 ||
           strcmp(name, "Http") == 0 ||
           strcmp(name, "Regex") == 0 ||
           strcmp(name, "Fiber") == 0 ||
           strcmp(name, "Db") == 0 ||
           strcmp(name, "BreakSignal") == 0 ||
           strcmp(name, "ContinueSignal") == 0 ||
           strcmp(name, "true") == 0 ||
           strcmp(name, "false") == 0 ||
           strcmp(name, "nil") == 0;
}

/* Check if a name exists as a user-defined global (compile-time tracking) */
static int bc_is_user_global(const char *name) {
    if (!g_bc) return 0;
    for (int i = 0; i < g_bc->known_global_count; i++) {
        if (strcmp(g_bc->known_globals[i], name) == 0) return 1;
    }
    /* Also check the VM's runtime globals table */
    if (g_vm) {
        HashTable *globals = (HashTable *)g_vm->globals;
        if (globals && ht_find(globals, (ht_key_t)name)) return 1;
    }
    return 0;
}

/* Register a name as a known global at compile time */
static void bc_register_global(BcCompiler *bc, const char *name) {
    /* Skip if already known */
    for (int i = 0; i < bc->known_global_count; i++) {
        if (strcmp(bc->known_globals[i], name) == 0) return;
    }
    if (bc->known_global_count >= bc->known_global_capacity) {
        bc->known_global_capacity = bc->known_global_capacity ? bc->known_global_capacity * 2 : 32;
        bc->known_globals = (char **)realloc(bc->known_globals,
            (size_t)bc->known_global_capacity * sizeof(char *));
    }
    bc->known_globals[bc->known_global_count++] = strdup(name);
}

static int bc_resolve_local(BcCompiler *bc, const char *name) {
    for (int i = bc->local_count - 1; i >= 0; i--) {
        if (strcmp(bc->locals[i].name, name) == 0)
            return bc->locals[i].slot;
    }
    return -1; /* not a local */
}

/* Check if a resolved local slot is boxed in a cell (captured by inner block) */
static int bc_local_is_captured(BcCompiler *bc, int slot) {
    for (int i = 0; i < bc->local_count; i++) {
        if (bc->locals[i].slot == slot)
            return bc->locals[i].is_captured;
    }
    return 0;
}

static int bc_add_local(BcCompiler *bc, const char *name) {
    if (bc->local_count >= BC_MAX_LOCALS) return -1;
    int slot = bc->local_count;
    bc->locals[slot].name = strdup(name);
    bc->locals[slot].slot = slot;
    bc->locals[slot].depth = bc->scope_depth;
    bc->locals[slot].is_captured = 0;
    bc->local_count++;
    return slot;
}

static void bc_begin_scope(BcCompiler *bc) {
    bc->scope_depth++;
}

static void bc_end_scope(BcCompiler *bc) {
    while (bc->local_count > 0 &&
           bc->locals[bc->local_count - 1].depth >= bc->scope_depth) {
        bc->local_count--;
        free(bc->locals[bc->local_count].name);
        bc->locals[bc->local_count].name = NULL;
    }
    bc->scope_depth--;
}

/* --- Capture scanning --- */

typedef struct {
    char **names;
    int   *slots;  /* enclosing scope slot indices */
    int    count;
    int    capacity;
} BcCaptureList;

static void bc_capture_init(BcCaptureList *cl) {
    cl->names = NULL;
    cl->slots = NULL;
    cl->count = 0;
    cl->capacity = 0;
}

static int bc_capture_has(BcCaptureList *cl, const char *name) {
    for (int i = 0; i < cl->count; i++)
        if (strcmp(cl->names[i], name) == 0) return 1;
    return 0;
}

static void bc_capture_add(BcCaptureList *cl, const char *name, int slot) {
    if (bc_capture_has(cl, name)) return;
    if (cl->count >= cl->capacity) {
        cl->capacity = cl->capacity == 0 ? 4 : cl->capacity * 2;
        cl->names = (char **)realloc(cl->names, sizeof(char *) * (size_t)cl->capacity);
        cl->slots = (int *)realloc(cl->slots, sizeof(int) * (size_t)cl->capacity);
    }
    cl->names[cl->count] = strdup(name);
    cl->slots[cl->count] = slot;
    cl->count++;
}

static void bc_capture_free(BcCaptureList *cl) {
    for (int i = 0; i < cl->count; i++) free(cl->names[i]);
    free(cl->names);
    free(cl->slots);
    cl->names = NULL;
    cl->slots = NULL;
    cl->count = 0;
    cl->capacity = 0;
}

static int bc_is_builtin_global(const char *name) {
    return strcmp(name, "Object") == 0 || strcmp(name, "Number") == 0 ||
           strcmp(name, "String") == 0 || strcmp(name, "Array") == 0 ||
           strcmp(name, "Block") == 0 || strcmp(name, "Nil") == 0 ||
           strcmp(name, "Boolean") == 0 || strcmp(name, "Error") == 0 ||
           strcmp(name, "Dict") == 0 || strcmp(name, "Math") == 0 ||
           strcmp(name, "Console") == 0 || strcmp(name, "File") == 0 ||
           strcmp(name, "System") == 0 || strcmp(name, "FFI") == 0 ||
           strcmp(name, "Date") == 0 ||
           strcmp(name, "Http") == 0 ||
           strcmp(name, "Regex") == 0 ||
           strcmp(name, "Fiber") == 0 ||
           strcmp(name, "Db") == 0 ||
           strcmp(name, "BreakSignal") == 0 ||
           strcmp(name, "ContinueSignal") == 0 ||
           strcmp(name, "true") == 0 || strcmp(name, "false") == 0 ||
           strcmp(name, "nil") == 0;
}

static int bc_name_in_list(const char *name, char **list, int count) {
    for (int i = 0; i < count; i++)
        if (strcmp(list[i], name) == 0) return 1;
    return 0;
}

/* Resolve a name against the current scope's locals (for capture scanning).
 * Called from NODE_BLOCK before entering bc_compile_chunk, so bc->locals
 * contains the enclosing scope's locals.
 * Returns the slot index if found, -1 otherwise. */
static int bc_resolve_enclosing(BcCompiler *bc, const char *name) {
    for (int i = bc->local_count - 1; i >= 0; i--) {
        if (strcmp(bc->locals[i].name, name) == 0)
            return bc->locals[i].slot;
    }
    return -1; /* not in enclosing scope */
}

static void bc_scan_free_vars(AstNode *node, char **block_params, int param_count,
                              BcCaptureList *caps, BcCompiler *bc);

static void bc_scan_free_vars_list(NodeList *list, char **block_params, int param_count,
                                   BcCaptureList *caps, BcCompiler *bc) {
    for (int i = 0; i < list->count; i++)
        bc_scan_free_vars(list->items[i], block_params, param_count, caps, bc);
}

static void bc_scan_free_vars(AstNode *node, char **block_params, int param_count,
                              BcCaptureList *caps, BcCompiler *bc) {
    if (!node) return;
    switch (node->type) {
        case NODE_IDENT: {
            const char *name = node->as.ident.name;
            if (bc_is_builtin_global(name)) break;
            if (bc_name_in_list(name, block_params, param_count)) break;
            if (bc_capture_has(caps, name)) break;
            /* Check if it's a local in the enclosing scope */
            int slot = bc_resolve_enclosing(bc, name);
            if (slot >= 0) {
                bc_capture_add(caps, name, slot);
            }
            break;
        }
        case NODE_BLOCK:
            /* Scan INTO nested blocks to find transitive captures.
             * Variables used in a nested block that aren't the nested
             * block's own params still need to be captured at THIS level
             * so they're available when the nested block captures them. */
            {
                int nested_pc = node->as.block.params.count;
                char **nested_params = NULL;
                /* Build combined exclusion list: current params + nested params */
                int combined_count = param_count + nested_pc;
                char **combined = NULL;
                if (combined_count > 0) {
                    combined = (char **)malloc(sizeof(char *) * (size_t)combined_count);
                    for (int i = 0; i < param_count; i++)
                        combined[i] = block_params[i];
                    for (int i = 0; i < nested_pc; i++)
                        combined[param_count + i] = node->as.block.params.items[i]->as.ident.name;
                    nested_params = combined;
                }
                bc_scan_free_vars_list(&node->as.block.body,
                                       nested_params, combined_count, caps, bc);
                free(combined);
            }
            break;
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_STRING_LIT:
        case NODE_SELF:
            break;
        case NODE_SEND:
            bc_scan_free_vars(node->as.send.receiver, block_params, param_count, caps, bc);
            bc_scan_free_vars_list(&node->as.send.args, block_params, param_count, caps, bc);
            break;
        case NODE_BINARY:
            bc_scan_free_vars(node->as.binary.left, block_params, param_count, caps, bc);
            bc_scan_free_vars(node->as.binary.right, block_params, param_count, caps, bc);
            break;
        case NODE_ASSIGN: {
            /* Check if assignment target is an enclosing local (needs capture) */
            const char *name = node->as.assign.name;
            if (!bc_is_builtin_global(name) &&
                !bc_name_in_list(name, block_params, param_count) &&
                !bc_capture_has(caps, name)) {
                int slot = bc_resolve_enclosing(bc, name);
                if (slot >= 0) bc_capture_add(caps, name, slot);
            }
            bc_scan_free_vars(node->as.assign.value, block_params, param_count, caps, bc);
            break;
        }
        case NODE_CLONE:
            bc_scan_free_vars(node->as.clone.proto, block_params, param_count, caps, bc);
            bc_scan_free_vars_list(&node->as.clone.members, block_params, param_count, caps, bc);
            break;
        case NODE_METHOD_DEF:
            bc_scan_free_vars_list(&node->as.method_def.body, block_params, param_count, caps, bc);
            break;
        case NODE_SLOT_DEF:
            bc_scan_free_vars(node->as.slot_def.value, block_params, param_count, caps, bc);
            break;
        case NODE_ARRAY_LIT:
            bc_scan_free_vars_list(&node->as.array_lit.elements, block_params, param_count, caps, bc);
            break;
        case NODE_TRY_CATCH:
            bc_scan_free_vars(node->as.try_catch.try_block, block_params, param_count, caps, bc);
            if (node->as.try_catch.catch_block)
                bc_scan_free_vars(node->as.try_catch.catch_block, block_params, param_count, caps, bc);
            if (node->as.try_catch.finally_block)
                bc_scan_free_vars(node->as.try_catch.finally_block, block_params, param_count, caps, bc);
            break;
        case NODE_RAISE:
            bc_scan_free_vars(node->as.raise.expr, block_params, param_count, caps, bc);
            break;
        case NODE_SLOT_ASSIGN:
            bc_scan_free_vars(node->as.slot_assign.receiver, block_params, param_count, caps, bc);
            bc_scan_free_vars(node->as.slot_assign.value, block_params, param_count, caps, bc);
            break;
        case NODE_RETURN:
            if (node->as.return_stmt.expr)
                bc_scan_free_vars(node->as.return_stmt.expr, block_params, param_count, caps, bc);
            break;
        case NODE_DICT_LIT:
            bc_scan_free_vars_list(&node->as.dict_lit.values, block_params, param_count, caps, bc);
            break;
        default:
            break;
    }
}

/* Pre-scan: identify which locals in the current scope will be captured
 * by nested blocks, so we can box them with cells before use. */
static void bc_prescan_list(BcCompiler *bc, NodeList *list);

static void bc_prescan_node(BcCompiler *bc, AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case NODE_BLOCK: {
            /* This block captures free vars from our scope — mark them */
            char **bparams = NULL;
            int pcount = node->as.block.params.count;
            if (pcount > 0) {
                bparams = (char **)malloc(sizeof(char *) * (size_t)pcount);
                for (int j = 0; j < pcount; j++)
                    bparams[j] = node->as.block.params.items[j]->as.ident.name;
            }
            BcCaptureList caps = {NULL, NULL, 0, 0};
            bc_scan_free_vars_list(&node->as.block.body, bparams, pcount, &caps, bc);
            for (int j = 0; j < caps.count; j++) {
                int slot = bc_resolve_local(bc, caps.names[j]);
                if (slot >= 0)
                    bc->locals[slot].is_captured = 1;
            }
            bc_capture_free(&caps);
            free(bparams);
            break;
        }
        case NODE_ASSIGN:
            bc_prescan_node(bc, node->as.assign.value);
            break;
        case NODE_SEND:
            bc_prescan_node(bc, node->as.send.receiver);
            bc_prescan_list(bc, &node->as.send.args);
            break;
        case NODE_BINARY:
            bc_prescan_node(bc, node->as.binary.left);
            bc_prescan_node(bc, node->as.binary.right);
            break;
        case NODE_TRY_CATCH:
            bc_prescan_node(bc, node->as.try_catch.try_block);
            if (node->as.try_catch.catch_block)
                bc_prescan_node(bc, node->as.try_catch.catch_block);
            if (node->as.try_catch.finally_block)
                bc_prescan_node(bc, node->as.try_catch.finally_block);
            break;
        case NODE_SLOT_ASSIGN:
            bc_prescan_node(bc, node->as.slot_assign.receiver);
            bc_prescan_node(bc, node->as.slot_assign.value);
            break;
        case NODE_RETURN:
            if (node->as.return_stmt.expr)
                bc_prescan_node(bc, node->as.return_stmt.expr);
            break;
        case NODE_DICT_LIT:
            bc_prescan_list(bc, &node->as.dict_lit.values);
            break;
        default:
            break;
    }
}

static void bc_prescan_list(BcCompiler *bc, NodeList *list) {
    for (int i = 0; i < list->count; i++)
        bc_prescan_node(bc, list->items[i]);
}

/* Check if a name is a known slot from the enclosing clone block */
static int bc_is_known_slot(BcCompiler *bc, const char *name) {
    for (int i = 0; i < bc->known_slot_count; i++) {
        if (strcmp(bc->known_slots[i], name) == 0) return 1;
    }
    return 0;
}

/* Pre-register method-local variables: walk the body for bare NODE_ASSIGN
 * names that aren't already locals or builtins, and register them as locals.
 * Skip names that are known slots — those should be slot writes to self.
 * This ensures prescan can correctly identify which locals nested blocks capture. */
static void bc_preregister_method_locals(BcCompiler *bc, NodeList *body) {
    for (int i = 0; i < body->count; i++) {
        AstNode *stmt = body->items[i];
        if (stmt && stmt->type == NODE_ASSIGN) {
            const char *name = stmt->as.assign.name;
            if (!is_builtin_global(name) && !bc_is_user_global(name) &&
                !bc_is_known_slot(bc, name) && bc_resolve_local(bc, name) < 0) {
                bc_add_local(bc, name);
            }
        } else if (stmt && stmt->type == NODE_DESTRUCTURE) {
            for (int j = 0; j < stmt->as.destructure.targets.count; j++) {
                const char *name = stmt->as.destructure.targets.items[j]->as.ident.name;
                if (!is_builtin_global(name) && !bc_is_user_global(name) &&
                    !bc_is_known_slot(bc, name) && bc_resolve_local(bc, name) < 0) {
                    bc_add_local(bc, name);
                }
            }
        }
    }
}

/* Compile a block/method body into a new bytecode chunk.
 * If captures is non-NULL, register captured variable names as locals
 * after params (slots param_count, param_count+1, ...).
 * Returns the chunk_id, or -1 on error. */
static int bc_compile_chunk(BcCompiler *bc, NodeList *body, NodeList *params,
                            BcCaptureList *captures, int is_method) {
    if (bc->chunk_count >= bc->chunk_capacity) {
        bc->chunk_capacity *= 2;
        bc->chunks = (BcChunk **)realloc(bc->chunks,
            (size_t)bc->chunk_capacity * sizeof(BcChunk *));
    }

    int chunk_id = bc->chunk_count++;

    /* Allocate chunk on heap */
    BcChunk *chunk = (BcChunk *)calloc(1, sizeof(BcChunk));
    chunk->code = (uint8_t *)malloc(BC_CHUNK_CODE);

    /* Record param names (strdup since AST may be freed).
     * Default params (NODE_ASSIGN) encode name in assign.name. */
    chunk->param_count = params ? params->count : 0;
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        if (params->items[i]->type == NODE_ASSIGN) {
            chunk->param_names[i] = strdup(params->items[i]->as.assign.name);
        } else {
            chunk->param_names[i] = strdup(params->items[i]->as.ident.name);
        }
    }

    /* Save current bytecode, line map, and local scope */
    uint8_t *outer_buf = NULL;
    int *outer_line_map = NULL;
    size_t saved_len = bc->len;
    if (saved_len > 0) {
        outer_buf = (uint8_t *)malloc(saved_len);
        memcpy(outer_buf, bc->code, saved_len);
        outer_line_map = (int *)malloc(saved_len * sizeof(int));
        memcpy(outer_line_map, bc->line_map, saved_len * sizeof(int));
    }
    int saved_current_line = bc->current_line;

    /* Save outer local state */
    BcLocal saved_locals[BC_MAX_LOCALS];
    int saved_local_count = bc->local_count;
    int saved_scope_depth = bc->scope_depth;
    int saved_in_method = bc->in_method;
    BcLocal *saved_enclosing = bc->enclosing_locals;
    int saved_enclosing_count = bc->enclosing_local_count;
    if (saved_local_count > 0)
        memcpy(saved_locals, bc->locals, sizeof(BcLocal) * (size_t)saved_local_count);

    /* Set enclosing locals for capture scanning later */
    bc->enclosing_locals = saved_local_count > 0 ? saved_locals : NULL;
    bc->enclosing_local_count = saved_local_count;

    /* Start fresh local scope for chunk */
    bc->local_count = 0;
    bc->scope_depth = 0;
    bc->len = 0;
    if (is_method) bc->in_method = 1;

    /* Register params as locals (slot 0, 1, ...) */
    for (int i = 0; i < chunk->param_count && i < 16; i++) {
        bc_add_local(bc, chunk->param_names[i]);
    }

    /* Register captured variables as locals (slots after params).
     * Mark them as captured so reads/writes go through cell indirection. */
    if (captures) {
        for (int i = 0; i < captures->count && i < 16; i++) {
            int slot = bc_add_local(bc, captures->names[i]);
            bc->locals[slot].is_captured = 1;
        }
    }

    /* Pre-register locals: scan body for bare assignments that create new
     * local variables (not params, captures, builtins, or user globals).
     * This ensures prescan can identify which locals nested blocks capture,
     * enabling proper cell-boxing for closures that escape their scope. */
    bc_preregister_method_locals(bc, body);

    /* Pre-scan body for nested blocks to identify params that will be captured.
     * This must happen AFTER params are registered as locals. */
    bc_prescan_list(bc, body);

    /* Box any locals that are marked as captured (params + method-created locals).
     * Skip capture locals (they're already cells from enclosing scope).
     * Emit: GET_LOCAL(slot) → cell_new → SET_LOCAL(slot) */
    int capture_start = chunk->param_count;
    int capture_end = chunk->param_count + (captures ? captures->count : 0);
    for (int i = 0; i < bc->local_count; i++) {
        if (bc->locals[i].is_captured && (i < capture_start || i >= capture_end)) {
            emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)i);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_cell_new);
            emit_byte(bc, 1);
            emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)i);
            emit_op(bc, CLAYVM_OP_POP);
        }
    }

    /* Emit default parameter values: for each param with a default expression,
     * check if it's nil (not provided by caller) and set the default. */
    if (params) {
        for (int i = 0; i < params->count && i < 16; i++) {
            if (params->items[i]->type == NODE_ASSIGN) {
                AstNode *default_expr = params->items[i]->as.assign.value;
                /* Check if param is nil (wasn't provided by caller) */
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)i);
                if (bc_local_is_captured(bc, i)) {
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_cell_get);
                    emit_byte(bc, 1);
                }
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_is_nil);
                emit_byte(bc, 1);
                int skip_jump = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
                /* Param was nil — emit default and store */
                if (bc_local_is_captured(bc, i)) {
                    /* Cell-boxed: cell_set(cell, default_val) */
                    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)i);
                    bc_emit_expr(bc, default_expr);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_cell_set);
                    emit_byte(bc, 2);
                    emit_op(bc, CLAYVM_OP_POP);
                } else {
                    bc_emit_expr(bc, default_expr);
                    emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)i);
                    emit_op(bc, CLAYVM_OP_POP);
                }
                patch_jump(bc, skip_jump);
            }
        }
    }

    /* Compile body: intermediate stmts POP, last expr stays on stack */
    for (int i = 0; i < body->count; i++) {
        AstNode *stmt = body->items[i];
        int is_last = (i == body->count - 1);
        if (is_last && stmt->type != NODE_RETURN &&
            stmt->type != NODE_BREAK && stmt->type != NODE_CONTINUE) {
            bc_emit_expr(bc, stmt);
        } else {
            bc_emit_stmt(bc, stmt);
        }
    }

    /* If body was empty, push nil as return value */
    if (body->count == 0) {
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
            (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
    }

    chunk->local_count = bc->local_count;

    emit_op(bc, CLAYVM_OP_HALT);

    /* Copy compiled bytecode and line map to chunk */
    chunk->len = bc->len;
    if (chunk->len > BC_CHUNK_CODE) chunk->len = BC_CHUNK_CODE;
    memcpy(chunk->code, bc->code, chunk->len);
    chunk->line_map = (int *)malloc(chunk->len * sizeof(int));
    memcpy(chunk->line_map, bc->line_map, chunk->len * sizeof(int));

    /* Restore outer code buffer, line map, and local state */
    bc->len = saved_len;
    if (outer_buf) {
        memcpy(bc->code, outer_buf, saved_len);
        free(outer_buf);
    }
    if (outer_line_map) {
        memcpy(bc->line_map, outer_line_map, saved_len * sizeof(int));
        free(outer_line_map);
    }
    bc->current_line = saved_current_line;

    /* Free chunk-local names (they were strdup'd by bc_add_local) */
    for (int i = 0; i < bc->local_count; i++) {
        free(bc->locals[i].name);
    }

    bc->local_count = saved_local_count;
    bc->scope_depth = saved_scope_depth;
    bc->in_method = saved_in_method;
    bc->enclosing_locals = saved_enclosing;
    bc->enclosing_local_count = saved_enclosing_count;
    if (saved_local_count > 0)
        memcpy(bc->locals, saved_locals, sizeof(BcLocal) * (size_t)saved_local_count);

    bc->chunks[chunk_id] = chunk;
    return chunk_id;
}

static void bc_emit_expr(BcCompiler *bc, AstNode *node) {
    if (!node || bc->had_error) return;
    bc->current_line = node->line;

    switch (node->type) {
        case NODE_INT_LIT: {
            int idx = add_constant_int(bc, node->as.int_lit.value);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_FLOAT_LIT: {
            int idx = clayvm_vm_add_constant(bc->vm, CLAYVM_DOUBLE((double)node->as.float_lit.value));
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_STRING_LIT: {
            int idx = add_constant_string(bc, node->as.string_lit.value);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)idx);
            break;
        }

        case NODE_IDENT: {
            const char *ident_name = node->as.ident.name;
            /* Check locals first */
            int slot = bc_resolve_local(bc, ident_name);
            if (slot >= 0) {
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)slot);
                /* If this local is boxed in a cell, unwrap it */
                if (bc_local_is_captured(bc, slot)) {
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_cell_get);
                    emit_byte(bc, 1);
                }
            } else if (bc->in_method && !is_builtin_global(ident_name) && !bc_is_user_global(ident_name)) {
                /* Inside a method: bare identifier → implicit self slot lookup.
                 * Emit: load self, push slot name, send(name, 0 args) */
                int self_idx = add_constant_string(bc, "self");
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)self_idx);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_global);
                emit_byte(bc, 1);
                int sel_idx = add_constant_string(bc, ident_name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_send);
                emit_byte(bc, 2);
            } else {
                /* Load global variable via native call */
                int name_idx = add_constant_string(bc, ident_name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_global);
                emit_byte(bc, 1);
            }
            break;
        }

        case NODE_SELF: {
            /* Load self from globals (set by method trampoline) */
            int name_idx = add_constant_string(bc, "self");
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_load_global);
            emit_byte(bc, 1);
            break;
        }

        case NODE_ASSIGN: {
            /* Check if assigning to an existing local */
            int slot = bc_resolve_local(bc, node->as.assign.name);
            if (slot >= 0) {
                if (bc_local_is_captured(bc, slot)) {
                    /* Cell-boxed local: GET_LOCAL(cell), value, cell_set */
                    emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)slot);
                    bc_emit_expr(bc, node->as.assign.value);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_cell_set);
                    emit_byte(bc, 2);
                } else {
                    bc_emit_expr(bc, node->as.assign.value);
                    emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)slot);
                }
                /* Value stays on stack (expression context) */
            } else if (bc->in_method && !is_builtin_global(node->as.assign.name) && !bc_is_user_global(node->as.assign.name)) {
                /* Inside a method scope: write to self slot.
                 * Method-level locals are pre-registered; this path handles
                 * bare assigns inside blocks within methods. */
                int self_idx = add_constant_string(bc, "self");
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)self_idx);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_global);
                emit_byte(bc, 1);
                int slot_name = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)slot_name);
                bc_emit_expr(bc, node->as.assign.value);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_set_slot);
                emit_byte(bc, 3);
            } else {
                /* Global: push name first, compile value, call store_global */
                int name_idx = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                bc_emit_expr(bc, node->as.assign.value);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_store_global);
                emit_byte(bc, 2);
                /* Result left on stack for expression use */
            }
            break;
        }

        /* NODE_DESTRUCTURE is desugared by the parser into NODE_ASSIGN + NODE_SEND
         * chains, so it should never reach this point. */
        case NODE_DESTRUCTURE:
            break;

        case NODE_SEND: {
            /* Push receiver */
            bc_emit_expr(bc, node->as.send.receiver);
            /* Push selector as string constant */
            int sel_idx = add_constant_string(bc, node->as.send.message);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
            /* Push args */
            for (int i = 0; i < node->as.send.args.count; i++) {
                bc_emit_expr(bc, node->as.send.args.items[i]);
            }
            /* Call native send or super_send */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            int send_fn = node->as.send.is_super ? bc->native_super_send : bc->native_send;
            emit_u16(bc, (uint16_t)send_fn);
            emit_byte(bc, (uint8_t)(2 + node->as.send.args.count));
            break;
        }

        case NODE_BINARY: {
            const char *op = node->as.binary.op;

            if (strcmp(op, "&&") == 0) {
                /* Short-circuit AND: if left is falsy, skip right */
                bc_emit_expr(bc, node->as.binary.left);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_is_truthy);
                emit_byte(bc, 1);
                int false_jump = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
                bc_emit_expr(bc, node->as.binary.right);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_to_bool);
                emit_byte(bc, 1);
                int end_jump = emit_jump(bc, CLAYVM_OP_JUMP);
                patch_jump(bc, false_jump);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)clayvm_vm_add_constant(bc->vm,
                        clay_to_vm(CLAY_FALSE)));
                patch_jump(bc, end_jump);
            } else if (strcmp(op, "||") == 0) {
                /* Short-circuit OR: if left is truthy, skip right */
                bc_emit_expr(bc, node->as.binary.left);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_is_truthy);
                emit_byte(bc, 1);
                int true_jump = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)clayvm_vm_add_constant(bc->vm,
                        clay_to_vm(CLAY_TRUE)));
                int end_jump = emit_jump(bc, CLAYVM_OP_JUMP);
                patch_jump(bc, true_jump);
                bc_emit_expr(bc, node->as.binary.right);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_to_bool);
                emit_byte(bc, 1);
                patch_jump(bc, end_jump);
            } else if (strcmp(op, "??") == 0) {
                /* Null-coalescing: if left is not nil, use it; else use right.
                 * Stack protocol: emit left, DUP, is_nil, branch. */
                bc_emit_expr(bc, node->as.binary.left);
                emit_op(bc, CLAYVM_OP_DUP);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_is_nil);
                emit_byte(bc, 1);
                /* is_nil returns 1 if nil, 0 if not nil.
                 * JUMP_IF_FALSE jumps when value is 0 (not nil). */
                int not_nil_jump = emit_jump(bc, CLAYVM_OP_JUMP_IF_FALSE);
                /* Fall through: IS nil → discard nil, emit right */
                emit_op(bc, CLAYVM_OP_POP);
                bc_emit_expr(bc, node->as.binary.right);
                int end_jump = emit_jump(bc, CLAYVM_OP_JUMP);
                /* NOT nil → left value is already on stack */
                patch_jump(bc, not_nil_jump);
                patch_jump(bc, end_jump);
            } else if (strcmp(op, "..") == 0) {
                bc_emit_expr(bc, node->as.binary.left);
                bc_emit_expr(bc, node->as.binary.right);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_make_range);
                emit_byte(bc, 2);
            } else {
                /* Map arithmetic operators to VM opcodes for fast path */
                int vm_op = -1;
                int is_cmp = 0;
                if      (strcmp(op, "+")  == 0) vm_op = CLAYVM_OP_ADD;
                else if (strcmp(op, "-")  == 0) vm_op = CLAYVM_OP_SUB;
                else if (strcmp(op, "*")  == 0) vm_op = CLAYVM_OP_MUL;
                else if (strcmp(op, "/")  == 0) vm_op = CLAYVM_OP_DIV;
                else if (strcmp(op, "%")  == 0) vm_op = CLAYVM_OP_MOD;
                else if (strcmp(op, "==") == 0) { vm_op = CLAYVM_OP_EQ;  is_cmp = 1; }
                else if (strcmp(op, "!=") == 0) { vm_op = CLAYVM_OP_NEQ; is_cmp = 1; }
                else if (strcmp(op, "<")  == 0) { vm_op = CLAYVM_OP_LT;  is_cmp = 1; }
                else if (strcmp(op, ">")  == 0) { vm_op = CLAYVM_OP_GT;  is_cmp = 1; }
                else if (strcmp(op, "<=") == 0) { vm_op = CLAYVM_OP_LTE; is_cmp = 1; }
                else if (strcmp(op, ">=") == 0) { vm_op = CLAYVM_OP_GTE; is_cmp = 1; }

                if (vm_op >= 0) {
                    /* Emit operands then VM opcode */
                    bc_emit_expr(bc, node->as.binary.left);
                    bc_emit_expr(bc, node->as.binary.right);
                    emit_op(bc, (uint8_t)vm_op);
                    if (is_cmp) {
                        /* Normalize int64 0/1 to Clay boolean */
                        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                        emit_u16(bc, (uint16_t)bc->native_to_bool);
                        emit_byte(bc, 1);
                    }
                } else {
                    /* Comparisons and other ops: use native_send
                     * (comparisons must return Clay booleans, not VM ints) */
                    bc_emit_expr(bc, node->as.binary.left);
                    int sel_idx = add_constant_string(bc, op);
                    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sel_idx);
                    bc_emit_expr(bc, node->as.binary.right);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_send);
                    emit_byte(bc, 3); /* receiver + selector + 1 arg */
                }
            }
            break;
        }

        case NODE_BLOCK: {
            /* Scan for free variables BEFORE compiling the chunk
             * (enclosing_locals is set up inside bc_compile_chunk) */
            char **param_names = NULL;
            int param_count = node->as.block.params.count;
            if (param_count > 0) {
                param_names = (char **)malloc(sizeof(char *) * (size_t)param_count);
                for (int i = 0; i < param_count; i++)
                    param_names[i] = node->as.block.params.items[i]->as.ident.name;
            }

            BcCaptureList caps;
            bc_capture_init(&caps);
            for (int i = 0; i < node->as.block.body.count; i++)
                bc_scan_free_vars(node->as.block.body.items[i],
                                  param_names, param_count, &caps, bc);
            free(param_names);

            /* Compile block body to a new chunk (with capture locals) */
            int chunk_id = bc_compile_chunk(bc, &node->as.block.body,
                                            &node->as.block.params, &caps, 0);
            if (chunk_id < 0) {
                bc_capture_free(&caps);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
                break;
            }

            /* Store capture info in the chunk */
            BcChunk *chunk = bc->chunks[chunk_id];
            chunk->capture_count = caps.count < 16 ? caps.count : 16;
            for (int i = 0; i < chunk->capture_count; i++) {
                chunk->capture_names[i] = strdup(caps.names[i]);
                chunk->capture_slots[i] = caps.slots[i];
            }

            /* Emit: push chunk_id, push param_count */
            int id_idx = add_constant_int(bc, chunk_id);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)id_idx);
            int pc_idx = add_constant_int(bc, node->as.block.params.count);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)pc_idx);

            /* Push captured values (read from enclosing scope's locals) */
            for (int i = 0; i < chunk->capture_count; i++) {
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)caps.slots[i]);
            }

            /* Push capture_count */
            int cc_idx = add_constant_int(bc, chunk->capture_count);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)cc_idx);

            /* call native_make_block(chunk_id, param_count, captures..., capture_count) */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_make_block);
            emit_byte(bc, (uint8_t)(3 + chunk->capture_count));

            bc_capture_free(&caps);
            break;
        }

        case NODE_ARRAY_LIT: {
            /* Create empty array via native */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_make_array);
            emit_byte(bc, 0);
            /* For each element: DUP array, compile element, send append */
            int append_idx = add_constant_string(bc, "append");
            for (int i = 0; i < node->as.array_lit.elements.count; i++) {
                emit_op(bc, CLAYVM_OP_DUP); /* keep array on stack */
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)append_idx);
                bc_emit_expr(bc, node->as.array_lit.elements.items[i]);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_send);
                emit_byte(bc, 3); /* arr, "append", elem */
                emit_op(bc, CLAYVM_OP_POP); /* discard send result */
            }
            /* Array is left on stack */
            break;
        }

        case NODE_DICT_LIT: {
            /* Create empty Dict via native */
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_make_dict);
            emit_byte(bc, 0);
            /* For each key-value pair: DUP dict, send set(key, val) */
            int set_idx = add_constant_string(bc, "set");
            for (int i = 0; i < node->as.dict_lit.keys.count; i++) {
                emit_op(bc, CLAYVM_OP_DUP); /* keep dict on stack */
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)set_idx);
                /* Push key as string constant */
                AstNode *key_node = node->as.dict_lit.keys.items[i];
                const char *key_str = key_node->as.string_lit.value;
                int key_idx = add_constant_string(bc, key_str);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)key_idx);
                /* Push value expression */
                bc_emit_expr(bc, node->as.dict_lit.values.items[i]);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_send);
                emit_byte(bc, 4); /* dict, "set", key, val */
                emit_op(bc, CLAYVM_OP_POP); /* discard send result */
            }
            /* Dict is left on stack */
            break;
        }

        case NODE_CLONE: {
            /* Push proto, call clone, then set slots/methods */
            bc_emit_expr(bc, node->as.clone.proto);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_clone);
            emit_byte(bc, 1);

            /* Collect slot names for Io-style assignment in methods */
            const char **saved_slots = bc->known_slots;
            int saved_slot_count = bc->known_slot_count;
            int slot_name_count = 0;
            for (int i = 0; i < node->as.clone.members.count; i++) {
                if (node->as.clone.members.items[i]->type == NODE_SLOT_DEF)
                    slot_name_count++;
            }
            const char **slot_names = NULL;
            if (slot_name_count > 0) {
                slot_names = (const char **)malloc((size_t)slot_name_count * sizeof(const char *));
                int si = 0;
                for (int i = 0; i < node->as.clone.members.count; i++) {
                    if (node->as.clone.members.items[i]->type == NODE_SLOT_DEF)
                        slot_names[si++] = node->as.clone.members.items[i]->as.slot_def.name;
                }
            }
            bc->known_slots = slot_names;
            bc->known_slot_count = slot_name_count;

            /* Set slots and methods on the cloned object */
            for (int i = 0; i < node->as.clone.members.count; i++) {
                AstNode *member = node->as.clone.members.items[i];
                if (member->type == NODE_SLOT_DEF) {
                    emit_op(bc, CLAYVM_OP_DUP); /* keep obj on stack */
                    int name_idx = add_constant_string(bc, member->as.slot_def.name);
                    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                    bc_emit_expr(bc, member->as.slot_def.value);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_set_slot);
                    emit_byte(bc, 3);
                    emit_op(bc, CLAYVM_OP_POP); /* discard set_slot return */
                } else if (member->type == NODE_METHOD_DEF) {
                    /* Compile method body to a chunk */
                    int chunk_id = bc_compile_chunk(bc,
                        &member->as.method_def.body,
                        &member->as.method_def.params, NULL, 1);
                    if (chunk_id < 0) {
                        continue;
                    }

                    /* Register in method slot table (dynamic, no limit) */
                    int slot_idx = alloc_method_slot();
                    g_method_slots[slot_idx]->chunk_id = chunk_id;
                    g_method_slots[slot_idx]->param_count = member->as.method_def.params.count;

                    /* Emit: DUP obj, push name, push slot_idx, call set_method */
                    emit_op(bc, CLAYVM_OP_DUP);
                    int name_idx = add_constant_string(bc, member->as.method_def.name);
                    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
                    int slot_const = add_constant_int(bc, slot_idx);
                    emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)slot_const);
                    emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                    emit_u16(bc, (uint16_t)bc->native_set_method);
                    emit_byte(bc, 3);
                    emit_op(bc, CLAYVM_OP_POP); /* discard set_method return */
                }
            }

            /* Restore slot context */
            free(slot_names);
            bc->known_slots = saved_slots;
            bc->known_slot_count = saved_slot_count;
            break;
        }

        case NODE_TRY_CATCH: {
            /* Compile try block (produces a block object on stack) */
            bc_emit_expr(bc, node->as.try_catch.try_block);
            /* Compile catch block (or push nil if absent) */
            if (node->as.try_catch.catch_block) {
                bc_emit_expr(bc, node->as.try_catch.catch_block);
            } else {
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
            }
            /* Compile finally block (or push nil if absent) */
            if (node->as.try_catch.finally_block) {
                bc_emit_expr(bc, node->as.try_catch.finally_block);
            } else {
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                    (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
            }
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_try_catch);
            emit_byte(bc, 3);
            break;
        }

        case NODE_RAISE: {
            bc_emit_expr(bc, node->as.raise.expr);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_raise);
            emit_byte(bc, 1);
            break;
        }

        case NODE_SLOT_ASSIGN: {
            /* receiver slot = value → set_slot, return assigned value */
            bc_emit_expr(bc, node->as.slot_assign.receiver);
            int sa_name_idx = add_constant_string(bc, node->as.slot_assign.slot_name);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)sa_name_idx);
            bc_emit_expr(bc, node->as.slot_assign.value);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_set_slot);
            emit_byte(bc, 3);
            /* native_set_slot now returns the assigned value */
            break;
        }

        default:
            /* Unsupported node — push nil */
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
            break;
    }
}

static int is_clone_expr(AstNode *node) {
    if (!node) return 0;
    if (node->type == NODE_CLONE) return 1;
    if (node->type == NODE_SEND && strcmp(node->as.send.message, "clone") == 0)
        return 1;
    return 0;
}

static void bc_emit_stmt(BcCompiler *bc, AstNode *node) {
    if (!node || bc->had_error) return;
    bc->current_line = node->line;

    if (node->type == NODE_ASSIGN) {
        /* Check if assigning to an existing local */
        int slot = bc_resolve_local(bc, node->as.assign.name);
        if (slot >= 0) {
            if (bc_local_is_captured(bc, slot)) {
                /* Cell-boxed local: GET_LOCAL(cell), value, cell_set, POP */
                emit_op_u16(bc, CLAYVM_OP_GET_LOCAL, (uint16_t)slot);
                bc_emit_expr(bc, node->as.assign.value);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_cell_set);
                emit_byte(bc, 2);
            } else {
                bc_emit_expr(bc, node->as.assign.value);
                emit_op_u16(bc, CLAYVM_OP_SET_LOCAL, (uint16_t)slot);
            }
            emit_op(bc, CLAYVM_OP_POP); /* discard (stmt context) */
        } else if (bc->in_method && !is_builtin_global(node->as.assign.name) && !bc_is_user_global(node->as.assign.name)) {
            bc_emit_expr(bc, node);  /* delegates to bc_emit_expr NODE_ASSIGN */
            emit_op(bc, CLAYVM_OP_POP);
        } else {
            /* Global: register name, push name, then compile value */
            bc_register_global(bc, node->as.assign.name);
            int name_idx = add_constant_string(bc, node->as.assign.name);
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
            bc_emit_expr(bc, node->as.assign.value);
            emit_op(bc, CLAYVM_OP_CALL_NATIVE);
            emit_u16(bc, (uint16_t)bc->native_store_global);
            emit_byte(bc, 2);
            emit_op(bc, CLAYVM_OP_POP); /* discard result */

            /* Auto-set _name slot on clone results: Cat = Object clone → Cat._name = "Cat" */
            if (is_clone_expr(node->as.assign.value)) {
                /* Load the just-stored global back */
                int load_name = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)load_name);
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_load_global);
                emit_byte(bc, 1);
                /* Push slot name "_name" */
                int slot_name = add_constant_string(bc, "_name");
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)slot_name);
                /* Push the variable name as value */
                int val_name = add_constant_string(bc, node->as.assign.name);
                emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)val_name);
                /* set_slot(obj, name, value) */
                emit_op(bc, CLAYVM_OP_CALL_NATIVE);
                emit_u16(bc, (uint16_t)bc->native_set_slot);
                emit_byte(bc, 3);
                emit_op(bc, CLAYVM_OP_POP);
            }
        }
        return;
    }

    /* Slot assignment: receiver slot = value */
    if (node->type == NODE_SLOT_ASSIGN) {
        /* Push receiver, slot name, value → call set_slot */
        bc_emit_expr(bc, node->as.slot_assign.receiver);
        int name_idx = add_constant_string(bc, node->as.slot_assign.slot_name);
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
        bc_emit_expr(bc, node->as.slot_assign.value);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_set_slot);
        emit_byte(bc, 3);
        emit_op(bc, CLAYVM_OP_POP);
        return;
    }

    /* Return statement: emit expression (or nil), then stop VM to exit chunk */
    if (node->type == NODE_RETURN) {
        if (node->as.return_stmt.expr) {
            bc_emit_expr(bc, node->as.return_stmt.expr);
        } else {
            emit_op_u16(bc, CLAYVM_OP_PUSH_CONST,
                (uint16_t)clayvm_vm_add_constant(bc->vm, CLAYVM_NIL_VAL));
        }
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_stop_vm);
        emit_byte(bc, 0);
        return;
    }

    /* Break: raise a BreakSignal */
    if (node->type == NODE_BREAK) {
        int name_idx = add_constant_string(bc, "BreakSignal");
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_load_global);
        emit_byte(bc, 1);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_raise);
        emit_byte(bc, 1);
        return;
    }

    /* Continue: raise a ContinueSignal */
    if (node->type == NODE_CONTINUE) {
        int name_idx = add_constant_string(bc, "ContinueSignal");
        emit_op_u16(bc, CLAYVM_OP_PUSH_CONST, (uint16_t)name_idx);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_load_global);
        emit_byte(bc, 1);
        emit_op(bc, CLAYVM_OP_CALL_NATIVE);
        emit_u16(bc, (uint16_t)bc->native_raise);
        emit_byte(bc, 1);
        return;
    }

    /* Expression statement */
    bc_emit_expr(bc, node);
    emit_op(bc, CLAYVM_OP_POP);
}

int bc_compile(BcCompiler *bc, AstNode *program) {
    if (!program || program->type != NODE_PROGRAM) return -1;

    bc->len = 0;
    bc->had_error = 0;

    for (int i = 0; i < program->as.program.stmts.count; i++) {
        AstNode *stmt = program->as.program.stmts.items[i];
        int is_last = (i == program->as.program.stmts.count - 1);

        if (is_last && stmt->type != NODE_ASSIGN && stmt->type != NODE_SLOT_ASSIGN) {
            /* Last expression: leave on stack for REPL auto-print */
            bc_emit_expr(bc, stmt);
        } else {
            bc_emit_stmt(bc, stmt);
        }
    }

    emit_op(bc, CLAYVM_OP_HALT);
    return bc->had_error ? -1 : 0;
}
