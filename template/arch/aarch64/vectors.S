/*
 * OROS - OROS' Real-Time Operating System
 * Copyright (C) 2026  Tiloup9291 <-> John Doe
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * vectors.S — AArch64 Exception Vector table (EL1)
 *
 * Context : run in EL1 (QEMU '-M virt -kernel' boot in EL1).
 * We use ELR_EL1 / SPSR_EL1 / ESR_EL1 / FAR_EL1 and VBAR_EL1.
 *
 * Handle :
 *   - Synchronous exceptions (faults) -> exception_handler() (diagnostic UART).
 *   - IRQ → save the complete context (272-byte trap frame), then
 *     irq_handler(sp_saved) ; the return value is the SP of the context to
 *     restore (enables preemption: return the SP of another thread).
 *
 * Table aligned to 2 KiB (VBAR_EL1). Each entry = 0x80 bytes.
 */

    .section .text
    .align 11
    .globl vector_table
vector_table:

/* Save x0..x30 + ELR_EL1 + SPSR_EL1 + v0..v31 + FPCR/FPSR on stack
 * (800 bytes, aligned 16: must stay equal to TRAPFRAME_SIZE). */
.macro SAVE_CONTEXT
    sub     sp, sp, #800
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    stp     x30, xzr, [sp, #16 * 15]
    mrs     x9,  elr_el1
    mrs     x10, spsr_el1
    stp     x9,  x10, [sp, #16 * 16]
    /* v0..v31: offset 272 (=16*17) .. 272+512 = 784. */
    stp     q0,  q1,  [sp, #16 * 17]
    stp     q2,  q3,  [sp, #16 * 19]
    stp     q4,  q5,  [sp, #16 * 21]
    stp     q6,  q7,  [sp, #16 * 23]
    stp     q8,  q9,  [sp, #16 * 25]
    stp     q10, q11, [sp, #16 * 27]
    stp     q12, q13, [sp, #16 * 29]
    stp     q14, q15, [sp, #16 * 31]
    stp     q16, q17, [sp, #16 * 33]
    stp     q18, q19, [sp, #16 * 35]
    stp     q20, q21, [sp, #16 * 37]
    stp     q22, q23, [sp, #16 * 39]
    stp     q24, q25, [sp, #16 * 41]
    stp     q26, q27, [sp, #16 * 43]
    stp     q28, q29, [sp, #16 * 45]
    stp     q30, q31, [sp, #16 * 47]
    /* FPCR / FPSR: offsets 784 and 792 (must be 8-aligned for str/ldr x). */
    mrs     x9,  fpcr
    mrs     x10, fpsr
    str     x9,  [sp, #784]
    str     x10, [sp, #792]
.endm

/* Restore ELR_EL1 + SPSR_EL1 + x0..x30 + v0..v31 + FPCR/FPSR from stack. */
.macro RESTORE_CONTEXT
    ldp     x9,  x10, [sp, #16 * 16]
    msr     elr_el1,  x9
    msr     spsr_el1, x10
    ldp     q30, q31, [sp, #16 * 47]
    ldp     q28, q29, [sp, #16 * 45]
    ldp     q26, q27, [sp, #16 * 43]
    ldp     q24, q25, [sp, #16 * 41]
    ldp     q22, q23, [sp, #16 * 39]
    ldp     q20, q21, [sp, #16 * 37]
    ldp     q18, q19, [sp, #16 * 35]
    ldp     q16, q17, [sp, #16 * 33]
    ldp     q14, q15, [sp, #16 * 31]
    ldp     q12, q13, [sp, #16 * 29]
    ldp     q10, q11, [sp, #16 * 27]
    ldp     q8,  q9,  [sp, #16 * 25]
    ldp     q6,  q7,  [sp, #16 * 23]
    ldp     q4,  q5,  [sp, #16 * 21]
    ldp     q2,  q3,  [sp, #16 * 19]
    ldp     q0,  q1,  [sp, #16 * 17]
    ldr     x9,  [sp, #784]
    ldr     x10, [sp, #792]
    msr     fpcr, x9
    msr     fpsr, x10
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp, sp, #800
.endm

.macro VEC_SYNC index
    .align 7
    mov     x0, #\index
    b       sync_common
.endm

.macro VEC_IRQ
    .align 7
    b       irq_entry
.endm

.macro VEC_OTHER index
    .align 7
    mov     x0, #\index
    b       sync_common
.endm

    /* Group 0 : Current EL with SP0 */
    VEC_SYNC 0
    VEC_IRQ
    VEC_OTHER 2
    VEC_OTHER 3

    /* Group 1 : Current EL with SPx (our case in EL1h) */
    VEC_SYNC 4
    VEC_IRQ
    VEC_OTHER 6
    VEC_OTHER 7

    /* Group 2 : Lower EL AArch64 */
    VEC_SYNC 8
    VEC_IRQ
    VEC_OTHER 10
    VEC_OTHER 11

    /* Group 3 : Lower EL AArch32 */
    VEC_SYNC 12
    VEC_IRQ
    VEC_OTHER 14
    VEC_OTHER 15

/* ---- Synchronous exceptions : diagnostic ---- */
/* x0 = index ; we pass ESR/ELR/FAR (EL1) to the C handler. */
sync_common:
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      exception_handler
hang_sync:
    wfe
    b       hang_sync

/* ---- IRQ : save -> irq_handler -> restore ---- */
irq_entry:
    SAVE_CONTEXT
    mov     x0, sp                 /* arg0 = sp_saved (trap frame pointer) */
    bl      irq_handler            /* x0 = sp_to_restore */
    mov     sp, x0
    RESTORE_CONTEXT
    eret

    .size vector_table, . - vector_table
