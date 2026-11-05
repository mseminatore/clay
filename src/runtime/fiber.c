/*
 * fiber.c - Fiber (coroutine) prototype for the Clay runtime
 *
 * Each Clay fiber runs its block on its own C stack (Windows Fibers API, or
 * the assembly context switch in fiber_amd64.S / fiber_arm64.S), so yield()
 * works from any depth — including inside C-level loops like whileTrue,
 * each and map. This is shared by both backends:
 *
 *   - natively compiled programs need nothing more: block code is C;
 *   - the bytecode VM additionally gives each fiber its own VM stack and frame
 *     array, which it swaps in and out through clay_fiber_hooks (set by
 *     bc_compiler.c when the VM is created).
 */
#include "clay.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include "fiber_ctx.h"
#endif

#define FIBER_STACK_SIZE (256 * 1024)

const ClayFiberHooks *clay_fiber_hooks = NULL;

typedef struct {
    ClayObject *block;          /* retained block object */
    ClayValue yield_value;      /* last yielded value (owned until resume returns it) */
    ClayValue result;           /* final return value (owned until resume returns it) */
    int status;                 /* 0=created, 1=suspended, 2=done */
    void *engine;               /* execution-engine state (VM stacks), or NULL */

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

static CLAY_THREAD_LOCAL FiberContext *g_current_fiber = NULL;
#ifdef _WIN32
static CLAY_THREAD_LOCAL int g_main_thread_is_fiber = 0;
#endif

/* Helper: get FiberContext from a fiber object */
static FiberContext *get_fiber_ctx(ClayObject *self) {
    ClayValue v = clay_get_slot(self, "_ctx");
    if (!CLAY_IS_INT(v)) return NULL;
    return (FiberContext *)(intptr_t)clay_to_int(v);
}

/* --- Fiber entry functions (run on the fiber's own C stack) --- */
static void fiber_run_block(FiberContext *ctx) {
    clay_error_stack.head = NULL;  /* fiber starts with a clean error stack */
    ctx->result = ctx->block->as.block.func(ctx->block, 0, NULL);
    ctx->status = 2;
    ctx->saved_error_head = clay_error_stack.head;
}

#ifdef _WIN32
static VOID CALLBACK fiber_entry_win(LPVOID param) {
    FiberContext *ctx = (FiberContext *)param;
    fiber_run_block(ctx);
    SwitchToFiber(ctx->caller_fiber);
}
#else
static void fiber_entry_unix(void *param) {
    FiberContext *ctx = (FiberContext *)param;
    fiber_run_block(ctx);
    clay_fiber_swap(&ctx->fiber_regs, &ctx->caller_regs);
}
#endif

/* Fiber yield(value) — switch back to the caller's C stack */
static ClayValue fiber_yield(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (!g_current_fiber) return CLAY_NIL;
    FiberContext *ctx = g_current_fiber;
    /* Keep our own reference: the yielded value may be a temporary of the
     * fiber's code, and resume() hands it to the caller as an owned result */
    clay_release_val(ctx->yield_value);
    ctx->yield_value = (argc > 0) ? argv[0] : CLAY_NIL;
    clay_retain_val(ctx->yield_value);
    ctx->status = 1;
    ctx->saved_error_head = clay_error_stack.head;
#ifdef _WIN32
    SwitchToFiber(ctx->caller_fiber);
#else
    clay_fiber_swap(&ctx->fiber_regs, &ctx->caller_regs);
#endif
    return CLAY_NIL;
}

/* Fiber new(block) — create a fiber with its own C stack */
static ClayValue fiber_new(ClayObject *self, int argc, ClayValue *argv) {
    (void)self;
    if (argc < 1 || !CLAY_IS_TYPE(argv[0], CLAY_TYPE_BLOCK)) return CLAY_NIL;
    ClayObject *block = CLAY_OBJ(argv[0]);

    ClayObject *fiber = clay_clone(clay_Fiber);
    clay_set_slot(fiber, "_name", CLAY_VAL(clay_string_new("Fiber")));

    FiberContext *ctx = (FiberContext *)calloc(1, sizeof(FiberContext));
    ctx->block = block;
    clay_retain(block);
    ctx->status = 0;
    ctx->yield_value = CLAY_NIL;
    ctx->result = CLAY_NIL;
    ctx->engine = clay_fiber_hooks ? clay_fiber_hooks->create() : NULL;

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

/* Fiber resume() — switch to the fiber's C stack, run until yield or completion */
static ClayValue fiber_resume(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    FiberContext *ctx = get_fiber_ctx(self);
    if (!ctx || ctx->status == 2) return CLAY_NIL;

    if (clay_fiber_hooks) clay_fiber_hooks->enter(ctx->engine, ctx->status == 0);

    FiberContext *prev = g_current_fiber;
    g_current_fiber = ctx;

    /* Save caller's error stack head and swap in the fiber's */
    ClayTryFrame *caller_error_head = clay_error_stack.head;
    clay_error_stack.head = ctx->saved_error_head;

#ifdef _WIN32
    ctx->caller_fiber = GetCurrentFiber();
    SwitchToFiber(ctx->os_fiber);
#else
    clay_fiber_swap(&ctx->caller_regs, &ctx->fiber_regs);
#endif

    /* Back here after yield() or completion */
    g_current_fiber = prev;
    clay_error_stack.head = caller_error_head;

    int done = (ctx->status == 2);
    if (clay_fiber_hooks) clay_fiber_hooks->leave(ctx->engine, done);

    /* Transfer our reference to the caller (method results are owned) */
    ClayValue retval;
    if (done) { retval = ctx->result;      ctx->result = CLAY_NIL; }
    else      { retval = ctx->yield_value; ctx->yield_value = CLAY_NIL; }

    /* Free the C stack once the fiber has finished */
    if (done) {
#ifdef _WIN32
        if (ctx->os_fiber) { DeleteFiber(ctx->os_fiber); ctx->os_fiber = NULL; }
#else
        free(ctx->c_stack); ctx->c_stack = NULL;
#endif
    }
    return retval;
}

/* Fiber isDone() — check if the fiber has completed */
static ClayValue fiber_isDone(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    FiberContext *ctx = get_fiber_ctx(self);
    if (!ctx) return CLAY_TRUE;
    return CLAY_BOOL(ctx->status == 2);
}

/* Register Fiber methods on the prototype (called from clay_bootstrap) */
void clay_fiber_init_methods(ClayObject *proto) {
    clay_set_method(proto, "new", fiber_new);
    clay_set_method(proto, "resume", fiber_resume);
    clay_set_method(proto, "isDone", fiber_isDone);
    clay_set_method(proto, "yield", fiber_yield);
}

/* Free a FiberContext and its resources (called from memory.c on dealloc) */
void clay_fiber_context_free(intptr_t ctx_ptr) {
    if (!ctx_ptr) return;
    FiberContext *ctx = (FiberContext *)ctx_ptr;
    if (ctx->block) {
        clay_release(ctx->block);
        ctx->block = NULL;
    }
    clay_release_val(ctx->yield_value);
    clay_release_val(ctx->result);
#ifdef _WIN32
    if (ctx->os_fiber) { DeleteFiber(ctx->os_fiber); ctx->os_fiber = NULL; }
#else
    free(ctx->c_stack); ctx->c_stack = NULL;
#endif
    if (ctx->engine && clay_fiber_hooks) clay_fiber_hooks->destroy(ctx->engine);
    ctx->engine = NULL;
    free(ctx);
}
