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
 * mmu.c — Minimal AArch64 MMU (Unlock unaligned access to newlib)
 *
 * WHY : without MMU, ALL memory is handled as "Device" type. On
 * Device memory, UNALIGNED accesses ALWAYS generate an Alignment fault.
 * newlib's printf() performs such accesses -> we need to enable the MMU and mark
 * RAM (including our code) in "Normal Memory".
 *
 * MAPPING (identity, level 1 block of 1 GiB, 4 entries = 4 GiB) :
 *
 *   Target RK3328 (hardware) : RAM at 0x00000000, devices at 0xFF000000
 *     [0] 0x00000000..0x3FFFFFFF : Normal (RAM + our code @0x00200000)
 *     [1] 0x40000000..0x7FFFFFFF : Normal (RAM)
 *     [2] 0x80000000..0xBFFFFFFF : Normal (RAM)
 *     [3] 0xC0000000..0xFFFFFFFF : Device (MMIO : UART 0xFF130000, GIC 0xFF81xxxx)
 *
 *   Target QEMU 'virt' (-DMMU_QEMU) : low devices (GIC 0x08.., UART 0x09..),
 *   RAM at 0x40000000
 *     [0] 0x00000000..0x3FFFFFFF : Device (MMIO QEMU)
 *     [1] 0x40000000..0x7FFFFFFF : Normal (RAM + our code @0x40080000)
 *     [2] 0x80000000..0xBFFFFFFF : Normal
 *     [3] 0xC0000000..0xFFFFFFFF : Normal
 *
 * Independent of Linux : ARM system registers only.
 */

#include <stdint.h>

/* ---- Attributes MAIR ---- */
#define MAIR_IDX_DEVICE   0      /* Device-nGnRnE : 0x00 */
#define MAIR_IDX_NORMAL   1      /* Normal, WB WA cacheable : 0xFF */
#define MAIR_VALUE        ((0x00UL << (8 * MAIR_IDX_DEVICE)) | \
                           (0xFFUL << (8 * MAIR_IDX_NORMAL)))

/* ---- Block descriptor bits (level 1) ---- */
#define DESC_VALID        (1UL << 0)
#define DESC_BLOCK        (0UL << 1)
#define DESC_AF           (1UL << 10)
#define DESC_SH_INNER     (3UL << 8)
#define DESC_ATTR(idx)    (((uint64_t)(idx)) << 2)

#define BLOCK_DEVICE(pa)  ((uint64_t)(pa) | DESC_VALID | DESC_BLOCK | \
                           DESC_AF | DESC_ATTR(MAIR_IDX_DEVICE))
#define BLOCK_NORMAL(pa)  ((uint64_t)(pa) | DESC_VALID | DESC_BLOCK | \
                           DESC_AF | DESC_SH_INNER | DESC_ATTR(MAIR_IDX_NORMAL))

/* ---- TCR_EL1 : granularity 4K, VA 32 bits (T0SZ=32) ---- */
#define TCR_T0SZ          (32UL << 0)
#define TCR_IRGN0_WBWA    (1UL << 8)
#define TCR_ORGN0_WBWA    (1UL << 10)
#define TCR_SH0_INNER     (3UL << 12)
#define TCR_TG0_4K        (0UL << 14)
#define TCR_IPS_4G        (0UL << 32)   /* IPS = 32 bits (4 GiB) */
#define TCR_VALUE         (TCR_T0SZ | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | \
                           TCR_SH0_INNER | TCR_TG0_4K | TCR_IPS_4G)

/* ---- SCTLR ---- */
#define SCTLR_M           (1UL << 0)
#define SCTLR_C           (1UL << 2)
#define SCTLR_I           (1UL << 12)

static uint64_t l1_table[4] __attribute__((aligned(4096)));

static inline uint64_t current_el(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 0x3;
}

/* Build level-1 translation table (once, by core 0). */
void mmu_build_tables(void)
{
#if defined(MMU_QEMU)
    /* QEMU : low devices, RAM at 0x40000000. */
    l1_table[0] = BLOCK_DEVICE(0x00000000UL);
    l1_table[1] = BLOCK_NORMAL(0x40000000UL);
    l1_table[2] = BLOCK_NORMAL(0x80000000UL);
    l1_table[3] = BLOCK_NORMAL(0xC0000000UL);
#else
    /* RK3328 : lower RAM (contains our code @0x00200000), high devices. */
    l1_table[0] = BLOCK_NORMAL(0x00000000UL);
    l1_table[1] = BLOCK_NORMAL(0x40000000UL);
    l1_table[2] = BLOCK_NORMAL(0x80000000UL);
    l1_table[3] = BLOCK_DEVICE(0xC0000000UL);   /* RK3328 MMIO (0xFF...) */
#endif
     /* Publish table for ALL cores. Secondary cores brought up through
      * PSCI may have caches where this table is not yet visible to their
      * page-table walker. Explicitly clean the cache lines covering
      * l1_table to the PoC, then issue a DSB.
      * (Harmless for core 0.) */
    uintptr_t base = (uintptr_t)l1_table;
    for (uintptr_t p = base; p < base + sizeof(l1_table); p += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * mmu_enable_cpu — programs the MMU registers of the CURRENT CORE and enables
 * the MMU.
 *
 * A secondary core brought up by the RK3328 ATF may
 * start with ITS OWN MMU ALREADY ENABLED (SCTLR.M=1) and a different
 * TCR/TTBR configuration (smaller T0SZ => 4-level page-table walk).
 * Reprogramming TCR/TTBR while M=1 is UNPREDICTABLE, and the TLB retains
 * the previous mappings, resulting in a Data Abort (level-0 translation
 * fault) on our addresses (board symptom: level-0 fault on the
 * secondary_main stack).
 *
 * Cases:
 *   - If the MMU is ALREADY enabled (M=1, ATF secondary-core case):
 *     cleanly DISABLE it (M=0, C=0) and issue an ISB BEFORE
 *     reprogramming.
 *   - If the MMU is disabled (M=0, QEMU/board primary-core case):
 *     change NOTHING else.
 *   Then: invalidate the TLB (TLBI), program MAIR/TCR/TTBR0, issue an
 *   ISB, enable M|C|I, and issue another ISB.
 */
void mmu_enable_cpu(void)
{
    uint64_t el = current_el();

    if (el == 2) {
        uint64_t sctlr;
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
        if (sctlr & SCTLR_M) {                 /* Active inherited MMU -> OFF first */
            sctlr &= ~(SCTLR_M | SCTLR_C);
            __asm__ volatile("msr sctlr_el2, %0" :: "r"(sctlr));
            __asm__ volatile("isb");
        }
        __asm__ volatile("tlbi alle2; dsb sy; isb" ::: "memory");
        __asm__ volatile("msr mair_el2, %0" :: "r"(MAIR_VALUE));
        __asm__ volatile("msr tcr_el2,  %0" :: "r"(TCR_VALUE));
        __asm__ volatile("msr ttbr0_el2, %0" :: "r"((uint64_t)l1_table));
        __asm__ volatile("isb");
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
        sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
        __asm__ volatile("dsb sy");
        __asm__ volatile("msr sctlr_el2, %0" :: "r"(sctlr));
        __asm__ volatile("isb");
    } else {
        uint64_t sctlr;
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        if (sctlr & SCTLR_M) {                 /* Active inherited MMU -> OFF first */
            sctlr &= ~(SCTLR_M | SCTLR_C);
            __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
            __asm__ volatile("isb");
        }
        __asm__ volatile("tlbi vmalle1; dsb sy; isb" ::: "memory");
        __asm__ volatile("msr mair_el1, %0" :: "r"(MAIR_VALUE));
        __asm__ volatile("msr tcr_el1,  %0" :: "r"(TCR_VALUE));
        __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l1_table));
        __asm__ volatile("isb");
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
        __asm__ volatile("dsb sy");
        __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
        __asm__ volatile("isb");
    }
}

/*
 * mmu_enable — Used by core 0 : build table THEN enable MMU
 * on this core. Secondary cores ONLY call mmu_enable_cpu().
 */
void mmu_enable(void)
{
    mmu_build_tables();
    mmu_enable_cpu();
}
