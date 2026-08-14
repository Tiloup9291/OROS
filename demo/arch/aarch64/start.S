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
 * start.S — AArch64 baremetal entry point (RK3328 / Orange Pi R1 Plus LTS)
 *
 * Role :
 *   1. Core 0 (primary) : switch EL2->EL1 if necessary (board's U-Boot), stack,
 *      FP/SIMD, SCTLR.A=0, clear BSS, jump in C (kmain).
 *   2. Secondary cores (secondary_entry, woken by PSCI CPU_ON):
 *      same switch EL2->EL1, VBAR, clean stack (g_sec_sp[core]), activate core's MMU
 *      IN ASSEMBLER, then secondary_main().
 *
 * Complete kernel run in EL1 (QEMU boot in EL1 ; board in EL2 -> switch).
 * No Linux dependencies. AAPCS64 convention.
 */

    .section .text.boot
    .globl _start
_start:
    /* --- Keep only the primary core running (MPIDR_EL1.Aff0 == 0) --- */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core

hang_secondary:
    wfe
    b       hang_secondary

primary_core:
    /* ================================================================
     * Step 1 : if in EL2, switch to EL1 (otherwise go directly).
     * ================================================================ */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    in_el1                 /* already in EL1 (QEMU) -> continue */

    mov     x1, #(1 << 31)         /* HCR_EL2.RW = 1 : EL1 in AArch64 */
    msr     hcr_el2, x1

    mrs     x1, cptr_el2
    bic     x1, x1, #(1 << 10)     /* CPTR_EL2.TFP = 0 : no FP/SIMD trap */
    msr     cptr_el2, x1

    mrs     x1, cnthctl_el2
    orr     x1, x1, #(1 << 0)
    orr     x1, x1, #(1 << 1)      /* EL1PCEN | EL1PCTEN : EL1 physical timer */
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr

    ldr     x1, =0x30d00800        /* SCTLR_EL1 reset-like (MMU off, A off) */
    msr     sctlr_el1, x1

    mov     x1, #0x5               /* SPSR = EL1h */
    orr     x1, x1, #(0xF << 6)    /* DAIF masked during init */
    msr     spsr_el2, x1

    adr     x1, in_el1
    msr     elr_el2, x1

    ldr     x1, =__stack_top
    msr     sp_el1, x1

    eret                           /* -> in_el1, now in EL1 */

/* ================================================================
 * From now on : we are in EL1 (Original QEMU, or after switching board).
 * ================================================================ */
in_el1:
    ldr     x1, =__stack_top
    mov     sp, x1

    mrs     x1, cpacr_el1
    orr     x1, x1, #(3 << 20)     /* FP/SIMD at EL1 */
    msr     cpacr_el1, x1
    isb

    mrs     x1, sctlr_el1
    bic     x1, x1, #(1 << 1)      /* SCTLR_EL1.A = 0 (no alignment check) */
    msr     sctlr_el1, x1
    isb

    /* --- Erase BSS --- */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
bss_loop:
    cmp     x1, x2
    b.ge    bss_done
    str     xzr, [x1], #8
    b       bss_loop
bss_done:

    bl      kmain

halt:
    wfe
    b       halt

    .size _start, . - _start

/* =====================================================================
 * secondary_entry — secondary cores entry point (PSCI CPU_ON).
 *
 * RK3328 ATF start secondary cores in EL2 -> switch EL2->EL1 like
 * core 0, install VBAR_EL1 (sec_vectors), clean stack (g_sec_sp[core]),
 * then enable MMU of THIS core IN ASSEMBLER (mmu_enable_cpu) BEFORE all
 * C code : without MMU, RAM is "Device" and the C prologue / the spinlock's exclusive
 * accesses would fault. Finally, call secondary_main().
 *
 * Note : g_sec_sp[core] is published at Point of Coherency (dc cvac) by
 * core 0 in smp_start_core BEFORE CPU_ON, else secondary read 0.
 * ===================================================================== */
    .globl secondary_entry
secondary_entry:
    /* If EL2, switch in EL1 (identical to core 0). */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    sec_in_el1

    mov     x1, #(1 << 31)         /* HCR_EL2.RW = 1 */
    msr     hcr_el2, x1
    mrs     x1, cptr_el2
    bic     x1, x1, #(1 << 10)     /* CPTR_EL2.TFP = 0 */
    msr     cptr_el2, x1
    mrs     x1, cnthctl_el2
    orr     x1, x1, #(1 << 0)
    orr     x1, x1, #(1 << 1)      /* EL1PCEN | EL1PCTEN */
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr
    ldr     x1, =0x30d00800
    msr     sctlr_el1, x1
    mov     x1, #0x5               /* SPSR = EL1h */
    orr     x1, x1, #(0xF << 6)    /* DAIF masked */
    msr     spsr_el2, x1
    adr     x1, sec_in_el1
    msr     elr_el2, x1
    ldr     x1, =__stack_top
    msr     sp_el1, x1
    eret

sec_in_el1:
    /* VBAR_EL1 on this core (exception catch -> safe loop). */
    adr     x1, sec_vectors
    msr     vbar_el1, x1
    isb

    /* FP/SIMD in EL1. */
    mrs     x1, cpacr_el1
    orr     x1, x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb
    /* SCTLR_EL1.A = 0. */
    mrs     x1, sctlr_el1
    bic     x1, x1, #(1 << 1)
    msr     sctlr_el1, x1
    isb

    /* Clean stack : g_sec_sp[core_id] (published at PoC by core 0). */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    ldr     x1, =g_sec_sp
    ldr     x2, [x1, x0, lsl #3]
    mov     sp, x2

    /* Enable MMU of THIS core IN ASM before entering in C. */
    bl      mmu_enable_cpu

    bl      secondary_main

sec_halt:
    wfe
    b       sec_halt

    .size secondary_entry, . - secondary_entry

/* =====================================================================
 * sec_vectors — minimal vectors table EL1 for secondary cores.
 * Secondary don't take IRQ (idle in wfe) ; all
 * unwanted exceptions are caught here (loop wfe) instead of going to a
 * undefined vector. Will be replaced by true per-core table.
 * 16 entries of 0x80 bytes, table aligned on 2 KiB.
 * ===================================================================== */
    .balign 2048
sec_vectors:
    .rept 16
    .balign 128
    wfe
    b       . - 4
    .endr
    .size sec_vectors, . - sec_vectors
