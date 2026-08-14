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
 * class_hid.c — HID keyboard class driver (boot protocol)
 *
 * On top of usb_core (independent of the EHCI/OHCI HCD). Sequence:
 *   1. SET_PROTOCOL(boot=0)  (bmRT=0x21, bReq=0x0B, wIndex=iface)
 *   2. SET_IDLE(0)           (bmRT=0x21, bReq=0x0A)   — STALL tolerated
 *   3. INTERRUPT IN loop on the int EP (8-byte boot report) -> decoding.
 *
 * Boot report: [0]=modifiers, [1]=reserved, [2..7]=keycodes (up to 6).
 */

#include <stdio.h>
#include <string.h>

#include "class_hid.h"
#include "usb_core.h"

/* ------------------------------------------------------------------ */
/* USB HID keycode → ASCII table (boot protocol, US layout)            */
/* ------------------------------------------------------------------ */
/* Index = keycode (0x00..0x63). 0 = no printable character.           */
/* Table of 0x64 entries, filled explicitly by index to avoid any      */
/* shift error (C99 designated initializers).                          */
static const char hid_map[0x64] = {
    /* letters a..z: 0x04..0x1D */
    ['\x04']='a',['\x05']='b',['\x06']='c',['\x07']='d',['\x08']='e',
    ['\x09']='f',['\x0A']='g',['\x0B']='h',['\x0C']='i',['\x0D']='j',
    ['\x0E']='k',['\x0F']='l',['\x10']='m',['\x11']='n',['\x12']='o',
    ['\x13']='p',['\x14']='q',['\x15']='r',['\x16']='s',['\x17']='t',
    ['\x18']='u',['\x19']='v',['\x1A']='w',['\x1B']='x',['\x1C']='y',
    ['\x1D']='z',
    /* digits 1..0: 0x1E..0x27 */
    ['\x1E']='1',['\x1F']='2',['\x20']='3',['\x21']='4',['\x22']='5',
    ['\x23']='6',['\x24']='7',['\x25']='8',['\x26']='9',['\x27']='0',
    /* special keys */
    ['\x28']='\n',['\x29']=27,['\x2A']='\b',['\x2B']='\t',['\x2C']=' ',
    /* symbols 0x2D..0x38 */
    ['\x2D']='-',['\x2E']='=',['\x2F']='[',['\x30']=']',['\x31']='\\',
    ['\x33']=';',['\x34']='\'',['\x35']='`',['\x36']=',',['\x37']='.',
    ['\x38']='/',
    /* keypad (0x54..0x63) */
    ['\x54']='/',['\x55']='*',['\x56']='-',['\x57']='+',['\x58']='\n',
    ['\x59']='1',['\x5A']='2',['\x5B']='3',['\x5C']='4',['\x5D']='5',
    ['\x5E']='6',['\x5F']='7',['\x60']='8',['\x61']='9',['\x62']='0',
    ['\x63']='.',
};

/* Shift uppercase / symbols (US layout). */
static const char hid_map_shift[0x64] = {
    ['\x04']='A',['\x05']='B',['\x06']='C',['\x07']='D',['\x08']='E',
    ['\x09']='F',['\x0A']='G',['\x0B']='H',['\x0C']='I',['\x0D']='J',
    ['\x0E']='K',['\x0F']='L',['\x10']='M',['\x11']='N',['\x12']='O',
    ['\x13']='P',['\x14']='Q',['\x15']='R',['\x16']='S',['\x17']='T',
    ['\x18']='U',['\x19']='V',['\x1A']='W',['\x1B']='X',['\x1C']='Y',
    ['\x1D']='Z',
    ['\x1E']='!',['\x1F']='@',['\x20']='#',['\x21']='$',['\x22']='%',
    ['\x23']='^',['\x24']='&',['\x25']='*',['\x26']='(',['\x27']=')',
    ['\x28']='\n',['\x29']=27,['\x2A']='\b',['\x2B']='\t',['\x2C']=' ',
    ['\x2D']='_',['\x2E']='+',['\x2F']='{',['\x30']='}',['\x31']='|',
    ['\x33']=':',['\x34']='"',['\x35']='~',['\x36']='<',['\x37']='>',
    ['\x38']='?',
};


#define HID_MOD_LSHIFT  0x02
#define HID_MOD_RSHIFT  0x20

/* ------------------------------------------------------------------ */
/* Probe                                                                */
/* ------------------------------------------------------------------ */
usb_status_t hid_kbd_probe(hid_kbd_t *kbd, usb_device_t *dev)
{
    memset(kbd, 0, sizeof(*kbd));
    kbd->dev = dev;

    /* Check the interface class: HID / Boot / Keyboard. */
    if (dev->if_class != USB_CLASS_HID) {
        printf("[hid] interface class %02X (not HID) — aborting\n", dev->if_class);
        return USB_EINVAL;
    }
    if (dev->if_subclass != USB_HID_SUBCLASS_BOOT ||
        dev->if_protocol != USB_HID_PROTO_KEYBOARD) {
        printf("[hid] HID but not Boot/Keyboard (sub=%02X proto=%02X) — "
               "trying anyway\n", dev->if_subclass, dev->if_protocol);
    }
    kbd->iface = 0;   /* first interface (usb_core keeps alt 0) */

    /* Find the interrupt IN endpoint. */
    const usb_endpoint_t *ep = usb_find_endpoint(dev, USB_EP_XFER_INT, /*in=*/1);
    if (!ep) {
        printf("[hid] no interrupt IN endpoint — aborting\n");
        return USB_EINVAL;
    }
    kbd->ep_int_in = ep->address;
    kbd->ep_maxpkt = ep->max_packet ? ep->max_packet : 8;
    if (kbd->ep_maxpkt > 64) kbd->ep_maxpkt = 64;

    printf("[hid] keyboard : iface=%u ep int IN=0x%02X maxpkt=%u\n",
           kbd->iface, kbd->ep_int_in, kbd->ep_maxpkt);

    /* SET_PROTOCOL(boot=0). */
    usb_status_t st = usb_control(dev,
        USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        USB_HID_REQ_SET_PROTOCOL, USB_HID_PROTO_BOOT, kbd->iface, NULL, 0);
    if (st != USB_OK && st != USB_ESTALL)
        printf("[hid] SET_PROTOCOL failed (%d), continuing\n", (int)st);

    /* SET_IDLE(0): reports only on change. STALL tolerated. */
    st = usb_control(dev,
        USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        USB_HID_REQ_SET_IDLE, 0x0000, kbd->iface, NULL, 0);
    if (st != USB_OK && st != USB_ESTALL)
        printf("[hid] SET_IDLE failed (%d), continuing\n", (int)st);

    printf("[hid] keyboard configured (boot protocol).\n");
    return USB_OK;
}

/* Is a key already present in the previous report? */
static int in_prev(const hid_kbd_t *kbd, uint8_t key)
{
    for (int i = 0; i < 6; i++)
        if (kbd->prev[i] == key) return 1;
    return 0;
}

int hid_kbd_poll(hid_kbd_t *kbd, uint32_t timeout_ms)
{
    uint8_t report[8];
    uint32_t got = 0;

    memset(report, 0, sizeof(report));
    usb_status_t st = kbd->dev->hcd->int_in(kbd->dev, kbd->ep_int_in,
                                            report, 8, &got);
    if (st != USB_OK || got < 3)
        return -1;   /* NAK (nothing new) or error — normal when no key */

    uint8_t mods = report[0];
    int shift = (mods & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) ? 1 : 0;
    int n_new = 0;

    for (int i = 2; i < 8; i++) {
        uint8_t key = report[i];
        if (key == 0 || key == 1 /* rollover */)
            continue;
        if (in_prev(kbd, key))
            continue;   /* key already held → no repetition */
        if (key < 0x66) {
            char c = shift ? hid_map_shift[key] : hid_map[key];
            if (c) {
                putchar(c);
                fflush(stdout);
            } else {
                printf("[hid] keycode 0x%02X\n", key);
            }
        }
        n_new++;
    }

    /* Remember the current keycodes (positions 2..7). */
    for (int i = 0; i < 6; i++)
        kbd->prev[i] = report[i + 2];

    return n_new;
}
