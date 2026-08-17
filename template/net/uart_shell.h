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
 * uart_shell.h — unified UART console on top of the `net_shell` shell.
 *
 * The shell/CLI must accept input from UART / TCP / USB, output over
 * UART + TCP. The TCP (telnet:23) and SSH (:22) transports ALREADY share the
 * same transport-agnostic interpreter `net_shell_exec()`. This module adds
 * the 3rd transport: the UART SERIAL CONSOLE.
 *
 * It relies on the interrupt-driven RX (lock-free SPSC ring fed by
 * IRQ 89 routed to Core2): bytes are read via `uart_getc()` (non-blocking),
 * a line is accumulated (with local echo + backspace), and on each
 * '\r'/'\n' the line is executed via `net_shell_exec()` with a UART output.
 *
 * Runs on Core2 (IO_SOFT), called in polling from the `net_task` loop
 * alongside lwIP/SSH. No blocking: `uart_shell_poll()` processes what is
 * available and returns.
 */
#ifndef RTOS_NET_UART_SHELL_H
#define RTOS_NET_UART_SHELL_H

/* Starts the UART shell console: displays the banner + the 1st `oros>`
 * prompt on the serial console. To be called once, before the polling loop. */
void uart_shell_start(void);

/* Processes bytes received on the UART (non-blocking). To be called in a
 * loop on Core2 (in `net_task`). Executes a command on each complete line. */
void uart_shell_poll(void);

/* Injects ONE character into the shell line editor, whatever its source
 * (UART RX ring, or USB-A HID keyboard through drivers/usb/kbd_service.c).
 * Same behaviour as a serial keystroke: local echo, backspace, Ctrl-C, and
 * execution on CR/LF. Non-blocking. */
void uart_shell_feed(char c);

/* Number of commands executed via the UART console (diagnostic/summary). */
unsigned uart_shell_commands(void);

#endif /* RTOS_NET_UART_SHELL_H */
