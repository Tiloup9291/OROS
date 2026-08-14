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
 * ssh_server.h — SSH server (wolfSSH + wolfCrypt) over lwIP raw TCP.
 *
 * "Single session" SSH server written with lwIP's RAW API (tcp_accept/recv/
 * sent/err/poll callbacks), compatible with NO_SYS=1 (no lwIP threads/
 * sockets). It coexists with the telnet shell: SSH on port 22,
 * telnet on port 23. Runs in Core2's (IO_SOFT) polling loop.
 *
 * Chain: the PC client runs `ssh oros@<ip_board>` -> SSH handshake (curve25519
 * KEX, ed25519 host key), password authentication (oros/oros), opening a
 * shell channel -> each typed line is passed to net_shell_exec() (the SAME
 * interpreter as telnet: help/net/stats/ecat/uptime/echo/quit). Encrypted
 * output is returned over the SSH channel.
 *
 * Succes on: from a PC, `ssh oros@<ip_board>`, authenticate
 * (password oros), get the `oros>` prompt and execute commands.
 */
#ifndef RTOS_NET_SSH_SERVER_H
#define RTOS_NET_SSH_SERVER_H

#include <stdint.h>

/* SSH TCP listening port (22 = ssh). */
#ifndef SSH_SERVER_PORT
#define SSH_SERVER_PORT   22
#endif

/* Authentication credentials (password). Configurable here. */
#ifndef SSH_USER
#define SSH_USER   "oros"
#endif
#ifndef SSH_PASS
#define SSH_PASS   "oros"
#endif

/*
 * Initializes wolfSSL/wolfSSH + loads the host key + creates the SSH
 * context, then starts the TCP server (bind + listen on SSH_SERVER_PORT).
 * To be called once, after lwip_init() and enabling the netif. Returns 0 if
 * OK, <0 otherwise. Non-blocking: the service is callback-driven +
 * ssh_server_poll().
 */
int ssh_server_start(void);

/*
 * Advances the SSH state machine of the current session (handshake or data
 * exchange). To be called regularly from Core2's main-loop (alongside
 * netif_r8152_poll() and sys_check_timeouts()). Non-blocking.
 */
void ssh_server_poll(void);

/* Returns 1 if an SSH session is in progress (handshake or shell
 * established), 0 otherwise. Used by the main-loop to stay very reactive
 * (no delay) while a client is connected. */
int ssh_server_session_active(void);

/* Counters for the summary/DoD. */
uint32_t ssh_server_sessions(void);   /* accepted TCP sessions            */
uint32_t ssh_server_auth_ok(void);    /* successful authentications       */
uint32_t ssh_server_commands(void);   /* executed shell commands          */


#endif /* RTOS_NET_SSH_SERVER_H */
