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
 * usb_core.c — Common USB-core layer (enumeration + standard requests)
 *
 * Controller-independent: every transfer operation goes through
 * dev->hcd->{control,bulk,int_in}. usb_core.h / usb.h .
 */

#include <string.h>
#include <stdio.h>

#include "usb_core.h"

/* Active HCD (informational; dev->hcd is authoritative for transfers). */
static const usb_hcd_ops_t *g_active_hcd;

void usb_core_set_hcd(const usb_hcd_ops_t *hcd)
{
    g_active_hcd = hcd;
}

/* ------------------------------------------------------------------ */
/* Typed control transfer                                              */
/* ------------------------------------------------------------------ */
usb_status_t usb_control(usb_device_t *dev, uint8_t bmRequestType,
                         uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                         void *data, uint16_t wLength)
{
    if (!dev || !dev->hcd || !dev->hcd->control)
        return USB_EINVAL;

    usb_setup_t setup;
    setup.bmRequestType = bmRequestType;
    setup.bRequest      = bRequest;
    setup.wValue        = wValue;
    setup.wIndex        = wIndex;
    setup.wLength       = wLength;

    return dev->hcd->control(dev, &setup, data, wLength);
}

/* ------------------------------------------------------------------ */
/* Standard requests                                                   */
/* ------------------------------------------------------------------ */
usb_status_t usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index,
                                void *buf, uint16_t len)
{
    /* wValue = (type << 8) | index ; wIndex = 0 (langid 0 for string 0). */
    return usb_control(dev,
                       USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                       USB_REQ_GET_DESCRIPTOR,
                       (uint16_t)((type << 8) | index), 0,
                       buf, len);
}

usb_status_t usb_set_address(usb_device_t *dev, uint8_t address)
{
    usb_status_t st = usb_control(dev,
                       USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                       USB_REQ_SET_ADDRESS, address, 0, NULL, 0);
    if (st == USB_OK)
        dev->address = address;
    return st;
}

usb_status_t usb_set_configuration(usb_device_t *dev, uint8_t config_value)
{
    usb_status_t st = usb_control(dev,
                       USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                       USB_REQ_SET_CONFIGURATION, config_value, 0, NULL, 0);
    if (st == USB_OK)
        dev->config_value = config_value;
    return st;
}

/* ------------------------------------------------------------------ */
/* Configuration parsing (interfaces + endpoints)                     */
/* ------------------------------------------------------------------ */
static void parse_config(usb_device_t *dev, const uint8_t *cfg, uint16_t total)
{
    dev->num_endpoints = 0;
    dev->if_class = dev->if_subclass = dev->if_protocol = 0;

    uint16_t off = 0;
    int have_iface = 0;
    while (off + 2u <= total) {
        uint8_t blen = cfg[off];
        uint8_t btype = cfg[off + 1];
        if (blen == 0)
            break;                       /* anti-loop protection */
        if (off + blen > total)
            break;

        if (btype == USB_DT_INTERFACE && blen >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t *ifd =
                (const usb_interface_descriptor_t *)&cfg[off];
            /* We keep the FIRST interface (alt 0). */
            if (!have_iface || ifd->bAlternateSetting == 0) {
                dev->if_class    = ifd->bInterfaceClass;
                dev->if_subclass = ifd->bInterfaceSubClass;
                dev->if_protocol = ifd->bInterfaceProtocol;
                have_iface = 1;
            }
        } else if (btype == USB_DT_ENDPOINT &&
                   blen >= sizeof(usb_endpoint_descriptor_t)) {
            const usb_endpoint_descriptor_t *epd =
                (const usb_endpoint_descriptor_t *)&cfg[off];
            if (dev->num_endpoints < USB_MAX_ENDPOINTS) {
                usb_endpoint_t *ep = &dev->endpoints[dev->num_endpoints++];
                ep->address    = epd->bEndpointAddress;
                ep->type       = epd->bmAttributes & USB_EP_XFER_MASK;
                ep->max_packet = epd->wMaxPacketSize;
                ep->interval   = epd->bInterval;
            }
        }
        off += blen;
    }
}

const usb_endpoint_t *usb_find_endpoint(const usb_device_t *dev,
                                        uint8_t type, int dir_in)
{
    for (uint8_t i = 0; i < dev->num_endpoints; i++) {
        const usb_endpoint_t *ep = &dev->endpoints[i];
        int is_in = (ep->address & USB_EP_DIR_IN) ? 1 : 0;
        if (ep->type == type && is_in == (dir_in ? 1 : 0))
            return ep;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                        */
/* ------------------------------------------------------------------ */
usb_status_t usb_enumerate(const usb_hcd_ops_t *hcd, usb_device_t *dev)
{
    static uint8_t cfg_buf[512] __attribute__((aligned(64)));
    usb_status_t st;

    if (!hcd)
        return USB_EINVAL;

    memset(dev, 0, sizeof(*dev));
    dev->hcd = hcd;
    dev->max_packet0 = 8;           /* lower bound until we know */

    /* 1) root port reset + connection detection + speed. */
    st = hcd->port_reset(&dev->speed);
    if (st != USB_OK)
        return st;                  /* USB_ENOTCONN if nothing plugged in */

    /* Default EP0 size by speed (SS=512, HS=64, FS=8/64, LS=8). */
    switch (dev->speed) {
    case USB_SPEED_SUPER: dev->max_packet0 = 512; break;
    case USB_SPEED_HIGH:  dev->max_packet0 = 64;  break;
    case USB_SPEED_FULL:  dev->max_packet0 = 64;  break;
    default:              dev->max_packet0 = 8;   break;
    }

    /* 2) HCD resource allocation (xHCI slot / hardware addressing). */
    if (hcd->device_alloc) {
        st = hcd->device_alloc(dev);
        if (st != USB_OK)
            return st;
    }

    /* 3) GET_DESCRIPTOR(device, first 8 bytes) → bMaxPacketSize0.
     *    (On USB2 the real EP0 may differ from the default; on xHCI
     *     SuperSpeed it is always 512, we keep it.) */
    usb_device_descriptor_t dd;
    memset(&dd, 0, sizeof(dd));
    st = usb_get_descriptor(dev, USB_DT_DEVICE, 0, &dd, 8);
    if (st != USB_OK)
        return st;
    if (dd.bMaxPacketSize0 && dev->speed != USB_SPEED_SUPER)
        dev->max_packet0 = dd.bMaxPacketSize0;

    /* 4) SET_ADDRESS (fixed address 1: only one device on this root port).
     *    On xHCI, the address is handled by the Address Device command in
     *    device_alloc; the standard SET_ADDRESS remains harmless/already done —
     *    the HCD may treat it as a no-op. */
    st = usb_set_address(dev, 1);
    if (st != USB_OK && st != USB_ESTALL)
        return st;
    dev->address = 1;

    /* 5) GET_DESCRIPTOR(device, full) → VID/PID/class. */
    st = usb_get_descriptor(dev, USB_DT_DEVICE, 0, &dev->dev_desc,
                            sizeof(dev->dev_desc));
    if (st != USB_OK)
        return st;

    /* 6) GET_DESCRIPTOR(config): first 9 bytes for wTotalLength,
     *    then the full config. */
    usb_config_descriptor_t cd;
    st = usb_get_descriptor(dev, USB_DT_CONFIG, 0, &cd, sizeof(cd));
    if (st != USB_OK)
        return st;

    uint16_t total = cd.wTotalLength;
    if (total > sizeof(cfg_buf))
        total = sizeof(cfg_buf);
    st = usb_get_descriptor(dev, USB_DT_CONFIG, 0, cfg_buf, total);
    if (st != USB_OK)
        return st;
    parse_config(dev, cfg_buf, total);

    /* 7) configure the endpoints on the HCD side (xHCI Configure Endpoint)
     *    then SET_CONFIGURATION. */
    if (hcd->configure_eps) {
        st = hcd->configure_eps(dev);
        if (st != USB_OK)
            return st;
    }
    st = usb_set_configuration(dev, cd.bConfigurationValue);
    if (st != USB_OK)
        return st;

    return USB_OK;
}
