/*
 * fiber_ctx.h - Platform-agnostic fiber context switching
 *
 * Provides minimal assembly-based context switching for coroutines.
 * Windows uses the Fibers API (no assembly needed).
 * Unix uses custom assembly for x86_64 and ARM64.
 */
#ifndef CLAY_FIBER_CTX_H
#define CLAY_FIBER_CTX_H

#include <stdint.h>
#include <stddef.h>

#ifndef _WIN32

/*
 * Fiber register context — stores callee-saved registers + SP + return addr.
 * Layout must match the assembly in fiber_amd64.S / fiber_arm64.S exactly.
 */
#if defined(__x86_64__) || defined(_M_X64)
typedef struct {
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp;
    uint64_t rip;
} clay_fiber_regs_t;  /* 64 bytes */
#elif defined(__aarch64__) || defined(_M_ARM64)
typedef struct {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;  /* frame pointer */
    uint64_t x30;  /* link register */
    uint64_t sp;
    uint64_t d8, d9, d10, d11, d12, d13, d14, d15; /* callee-saved SIMD */
} clay_fiber_regs_t;  /* 168 bytes */
#else
#error "Unsupported architecture for fiber context switching"
#endif

/*
 * clay_fiber_swap(old, new) — save current registers to *old, restore from *new.
 * Implemented in fiber_amd64.S or fiber_arm64.S.
 */
void clay_fiber_swap(clay_fiber_regs_t *old_ctx, clay_fiber_regs_t *new_ctx);

/*
 * clay_fiber_init_stack - set up a fiber context to start at entry_fn(arg)
 * on the given stack. The stack grows downward; stack_base points to the
 * bottom (lowest address), stack_size is in bytes.
 */
void clay_fiber_init_stack(clay_fiber_regs_t *ctx,
                           void *stack_base, size_t stack_size,
                           void (*entry_fn)(void *), void *arg);

#endif /* !_WIN32 */
#endif /* CLAY_FIBER_CTX_H */
