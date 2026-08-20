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
 * config.h — Central OROS configuration
 *
 * Kernel compile-time parameters. Independent of Linux.
 */
#ifndef RTOS_CONFIG_H
#define RTOS_CONFIG_H

/* ------------------------------------------------------------------ */
/* Real-time — PLC model                                              */
/* ------------------------------------------------------------------ */
/* Period of the hard-RT scan cycle (programmable logic controller). 1 ms = 1 kHz.
 * This clocks Core1's PLC ENGINE — DO NOT change it for the EtherCAT WCET
 * campaign (use CFG_ECAT_CYCLE_US below). */
#define CFG_CYCLE_US            1000u        /* PLC T_cycle = 1 ms */

/* Preemptive scheduling tick frequency (for soft core / debug). */
#define CFG_TICK_HZ             1000u        /* 1000 ticks/s = 1 ms */

/* ------------------------------------------------------------------ */
/* Threads / scheduler                                                */
/* ------------------------------------------------------------------ */
#define CFG_MAX_THREADS         32u          /* max number of static threads */
#define CFG_NUM_PRIORITIES      32u          /* 0 = highest priority */
/* Stack per thread: 32 KiB. Raised from 16 KiB for the SFTP service: the
 * wolfSSH path canonicalization/confinement helpers (GetAndCleanPath,
 * SFTP_RecvRealPath) stack several WOLFSSH_MAX_FILENAME buffers at once, and
 * the FatFs FIL object embeds a FF_MAX_SS (512 B) sector window. */
#define CFG_THREAD_STACK_SIZE   (32u * 1024u)/* stack per thread: 32 KiB */
#define CFG_IDLE_STACK_SIZE     (4u * 1024u) /* idle task stack */

/* ------------------------------------------------------------------ */
/* PLC cyclic tasks                                                   */
/* ------------------------------------------------------------------ */
#define CFG_MAX_CYCLIC_TASKS    16u

/* ------------------------------------------------------------------ */
/* Logging lock-free                                                  */
/* ------------------------------------------------------------------ */
#define CFG_LOG_RING_ENTRIES    256u         /* entries per ring (power of 2) */
#define CFG_LOG_MSG_MAXLEN      96u          /* max length of a text message */

/* ------------------------------------------------------------------ */
/* SMP — partitioning by criticality                                  */
/* ------------------------------------------------------------------ */
/* Current state embeds a real-time EtherCAT MASTER on the
 * native GMAC (YT8531C PHY). A hard-RT core is RESERVED for it.
 *   Core0 = RT_HARD dedicated to EtherCAT (fixed cycle, native GMAC, isolated IRQs)
 *   Core1 = RT_HARD  (critical PLC tasks)
 *   Core2 = IO_SOFT  (USB host + USB-Ethernet RTL8153B + lwIP + SSH + shell + logs + sdmmc + fatfs)
 *   Core3 = RT_SOFT  (less critical periodic tasks)
 * IP/SSH network = 2nd port (USB-Ethernet RTL8153B, USB 2.0). EtherCAT = native GMAC. */
#define CFG_NUM_CORES           4u
#define CFG_CORE_ECAT_HARD      0u   /* dedicated EtherCAT master (RT_HARD) */
#define CFG_CORE_RT_HARD_1      1u   /* critical PLC (RT_HARD) */
#define CFG_CORE_IO_SOFT        2u   /* USB/USB-Eth/lwIP/SSH/shell/logs */
#define CFG_CORE_RT_SOFT        3u   /* periodic soft-RT */

/* Compatibility alias: the old RT_HARD_0 name now points to the EtherCAT core */
#define CFG_CORE_RT_HARD_0      CFG_CORE_ECAT_HARD

/* ------------------------------------------------------------------ */
/* EtherCAT master                                                    */
/* ------------------------------------------------------------------ */
/* EtherCAT cycle period on Core0 (thread `ecat_master`, synchronous polling,
 * GMAC IRQ disabled).
 *
 * WCET CAMPAIGN: to test a shorter period,
 *    CHANGE ONLY THE VALUE BELOW (`CFG_ECAT_CYCLE_US`).
 *    DO NOT touch `CFG_CYCLE_US` (it clocks Core1's PLC engine).
 *    Examples: 1000u (=1 ms, default) · 500u · 250u · 100u.
 *    At 600 MHz, processing WCET ~29 µs → 250 µs = ~11.6% cycle load.
 *    After modifying: `make`, reflash, rerun the campaign (the shell command
 *    `wcet` displays the new period and recomputes the load in %). */
#define CFG_ECAT_CYCLE_US       1000u   /* EtherCAT master cycle (Core0), in µs */

#endif /* RTOS_CONFIG_H */
