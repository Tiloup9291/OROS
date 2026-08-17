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
 * app_core1.c — Core1 (RT_HARD): PLC scan engine, READY TO RUN.
 *
 * The loop is complete: plc_init(CFG_CYCLE_US, ...) + plc_register_task() +
 * plc_run() (infinite scan cycle: READ INPUTS -> LOGIC -> WRITE OUTPUTS ->
 * WAIT next tick, absolute cadence, jitter/overrun statistics).
 *
 * The programmer only fills in the 3 hooks below (TODO):
 *   app_read_inputs()   — copy the field inputs into the memory image
 *   app_control()       — user logic (run-to-completion)
 *   app_write_outputs() — copy the memory image to the field outputs
 *
 * REAL-TIME CONSTRAINTS (Core1): no printf/uart/FatFs/lwIP (Core2 does the
 * I/O). Traces via klog_write() / klog_write_u(); inter-core signalling via
 * mailbox_send_notify(CFG_CORE_IO_SOFT, value). Statistics are readable from
 * the shell (`stats`) through plc_get_stats().
 */

#include <stdint.h>
#include <stddef.h>

#include "app.h"
#include "../kernel/plc.h"
#include "../kernel/config.h"
#include "../kernel/klog.h"
#include "../drivers/gpio/gpio.h"

/* ------------------------------------------------------------------ */
/* Process image (input / output memory of the PLC cycle)              */
/* ------------------------------------------------------------------ */
/* Volatile: readable by the diagnostics of the other partitions. */
volatile uint32_t app_pi[8];    /* process INPUTS  (image) */
volatile uint32_t app_po[8];    /* process OUTPUTS (image) */

/* Engine statistics (updated each cycle, readable by the shell). */
static plc_stats_t s_plc_stats;

/* ------------------------------------------------------------------ */
/* 1) READ INPUTS — start of the scan cycle                            */
/* ------------------------------------------------------------------ */
void app_read_inputs(void)
{
    /* ---------------- TODO: read the real inputs ------------------- *
     * Example (13-pin header, input pin13 = GPIO2_A2, pull-up):
     *     app_pi[0] = gpio_get_value(GPIO_BANK2, GPIO_PIN(GPIO_GROUP_A, 2));
     * The GPIO driver is already initialized (drivers_init) and is
     * RT-safe (direct MMIO, no lock).
     * -------------------------------------------------------------- */
}

/* ------------------------------------------------------------------ */
/* 2) LOGIC — user program, run-to-completion                          */
/* ------------------------------------------------------------------ */
void app_control(void *arg)
{
    (void)arg;

    /* ---------------- TODO: control logic -------------------------- *
     * Reads app_pi[], writes app_po[]. Bounded execution time
     * (no wait, no unbounded loop) so that the 1 ms cycle holds.
     *
     * Example:
     *     app_po[0] = app_pi[0] ? 1u : 0u;
     * -------------------------------------------------------------- */
}

/* ------------------------------------------------------------------ */
/* 3) WRITE OUTPUTS — end of the scan cycle                            */
/* ------------------------------------------------------------------ */
void app_write_outputs(void)
{
    /* ---------------- TODO: drive the real outputs ----------------- *
     * Example (13-pin header, output pin10 = GPIO3_C0):
     *     gpio_set_value(GPIO_BANK3, GPIO_PIN(GPIO_GROUP_C, 0),
     *                    app_po[0] ? GPIO_HIGH : GPIO_LOW);
     * -------------------------------------------------------------- */
}

/* ------------------------------------------------------------------ */
/* Core1 thread entry point — READY loop (never returns)               */
/* ------------------------------------------------------------------ */
void app_core1_entry(void *arg)
{
    (void)arg;

    /* Cyclic engine: period CFG_CYCLE_US (1 ms by default, see config.h). */
    plc_init(CFG_CYCLE_US, app_read_inputs, app_write_outputs);

    /* Main task: every cycle. Add other tasks here with a larger
     * `every_cycles` divider (e.g. 10 = every 10 ms). */
    plc_register_task(app_control, NULL, 1u, "app_control");

    klog_write("[plc] Core1 scan engine started");

    /* Infinite scan loop (never returns). Statistics published for `stats`. */
    plc_run(&s_plc_stats);
}
