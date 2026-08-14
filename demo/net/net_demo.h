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
 * net_demo.h — lwIP IP stack over the USB-Ethernet RTL8153B.
 *
 * Enumerates the RTL8153B (xHCI/DWC3), attaches the r8152 driver, brings up a
 * lwIP netif (static IP), then runs the stack (RX polling + timers) to
 * respond to ping (ICMP echo) and ARP from a PC on the network.
 *
 * Runs on Core 2 (IO_SOFT). Neutralized on QEMU (no xHCI).
 */
#ifndef RTOS_NET_DEMO_H
#define RTOS_NET_DEMO_H

/* Runs the network demo (blocks for ~the test window then returns). */
void net_demo_run(void);

#endif /* RTOS_NET_DEMO_H */
