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
 * uart_shell.c — unified UART console on top of the `net_shell` shell.
 *
 * 3rd shell transport (after telnet:23 and SSH:22), wired to the SAME
 * transport-agnostic interpreter `net_shell_exec()` (UART/TCP/USB
 * input, UART+TCP output). Non-blocking read from the interrupt-driven RX
 * ring (IRQ 89 routed to Core2), line accumulation with local echo
 * + backspace, execution on each CR/LF. Runs on Core2, called in polling
 * from `net_task` alongside lwIP/SSH.
 */
#include "uart_shell.h"
#include "net_shell.h"
#include "../drivers/uart/uart.h"

#include <stddef.h>

/* Max length of a command line on the UART console. */
#define UART_SHELL_LINE_MAX  128u

static char     s_line[UART_SHELL_LINE_MAX];
static unsigned s_len;
static unsigned s_commands;
static int      s_active;      /* 1 once the banner has been displayed */

/* Shell output callback: writes to the UART console.
 * `net_shell` produces '\n' line endings; uart_puts converts '\n'->'\r\n'
 * but operates on NULL-terminated strings; so we write byte by byte
 * (uart_putc also handles blocking TX on a full FIFO). */
static void uart_shell_out(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n')
            uart_putc('\r');    /* CRLF for a serial terminal */
        uart_putc(c);
    }
}

void uart_shell_start(void)
{
    s_len = 0;
    s_commands = 0;
    /* Banner + 1st prompt on the serial console. */
    uart_puts("\r\n");
    net_shell_welcome(uart_shell_out, NULL);
    s_active = 1;
}

/* Injects ONE character into the line editor (transport-agnostic: UART RX
 * ring or USB-A HID keyboard through kbd_service). Non-blocking. */
void uart_shell_feed(char c)
{
    if (!s_active)
        return;

    if (c == '\r' || c == '\n') {
        /* End of line: execute the accumulated command. */
        uart_putc('\r');
        uart_putc('\n');
        s_line[s_len] = '\0';
        if (s_len > 0) {
            (void)net_shell_exec(s_line, uart_shell_out, NULL);
            s_commands++;
            /* quit/exit does not "close" the serial console (no
             * session): we simply redisplay the prompt (the shell has
             * written "bye."). */
        }
        s_len = 0;
        net_shell_prompt(uart_shell_out, NULL);
    } else if (c == 0x7f || c == 0x08) {
        /* DEL / Backspace: erase the last character (visual echo). */
        if (s_len > 0) {
            s_len--;
            uart_putc('\b');
            uart_putc(' ');
            uart_putc('\b');
        }
    } else if (c == 0x03) {
        /* Ctrl-C: cancel the current line. */
        uart_puts("^C\r\n");
        s_len = 0;
        net_shell_prompt(uart_shell_out, NULL);
    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
        /* Printable character: add it to the line + local echo. */
        if (s_len < UART_SHELL_LINE_MAX - 1u) {
            s_line[s_len++] = c;
            uart_putc(c);
        }
        /* Full line: ignore the following bytes until CR/LF. */
    }
    /* Other control bytes: ignored. */
}

void uart_shell_poll(void)
{
    if (!s_active)
        return;

    char c;
    /* Process everything available in the UART RX ring (non-blocking). */
    while (uart_getc(&c))
        uart_shell_feed(c);
}

unsigned uart_shell_commands(void)
{
    return s_commands;
}
