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
 * klog.c — Lock-free per-core logging
 *
 * One SPSC (single-producer single-consumer) ring buffer per core:
 *   - producer = the core that writes its logs (klog_write);
 *   - consumer = Core 2 (klog_drain_to_uart).
 *
 * The ring uses volatile head/tail indices. Since there is only one
 * producer and one consumer per ring, no critical section is needed
 * (lock-free), so there is no impact on the hard-RT cores' WCET.
 *
 *
 * Each message is pre-formatted as text. Later, one
 * may switch to a light binary format formatted on the Core 2 side.
 *
 * Independent of Linux.
 */

#include "klog.h"
#include "../drivers/uart/uart.h"

/* Cycle counter for timestamping. */
static inline uint64_t cycles_now(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint32_t core_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

/* A log entry. */
typedef struct {
    uint64_t ts;                       /* timestamp (cycles) */
    uint32_t core;
    char     text[CFG_LOG_MSG_MAXLEN];
} log_entry_t;

/* One ring per core. */
typedef struct {
    volatile uint32_t head;            /* written by the producer */
    volatile uint32_t tail;            /* written by the consumer */
    volatile uint32_t dropped;
    log_entry_t       buf[CFG_LOG_RING_ENTRIES];
} log_ring_t;

static log_ring_t g_rings[CFG_NUM_CORES];

#define RING_MASK   (CFG_LOG_RING_ENTRIES - 1u)

void klog_init(void)
{
    for (uint32_t c = 0; c < CFG_NUM_CORES; c++) {
        g_rings[c].head = 0;
        g_rings[c].tail = 0;
        g_rings[c].dropped = 0;
    }
}

/* Bounded copy of a source string to dest (max size n, always terminated). */
static void copy_str(char *dst, const char *src, uint32_t n)
{
    uint32_t i = 0;
    if (n == 0) return;
    while (src && src[i] && i < n - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Appends a hex integer to the end of dst (remaining space bounded). */
static void append_hex(char *dst, uint64_t v, uint32_t cap)
{
    static const char hd[] = "0123456789ABCDEF";
    uint32_t len = 0;
    while (dst[len] && len < cap) len++;
    if (len + 3 >= cap) return;    /* not enough room for "0x" + at least 1 digit */
    dst[len++] = '0';
    dst[len++] = 'x';
    int started = 0;
    for (int shift = 60; shift >= 0 && len < cap - 1; shift -= 4) {
        uint32_t nib = (uint32_t)((v >> shift) & 0xF);
        if (nib || started || shift == 0) {
            dst[len++] = hd[nib];
            started = 1;
        }
    }
    dst[len] = '\0';
}

static void ring_push(log_ring_t *r, const char *msg, int with_val, uint64_t val)
{
    uint32_t head = r->head;
    uint32_t next = (head + 1u) & RING_MASK;

    if (next == r->tail) {
        /* Ring full: count the drop, do not block. */
        r->dropped++;
        return;
    }

    log_entry_t *e = &r->buf[head];
    e->ts   = cycles_now();
    e->core = core_id();
    copy_str(e->text, msg, CFG_LOG_MSG_MAXLEN);
    if (with_val)
        append_hex(e->text, val, CFG_LOG_MSG_MAXLEN);

    /* Publish after writing the data (barrier for ordering). */
    __asm__ volatile("dmb ish" ::: "memory");
    r->head = next;
}

void klog_write(const char *msg)
{
    ring_push(&g_rings[core_id()], msg, 0, 0);
}

void klog_write_u(const char *msg, uint64_t value)
{
    ring_push(&g_rings[core_id()], msg, 1, value);
}

/* Emits a decimal integer on the UART (for readable timestamp). */
static void uart_put_u64(uint64_t v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v && i < 24) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i--) uart_putc(tmp[i]);
}

uint32_t klog_drain_to_uart(void)
{
    uint32_t emitted = 0;

    for (uint32_t c = 0; c < CFG_NUM_CORES; c++) {
        log_ring_t *r = &g_rings[c];
        while (r->tail != r->head) {
            log_entry_t *e = &r->buf[r->tail];
            /* Format: [ts core] message */
            uart_puts("[");
            uart_put_u64(e->ts);
            uart_puts(" c");
            uart_put_u64(e->core);
            uart_puts("] ");
            uart_puts(e->text);
            uart_puts("\n");

            __asm__ volatile("dmb ish" ::: "memory");
            r->tail = (r->tail + 1u) & RING_MASK;
            emitted++;
        }
    }
    return emitted;
}

uint32_t klog_dropped(void)
{
    uint32_t total = 0;
    for (uint32_t c = 0; c < CFG_NUM_CORES; c++)
        total += g_rings[c].dropped;
    return total;
}
