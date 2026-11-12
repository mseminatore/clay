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
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE       /* pthread_get_stackaddr_np */
#elif !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE            /* pthread_getattr_np */
#endif
#include "clay.h"
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include "fiber_ctx.h"
#endif

#define FIBER_STACK_SIZE (256 * 1024)

/* --- C stack guard (see CLAY_STACK_CHECK in clay.h) --- */

CLAY_THREAD_LOCAL char *clay_stack_limit = NULL;

/* the room kept below the limit for what runs after a check: natives, the
 * error being raised, unwinding */
static size_t stack_margin(size_t size) {
    size_t m = size / 4;
    return m > 128 * 1024 ? 128 * 1024 : m;
}

void clay_stack_init(void) {
    char *lo = NULL;
    size_t size = 0;
#if defined(_WIN32)
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    lo = (char *)low;
    size = (size_t)(high - low);
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    size = pthread_get_stacksize_np(self);
    lo = (char *)pthread_get_stackaddr_np(self) - size;      /* the address is the top */
#elif defined(__linux__)
    if (syscall(SYS_gettid) == (long)getpid()) {
        /* the main thread: its stack may grow to the soft limit. Count from
         * where we are now rather than from the top of the mapping, which
         * leaves out the arguments and environment above us (and a big one
         * only makes the limit stricter, never looser) */
        struct rlimit rl;
        size = 8u * 1024 * 1024;
        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
            size = (size_t)rl.rlim_cur;
        if (size > 1024u * 1024 * 1024) size = 1024u * 1024 * 1024;
        size_t above = 512 * 1024;      /* what is above this call */
        if (size > above + 64 * 1024) {
            lo = (char *)__builtin_frame_address(0) - (size - above);
            size -= above;
        } else {
            size = 0;
        }
    } else {
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void *addr = NULL;
            if (pthread_attr_getstack(&attr, &addr, &size) == 0) lo = (char *)addr;
            pthread_attr_destroy(&attr);
        }
    }
#endif
    if (!lo || size < 64 * 1024) { clay_stack_limit = NULL; return; }
    clay_stack_limit = lo + stack_margin(size);
}

/* --- A big stack for the program ---
 *
 * Recursion in Clay uses C stack, and the operating system's default for the
 * main thread (8 MB on Linux, 1 MB on Windows) is small for it. The program
 * runs on a thread whose stack is CLAY_STACK_MB megabytes (default 256): only
 * address space is reserved, pages are committed as they are used. 0 keeps the
 * main thread and the system's stack size. */
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
/* AddressSanitizer gives up unpoisoning the stack after a longjmp when the
 * stack is bigger than 64 MB, and then reports false errors */
#define CLAY_DEFAULT_STACK_MB 60
#else
#define CLAY_DEFAULT_STACK_MB 256
#endif

typedef struct {
    int (*fn)(int, char **);
    int argc;
    char **argv;
    int rc;
} StackJob;

size_t clay_stack_bytes(void) {
    long mb = CLAY_DEFAULT_STACK_MB;
    const char *env = getenv("CLAY_STACK_MB");
    if (env && *env) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end && *end == '\0' && v >= 0) mb = v;
    }
    if (mb > 16384) mb = 16384;
    if (sizeof(void *) < 8 && mb > 512) mb = 512;       /* 32-bit address space */
    return (size_t)mb * 1024u * 1024u;
}

#ifdef _WIN32
static DWORD WINAPI stack_job_entry(LPVOID p) {
    StackJob *job = (StackJob *)p;
    job->rc = job->fn(job->argc, job->argv);
    return 0;
}

int clay_run_with_stack(int (*fn)(int, char **), int argc, char **argv) {
    size_t bytes = clay_stack_bytes();
    StackJob job = { fn, argc, argv, 0 };
    if (bytes == 0) return fn(argc, argv);
    HANDLE h = CreateThread(NULL, bytes, stack_job_entry, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) return fn(argc, argv);                      /* no thread: the main one will do */
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return job.rc;
}
#else
static void *stack_job_entry(void *p) {
    StackJob *job = (StackJob *)p;
    job->rc = job->fn(job->argc, job->argv);
    return NULL;
}

int clay_run_with_stack(int (*fn)(int, char **), int argc, char **argv) {
    size_t bytes = clay_stack_bytes();
    StackJob job = { fn, argc, argv, 0 };
    if (bytes == 0) return fn(argc, argv);
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr) != 0) return fn(argc, argv);
    if (pthread_attr_setstacksize(&attr, bytes) != 0 ||
        pthread_create(&thread, &attr, stack_job_entry, &job) != 0) {
        pthread_attr_destroy(&attr);
        return fn(argc, argv);                          /* no thread: the main one will do */
    }
    pthread_attr_destroy(&attr);
    pthread_join(thread, NULL);
    return job.rc;
}
#endif

void clay_stack_overflow(void) {
    clay_raise(CLAY_VAL(clay_error_new("stack overflow: recursion is too deep", "RuntimeError")));
}

const ClayFiberHooks *clay_fiber_hooks = NULL;

typedef struct {
    ClayObject *block;          /* retained block object */
    ClayValue yield_value;      /* last yielded value (owned until resume returns it) */
    ClayValue result;           /* final return value (owned until resume returns it) */
    int status;                 /* 0=created, 1=suspended, 2=done */
    int failed;                 /* done because an error was raised in it... */
    ClayValue error;            /* ...this one: resume() raises it in its caller */
    void *engine;               /* execution-engine state (VM stacks), or NULL */

    /* Saved error stack head — isolate try frames per fiber */
    ClayTryFrame *saved_error_head;
    ClayUnwindList unwind;      /* the fiber's clay_unwind_list */

#ifdef _WIN32
    LPVOID os_fiber;            /* Windows fiber handle */
    LPVOID caller_fiber;        /* caller's fiber handle (set by resume) */
#else
    clay_fiber_regs_t fiber_regs;
    clay_fiber_regs_t caller_regs;
    char *c_stack;              /* allocated C stack */
#endif
    char *stack_limit;          /* its clay_stack_limit while it runs (NULL: unchecked) */
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
#ifdef _WIN32
    clay_stack_init();                    /* this fiber's own stack */
    ctx->stack_limit = clay_stack_limit;
#endif
    clay_error_stack.head = NULL;  /* fiber starts with a clean error stack */
    /* An error nothing in the fiber catches ends the fiber; resume() raises
     * it in whoever resumed it, where a try can catch it */
    ClayTryFrame tf;
    if (clay_try_push(&tf) == 0) {
        ctx->result = ctx->block->as.block.func(ctx->block, 0, NULL);
        clay_try_pop();
    } else {
        ctx->failed = 1;
        ctx->error = clay_error_stack.current_error;
    }
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
    clay_set_slot_take(fiber, "_name", CLAY_VAL(clay_string_new("Fiber")));

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
    ctx->stack_limit = ctx->c_stack + stack_margin(FIBER_STACK_SIZE);
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
    ClayUnwindList *caller_unwind = clay_unwind_list;
    clay_unwind_list = &ctx->unwind;

#ifdef _WIN32
    ctx->caller_fiber = GetCurrentFiber();
    char *caller_limit = clay_stack_limit;
    clay_stack_limit = ctx->stack_limit;
    SwitchToFiber(ctx->os_fiber);
#else
    char *caller_limit = clay_stack_limit;
    clay_stack_limit = ctx->stack_limit;
    clay_fiber_swap(&ctx->caller_regs, &ctx->fiber_regs);
#endif
    clay_stack_limit = caller_limit;

    /* Back here after yield() or completion */
    g_current_fiber = prev;
    clay_error_stack.head = caller_error_head;
    clay_unwind_list = caller_unwind;

    int done = (ctx->status == 2);
    if (clay_fiber_hooks) clay_fiber_hooks->leave(ctx->engine, done);

    /* Transfer our reference to the caller (method results are owned) */
    ClayValue retval;
    if (done) { retval = ctx->result;      ctx->result = CLAY_NIL; }
    else      { retval = ctx->yield_value; ctx->yield_value = CLAY_NIL; }
    ClayValue failure = CLAY_NIL;
    if (ctx->failed) { failure = ctx->error; ctx->error = CLAY_NIL; ctx->failed = 0; }

    /* Free the C stack once the fiber has finished */
    if (done) {
#ifdef _WIN32
        if (ctx->os_fiber) { DeleteFiber(ctx->os_fiber); ctx->os_fiber = NULL; }
#else
        free(ctx->c_stack); ctx->c_stack = NULL;
#endif
    }
    if (!CLAY_IS_NIL(failure)) clay_raise(failure);    /* the fiber's error, in the caller */
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
    clay_unwind_list_free(&ctx->unwind);
    free(ctx);
}
