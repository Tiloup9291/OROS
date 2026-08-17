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
 * drivers_init.c — PERMANENT bring-up of the Core2-owned drivers.
 *
 * See drivers_init.h for the ownership map. Every failure is non-blocking:
 * the subsystem stays unavailable, the rest of the system keeps running
 * (degraded mode, one error line only).
 *
 * The USB-Ethernet chain (xHCI + RTL8153B + lwIP + telnet + SSH) is NOT
 * initialized here: it is owned by net_task_entry() which also runs the
 * permanent RX/timers/link-hot-plug loop.
 */

#include <stdio.h>

#include "drivers_init.h"
#include "usb/kbd_service.h"
#include "../fs/fs_mount.h"

static unsigned g_status;

unsigned drivers_init(void)
{
    unsigned ok = 0;

    printf("[drv] permanent driver bring-up...\n");

    /* --- micro-SD card + PERMANENT FAT mount (volume 0:) --- */
    if (fs_mount_init() == 0)
        ok |= DRV_OK_FS;

    /* --- USB-A: EHCI + OHCI controllers, HOT-PLUG keyboard service ---
     * Comes up EVEN IF no keyboard is plugged in: the service then scans
     * periodically and attaches a keyboard connected later. */
    if (kbd_service_init() == USB_OK)
        ok |= DRV_OK_KBD;

    g_status = ok;
    printf("[drv] ready : fs=%s keyboard=%s "
           "(usb-eth: net task, ethercat/gmac: Core0)\n",
           (ok & DRV_OK_FS)  ? "mounted"   : "unavailable",
           (ok & DRV_OK_KBD) ? "hot-plug"  : "unavailable");
    return ok;
}

unsigned drivers_status(void)
{
    return g_status;
}
