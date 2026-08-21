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
 * context.S — Starting first thread
 *
 * cpu_start_first(uint64_t sp) : install SP on initial trap frame of a
 * thread and do eret to its entry point. Used once per
 * sched_start() to start scheduling. Never come back.
 *
 * Trap frame must have the same layout of vectors.S (see TRAPFRAME_SIZE) :
 *   [x0..x30, pad][ELR_EL1][SPSR_EL1][v0..v31][FPCR][FPSR], total: 800 bytes.
 * The initial NEON/FPCR/FPSR values are zero (reset state): the frame is
 * zeroed by thread_create_on(), so there is nothing to restore here.
 */

    .section .text
    .globl cpu_start_first
cpu_start_first:
    mov     sp, x0                    /* SP = trap frame of 1st thread */

    /* Restore ELR_EL1 / SPSR_EL1 (running in EL1) */
    ldp     x9,  x10, [sp, #16 * 16]
    msr     elr_el1,  x9
    msr     spsr_el1, x10

    /* Restore x0..x30 */
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

    eret

    .size cpu_start_first, . - cpu_start_first
