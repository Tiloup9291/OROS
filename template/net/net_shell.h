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
 * net_shell.h — Remote shell command interpreter.
 *
 * Minimal "telnet-like" shell, transport-INDEPENDENT: it receives a line of
 * text (a command + arguments) and writes its response via an output
 * callback provided by the caller (TCP server; reusable over UART).
 *
 * Basic commands + EtherCAT DIAGNOSTIC commands, read from
 * the shared ecat_diag snapshot (published by the EtherCAT partition).
 *
 * Note: adding a command = edit the static table in net_shell.c +
 * recompile.
 */
#ifndef RTOS_NET_SHELL_H
#define RTOS_NET_SHELL_H

#include <stddef.h>

/*
 * Shell output function: writes `len` bytes of `data` to the transport
 * (TCP session, UART...). `ctx` is an opaque context passed through as-is.
 */
typedef void (*shell_out_fn)(void *ctx, const char *data, size_t len);

/*
 * Executes a command line (`line`, NUL-terminated, without CR/LF).
 * Writes the result via `out(ctx, ...)`. Returns 1 if the session must
 * close (`quit`/`exit` commands), 0 otherwise.
 */
int net_shell_exec(const char *line, shell_out_fn out, void *ctx);

/*
 * Writes the welcome banner + a first prompt via `out`. To be called when
 * opening a session.
 */
void net_shell_welcome(shell_out_fn out, void *ctx);

/* Writes the command prompt ("oros> ") via `out`. */
void net_shell_prompt(shell_out_fn out, void *ctx);

#endif /* RTOS_NET_SHELL_H */
