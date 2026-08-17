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
 * kbd_service.h — PERMANENT USB-A keyboard service (HOT-PLUG).
 *
 * Production service (Core2 / IO_SOFT), state machine polled from the
 * supervisor loop. Contract:
 *   - The EHCI + OHCI controllers are initialized ONCE at boot
 *     (drivers_init) — even if NO keyboard is plugged in.
 *   - If no keyboard is present, the service stays in SCAN state and retries
 *     an enumeration every KBD_SCAN_PERIOD_MS: a keyboard plugged in LATER is
 *     detected and becomes usable, with no reboot.
 *   - Once attached (READY), each keystroke is injected into the shell line
 *     editor (uart_shell_feed) exactly like a serial keystroke.
 *   - Unplug: repeated interrupt-IN failures + loss of the port CS/CCS bit
 *     -> back to SCAN (and re-detection on the next plug).
 *   - CONSOLE SILENCE: one single message per state TRANSITION
 *     (attached / removed), never a periodic message.
 */
#ifndef RTOS_DRIVERS_KBD_SERVICE_H
#define RTOS_DRIVERS_KBD_SERVICE_H

#include "usb.h"

/* Service states (readable for diagnostics). */
typedef enum {
    KBD_STATE_DISABLED = 0,   /* no USB2 controller (QEMU) — service inactive */
    KBD_STATE_SCAN,           /* no keyboard: periodic re-enumeration */
    KBD_STATE_READY,          /* keyboard attached: reports polled */
} kbd_state_t;

/* Re-enumeration period when no keyboard is present (ms). */
#define KBD_SCAN_PERIOD_MS   1000u

/*
 * kbd_service_init — initializes the USB2 controllers (EHCI then OHCI) and
 * arms the service. Returns USB_OK if at least one controller is up (the
 * service then works WITH OR WITHOUT a keyboard plugged in), USB_ENODEV if
 * there is no USB2 controller at all (QEMU) — in that case the service is
 * disabled and kbd_service_poll() is a no-op.
 * Emits ONE init status line.
 */
usb_status_t kbd_service_init(void);

/*
 * kbd_service_poll — advances the state machine (NON-BLOCKING, to be called
 * in the permanent Core2 loop):
 *   SCAN  : every KBD_SCAN_PERIOD_MS, tries EHCI then OHCI enumeration
 *           (a plug is detected here).
 *   READY : reads the boot reports; characters -> uart_shell_feed()
 *           (via the hid_kbd sink) ; detects the unplug.
 * Returns the number of keys processed during this call (0 most of the time).
 */
int kbd_service_poll(void);

/*
 * kbd_service_set_sink — installs the consumer of the decoded characters.
 * The application layer (app_core2.c) installs a sink that first calls
 * app_on_key() then uart_shell_feed(). If no sink is installed, the
 * characters are dropped (the driver stays autonomous and silent).
 */
void kbd_service_set_sink(void (*sink)(char c));

/* Current state (diagnostics / shell). */
kbd_state_t kbd_service_state(void);

/* Total number of keys injected into the shell since boot (diagnostics). */
unsigned kbd_service_keys(void);

#endif /* RTOS_DRIVERS_KBD_SERVICE_H */
