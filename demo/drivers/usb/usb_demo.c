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
 * usb_demo.c — Demo (USB host xHCI + enumeration)
 *
 * Objective: initialize the xHCI/DWC3 controller (@0xFF600000),
 * reset the root port, enumerate THE present device and display its VID/PID
 * + class. On the Orange Pi R1 Plus LTS, the expected device is the internal
 * RTL8153B (0BDA:8153). On QEMU: xhci_init() returns USB_ENODEV -> demo ignored.
 *
 * Runs on Core 2 (IO_SOFT).
 */

#include <stdio.h>
#include <string.h>

#include "usb_demo.h"
#include "usb_core.h"
#include "hcd_xhci.h"
#include "class_r8152.h"
#include "hcd_ehci.h"
#include "hcd_ohci.h"
#include "class_hid.h"
#include "../../arch/aarch64/timer.h"



static const char *speed_str(usb_speed_t s)
{
    switch (s) {
    case USB_SPEED_LOW:   return "Low (1.5M)";
    case USB_SPEED_FULL:  return "Full (12M)";
    case USB_SPEED_HIGH:  return "High (480M)";
    case USB_SPEED_SUPER: return "Super (5G)";
    default:              return "?";
    }
}

/* ===================================================================== */
/* HID keyboard (EHCI/OHCI USB2)                                         */
/*                                                                       */
/* A Low/Full-Speed keyboard is driven by the OHCI (companion), a        */
/* High-Speed device by the EHCI.                                        */
/* We initialize EHCI then OHCI, enumerate the present device, and if    */
/* it is an HID keyboard we attach it and display the keystrokes ~10 s.  */
/* ===================================================================== */
static void usb_demo_hid(void)
{
    printf("\n===== DEMO USB2 EHCI/OHCI + keyboard HID =====\n");

    const usb_hcd_ops_t *hcd = NULL;
    usb_device_t kdev;
    usb_status_t st;
    int enumerated = 0;

    /* --- 1) EHCI first (High-Speed device). --- */
    st = ehci_init();
    if (st == USB_ENODEV) {
        printf("[usb] no EHCI controller (QEMU) : Demo ignored.\n");
        return;
    }
    if (st == USB_OK) {
        usb_core_set_hcd(&ehci_hcd_ops);
        st = usb_enumerate(&ehci_hcd_ops, &kdev);
        if (st == USB_OK) {
            hcd = &ehci_hcd_ops;
            enumerated = 1;
            printf("[usb] HS device enumerated by EHCI (VID:PID=%04X:%04X)\n",
                   kdev.dev_desc.idVendor, kdev.dev_desc.idProduct);
        } else if (ehci_port_ceded_to_companion()) {
            printf("[usb] Low/Full-Speed device: switch to OHCI...\n");
        } else if (st == USB_ENOTCONN) {
            printf("[usb] EHCI : no device on USB-A port.\n");
        } else {
            printf("[usb] EHCI : enumeration failed (code %d)\n", (int)st);
        }
    }

    /* --- 2) OHCI (companion, LS/FS keyboard). --- */
    if (!enumerated) {
        st = ohci_init();
        if (st == USB_OK) {
            usb_core_set_hcd(&ohci_hcd_ops);
            st = usb_enumerate(&ohci_hcd_ops, &kdev);
            if (st == USB_OK) {
                hcd = &ohci_hcd_ops;
                enumerated = 1;
                printf("[usb] LS/FS device enumerated by OHCI (VID:PID=%04X:%04X)\n",
                       kdev.dev_desc.idVendor, kdev.dev_desc.idProduct);
            } else if (st == USB_ENOTCONN) {
                printf("[usb] OHCI : no device on USB-A port.\n");
            } else {
                printf("[usb] OHCI : enumeration failed (code %d)\n", (int)st);
            }
        } else if (st != USB_ENODEV) {
            printf("[usb] OHCI init failed (code %d)\n", (int)st);
        }
    }

    if (!enumerated) {
        printf(">>> Demo: No USB2 device enumerated "
               "(connect a keyboard in USB-A port). <<<\n");
        return;
    }
    (void)hcd;

    /* --- 3) Display the device + attach HID if keyboard. --- */
    printf("[usb] class iface=%02X/%02X/%02X, %u endpoints\n",
           kdev.if_class, kdev.if_subclass, kdev.if_protocol,
           kdev.num_endpoints);
    for (uint8_t i = 0; i < kdev.num_endpoints; i++) {
        const usb_endpoint_t *ep = &kdev.endpoints[i];
        const char *tn = (ep->type == USB_EP_XFER_INT) ? "int" :
                         (ep->type == USB_EP_XFER_BULK) ? "bulk" : "ctrl";
        printf("    ep 0x%02X %-4s %s maxpkt=%u interval=%u\n",
               ep->address, tn, (ep->address & USB_EP_DIR_IN) ? "IN " : "OUT",
               ep->max_packet, ep->interval);
    }

    if (kdev.if_class != USB_CLASS_HID) {
        printf(">>> Demo : USB2 device enumerated (class %02X, not a "
               "HID keyboard). Enum OK. <<<\n", kdev.if_class);
        return;
    }

    hid_kbd_t kbd;
    st = hid_kbd_probe(&kbd, &kdev);
    if (st != USB_OK) {
        printf(">>> Demo : HID keyboard detected but init failed (%d). <<<\n",
               (int)st);
        return;
    }

    printf("\n[usb] Type on USB keyboard (~15 s) — the keys are displayed :\n");
    printf("----------------------------------------------------------\n");
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(15u * 1000000u);
    int total_keys = 0;
    while (timer_now_ticks() < deadline) {
        int n = hid_kbd_poll(&kbd, 50);
        if (n > 0) total_keys += n;
        /* small pause so as not to saturate the USB (bInterval ~ 8-10 ms). */
        uint64_t t = timer_now_ticks() + timer_us_to_ticks(8000);
        while (timer_now_ticks() < t) __asm__ volatile("nop");
    }
    printf("\n----------------------------------------------------------\n");
    printf(">>> Demo : HID keyboard  OK — %d key(s) detected. <<<\n",
           total_keys);
}

void usb_demo_run(void)
{
    printf("\n===== Demo USB host xHCI + enumeration =====\n");

    /* 1) Initialize the xHCI controller. */
    usb_status_t st = xhci_init();
    if (st == USB_ENODEV) {
        printf("[usb] No xHCI controller (QEMU) : demo ignored.\n");
        usb_demo_hid();   /* will try EHCI/OHCI (ENODEV on QEMU too) */
        return;
    }
    if (st != USB_OK) {
        printf("[usb] xHCI init failed (code %d)\n", (int)st);
        usb_demo_hid();
        return;
    }
    usb_core_set_hcd(&xhci_hcd_ops);


    /* 2) Enumerate the device present on the root port. */
    usb_device_t dev;
    st = usb_enumerate(&xhci_hcd_ops, &dev);
    if (st == USB_ENOTCONN) {
        printf("[usb] No device detected on root port.\n");
        printf(">>> Demo: xHCI up but nothing to enumerated. <<<\n");
        usb_demo_hid();
        return;
    }
    if (st != USB_OK) {
        printf("[usb] enumeration failed (code %d)\n", (int)st);
        printf(">>> Demo: enumeration error, see logs. <<<\n");
        usb_demo_hid();
        return;
    }


    /* 3) Display the result. */
    printf("\n[usb] DEVICE ENUMERATED :\n");
    printf("  Speed         : %s\n", speed_str(dev.speed));
    printf("  VID:PID       : %04X:%04X\n",
           dev.dev_desc.idVendor, dev.dev_desc.idProduct);
    printf("  bcdUSB        : %04X   bcdDevice : %04X\n",
           dev.dev_desc.bcdUSB, dev.dev_desc.bcdDevice);
    printf("  device class  : %02X/%02X/%02X\n",
           dev.dev_desc.bDeviceClass, dev.dev_desc.bDeviceSubClass,
           dev.dev_desc.bDeviceProtocol);
    printf("  iface class   : %02X/%02X/%02X\n",
           dev.if_class, dev.if_subclass, dev.if_protocol);
    printf(" enabled config : %u   endpoints : %u\n",
           dev.config_value, dev.num_endpoints);
    for (uint8_t i = 0; i < dev.num_endpoints; i++) {
        const usb_endpoint_t *ep = &dev.endpoints[i];
        const char *tn = (ep->type == USB_EP_XFER_BULK) ? "bulk" :
                         (ep->type == USB_EP_XFER_INT)  ? "int"  :
                         (ep->type == USB_EP_XFER_ISO)  ? "iso"  : "ctrl";
        printf("    ep 0x%02X  %-4s  %s  maxpkt=%u\n",
               ep->address, tn,
               (ep->address & USB_EP_DIR_IN) ? "IN " : "OUT",
               ep->max_packet);
    }

    /* 4) Result. */
    int is_rtl = (dev.dev_desc.idVendor == USB_VID_REALTEK &&
                  dev.dev_desc.idProduct == USB_PID_RTL8153);
    if (is_rtl)
        printf("\n>>> Demo : RTL8153B DETECTED (0BDA:8153). Enum OK. <<<\n");
    else
        printf("\n>>> Demo : device enumerated (VID/PID above). Enum OK. <<<\n");

    /* ================================================================= */
    /* RTL8153B driver (r8152) — MAC, link, frame TX/RX                  */
    /* ================================================================= */
    if (!is_rtl) {
        printf("[usb] none-RTL8153B device: Demo (r8152) not applicable.\n");
        usb_demo_hid();
        return;
    }


    printf("\n===== DEMO driver RTL8153B r8152: MAC + TX/RX) =====\n");

    /* a) Attach the r8152 driver: reads version + MAC, HW init, RE|TE. */
    r8152_dev_t rt;
    st = r8152_probe(&rt, &dev);
    if (st != USB_OK) {
        printf("[usb] r8152_probe failed (code %d)\n", (int)st);
        printf(">>> Demo : r8152 init failed, see logs. <<<\n");
        return;
    }

    /* b) Wait for the Ethernet link (cable plugged in). Non-blocking on
     *    failure: we can still prove the bulk TX chain. */
    st = r8152_link_wait(&rt, 5000);
    int link = (st == USB_OK);

    /* c) TX TEST: send a broadcast frame (ARP-like) to validate the
     *    bulk OUT chain + tx_desc. Destination FF:FF:FF:FF:FF:FF, source =
     *    our MAC, EtherType 0x0806 (ARP), minimal payload (filled with 0). */
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    memset(&frame[0], 0xFF, 6);                 /* dst broadcast */
    memcpy(&frame[6], rt.mac, 6);               /* src = our MAC */
    frame[12] = 0x08; frame[13] = 0x06;         /* EtherType ARP */
    /* Minimal ARP request (HTYPE=1, PTYPE=0x0800, HLEN=6, PLEN=4, OP=1). */
    frame[14] = 0x00; frame[15] = 0x01;         /* HTYPE Ethernet */
    frame[16] = 0x08; frame[17] = 0x00;         /* PTYPE IPv4 */
    frame[18] = 0x06; frame[19] = 0x04;         /* HLEN / PLEN */
    frame[20] = 0x00; frame[21] = 0x01;         /* OP = request */
    memcpy(&frame[22], rt.mac, 6);              /* sender HW addr */
    /* sender IP 0.0.0.0, target HW 0, target IP 0.0.0.0 (gratuitous probe). */

    st = r8152_send(&rt, frame, sizeof(frame));
    int tx_ok = (st == USB_OK);
    printf("[usb] TX ARP frame (%u o) : %s\n",
           (unsigned)sizeof(frame), tx_ok ? "OK (bulk OUT succeed)" : "FAILED");

    /* d) RX TEST: try to receive a frame (bulk IN). If the link is up, we
     *    often capture traffic (ARP/broadcast). Otherwise, timeout expected. */
    uint8_t rxbuf[1536];
    uint32_t rxlen = 0;
    int rx_ok = 0;
    for (int tries = 0; tries < 4 && !rx_ok; tries++) {
        st = r8152_recv(&rt, rxbuf, sizeof(rxbuf), &rxlen, 1000);
        if (st == USB_OK && rxlen > 0) {
            rx_ok = 1;
            printf("[usb] RX frame (%lu o) : "
                   "dst=%02X:%02X:%02X:%02X:%02X:%02X "
                   "src=%02X:%02X:%02X:%02X:%02X:%02X type=%02X%02X\n",
                   (unsigned long)rxlen,
                   rxbuf[0], rxbuf[1], rxbuf[2], rxbuf[3], rxbuf[4], rxbuf[5],
                   rxbuf[6], rxbuf[7], rxbuf[8], rxbuf[9], rxbuf[10], rxbuf[11],
                   rxbuf[12], rxbuf[13]);
        }
    }
    if (!rx_ok)
        printf("[usb] RX : no frame captured (link %s).\n",
               link ? "up but silenced" : "down");

    /* e) Result: Success if = MAC read + init OK + at least the bulk TX chain
     *    working (ideally link up + RX captured). */
    printf("\n>>> Demo: r8152 init OK, MAC readed, TX=%s, link=%s, RX=%s. <<<\n",
           tx_ok ? "OK" : "KO",
           link ? "UP" : "down",
           rx_ok ? "captured" : "none");

    /* ================================================================= */
    /* Demo: HID keyboard on USB2 (EHCI/OHCI, USB-A port).               */
    /* ================================================================= */
    usb_demo_hid();
}
