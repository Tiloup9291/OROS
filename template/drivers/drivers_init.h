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
 * drivers_init.h — PERMANENT bring-up of ALL the production drivers.
 *
 * Called once from Core2 (IO_SOFT) at the very beginning of app_core2_entry(),
 * BEFORE the permanent service loop. Order and ownership:
 *
 *   GPIO   : done by kmain (needed very early)  — always available
 *   UART   : done by kmain (interrupt-driven RX, IRQ routed to Core2)
 *   SDMMC  : here, through fs_mount_init()      — card + PERMANENT FAT mount
 *   USB-A  : here, kbd_service_init()           — EHCI + OHCI, HOT-PLUG keyboard
 *   USB-Eth: net_task_entry() (later)           — xHCI + RTL8153B + lwIP
 *   GMAC   : ecat_task_entry() on Core0         — DWMAC1000 + YT8531C (EtherCAT)
 *
 * PRINCIPLE: a driver that fails NEVER blocks the others (degraded mode). One
 * single status line per driver at init, one line per error, and NOTHING
 * periodic afterwards (silent production console).
 */
#ifndef OROS_DRIVERS_INIT_H
#define OROS_DRIVERS_INIT_H

/* Bitmask of the successfully initialized subsystems. */
#define DRV_OK_FS      (1u << 0)   /* micro-SD card + FAT volume 0: mounted */
#define DRV_OK_KBD     (1u << 1)   /* USB2 controllers up (keyboard hot-plug) */

/*
 * drivers_init — initializes the permanent I/O drivers owned by Core2.
 * Returns the bitmask of the subsystems that came up (DRV_OK_*). Never fails
 * globally: what is missing simply stays unavailable.
 */
unsigned drivers_init(void);

/* Last bitmask returned by drivers_init() (diagnostics). */
unsigned drivers_status(void);

#endif /* OROS_DRIVERS_INIT_H */
