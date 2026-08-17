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
 * app_core2.c — Core2 (IO_SOFT): INFRASTRUCTURE supervisor. Nothing to code.
 *
 * Sequence:
 *   1. drivers_init()      : SD card + PERMANENT FAT mount, EHCI/OHCI + USB-A
 *                            keyboard service (hot-plug, works with or without
 *                            a keyboard plugged in at boot).
 *   2. keyboard sink       : each keystroke goes through app_on_key() then into
 *                            the shell line editor (same shell as UART/telnet/
 *                            SSH). No raw echo, no periodic noise.
 *   3. net_task_entry()    : PERMANENT network stack (xHCI + RTL8153B + lwIP +
 *                            telnet:23 + SSH:22 + UART shell), whose infinite
 *                            loop ALSO drains klog, the mailbox, polls the
 *                            keyboard and handles Ethernet cable hot-plug.
 *                            Never returns.
 *
 * If the network is unavailable (no xHCI / no RTL8153B, e.g. QEMU),
 * net_task_entry() falls back to a LOCAL supervisor loop (klog + mailbox +
 * UART shell + keyboard): the console and the keyboard keep working.
 */

#include <stdint.h>
#include <stddef.h>

#include "app.h"
#include "../kernel/config.h"
#include "../drivers/drivers_init.h"
#include "../drivers/usb/kbd_service.h"
#include "../net/uart_shell.h"
#include "../net/net_task.h"

/* ------------------------------------------------------------------ */
/* Optional application keyboard hook                                  */
/* ------------------------------------------------------------------ */
/*
 * app_on_key — called for each character typed on the USB-A keyboard.
 * Return 1 to CONSUME the character (the shell will not see it), 0 to let it
 * through to the shell line editor.
 *
 * TODO (optional): intercept function keys, an operator HMI, etc.
 */
int app_on_key(char c)
{
    (void)c;
    return 0;   /* let everything through to the shell */
}

/* Sink installed into kbd_service: application hook then shell. */
static void app_kbd_sink(char c)
{
    if (app_on_key(c))
        return;                /* consumed by the application */
    uart_shell_feed(c);        /* same line editor as the serial console */
}

/* ------------------------------------------------------------------ */
/* Core2 thread entry point (never returns)                            */
/* ------------------------------------------------------------------ */
void app_core2_entry(void *arg)
{
    (void)arg;

    /* 1) All the permanent drivers owned by Core2. */
    (void)drivers_init();

    /* 2) USB-A keyboard -> shell (through the application hook). */
    kbd_service_set_sink(app_kbd_sink);

    /* 3) Permanent services loop (network + shell + keyboard + logs).
     *    Never returns; degrades gracefully if the network is absent. */
    net_task_entry(NULL);
}
