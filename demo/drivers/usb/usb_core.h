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
 * usb_core.h — Common USB-core layer (enumeration + standard requests)
 *
 * Controller-independent: relies on the abstract HCD interface
 * (usb_hcd_ops_t, usb.h). Provides:
 *   - the standard USB requests (GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIG...);
 *   - the device enumeration sequence (port reset -> addressing ->
 *     reading descriptors -> endpoint parsing -> SET_CONFIGURATION);
 *   - a typed control-transfer helper.
 *
 */
#ifndef RTOS_DRIVERS_USB_CORE_H
#define RTOS_DRIVERS_USB_CORE_H

#include "usb.h"

/* Registers the active HCD (xhci or ehci) for this stack instance.
 * (Only one active HCD at a time, enough for the demo; the
 * usb_device structure carries its own hcd pointer anyway.) */
void usb_core_set_hcd(const usb_hcd_ops_t *hcd);

/* Generic control transfer (uses dev->hcd->control). */
usb_status_t usb_control(usb_device_t *dev, uint8_t bmRequestType,
                         uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                         void *data, uint16_t wLength);

/* Usual standard requests. */
usb_status_t usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                                void *buf, uint16_t len);
usb_status_t usb_set_address(usb_device_t *dev, uint8_t address);
usb_status_t usb_set_configuration(usb_device_t *dev, uint8_t config_value);

/*
 * usb_enumerate — enumerates THE device present on the given HCD's root port.
 *
 * Sequence (USB 2.0):
 *   1. hcd->port_reset() -> speed
 *   2. hcd->device_alloc() (xHCI slot / addressing)
 *   3. GET_DESCRIPTOR(device, 8) to learn bMaxPacketSize0
 *   4. SET_ADDRESS
 *   5. GET_DESCRIPTOR(device, full) -> VID/PID/class
 *   6. GET_DESCRIPTOR(config, full) -> parse interfaces + endpoints
 *   7. hcd->configure_eps() (optional) + SET_CONFIGURATION
 *
 * Fills *dev. Returns USB_OK if a device was enumerated.
 */
usb_status_t usb_enumerate(const usb_hcd_ops_t *hcd, usb_device_t *dev);

/* Finds an endpoint by type/direction in an enumerated device.
 * dir_in = 1 for IN, 0 for OUT. Returns NULL if absent. */
const usb_endpoint_t *usb_find_endpoint(const usb_device_t *dev,
                                        uint8_t type, int dir_in);

#endif /* RTOS_DRIVERS_USB_CORE_H */
