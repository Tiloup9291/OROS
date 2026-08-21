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
 * klog.h — Lock-free per-core logging, SMP-extensible.
 */
#ifndef RTOS_KLOG_H
#define RTOS_KLOG_H

#include <stdint.h>
#include "config.h"

/* Initializes the log rings (all cores). */
void klog_init(void);

/* Writes a message (short text) into the current core's ring.
 * Non-blocking, O(1). If the ring is full: increments a drop counter. */
void klog_write(const char *msg);

/* Variant with a hex integer appended (useful in RT without printf). */
void klog_write_u(const char *msg, uint64_t value);

/* Drains all rings and emits the messages on the UART (called by Core 2 /
 * the log task). Returns the number of emitted messages. */
uint32_t klog_drain_to_uart(void);

/*
 * klog_drain_budgeted — emits AT MOST `budget` messages on the UART then
 * returns, so that draining never monopolizes the calling thread (the UART
 * is slow and blocking: an unbounded drain would stall the other services).
 * Rings are walked in order. Returns the number of messages actually
 * emitted; if `emitted` is non-NULL it receives the same value.
 * `budget == 0` means "no limit" (same behaviour as klog_drain_to_uart()).
 */
uint32_t klog_drain_budgeted(uint32_t budget, uint32_t *emitted);

/* Total number of dropped messages (overflow), across all cores. */
uint32_t klog_dropped(void);

#endif /* RTOS_KLOG_H */
