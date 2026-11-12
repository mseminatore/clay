/*
 * fiber_ctx.c - Fiber context initialization (stack setup)
 *
 * Sets up a fiber_regs_t so that the first clay_fiber_swap into it
 * starts executing entry_fn(arg) on the provided stack.
 */
#include "fiber_ctx.h"

#ifndef _WIN32

#include <string.h>

/* Declared in the assembly files */
extern void clay_fiber_entry_stub(void);

void clay_fiber_init_stack(clay_fiber_regs_t *ctx,
                           void *stack_base, size_t stack_size,
                           void (*entry_fn)(void *), void *arg)
{
    memset(ctx, 0, sizeof(*ctx));

    /* Stack grows downward; top = base + size, 16-byte aligned */
    uintptr_t stack_top = ((uintptr_t)stack_base + stack_size) & ~(uintptr_t)15;

#if defined(__x86_64__) || defined(_M_X64)
    /*
     * x86_64: the swap jumps to the stub (no return address is pushed), which
     * does callq *%r12. The ABI wants rsp 16-byte aligned at a call, so the
     * stack starts at stack_top, aligned: the entry function then begins with
     * rsp = 8 (mod 16) as the ABI says. (It used to start at stack_top - 8,
     * which ran the whole fiber misaligned.)
     * r12 = entry_fn, r13 = arg (used by clay_fiber_entry_stub).
     * rip = address of clay_fiber_entry_stub.
     */
    ctx->rsp = stack_top;
    ctx->rip = (uint64_t)(uintptr_t)clay_fiber_entry_stub;
    ctx->r12 = (uint64_t)(uintptr_t)entry_fn;
    ctx->r13 = (uint64_t)(uintptr_t)arg;
    ctx->rbp = 0;

#elif defined(__aarch64__) || defined(_M_ARM64)
    /*
     * ARM64: set sp to stack_top (already 16-byte aligned).
     * x30 (lr) = address of clay_fiber_entry_stub.
     * x19 = entry_fn, x20 = arg (used by the stub).
     */
    ctx->sp  = stack_top;
    ctx->x30 = (uint64_t)(uintptr_t)clay_fiber_entry_stub;
    ctx->x19 = (uint64_t)(uintptr_t)entry_fn;
    ctx->x20 = (uint64_t)(uintptr_t)arg;
    ctx->x29 = 0;
#endif
}

#endif /* !_WIN32 */
