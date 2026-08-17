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
 * pmu.c — Performance Monitoring Unit (PMU) AArch64
 *
 * Enable cycle counter (PMCCNTR_EL0) accessible in EL0/EL1 :
 *   - PMCR_EL0.E    (bit 0) : enable all counters.
 *   - PMCR_EL0.C    (bit 2) : reset cycle counter to 0.
 *   - PMCNTENSET_EL0.C (bit 31) : enable cycle counter.
 *   - PMUSERENR_EL0.EN (bit 0) : enable EL0 access (useful for later use).
 *
 * Each core has its own PMU -> pmu_init() is called by each core.
 *
 * Note (board) : on some ATF versions, PMU access from EL1 is allowed
 * (MDCR_EL3 does not trap PMCCNTR by default). If a trap ever occurs,
 * we could fall back to CNTPCT (timer) — not required here.
 *
 * Independent of Linux.
 */

#include "pmu.h"

#define PMCR_E   (1u << 0)      /* Enable all counters */
#define PMCR_C   (1u << 2)      /* Cycle counter reset */
#define PMCNTEN_C (1u << 31)    /* Cycle counter enable bit (PMCNTENSET_EL0) */
#define PMUSERENR_EN (1u << 0)  /* EL0 access enable */

void pmu_init(void)
{
    /* Enable EL0 access to counters (useful for instrumentation). */
    __asm__ volatile("msr pmuserenr_el0, %0" :: "r"((uint64_t)PMUSERENR_EN));

    /* Enable + reset cycle counter. */
    uint64_t pmcr;
    __asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    pmcr |= (PMCR_E | PMCR_C);
    __asm__ volatile("msr pmcr_el0, %0" :: "r"(pmcr));

    /* Enable cycle counter in PMCNTENSET_EL0. */
    __asm__ volatile("msr pmcntenset_el0, %0" :: "r"((uint64_t)PMCNTEN_C));
    __asm__ volatile("isb");
}

void pmu_reset_cycles(void)
{
    uint64_t pmcr;
    __asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    pmcr |= PMCR_C;
    __asm__ volatile("msr pmcr_el0, %0" :: "r"(pmcr));
    __asm__ volatile("isb");
}
