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
 * usb_demo.h — USB host
 *
 * Called from io_supervisor (Core 2) after fs_demo_run().
 * xHCI init + enumeration of a device (expected: RTL8153B 0BDA:8153).
 */
#ifndef RTOS_DRIVERS_USB_DEMO_H
#define RTOS_DRIVERS_USB_DEMO_H

void usb_demo_run(void);

#endif /* RTOS_DRIVERS_USB_DEMO_H */
