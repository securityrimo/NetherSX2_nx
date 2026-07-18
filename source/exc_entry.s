/* ThreadExceptionDump offsets from libnx thread_context.h. */
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
.equ D_SLOT_FRAME, 0x3E8
.equ D_SLOT_INDEX, 0x3F0
.equ D_SLOT_DEFERRED, 0x3E0
.equ D_SLOT_RESULT, 0x3E4
.equ D_SLOT_SHIFT, 10
.equ D_STACK_SHIFT, 16
.equ D_SLOT_MASK, 0xFF

/* ThreadExceptionFrameA64 offsets. */
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

    /* Reserve a context for this fault. */
    adrp x6, g_exc_slot_mask
    add  x6, x6, :lo12:g_exc_slot_mask
1:
    ldaxr w2, [x6]
    mvn   w3, w2
    and   w3, w3, #D_SLOT_MASK
    cbnz  w3, 2f
    yield
    b     1b
2:
    rbit  w4, w3
    clz   w4, w4
    mov   w5, #1
    lsl   w5, w5, w4
    orr   w3, w2, w5
    stxr  w7, w3, [x6]
    cbnz  w7, 1b

    adrp x2, g_exc_slots
    add  x2, x2, :lo12:g_exc_slots
    add  x2, x2, x4, lsl #D_SLOT_SHIFT
    str  x1, [x2, #D_SLOT_FRAME]
    str  w4, [x2, #D_SLOT_INDEX]
    str  wzr, [x2, #D_SLOT_DEFERRED]
    str  wzr, [x2, #D_SLOT_RESULT]

    str  w0, [x2, #D_ERR]

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
    str  x29, [x2, #D_FP]

    add  x3, x2, #D_X0
    ldp  x4, x5, [x1, #F_X0+0x00]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #F_X0+0x10]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #F_X0+0x20]
    stp  x4, x5, [x3], #16
    ldp  x4, x5, [x1, #F_X0+0x30]
    stp  x4, x5, [x3], #16
    ldr  x4, [x1, #F_X0+0x40]
    str  x4, [x3]

    ldr  x4, [x1, #F_LR]
    str  x4, [x2, #D_LR]
    ldr  x4, [x1, #F_SP]
    str  x4, [x2, #D_SP]
    ldr  x4, [x1, #F_ELR]
    str  x4, [x2, #D_PC]
    ldr  w4, [x1, #F_PSTATE]
    str  w4, [x2, #D_PSTATE]
    ldr  w4, [x1, #F_ESR]
    str  w4, [x2, #D_ESR]
    ldr  x4, [x1, #F_FAR]
    str  x4, [x2, #D_FAR]

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

    adrp x4, __nx_exception_stack
    add  x4, x4, :lo12:__nx_exception_stack
    ldr  w5, [x2, #D_SLOT_INDEX]
    add  x4, x4, x5, lsl #D_STACK_SHIFT
    add  x4, x4, #0x10, lsl #12
    mov  sp, x4
    sub  sp, sp, #16
    str  x2, [sp]

    mov  x0, x2
    bl   __libnx_exception_handler

    ldr  x2, [sp]
    add  sp, sp, #16
    ldr  x1, [x2, #D_SLOT_FRAME]

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
    ldr  x4, [x2, #D_LR]
    str  x4, [x1, #F_LR]
    ldr  x4, [x2, #D_SP]
    str  x4, [x1, #F_SP]
    ldr  x4, [x2, #D_PC]
    str  x4, [x1, #F_ELR]

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

    ldr  w8, [x2, #D_SLOT_DEFERRED]
    cbnz w8, 4f

    ldr  w4, [x2, #D_SLOT_INDEX]
    mov  w5, #1
    lsl  w5, w5, w4
    mvn  w5, w5
    adrp x6, g_exc_slot_mask
    add  x6, x6, :lo12:g_exc_slot_mask
3:
    ldaxr w3, [x6]
    and   w3, w3, w5
    stlxr w7, w3, [x6]
    cbnz  w7, 3b

4:
    mov  w0, wzr
    svc  #0x28
    brk  #0                         /* unreachable */
.size __libnx_exception_entry, . - __libnx_exception_entry

.global fastmem_fault_trampoline
.type fastmem_fault_trampoline, %function
.align 2
fastmem_fault_trampoline:
    mov  x19, x0
    bl   fastmem_run_deferred_fault
    str  w0, [x19, #D_SLOT_RESULT]
    mov  x0, xzr

.global fastmem_fault_resume_marker
.type fastmem_fault_resume_marker, %function
.size fastmem_fault_trampoline, . - fastmem_fault_trampoline
fastmem_fault_resume_marker:
    ldr  x0, [x0]
    b    fastmem_fault_resume_marker
.size fastmem_fault_resume_marker, . - fastmem_fault_resume_marker
