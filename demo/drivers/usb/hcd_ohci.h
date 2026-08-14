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
 * hcd_ohci.h — OHCI HCD (USB 1.1 Low/Full-Speed) for RK3328
 *
 * Target: usb_host0_ohci @0xFF5D0000 (standard OHCI, EHCI companion). On the
 * Orange Pi R1 Plus LTS, this controller drives a Low/Full-Speed USB KEYBOARD
 * plugged on the USB-A port (handed off by EHCI).
 *
 * OFFSETS/BITS: quoted from the OHCI 1.0 spec + u-boot drivers/usb/host/ohci.h
 * (never deduce an offset). Works in POLLING.
 *
 * On QEMU (-DMMU_QEMU): no controller → ohci_init() returns USB_ENODEV.
 */
#ifndef RTOS_DRIVERS_HCD_OHCI_H
#define RTOS_DRIVERS_HCD_OHCI_H

#include "usb.h"

/* MMIO base of the OHCI controller (device-tree usb_host0_ohci). */
#define OHCI_BASE            0xFF5D0000UL

/* HCD ops exported to usb_core. */
extern const usb_hcd_ops_t ohci_hcd_ops;

/* Initializes the OHCI controller. Returns USB_OK / USB_ENODEV. */
usb_status_t ohci_init(void);

#endif /* RTOS_DRIVERS_HCD_OHCI_H */
