/*
 * fiber_amd64.S - x86_64 context switch for coroutines (Linux, macOS)
 *
 * clay_fiber_swap(old_ctx, new_ctx):
 *   rdi = old_ctx (clay_fiber_regs_t *)
 *   rsi = new_ctx (clay_fiber_regs_t *)
 *
 * Saves callee-saved regs + rsp + return address to old_ctx,
 * restores from new_ctx, then "returns" into the new context.
 *
 * Register layout in clay_fiber_regs_t (8 bytes each):
 *   [0x00] rbx  [0x08] rbp  [0x10] r12  [0x18] r13
 *   [0x20] r14  [0x28] r15  [0x30] rsp  [0x38] rip
 */

#if defined(__x86_64__)

.text

#ifdef __APPLE__
.globl _clay_fiber_swap
_clay_fiber_swap:
#else
.globl clay_fiber_swap
.type  clay_fiber_swap, @function
clay_fiber_swap:
#endif
    /* Save callee-saved registers to old_ctx (rdi) */
    movq %rbx,  0x00(%rdi)
    movq %rbp,  0x08(%rdi)
    movq %r12,  0x10(%rdi)
    movq %r13,  0x18(%rdi)
    movq %r14,  0x20(%rdi)
    movq %r15,  0x28(%rdi)
    movq %rsp,  0x30(%rdi)
    /* Save return address (on top of stack) */
    movq (%rsp), %rax
    movq %rax,  0x38(%rdi)

    /* Restore callee-saved registers from new_ctx (rsi) */
    movq 0x00(%rsi), %rbx
    movq 0x08(%rsi), %rbp
    movq 0x10(%rsi), %r12
    movq 0x18(%rsi), %r13
    movq 0x20(%rsi), %r14
    movq 0x28(%rsi), %r15
    movq 0x30(%rsi), %rsp
    /* Jump to saved rip */
    movq 0x38(%rsi), %rax
    jmpq *%rax

#ifdef __APPLE__
.globl _clay_fiber_entry_stub
_clay_fiber_entry_stub:
#else
.globl clay_fiber_entry_stub
.type  clay_fiber_entry_stub, @function
clay_fiber_entry_stub:
#endif
    /* Called on first resume. r12 = entry_fn, r13 = arg (set by init_stack) */
    movq %r13, %rdi
    callq *%r12
    /* Should never return — entry_fn must switch back */
    ud2

#endif /* __x86_64__ */

/* Mark the stack non-executable (ELF). Kept outside the architecture guard:
 * this file is assembled on every platform, and an object without the note
 * makes the linker fall back to an executable stack for the whole binary. */
#if defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
