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
 * pmu.h — Performance Monitoring Unit (PMU) AArch64
 *
 * Expose cycle counter (PMCCNTR_EL0) to precisely measure Hard-RT cores latency/jitter (WCET measured).
 *
 * Each core has its own PMU : pmu_init() must by called BY EACH
 * core (core 0 in kmain, secondary in secondary_main).
 *
 * Independent of Linux : ARM system registers only.
 */
#ifndef RTOS_ARCH_PMU_H
#define RTOS_ARCH_PMU_H

#include <stdint.h>

/* Enable current core PMU and start cycle counter. */
void pmu_init(void);

/* Current cycle counter (PMCCNTR_EL0) of current core.
 * Resolution = 1 CPU cycle -> ideal for fine-grained jitter/WCET measurements. */
static inline uint64_t pmu_cycles(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

/* Reset current core cycle counter to 0. */
void pmu_reset_cycles(void);

#endif /* RTOS_ARCH_PMU_H */
