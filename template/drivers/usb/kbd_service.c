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
 * kbd_service.c — PERMANENT USB-A keyboard service (HOT-PLUG), Core2.
 *
 * Non-blocking state machine (see kbd_service.h):
 *
 *   DISABLED <-- no USB2 controller (QEMU): poll() is a no-op.
 *
 *   SCAN --(enumeration OK + HID/Boot/Keyboard)--> READY
 *     Every KBD_SCAN_PERIOD_MS: a NON-BLOCKING port test
 *     (ehci_port_connected / ohci_port_connected) avoids paying for a
 *     reset when nothing is plugged in; if a device IS present, a full
 *     enumeration is performed (EHCI first, then the OHCI companion for
 *     a Low/Full-Speed keyboard, like the EHCI hand-over path).
 *
 *   READY --(int-IN failure streak AND port no longer connected)--> SCAN
 *     Reading the boot reports is NAK-tolerant (nothing typed = -1), so the
 *     unplug is confirmed by the PORT bit, not by the NAKs alone.
 *
 * CONSOLE DISCIPLINE (production): one message on ATTACH, one on REMOVE,
 * one at init. Never anything periodic. Keystrokes go to the sink (shell
 * line editor), never to a raw putchar.
 */

#include <stdio.h>
#include <string.h>

#include "kbd_service.h"
#include "usb_core.h"
#include "hcd_ehci.h"
#include "hcd_ohci.h"
#include "class_hid.h"
#include "../../arch/aarch64/timer.h"

/* ------------------------------------------------------------------ */
/* Service state                                                       */
/* ------------------------------------------------------------------ */
static kbd_state_t   s_state = KBD_STATE_DISABLED;
static int           s_ehci_up;          /* EHCI controller initialized */
static int           s_ohci_up;          /* OHCI controller initialized */
static usb_device_t  s_dev;              /* enumerated keyboard */
static hid_kbd_t     s_kbd;              /* HID boot-keyboard context */
static uint64_t      s_next_scan;        /* next re-enumeration instant */
static uint64_t      s_next_poll;        /* next report read (bInterval) */
static unsigned      s_keys;             /* keys injected since boot */
static unsigned      s_fail_streak;      /* consecutive int-IN failures */
static void        (*s_sink)(char c);    /* consumer of the characters */

/* Report read interval (ms): a boot keyboard uses bInterval ~8-10 ms. */
#define KBD_POLL_PERIOD_MS   8u
/* int-IN failures before checking the port (a NAK at rest is normal). */
#define KBD_FAIL_STREAK      64u

/* ------------------------------------------------------------------ */
/* Character sink (installed into class_hid)                           */
/* ------------------------------------------------------------------ */
static void kbd_sink_trampoline(char c)
{
    s_keys++;
    if (s_sink)
        s_sink(c);
}

void kbd_service_set_sink(void (*sink)(char c))
{
    s_sink = sink;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static uint64_t ms_from_now(uint32_t ms)
{
    return timer_now_ticks() + timer_us_to_ticks((uint64_t)ms * 1000ull);
}

/* Is a device electrically present on the USB-A port? (non-blocking) */
static int port_connected(void)
{
    if (s_ehci_up && ehci_port_connected())
        return 1;
    if (s_ohci_up && ohci_port_connected())
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Init: controllers up, WITH OR WITHOUT a keyboard plugged in         */
/* ------------------------------------------------------------------ */
usb_status_t kbd_service_init(void)
{
    usb_status_t st;

    s_state       = KBD_STATE_DISABLED;
    s_ehci_up     = 0;
    s_ohci_up     = 0;
    s_keys        = 0;
    s_fail_streak = 0;

    /* EHCI (High-Speed). A missing controller is not an error here. */
    st = ehci_init();
    if (st == USB_OK)
        s_ehci_up = 1;
    else if (st != USB_ENODEV)
        printf("[kbd] ERROR: ehci_init failed (code %d)\n", (int)st);

    /* OHCI (EHCI companion, Low/Full-Speed keyboards). */
    st = ohci_init();
    if (st == USB_OK)
        s_ohci_up = 1;
    else if (st != USB_ENODEV)
        printf("[kbd] ERROR: ohci_init failed (code %d)\n", (int)st);

    if (!s_ehci_up && !s_ohci_up) {
        printf("[kbd] no USB2 controller : keyboard service disabled.\n");
        return USB_ENODEV;
    }

    /* Route the decoded characters through our trampoline. */
    hid_kbd_set_sink(kbd_sink_trampoline);

    /* Armed WITHOUT a keyboard: hot-plug will be caught by the scan. */
    s_state     = KBD_STATE_SCAN;
    s_next_scan = timer_now_ticks();   /* first attempt immediately */
    printf("[kbd] USB-A keyboard service armed (ehci=%s ohci=%s, hot-plug)\n",
           s_ehci_up ? "up" : "-", s_ohci_up ? "up" : "-");
    return USB_OK;
}

/* ------------------------------------------------------------------ */
/* SCAN: try to enumerate + attach a keyboard (silent on failure)      */
/* ------------------------------------------------------------------ */
static int kbd_try_attach(void)
{
    const usb_hcd_ops_t *hcd = NULL;

    /* Nothing electrically connected: nothing to do (no reset, no log). */
    if (!port_connected())
        return 0;

    /* 1) EHCI first (High-Speed keyboard / HS hub). */
    if (s_ehci_up) {
        usb_core_set_hcd(&ehci_hcd_ops);
        if (usb_enumerate(&ehci_hcd_ops, &s_dev) == USB_OK)
            hcd = &ehci_hcd_ops;
    }

    /* 2) OHCI companion (Low/Full-Speed keyboard, the usual case). */
    if (!hcd && s_ohci_up) {
        usb_core_set_hcd(&ohci_hcd_ops);
        if (usb_enumerate(&ohci_hcd_ops, &s_dev) == USB_OK)
            hcd = &ohci_hcd_ops;
    }

    if (!hcd)
        return 0;   /* not enumerable yet (device settling): retry later */

    /* 3) HID boot keyboard? A non-HID device is ignored (no noise). */
    if (s_dev.if_class != USB_CLASS_HID)
        return 0;

    if (hid_kbd_probe(&s_kbd, &s_dev) != USB_OK) {
        printf("[kbd] ERROR: HID keyboard detected but init failed.\n");
        return 0;
    }

    s_state       = KBD_STATE_READY;
    s_fail_streak = 0;
    s_next_poll   = timer_now_ticks();
    printf("[kbd] keyboard CONNECTED on USB-A (%04X:%04X, %s) -> shell input\n",
           s_dev.dev_desc.idVendor, s_dev.dev_desc.idProduct,
           (hcd == &ehci_hcd_ops) ? "ehci" : "ohci");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Permanent poll (non-blocking, called from the Core2 loop)           */
/* ------------------------------------------------------------------ */
int kbd_service_poll(void)
{
    if (s_state == KBD_STATE_DISABLED)
        return 0;

    uint64_t now = timer_now_ticks();

    /* ---- SCAN: periodic re-enumeration (late plug detection) ---- */
    if (s_state == KBD_STATE_SCAN) {
        if (now < s_next_scan)
            return 0;
        s_next_scan = ms_from_now(KBD_SCAN_PERIOD_MS);
        (void)kbd_try_attach();
        return 0;
    }

    /* ---- READY: read the boot reports at the endpoint rate ---- */
    if (now < s_next_poll)
        return 0;
    s_next_poll = ms_from_now(KBD_POLL_PERIOD_MS);

    int n = hid_kbd_poll(&s_kbd, KBD_POLL_PERIOD_MS);
    if (n >= 0) {
        s_fail_streak = 0;
        return n;
    }

    /* n < 0: NAK (nothing typed) OR transfer error. A NAK is normal at rest,
     * so the UNPLUG is confirmed by the port bit after a failure streak. */
    if (++s_fail_streak >= KBD_FAIL_STREAK) {
        s_fail_streak = 0;
        if (!port_connected()) {
            if (s_dev.hcd && s_dev.hcd->device_free)
                s_dev.hcd->device_free(&s_dev);
            memset(&s_kbd, 0, sizeof(s_kbd));
            memset(&s_dev, 0, sizeof(s_dev));
            s_state     = KBD_STATE_SCAN;
            s_next_scan = ms_from_now(KBD_SCAN_PERIOD_MS);
            printf("[kbd] keyboard REMOVED from USB-A : waiting for a new one.\n");
        }
    }
    return 0;
}

kbd_state_t kbd_service_state(void) { return s_state; }
unsigned    kbd_service_keys(void)  { return s_keys; }
