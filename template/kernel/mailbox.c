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
 * mailbox.c — Lock-free inter-core message queues
 *
 * Matrix of SPSC queues: g_mbx[src][dst] is written by 'src' (producer) and
 * read by 'dst' (consumer). Since each queue has exactly one producer and
 * one consumer, no lock is needed (volatile head/tail indices + dmb ish
 * barriers for publication/consumption ordering).
 *
 * Since the memory is Normal Inner-Shareable (mmu.c), a core's writes are
 * visible to the others after a dmb ish (hardware MOESI coherence protocol) —
 * no explicit cache cleanup (dc cvac) is needed.
 *
 * Independent of Linux.
 */

#include "mailbox.h"
#include "../arch/aarch64/gic.h"
#include "../arch/aarch64/smp.h"

#define MBX_MASK   (MBX_ENTRIES - 1u)

/* Source -> destination matrix. */
static mailbox_t g_mbx[CFG_NUM_CORES][CFG_NUM_CORES];

void mailbox_init(void)
{
    for (uint32_t s = 0; s < CFG_NUM_CORES; s++)
        for (uint32_t d = 0; d < CFG_NUM_CORES; d++) {
            g_mbx[s][d].head = 0;
            g_mbx[s][d].tail = 0;
            g_mbx[s][d].dropped = 0;
        }
}

int mailbox_send(uint32_t dst_core, uint64_t msg)
{
    if (dst_core >= CFG_NUM_CORES)
        return 0;

    uint32_t src = smp_core_id();
    mailbox_t *m = &g_mbx[src][dst_core];

    uint32_t head = m->head;
    uint32_t next = (head + 1u) & MBX_MASK;
    if (next == m->tail) {
        m->dropped++;          /* queue full: do not block an RT core */
        return 0;
    }

    m->buf[head] = msg;
    __asm__ volatile("dmb ish" ::: "memory");   /* publish the data before head */
    m->head = next;
    return 1;
}

int mailbox_send_notify(uint32_t dst_core, uint64_t msg)
{
    int ok = mailbox_send(dst_core, msg);
    if (ok)
        gic_send_sgi(IPI_MAILBOX, dst_core);     /* wake up the receiver */
    return ok;
}

int mailbox_recv(uint32_t src_core, uint64_t *out)
{
    if (src_core >= CFG_NUM_CORES || out == 0)
        return 0;

    uint32_t dst = smp_core_id();
    mailbox_t *m = &g_mbx[src_core][dst];

    if (m->tail == m->head)
        return 0;                                /* empty queue */

    *out = m->buf[m->tail];
    __asm__ volatile("dmb ish" ::: "memory");    /* consume before advancing tail */
    m->tail = (m->tail + 1u) & MBX_MASK;
    return 1;
}

int mailbox_recv_any(uint32_t *src_core_out, uint64_t *out)
{
    for (uint32_t s = 0; s < CFG_NUM_CORES; s++) {
        if (mailbox_recv(s, out)) {
            if (src_core_out)
                *src_core_out = s;
            return 1;
        }
    }
    return 0;
}

uint32_t mailbox_dropped(void)
{
    uint32_t total = 0;
    for (uint32_t s = 0; s < CFG_NUM_CORES; s++)
        for (uint32_t d = 0; d < CFG_NUM_CORES; d++)
            total += g_mbx[s][d].dropped;
    return total;
}
