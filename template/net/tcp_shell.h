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
 * tcp_shell.h — "telnet-like" TCP server for the remote shell.
 *
 * TCP server written with lwIP's RAW API (tcp_accept/tcp_recv/tcp_sent/
 * tcp_err callbacks), compatible with NO_SYS=1 (no lwIP threads/sockets).
 * Runs in Core2's (IO_SOFT) polling loop, alongside netif_r8152_poll()
 * and sys_check_timeouts().
 *
 * When a client connects (`telnet <ip_board> 23` or `nc <ip> 23`), the
 * server sends a banner + a prompt, then interprets each received line via
 * net_shell_exec(). Only one active session at a time (sufficient for
 * administration; additional connections are refused).
 *
 * Success on: from a PC, open a TCP session on the board's port, type
 * commands (help, net, ecat...) and receive the responses.
 */
#ifndef RTOS_NET_TCP_SHELL_H
#define RTOS_NET_TCP_SHELL_H

#include <stdint.h>

/* TCP listening port of the shell (23 = telnet). Configurable here. */
#ifndef TCP_SHELL_PORT
#define TCP_SHELL_PORT   23
#endif

/*
 * Starts the shell's TCP server (bind + listen on TCP_SHELL_PORT). To be
 * called once, after lwip_init() and enabling the netif. Returns 0 if OK,
 * <0 on failure (bind/listen). Non-blocking: the service is callback-driven
 * during sys_check_timeouts() / lwIP polling.
 */
int tcp_shell_start(void);

/* Returns the number of sessions accepted since startup (cumulative
 * counter, for the summary). */
uint32_t tcp_shell_sessions(void);

/* Returns the number of commands executed (across all sessions). */
uint32_t tcp_shell_commands(void);

#endif /* RTOS_NET_TCP_SHELL_H */
