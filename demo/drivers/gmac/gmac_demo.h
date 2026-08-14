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
 * gmac_demo.h — Demo: raw L2 GMAC (TX broadcast + RX polling).
 *
 * GMAC init (DWMAC1000 + YT8531C PHY), link-up wait, emission of a broadcast
 * test frame (custom EtherType), then an RX POLLING loop for a few seconds to
 * capture raw network traffic. No GMAC IRQ.
 *
 * Neutralized on QEMU (the driver returns GMAC_ENODEV).
 */
#ifndef RTOS_DRIVERS_GMAC_DEMO_H
#define RTOS_DRIVERS_GMAC_DEMO_H

/* Runs the demo (called from io_supervisor, Core2). */
void gmac_demo_run(void);

#endif /* RTOS_DRIVERS_GMAC_DEMO_H */
