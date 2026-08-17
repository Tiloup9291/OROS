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
 * class_r8152.h — RTL8153B USB-Ethernet class driver
 *
 * Drives the RTL8153B (VID 0BDA:PID 8153) enumerated on the RK3328 xHCI/DWC3
 * (device@2). It is a VENDOR device (not standard
 * CDC-ECM): its internal registers (PLA/USB pages, OCP MII registers) are
 * accessed through vendor CONTROL transfers (bRequest 0x05, type read=0xC0 /
 * write=0x40, wValue=index, wIndex=type|byte-enable). Ethernet frames travel
 * through 2 bulk endpoints (IN 0x81 / OUT 0x02) prefixed with a Realtek
 * descriptor (tx_desc / rx_desc), + 1 interrupt IN endpoint (0x83) for the
 * link state.
 *
 * The driver is CONTROLLER-INDEPENDENT: it only uses the usb_core / usb_hcd_ops
 * layer (control + bulk). Algorithm sources: u-boot
 * drivers/usb/eth/r8152.c + r8152.h (offsets/bits/sequences QUOTED, not
 * ported as-is).
 *
 * On QEMU: the device does not exist → the caller (usb_demo) never reaches it.
 */
#ifndef RTOS_DRIVERS_CLASS_R8152_H
#define RTOS_DRIVERS_CLASS_R8152_H

#include "usb.h"

/* Size of a MAC address. */
#define R8152_MAC_LEN   6u

/* r8152 driver context. */
typedef struct {
    usb_device_t *dev;              /* enumerated USB device (RTL8153B) */
    uint8_t  ep_in;                 /* bulk IN  (data, e.g. 0x81) */
    uint8_t  ep_out;                /* bulk OUT (data, e.g. 0x02) */
    uint8_t  ep_int;                /* interrupt IN (link, e.g. 0x83) */
    uint8_t  mac[R8152_MAC_LEN];    /* read MAC address (PLA_IDR) */
    uint8_t  version;               /* RTL version (RTL_VER_08/09 = 8153B) */
    uint8_t  link_up;               /* 1 if the Ethernet link is up */
    uint16_t ocp_base;             /* current OCP base (cache) */
} r8152_dev_t;

/*
 * r8152_probe — attaches the driver to a freshly enumerated RTL8153B device.
 * Reads the version + MAC, runs the init sequence (r8153b_init + first_init),
 * configures RX/TX and enables RE|TE. Does NOT block on the link.
 * Returns USB_OK if the hardware init succeeded.
 */
usb_status_t r8152_probe(r8152_dev_t *rt, usb_device_t *dev);

/*
 * r8152_link_wait — polls the link state (PLA_PHYSTATUS) until link-up or
 * timeout (ms). Updates rt->link_up. Returns USB_OK if the link is up.
 */
usb_status_t r8152_link_wait(r8152_dev_t *rt, uint32_t timeout_ms);

/*
 * r8152_link_status — NON-BLOCKING single read of the link state
 * (PLA_PHYSTATUS / LINK_STATUS). Updates rt->link_up and returns 1 (cable
 * plugged, link up) or 0 (link down). Never waits, never logs: designed to be
 * polled in the permanent Core2 loop for Ethernet cable HOT-PLUG.
 */
int r8152_link_status(r8152_dev_t *rt);

/*
 * r8152_send — transmits a raw Ethernet frame (tx_desc prefix + bulk OUT).
 * 'frame'/'len' = Ethernet frame (dst+src+ethertype+payload), without CRC.
 */
usb_status_t r8152_send(r8152_dev_t *rt, const void *frame, uint32_t len);

/*
 * r8152_recv — receives a frame (bulk IN). On success, *out_len = length of the
 * useful Ethernet frame (rx_desc decoded, CRC removed) and copies into 'buf'
 * (capacity 'buf_cap'). Returns USB_OK if a frame was received,
 * USB_ETIMEOUT if nothing within the delay.
 */
usb_status_t r8152_recv(r8152_dev_t *rt, void *buf, uint32_t buf_cap,
                        uint32_t *out_len, uint32_t timeout_ms);

#endif /* RTOS_DRIVERS_CLASS_R8152_H */
