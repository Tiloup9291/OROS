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
 * app.h — PRODUCTION application layer (the ONLY place to be coded).
 *
 * OROS boots, initializes ALL the drivers (permanently) and starts the 4
 * partition loops. Those loops are READY: the programmer only fills in the
 * hooks below (marked TODO in app_core*.c).
 *
 *   Core0 (RT_HARD) : permanent EtherCAT master   -> app_ecat_cycle()
 *   Core1 (RT_HARD) : PLC scan engine (1 kHz)     -> app_read_inputs()
 *                                                    app_control()
 *                                                    app_write_outputs()
 *   Core2 (IO_SOFT) : infrastructure supervisor   -> nothing to code
 *                     (klog, mailbox, UART shell, USB keyboard hot-plug,
 *                      lwIP + telnet + SSH, Ethernet link hot-plug)
 *   Core3 (RT_SOFT) : soft periodic loop          -> app_soft_periodic()
 *
 * REAL-TIME RULE (see API.md - 16): on Core0/Core1/Core3 do NOT use printf /
 * uart_puts / FatFs / lwIP. Use klog_write() (lock-free, drained by Core2)
 * and mailbox_send_notify() only.
 */
#ifndef OROS_APP_H
#define OROS_APP_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Thread entry points (created by kmain, never return)               */
/* ------------------------------------------------------------------ */
void app_core1_entry(void *arg);   /* PLC engine       (Core1, RT_HARD) */
void app_core2_entry(void *arg);   /* supervisor + I/O (Core2, IO_SOFT) */
void app_core3_entry(void *arg);   /* soft periodic    (Core3, RT_SOFT) */

/* ------------------------------------------------------------------ */
/* Core0 — EtherCAT hook (called EVERY EtherCAT cycle, RT_HARD)        */
/* ------------------------------------------------------------------ */
/*
 * app_ecat_cycle — process-data logic of the EtherCAT cycle.
 *   `di`  : 16 input bits just read from the slave (PDO 0x6000).
 *   ret   : 16 output bits to write to the slave  (PDO 0x7000).
 * Budget: must stay FAR below CFG_ECAT_CYCLE_US (see `wcet` shell command).
 */
uint16_t app_ecat_cycle(uint16_t di);

/* ------------------------------------------------------------------ */
/* Core1 — PLC hooks (scan cycle CFG_CYCLE_US, RT_HARD)                */
/* ------------------------------------------------------------------ */
void app_read_inputs(void);        /* image copy: field  -> memory */
void app_control(void *arg);       /* user logic (run-to-completion) */
void app_write_outputs(void);      /* image copy: memory -> field */

/* ------------------------------------------------------------------ */
/* Core3 — soft periodic hook (period APP_SOFT_PERIOD_US)              */
/* ------------------------------------------------------------------ */
void app_soft_periodic(void);

/* ------------------------------------------------------------------ */
/* Core2 — optional keyboard hook                                     */
/* ------------------------------------------------------------------ */
/*
 * app_on_key — called for each character typed on the USB-A keyboard
 * BEFORE it is injected into the shell line editor. Return 1 to consume the
 * character (the shell will not see it), 0 to let it through. Default
 * implementation (app_core2.c): returns 0.
 */
int app_on_key(char c);

#endif /* OROS_APP_H */
