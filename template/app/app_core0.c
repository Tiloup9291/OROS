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
 * app_core0.c — APPLICATION hook of Core0 (RT_HARD, EtherCAT).
 *
 * The permanent EtherCAT loop lives in lib/ethercat/ecat_task.c: it is
 * READY (reserve -> domain -> PDO -> activate -> fixed-period cycle with WCET
 * instrumentation). On EVERY cycle it calls app_ecat_cycle() below: this is
 * where the process-data logic of the machine is written.
 *
 * REAL-TIME CONSTRAINTS (Core0):
 *   - NO printf / uart_puts / FatFs / lwIP here (they would blow the WCET).
 *     Background traces: klog_write() / klog_write_u() (lock-free, drained
 *     by Core2), or mailbox_send_notify(CFG_CORE_IO_SOFT, value).
 *   - Bounded execution time: no unbounded loop, no dynamic allocation.
 *     Check the budget with the `wcet` shell command (proc max vs cycle_us).
 */

#include <stdint.h>

#include "app.h"
#include "../kernel/klog.h"
#include "../kernel/config.h"

/* ------------------------------------------------------------------ */
/* Process image (Core0-private, published to the shell by ecat_diag)  */
/* ------------------------------------------------------------------ */
static uint16_t s_do;          /* outputs of the previous cycle */
static uint64_t s_cycle;       /* local cycle counter */

/*
 * app_ecat_cycle — called on every EtherCAT cycle (period CFG_ECAT_CYCLE_US).
 *
 *   di  = 16 input bits read from the slave (PDO 0x6000:00)
 *   ret = 16 output bits to write to the slave (PDO 0x7000:00)
 *
 * TODO (programmer): write the real-time process logic here.
 *
 * Skeleton kept NEUTRAL on purpose: outputs are held at their previous value
 * (a fail-safe default, no blinking, no console output).
 */
uint16_t app_ecat_cycle(uint16_t di)
{
    s_cycle++;

    /* ---------------- TODO: application logic ---------------------- *
     * Example (to be removed / replaced):
     *     if (di & 0x0001)  s_do |=  0x0001;   // input 0 -> output 0
     *     else              s_do &= ~0x0001;
     *
     * Available (RT-safe) helpers:
     *     klog_write("[app] event");             // lock-free trace
     *     klog_write_u("[app] di=", di);         // trace + hex value
     *     mailbox_send_notify(CFG_CORE_IO_SOFT, value);
     * -------------------------------------------------------------- */
    (void)di;

    return s_do;
}
