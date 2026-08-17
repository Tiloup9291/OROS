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
 * net_task.h — PERMANENT IP/SSH network stack on Core2.
 *
 * "Infinite thread" version of the demo (net_demo_run). Initializes
 * once (xHCI + RTL8153B + lwIP + telnet:23 + SSH:22), then runs an INFINITE
 * polling loop (RX + lwIP timers + SSH state machine), WITHOUT an end
 * window. Runs on Core2 (IO_SOFT), IN PARALLEL with Core0's PERMANENT
 * EtherCAT master.
 *
 * The remote shell (telnet/SSH) reads the ecat_diag snapshot published
 * CONTINUOUSLY by Core0 -> EtherCAT is seen in real time (no longer "frozen
 * after 20 s").
 *
 * On QEMU (-DMMU_QEMU): no xHCI -> the task reports it and goes idle.
 */
#ifndef RTOS_NET_TASK_H
#define RTOS_NET_TASK_H

/* Entry point of the permanent network thread (Core2). Never returns.
 * 'arg' is ignored (thread_entry_t signature). */
void net_task_entry(void *arg);

#endif /* RTOS_NET_TASK_H */
