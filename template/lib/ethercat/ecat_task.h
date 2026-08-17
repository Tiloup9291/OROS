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
 * ecat_task.h — PERMANENT EtherCAT master on Core0.
 *
 * Unlike the demo (ecat_demo_run, bounded to ~20,000 cycles THEN
 * returns), this task runs CONTINUOUSLY on the dedicated hard-RT core
 * (Core0 = CFG_CORE_ECAT_HARD), IN PARALLEL with Core2's network/SSH.
 *
 * Model: thread entry point (never returns). Initializes once (reserves the
 * master, scans, ESM INIT->OP), then an infinite cycle loop clocked by the
 * Generic Timer (period CFG_ECAT_CYCLE_US), in synchronous POLLING (GMAC IRQ
 * disabled).
 *
 * Each cycle: receive -> domain_process -> EC_READ(DI)/EC_WRITE(DO)
 * -> domain_queue -> send, and CONTINUOUS PUBLICATION of the ecat_diag
 * snapshot (read by the remote shell on Core2). Cycle jitter is measured
 * via the PMU (WCET).
 *
 * If no master is available (GMAC missing/QEMU/no link): the task reports it
 * and goes idle (wfi) — it does not block the rest of the system.
 */
#ifndef RTOS_ETHERCAT_ECAT_TASK_H
#define RTOS_ETHERCAT_ECAT_TASK_H

/* Entry point of the permanent EtherCAT thread (Core0). Never returns.
 * 'arg' is ignored (thread_entry_t signature). */
void ecat_task_entry(void *arg);

#endif /* RTOS_ETHERCAT_ECAT_TASK_H */
