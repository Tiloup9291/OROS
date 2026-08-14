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
 * mailbox.h — Lock-free inter-core message queues
 *
 * Inter-partition communication WITHOUT locks: one SPSC (single-producer
 * single-consumer) ring per pair (sender -> receiver). Each message is a
 * 64-bit word (handle / index / small payload). The producer pushes
 * (mailbox_send), the consumer pops (mailbox_recv): O(1), non-blocking
 * operations -> no impact on the hard-RT cores' WCET.
 *
 * An IPI notification (SGI IPI_MAILBOX) can wake up the receiving core
 * (mailbox_send_notify). Cache coherence is ensured by barriers (dmb ish):
 * the memory is Normal Inner-Shareable (mmu.c) -> no dc cvac needed here.
 *
 * Independent of Linux.
 */
#ifndef RTOS_MAILBOX_H
#define RTOS_MAILBOX_H

#include <stdint.h>
#include "config.h"

/* Number of entries per queue (power of 2). */
#define MBX_ENTRIES   64u

typedef struct {
    volatile uint32_t head;                 /* written by the producer */
    volatile uint32_t tail;                 /* written by the consumer */
    volatile uint32_t dropped;              /* dropped messages (queue full) */
    uint64_t          buf[MBX_ENTRIES];
} mailbox_t;

/* Initializes all mailboxes (source->dest matrix). */
void mailbox_init(void);

/* Sends 'msg' from the current core to 'dst_core'. Returns 1 if accepted, 0 if
 * the queue is full (message counted as dropped). Non-blocking. */
int  mailbox_send(uint32_t dst_core, uint64_t msg);

/* Same + notifies the destination core with an IPI (SGI IPI_MAILBOX). */
int  mailbox_send_notify(uint32_t dst_core, uint64_t msg);

/* Pops a message intended for the current core, from 'src_core'.
 * Returns 1 if a message was extracted (*out filled), 0 if the queue is empty. */
int  mailbox_recv(uint32_t src_core, uint64_t *out);

/* Pops a message intended for the current core, from any sender
 * (scan of sources). Returns 1 if a message was extracted, 0 otherwise. */
int  mailbox_recv_any(uint32_t *src_core_out, uint64_t *out);

/* Total number of dropped messages (overflow), across all queues. */
uint32_t mailbox_dropped(void);

#endif /* RTOS_MAILBOX_H */
