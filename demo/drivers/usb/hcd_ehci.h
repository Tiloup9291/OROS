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
 * hcd_ehci.h — EHCI HCD (USB 2.0 High-Speed) for RK3328
 *
 * Target: usb_host0_ehci @0xFF5C0000 (standard EHCI). On the Orange Pi R1 Plus
 * LTS, this controller drives the USB-A port (HID keyboard) — BUT a Low/Full-
 * Speed keyboard is handed off to the OHCI companion.
 * EHCI remains useful for a High-Speed device (HS hub, HS mouse/keyboard).
 *
 * OFFSETS/BITS: quoted from the EHCI 1.0 spec + u-boot drivers/usb/host/ehci.h
 * (never deduce an offset). Works in POLLING.
 *
 * On QEMU (-DMMU_QEMU): no controller -> ehci_init() returns USB_ENODEV.
 */
#ifndef RTOS_DRIVERS_HCD_EHCI_H
#define RTOS_DRIVERS_HCD_EHCI_H

#include "usb.h"

/* MMIO base of the EHCI controller (device-tree usb_host0_ehci). */
#define EHCI_BASE            0xFF5C0000UL

/* HCD ops exported to usb_core. */
extern const usb_hcd_ops_t ehci_hcd_ops;

/* Initializes the EHCI controller. Returns USB_OK / USB_ENODEV. */
usb_status_t ehci_init(void);

/* Returns 1 if the last port_reset HANDED the device over to the OHCI
 * companion (Low/Full-Speed device): in that case, the caller must enumerate
 * through OHCI. */
int ehci_port_ceded_to_companion(void);

#endif /* RTOS_DRIVERS_HCD_EHCI_H */
