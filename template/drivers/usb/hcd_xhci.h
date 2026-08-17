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
 * hcd_xhci.h — xHCI / DWC3 HCD (USB 3.0) for RK3328
 *
 * Target: usbdrd3 @0xFF600000 (standard xHCI + DWC3 glue). On the Orange Pi R1
 * Plus LTS, this controller carries the RTL8153B (2nd Ethernet, device@2).
 *
 * OFFSETS/BITS: quoted from the Intel xHCI 1.x spec (standard, SoC-independent
 * registers) + DWC3 core (u-boot/Linux drivers/usb/dwc3/core.h for the G* glue
 * registers). Never deduce an offset.
 *
 * On QEMU (-DMMU_QEMU): no controller → xhci_init() returns USB_ENODEV.
 */
#ifndef RTOS_DRIVERS_HCD_XHCI_H
#define RTOS_DRIVERS_HCD_XHCI_H

#include "usb.h"

/* MMIO base of the xHCI/DWC3 controller (device-tree usbdrd3). */
#define XHCI_BASE            0xFF600000UL

/* The stack works in POLLING (no xHCI IRQ) to stay simple and
 * deterministic; the event ring is polled by the USB thread (Core2).
 * The IRQ could be added later (it is already routed to Core2 via the
 * gic_set_target loop of main.c). */


/* HCD ops exported to usb_core (xhci_ops.init, port_reset, ...). */
extern const usb_hcd_ops_t xhci_hcd_ops;

/* Shortcut: initializes the controller. Returns USB_OK / USB_ENODEV. */
usb_status_t xhci_init(void);

#endif /* RTOS_DRIVERS_HCD_XHCI_H */
