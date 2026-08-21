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
 * uart.h — UART driver (Synopsys DesignWare 8250 / PL011) for RK3328
 * transmission (TX) and interrupt-driven buffered reception (RX).
 */
#ifndef RTOS_DRIVERS_UART_H
#define RTOS_DRIVERS_UART_H

#include <stddef.h>
#include <stdint.h>


/* Initializes the debug UART. On real hardware, U-Boot has already configured
 * the baud rate; here we settle for a minimal idempotent init. */
void uart_init(void);

/* Emits a raw byte (blocking while the TX FIFO is full). */
void uart_putc(char c);

/* Emits a NUL-terminated C string. Converts '\n' to '\r\n'. */
void uart_puts(const char *s);

/* Emits a buffer of 'len' raw bytes (used by the newlib _write stub). */
size_t uart_write(const char *buf, size_t len);

/* Arms the TX spinlock that serializes concurrent writers (Core2 runs
 * several service threads, and the other cores may print too).
 * MUST be called by kmain() right AFTER mmu_enable(): the lock relies on
 * LDAXR/STXR, which need the MMU on (see kernel/sync.h). Until then the
 * output stays unlocked, which is safe because the early boot is
 * single-core and strictly sequential. */
void uart_tx_lock_enable(void);

/* ------------------------------------------------------------------ */
/* Buffered interrupt-driven RX                           */
/* ------------------------------------------------------------------ */
/* GIC INTID (SPI+32) of the RK3328 UART2 (debug console). */
#define UART_IRQ   89u

/* Enables interrupt-driven reception: programs the UART to raise an IRQ on
 * each received byte and flushes the RX FIFO. Call AFTER gic_enable_irq(UART_IRQ)
 * (the GIC routing to Core 2 is done by the caller). */
void uart_rx_init_irq(void);

/* RX interrupt service routine: flushes the hardware RX FIFO into the software
 * ring (lock-free SPSC). Call from the IRQ handler when intid == UART_IRQ. */
void uart_rx_isr(void);

/* Number of bytes available in the RX ring (non-blocking). */
uint32_t uart_rx_available(void);

/* Reads a byte from the RX ring. Returns 1 and fills *c if available, 0 otherwise
 * (non-blocking). */
int uart_getc(char *c);

/* Number of RX bytes dropped (ring full) since boot. */
uint32_t uart_rx_dropped(void);

#endif /* RTOS_DRIVERS_UART_H */

