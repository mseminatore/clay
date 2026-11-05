/*
 * fiber_arm64.S - ARM64/AArch64 context switch for coroutines (macOS, Linux)
 *
 * clay_fiber_swap(old_ctx, new_ctx):
 *   x0 = old_ctx (clay_fiber_regs_t *)
 *   x1 = new_ctx (clay_fiber_regs_t *)
 *
 * Saves callee-saved regs + sp + lr to old_ctx,
 * restores from new_ctx, then "returns" into the new context.
 *
 * Register layout in clay_fiber_regs_t (8 bytes each):
 *   [0x00] x19  [0x08] x20  [0x10] x21  [0x18] x22
 *   [0x20] x23  [0x28] x24  [0x30] x25  [0x38] x26
 *   [0x40] x27  [0x48] x28  [0x50] x29(fp) [0x58] x30(lr)
 *   [0x60] sp
 *   [0x68] d8   [0x70] d9   [0x78] d10  [0x80] d11
 *   [0x88] d12  [0x90] d13  [0x98] d14  [0xa0] d15
 */

#if defined(__aarch64__)

.text

#ifdef __APPLE__
.globl _clay_fiber_swap
.p2align 2
_clay_fiber_swap:
#else
.globl clay_fiber_swap
.type  clay_fiber_swap, %function
.p2align 2
clay_fiber_swap:
#endif
    /* Save callee-saved registers to old_ctx (x0) */
    stp x19, x20, [x0, #0x00]
    stp x21, x22, [x0, #0x10]
    stp x23, x24, [x0, #0x20]
    stp x25, x26, [x0, #0x30]
    stp x27, x28, [x0, #0x40]
    stp x29, x30, [x0, #0x50]
    mov x2, sp
    str x2, [x0, #0x60]
    /* Save callee-saved SIMD registers */
    stp d8,  d9,  [x0, #0x68]
    stp d10, d11, [x0, #0x78]
    stp d12, d13, [x0, #0x88]
    stp d14, d15, [x0, #0x98]

    /* Restore callee-saved registers from new_ctx (x1) */
    ldp x19, x20, [x1, #0x00]
    ldp x21, x22, [x1, #0x10]
    ldp x23, x24, [x1, #0x20]
    ldp x25, x26, [x1, #0x30]
    ldp x27, x28, [x1, #0x40]
    ldp x29, x30, [x1, #0x50]
    ldr x2, [x1, #0x60]
    mov sp, x2
    /* Restore callee-saved SIMD registers */
    ldp d8,  d9,  [x1, #0x68]
    ldp d10, d11, [x1, #0x78]
    ldp d12, d13, [x1, #0x88]
    ldp d14, d15, [x1, #0x98]

    /* Return into the new context (lr was restored) */
    ret

#ifdef __APPLE__
.globl _clay_fiber_entry_stub
.p2align 2
_clay_fiber_entry_stub:
#else
.globl clay_fiber_entry_stub
.type  clay_fiber_entry_stub, %function
.p2align 2
clay_fiber_entry_stub:
#endif
    /* Called on first resume. x19 = entry_fn, x20 = arg (set by init_stack) */
    mov x0, x20
    blr x19
    /* Should never return — entry_fn must switch back */
    brk #0

#endif /* __aarch64__ */

/* Mark the stack non-executable (ELF). Kept outside the architecture guard:
 * this file is assembled on every platform, and an object without the note
 * makes the linker fall back to an executable stack for the whole binary. */
#if defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
