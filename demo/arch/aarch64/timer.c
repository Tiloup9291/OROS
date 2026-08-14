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
 * timer.c — ARM Generic Timer (CNTP physical EL1)
 *
 * We use EL1 physical time (CNTP_*), whose registers CNTP_CTL_EL0,
 * CNTP_TVAL_EL0, CNTP_CVAL_EL0 and counter CNTPCT_EL0 are accessible
 * at both EL1 and EL2 (our case under U-Boot/QEMU).
 *
 * IRQ generated is PPI 30 (INTID 30), to activate in GIC.
 *
 * Independent of Linux : ARM system registers only.
 */

#include "timer.h"

/* Bits of CNTP_CTL_EL0 */
#define CNTP_CTL_ENABLE   (1u << 0)   /* timer enabled */
#define CNTP_CTL_IMASK    (1u << 1)   /* IRQ mask (1 = masked) */
#define CNTP_CTL_ISTATUS  (1u << 2)   /* condition reached (read) */

/* Current interval in ticks (for the periodic mode). */
static uint64_t g_period_ticks;

/* ---- Registers access ---- */
static inline uint64_t read_cntfrq(void)
{
    uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}
static inline uint64_t read_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
static inline void write_cntp_tval(uint64_t v)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}
static inline void write_cntp_ctl(uint64_t v)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}
static inline uint64_t read_cntp_ctl(void)
{
    uint64_t v; __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(v)); return v;
}

/* ---- API ---- */
uint64_t timer_frequency(void)
{
    return read_cntfrq();
}

uint64_t timer_now_ticks(void)
{
    return read_cntpct();
}

uint64_t timer_ticks_to_us(uint64_t ticks)
{
    uint64_t f = read_cntfrq();
    return (ticks * 1000000ull) / f;
}

uint64_t timer_us_to_ticks(uint64_t us)
{
    uint64_t f = read_cntfrq();
    return (us * f) / 1000000ull;
}

void timer_set_oneshot_us(uint32_t us)
{
    uint64_t ticks = timer_us_to_ticks(us);
    write_cntp_tval(ticks);
    /* enable, IRQ not masked */
    write_cntp_ctl(CNTP_CTL_ENABLE);
}

void timer_init_periodic(uint32_t hz)
{
    uint64_t f = read_cntfrq();
    g_period_ticks = f / hz;

    /* Program the first tick and start. */
    write_cntp_tval(g_period_ticks);
    write_cntp_ctl(CNTP_CTL_ENABLE);   /* ENABLE=1, IMASK=0 */
}

void timer_ack_and_reload(void)
{
    /* Reload the interval for the next tick.
     * CNTP_TVAL decrements ; Rewriting it resets the comparator. */
    write_cntp_tval(g_period_ticks);
    /* Ensure the timer remains enabled and the IRQ unmasked. */
    write_cntp_ctl(CNTP_CTL_ENABLE);
    (void)read_cntp_ctl();
}
