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
 * class_hid.h — HID keyboard class driver (boot protocol)
 *
 * Controller-independent: relies on usb_core (control transfers + interrupt
 * IN) on top of a HCD (EHCI or OHCI). Boot keyboard sequence:
 *   SET_PROTOCOL(boot) -> SET_IDLE(0) -> INTERRUPT IN loop (8-B boot report).
 *
 */
#ifndef RTOS_DRIVERS_CLASS_HID_H
#define RTOS_DRIVERS_CLASS_HID_H

#include "usb.h"

typedef struct {
    usb_device_t *dev;
    uint8_t       ep_int_in;    /* address of the interrupt IN endpoint */
    uint16_t      ep_maxpkt;    /* size of the boot report (>= 8) */
    uint8_t       iface;        /* HID interface number */
    uint8_t       prev[6];      /* keycodes of the previous report (anti-repeat) */
} hid_kbd_t;

/* Attaches the HID driver to a device enumerated as a boot keyboard.
 * Returns USB_OK if the HID/Boot/Keyboard interface is detected + configured. */
usb_status_t hid_kbd_probe(hid_kbd_t *kbd, usb_device_t *dev);

/*
 * Sink of the decoded characters. Set by the consumer (kbd_service ->
 * shell line editor). If NULL, the characters are DROPPED (production mode:
 * no unsolicited console output).
 */
typedef void (*hid_kbd_sink_fn)(char c);

/* Installs the character sink (NULL = drop). */
void hid_kbd_set_sink(hid_kbd_sink_fn sink);

/* Reads a boot report (INTERRUPT IN). Decodes the newly pressed keys and
 * passes them to the sink installed by hid_kbd_set_sink().
 * Returns the number of new keys, or -1 if no report is available (NAK =
 * no key pressed) or on a transfer error (used by kbd_service to detect an
 * unplug). timeout_ms: maximum wait for a report. */
int hid_kbd_poll(hid_kbd_t *kbd, uint32_t timeout_ms);

#endif /* RTOS_DRIVERS_CLASS_HID_H */
