/* exc_entry.s -- custom libnx exception entry with RESUME support.
 *
 * libnx's stock __libnx_exception_entry is CRASH-ONLY: it copies the kernel's
 * TLS exception frame into the __nx_exceptiondump global, calls the C handler,
 * and svcBreak()s the instant that handler RETURNS (see exception.s: the
 * returnentry is `bl __libnx_exception_handler; ...; bl svcBreak`). There is no
 * resume-on-return. That is exactly why the JIT write-redirect died on its first
 * codegen fault: the fast path handled the store and `return`ed -> svcBreak.
 *
 * To actually resume, the handler must write the new PC (and any changed GPRs)
 * into the *kernel* frame (x1 on entry, a ThreadExceptionFrameA64) and call
 * svcReturnFromException(0). __libnx_exception_entry is a WEAK symbol, so we
 * override it here: build a full ThreadExceptionDump for the C dispatcher, and
 * on the resume path propagate its (modified) PC/GPRs back into the kernel frame
 * before svcReturnFromException. x9..x28/x29 are NOT in the kernel frame and the
 * kernel restores them from the live CPU on resume (libnx's own entry restores
 * them before its redirecting svcReturnFromException), so we reload them live.
 */

/* ThreadExceptionDump (g_exc) field offsets -- must match libnx thread_context.h */
.equ D_ERR,    0x000
.equ D_X0,     0x010      /* cpu_gprs[0]  (CpuRegister = 8 bytes) */
.equ D_X9,     0x058      /* cpu_gprs[9]  = 0x010 + 9*8 */
.equ D_FP,     0x0F8      /* x29 */
.equ D_LR,     0x100      /* x30 */
.equ D_SP,     0x108
.equ D_PC,     0x110
.equ D_FPU,    0x120      /* fpu_gprs[0]  (FpuRegister = 16 bytes) */
.equ D_PSTATE, 0x320
.equ D_ESR,    0x32C
.equ D_FAR,    0x330

/* ThreadExceptionFrameA64 (kernel TLS frame, x1) field offsets */
.equ F_X0,     0x00       /* cpu_gprs[0..8] */
.equ F_LR,     0x48
.equ F_SP,     0x50
.equ F_ELR,    0x58
.equ F_PSTATE, 0x60
.equ F_ESR,    0x6C
.equ F_FAR,    0x70

.section .text.__libnx_exception_entry, "ax", %progbits
.global __libnx_exception_entry
.type   __libnx_exception_entry, %function
.align  2
__libnx_exception_entry:
    /* x0 = error_desc, x1 = kernel exception frame (== SP on entry) */

    /* ---- capture the full faulting context into g_exc ---- */
    adrp x2, g_exc
    add  x2, x2, :lo12:g_exc
    str  w0, [x2, #D_ERR]

    /* live x9..x28 -> g_exc.cpu_gprs[9..28] (not in the kernel frame; save first) */
    add  x3, x2, #D_X9
    stp  x9,  x10, [x3], #16
    stp  x11, x12, [x3], #16
    stp  x13, x14, [x3], #16
    stp  x15, x16, [x3], #16
    stp  x17, x18, [x3], #16
    stp  x19, x20, [x3], #16
    stp  x21, x22, [x3], #16
    stp  x23, x24, [x3], #16
    stp  x25, x26, [x3], #16
    stp  x27, x28, [x3], #16
    str  x29, [x2, #D_FP]           /* live fp (x29) */

    /* frame x0..x8 -> g_exc.cpu_gprs[0..8] */
    add  x3, x2, #D_X0
    ldp  x4, x5, [x1, #F_X0+0x00]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #F_X0+0x10]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #F_X0+0x20]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #F_X0+0x30]
    stp  x4, x5, [x3], #16
    ldr  x4, [x1, #F_X0+0x40]       /* x8 */
    str  x4, [x3]

    /* frame lr/sp/elr -> g_exc */
    ldr  x4, [x1, #F_LR]
    str  x4, [x2, #D_LR]
    ldr  x4, [x1, #F_SP]
    str  x4, [x2, #D_SP]
    ldr  x4, [x1, #F_ELR]
    str  x4, [x2, #D_PC]
    /* frame pstate/esr/far -> g_exc */
    ldr  w4, [x1, #F_PSTATE]
    str  w4, [x2, #D_PSTATE]
    ldr  w4, [x1, #F_ESR]
    str  w4, [x2, #D_ESR]
    ldr  x4, [x1, #F_FAR]
    str  x4, [x2, #D_FAR]

    /* live SIMD q0..q31 -> g_exc.fpu_gprs (decoder needs these for SIMD stores) */
    add  x3, x2, #D_FPU
    stp  q0,  q1,  [x3], #32
    stp  q2,  q3,  [x3], #32
    stp  q4,  q5,  [x3], #32
    stp  q6,  q7,  [x3], #32
    stp  q8,  q9,  [x3], #32
    stp  q10, q11, [x3], #32
    stp  q12, q13, [x3], #32
    stp  q14, q15, [x3], #32
    stp  q16, q17, [x3], #32
    stp  q18, q19, [x3], #32
    stp  q20, q21, [x3], #32
    stp  q22, q23, [x3], #32
    stp  q24, q25, [x3], #32
    stp  q26, q27, [x3], #32
    stp  q28, q29, [x3], #32
    stp  q30, q31, [x3], #32

    /* stash the kernel frame pointer for the resume path */
    adrp x3, g_exc_frame
    str  x1, [x3, :lo12:g_exc_frame]

    /* switch to the dedicated exception stack */
    adrp x4, __nx_exception_stack
    add  x4, x4, :lo12:__nx_exception_stack
    adrp x5, __nx_exception_stack_size
    ldr  x5, [x5, :lo12:__nx_exception_stack_size]
    add  x4, x4, x5
    mov  sp, x4

    /* Dispatch in C: __libnx_exception_handler(&g_exc). It RETURNS only when the
     * fault was a handled codegen store (resume); a real crash logs +
     * svcExitProcess() and never comes back. */
    adrp x0, g_exc
    add  x0, x0, :lo12:g_exc
    bl   __libnx_exception_handler

    /* ---- resume: write the (possibly modified) context back to the frame ---- */
    adrp x2, g_exc
    add  x2, x2, :lo12:g_exc
    adrp x3, g_exc_frame
    ldr  x1, [x3, :lo12:g_exc_frame]

    /* g_exc x0..x8 -> frame */
    add  x3, x2, #D_X0
    ldp  x4, x5, [x3], #16
    stp  x4, x5, [x1, #F_X0+0x00]
    ldp  x4, x5, [x3], #16
    stp  x4, x5, [x1, #F_X0+0x10]
    ldp  x4, x5, [x3], #16
    stp  x4, x5, [x1, #F_X0+0x20]
    ldp  x4, x5, [x3], #16
    stp  x4, x5, [x1, #F_X0+0x30]
    ldr  x4, [x3]
    str  x4, [x1, #F_X0+0x40]
    /* g_exc lr/sp/pc -> frame (pc = skip-PC set by the decoder) */
    ldr  x4, [x2, #D_LR]
    str  x4, [x1, #F_LR]
    ldr  x4, [x2, #D_SP]
    str  x4, [x1, #F_SP]
    ldr  x4, [x2, #D_PC]
    str  x4, [x1, #F_ELR]

    /* restore live x9..x28, fp (kernel resumes these from the live CPU) */
    add  x3, x2, #D_X9
    ldp  x9,  x10, [x3], #16
    ldp  x11, x12, [x3], #16
    ldp  x13, x14, [x3], #16
    ldp  x15, x16, [x3], #16
    ldp  x17, x18, [x3], #16
    ldp  x19, x20, [x3], #16
    ldp  x21, x22, [x3], #16
    ldp  x23, x24, [x3], #16
    ldp  x25, x26, [x3], #16
    ldp  x27, x28, [x3], #16
    ldr  x29, [x2, #D_FP]

    /* restore live SIMD q0..q31 -- the C handler (memcpy etc.) may have trashed
     * caller-saved vectors, and the kernel resumes SIMD from the live CPU. */
    add  x3, x2, #D_FPU
    ldp  q0,  q1,  [x3], #32
    ldp  q2,  q3,  [x3], #32
    ldp  q4,  q5,  [x3], #32
    ldp  q6,  q7,  [x3], #32
    ldp  q8,  q9,  [x3], #32
    ldp  q10, q11, [x3], #32
    ldp  q12, q13, [x3], #32
    ldp  q14, q15, [x3], #32
    ldp  q16, q17, [x3], #32
    ldp  q18, q19, [x3], #32
    ldp  q20, q21, [x3], #32
    ldp  q22, q23, [x3], #32
    ldp  q24, q25, [x3], #32
    ldp  q26, q27, [x3], #32
    ldp  q28, q29, [x3], #32
    ldp  q30, q31, [x3], #32

    /* svcReturnFromException(0): kernel eret's using the modified frame */
    mov  w0, wzr
    svc  #0x28
    brk  #0                         /* unreachable */
.size __libnx_exception_entry, . - __libnx_exception_entry
