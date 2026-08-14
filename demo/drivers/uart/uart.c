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
 * uart.c — DesignWare 8250 (RK3328) / PL011 (QEMU) UART driver
 *
 * transmission (TX, blocking) and interrupt-driven reception (RX) + lock-free (SPSC) ring buffer.
 *
 * RK3328: UART2 = debug console (base 0xFF130000, reg-shift 2, 32 bits).
 * QEMU 'virt': PL011 at 0x09000000 (built with -DUART_PL011).
 *
 * The RX ring is filled by the ISR (unique producer = Core 2, which receives
 * the GIC-routed UART IRQ) and drained by a consumer thread (SPSC):
 * no lock required, memory barriers are sufficient.
 */

#include "uart.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Lock-free RX ring buffer (SPSC) — shared by both controllers        */
/* ------------------------------------------------------------------ */
#define UART_RX_RING_SZ   256u   /* power of 2 */

static volatile uint8_t  g_rx_buf[UART_RX_RING_SZ];
static volatile uint32_t g_rx_head;   /* written by the ISR (producer) */
static volatile uint32_t g_rx_tail;   /* written by the consumer      */
static volatile uint32_t g_rx_dropped;

/* Pushes a byte into the ring (called by the ISR). */
static inline void rx_push(uint8_t b)
{
    uint32_t head = g_rx_head;
    uint32_t next = (head + 1u) & (UART_RX_RING_SZ - 1u);
    if (next == g_rx_tail) {
        g_rx_dropped++;           /* ring full: the byte is dropped */
        return;
    }
    g_rx_buf[head] = b;
    __asm__ volatile("dmb ish" ::: "memory");
    g_rx_head = next;
}

uint32_t uart_rx_available(void)
{
    return (g_rx_head - g_rx_tail) & (UART_RX_RING_SZ - 1u);
}

int uart_getc(char *c)
{
    uint32_t tail = g_rx_tail;
    if (tail == g_rx_head)
        return 0;                 /* empty */
    __asm__ volatile("dmb ish" ::: "memory");
    *c = (char)g_rx_buf[tail];
    g_rx_tail = (tail + 1u) & (UART_RX_RING_SZ - 1u);
    return 1;
}

uint32_t uart_rx_dropped(void)
{
    return g_rx_dropped;
}

/* --------- Compile-time selection of the UART target --------- */
#if defined(UART_PL011)
/* ---- ARM PL011 (QEMU virt) ---- */
#define UART_BASE      0x09000000UL
#define PL011_DR       0x00   /* Data register */
#define PL011_FR       0x18   /* Flag register */
#define PL011_FR_RXFE  (1u << 4)  /* Receive FIFO empty */
#define PL011_FR_TXFF  (1u << 5)  /* Transmit FIFO full */
#define PL011_IMSC     0x38   /* Interrupt mask set/clear */
#define PL011_MIS      0x40   /* Masked interrupt status */
#define PL011_ICR      0x44   /* Interrupt clear */
#define PL011_INT_RX   (1u << 4)  /* RX interrupt */
#define PL011_INT_RT   (1u << 6)  /* RX timeout interrupt */

static inline void mmio_write32(unsigned long addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}
static inline uint32_t mmio_read32(unsigned long addr)
{
    return *(volatile uint32_t *)addr;
}

void uart_init(void)
{
    /* QEMU/U-Boot have already configured the PL011. */
}

void uart_putc(char c)
{
    while (mmio_read32(UART_BASE + PL011_FR) & PL011_FR_TXFF)
        ; /* wait until the TX FIFO is no longer full */
    mmio_write32(UART_BASE + PL011_DR, (uint32_t)(unsigned char)c);
}

void uart_rx_init_irq(void)
{
    /* Clear pending IRQs, then unmask RX + RX-timeout. */
    mmio_write32(UART_BASE + PL011_ICR, 0x7FF);
    mmio_write32(UART_BASE + PL011_IMSC, PL011_INT_RX | PL011_INT_RT);
}

void uart_rx_isr(void)
{
    /* Drain the RX FIFO while bytes remain. */
    while (!(mmio_read32(UART_BASE + PL011_FR) & PL011_FR_RXFE)) {
        uint8_t b = (uint8_t)(mmio_read32(UART_BASE + PL011_DR) & 0xFF);
        rx_push(b);
    }
    /* Acknowledge the RX / RX-timeout sources. */
    mmio_write32(UART_BASE + PL011_ICR, PL011_INT_RX | PL011_INT_RT);
}

#else
/* ---- Synopsys DesignWare 8250 (RK3328, real hardware) ---- */
#ifndef UART_BASE
#define UART_BASE      0xFF130000UL   /* UART2 = RK3328 debug console */
#endif

#define REG_SHIFT      2              /* registers spaced by 4 bytes */
#define DW_RBR         (0x00 << REG_SHIFT)  /* Receive Buffer Register (read) */
#define DW_THR         (0x00 << REG_SHIFT)  /* Transmit Holding Register (write) */
#define DW_IER         (0x01 << REG_SHIFT)  /* Interrupt Enable Register */
#define DW_IIR         (0x02 << REG_SHIFT)  /* Interrupt Identification (read) */
#define DW_FCR         (0x02 << REG_SHIFT)  /* FIFO Control (write) */
#define DW_LSR         (0x05 << REG_SHIFT)  /* Line Status Register */

#define IER_ERBFI      (1u << 0)            /* "Received Data Available" IRQ */
#define FCR_FIFO_EN    (1u << 0)            /* enable the FIFOs */
#define FCR_RX_RST     (1u << 1)            /* reset RX FIFO */
#define FCR_TX_RST     (1u << 2)            /* reset TX FIFO */
#define LSR_DR         (1u << 0)            /* Data Ready (received byte available) */
#define LSR_THRE       (1u << 5)            /* THR empty (can accept a byte) */
#define LSR_TEMT       (1u << 6)            /* Transmitter empty */

static inline void mmio_write32(unsigned long addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}
static inline uint32_t mmio_read32(unsigned long addr)
{
    return *(volatile uint32_t *)addr;
}

void uart_init(void)
{
    /* U-Boot has already initialized the baud rate / line control: minimal
     * idempotent init, the divisor is not reprogrammed. */
}

void uart_putc(char c)
{
    while (!(mmio_read32(UART_BASE + DW_LSR) & LSR_THRE))
        ; /* wait until THR is ready */
    mmio_write32(UART_BASE + DW_THR, (uint32_t)(unsigned char)c);
}

void uart_rx_init_irq(void)
{
    /* Enable + reset the FIFOs (default RX trigger = 1 byte). */
    mmio_write32(UART_BASE + DW_FCR, FCR_FIFO_EN | FCR_RX_RST | FCR_TX_RST);
    /* Purge any residual byte from the RX FIFO. */
    while (mmio_read32(UART_BASE + DW_LSR) & LSR_DR)
        (void)mmio_read32(UART_BASE + DW_RBR);
    /* Enable the "Received Data Available" IRQ. */
    mmio_write32(UART_BASE + DW_IER, IER_ERBFI);
}

void uart_rx_isr(void)
{
    /* Reading the RBR acknowledges the RDA IRQ. Drain the whole FIFO. */
    while (mmio_read32(UART_BASE + DW_LSR) & LSR_DR) {
        uint8_t b = (uint8_t)(mmio_read32(UART_BASE + DW_RBR) & 0xFF);
        rx_push(b);
    }
    /* Read IIR to clear any pending causes (FIFO timeout). */
    (void)mmio_read32(UART_BASE + DW_IIR);
}

#endif /* UART_PL011 */

/* --------- Common part (controller-independent) --------- */

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');   /* CRLF for serial terminals */
        uart_putc(*s++);
    }
}

size_t uart_write(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n')
            uart_putc('\r');
        uart_putc(buf[i]);
    }
    return len;
}
