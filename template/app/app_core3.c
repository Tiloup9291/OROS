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
 * app_core3.c — Core3 (RT_SOFT): soft periodic loop, READY TO RUN.
 *
 * Loop clocked on the Generic Timer with an ABSOLUTE cadence (no drift),
 * period APP_SOFT_PERIOD_US (10 ms by default). Meant for less critical
 * periodic work: counters, supervision, sequencers, alarm computation...
 *
 * The programmer only fills in app_soft_periodic() (TODO).
 *
 * REAL-TIME CONSTRAINTS (Core3, soft): no printf/uart/FatFs/lwIP (Core2 owns
 * the I/O). Traces via klog_write(); signalling via mailbox_send_notify().
 * An overrun here does not endanger Core0/Core1 (strict partitioning), but
 * the loop resynchronizes to avoid accumulating lag.
 */

#include <stdint.h>

#include "app.h"
#include "../kernel/config.h"
#include "../kernel/klog.h"
#include "../arch/aarch64/timer.h"

/* Period of the soft periodic loop (µs). 10 ms = 100 Hz. */
#ifndef APP_SOFT_PERIOD_US
#define APP_SOFT_PERIOD_US   10000u
#endif

/* Statistics of the soft loop (readable while debugging). */
volatile uint64_t app_soft_cycles;
volatile uint64_t app_soft_overruns;

/* ------------------------------------------------------------------ */
/* Soft periodic hook                                                  */
/* ------------------------------------------------------------------ */
void app_soft_periodic(void)
{
    /* ---------------- TODO: soft periodic work --------------------- *
     * Called every APP_SOFT_PERIOD_US (10 ms). Examples: statistics
     * aggregation, state machines, timers, alarm computation.
     *
     * RT-safe helpers:
     *     klog_write("[soft] state X");
     *     klog_write_u("[soft] value=", v);
     *     mailbox_send_notify(CFG_CORE_IO_SOFT, v);
     * -------------------------------------------------------------- */
}

/* ------------------------------------------------------------------ */
/* Core3 thread entry point — READY loop (never returns)               */
/* ------------------------------------------------------------------ */
void app_core3_entry(void *arg)
{
    (void)arg;

    klog_write("[soft] Core3 periodic loop started");

    const uint64_t period_ticks = timer_us_to_ticks(APP_SOFT_PERIOD_US);
    uint64_t next = timer_now_ticks() + period_ticks;

    for (;;) {
        uint64_t now = timer_now_ticks();
        if (now >= next + period_ticks) {
            /* More than one period late: real overrun, resynchronize. */
            app_soft_overruns++;
            next = now + period_ticks;
        } else {
            while (timer_now_ticks() < next)
                __asm__ volatile("nop");
            next += period_ticks;
        }

        app_soft_periodic();
        app_soft_cycles++;
    }
}
