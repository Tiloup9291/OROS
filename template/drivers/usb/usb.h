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
 * usb.h — Common types of the USB host stack (RK3328)
 *
 * This layer is CONTROLLER-INDEPENDENT: it defines the standard USB protocol
 * structures (SETUP packet, descriptors) and the abstract interface of a HCD
 * (Host Controller Driver). The concrete HCDs (xHCI/DWC3 for USB3, EHCI/OHCI
 * for USB2) implement this interface; the usb_core layer (enumeration, standard
 * requests) and the class drivers (r8152, HID) are written ONCE on top of it.
 *
 * Board topology: proven by device-tree.
 *   - xHCI/DWC3 @0xFF600000  → RTL8153B (2nd Ethernet)
 *   - EHCI     @0xFF5C0000  → USB2 host (HID keyboard)
 *   - OHCI     @0xFF5D0000  → LS/FS fallback
 *
 * On QEMU 'virt' (-DMMU_QEMU) these RK3328 MMIOs do not exist: the HCDs
 * neutralize themselves (USB_ENODEV), like gpio/sdmmc.
 *
 * Independent of Linux: only standard USB definitions.
 */
#ifndef RTOS_DRIVERS_USB_H
#define RTOS_DRIVERS_USB_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Return codes of the USB stack                                      */
/* ------------------------------------------------------------------ */
typedef enum {
    USB_OK       = 0,
    USB_ENODEV   = -1,   /* no controller (QEMU) */
    USB_ENOTCONN = -2,   /* no device on the port */
    USB_ETIMEOUT = -3,   /* transfer timeout */
    USB_EIO      = -4,   /* transfer error (stall, babble, CRC) */
    USB_EINVAL   = -5,   /* invalid parameter */
    USB_ENOMEM   = -6,   /* no slot/resource left */
    USB_ESTALL   = -7,   /* endpoint stalled */
} usb_status_t;

/* ------------------------------------------------------------------ */
/* USB speeds                                                         */
/* ------------------------------------------------------------------ */
typedef enum {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW,       /* 1.5 Mbps */
    USB_SPEED_FULL,      /* 12 Mbps  */
    USB_SPEED_HIGH,      /* 480 Mbps */
    USB_SPEED_SUPER,     /* 5 Gbps   */
} usb_speed_t;

/* ------------------------------------------------------------------ */
/* SETUP packet (8 bytes) — USB 2.0                                   */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

/* bmRequestType — direction (bit 7) */
#define USB_DIR_OUT           0x00u   /* host -> device */
#define USB_DIR_IN            0x80u   /* device -> host */
/* bmRequestType — type (bits 6:5) */
#define USB_TYPE_STANDARD     0x00u
#define USB_TYPE_CLASS        0x20u
#define USB_TYPE_VENDOR       0x40u
/* bmRequestType — recipient (bits 4:0) */
#define USB_RECIP_DEVICE      0x00u
#define USB_RECIP_INTERFACE   0x01u
#define USB_RECIP_ENDPOINT    0x02u

/* bRequest — standard requests (USB 2.0) */
#define USB_REQ_GET_STATUS        0x00u
#define USB_REQ_CLEAR_FEATURE     0x01u
#define USB_REQ_SET_FEATURE       0x03u
#define USB_REQ_SET_ADDRESS       0x05u
#define USB_REQ_GET_DESCRIPTOR    0x06u
#define USB_REQ_SET_DESCRIPTOR    0x07u
#define USB_REQ_GET_CONFIGURATION 0x08u
#define USB_REQ_SET_CONFIGURATION 0x09u
#define USB_REQ_GET_INTERFACE     0x0Au
#define USB_REQ_SET_INTERFACE     0x0Bu

/* Descriptor types (wValue high byte of GET_DESCRIPTOR) */
#define USB_DT_DEVICE             0x01u
#define USB_DT_CONFIG             0x02u
#define USB_DT_STRING             0x03u
#define USB_DT_INTERFACE          0x04u
#define USB_DT_ENDPOINT           0x05u
#define USB_DT_HID                0x21u
#define USB_DT_HID_REPORT         0x22u

/* ------------------------------------------------------------------ */
/* Standard descriptors (USB 2.0)                                     */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;   /* bit7 = dir (IN if 1), bits3:0 = number */
    uint8_t  bmAttributes;       /* bits1:0 = type (00 ctrl,10 bulk,11 int) */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

/* bmAttributes — endpoint transfer type */
#define USB_EP_XFER_CONTROL   0x00u
#define USB_EP_XFER_ISO       0x01u
#define USB_EP_XFER_BULK      0x02u
#define USB_EP_XFER_INT       0x03u
#define USB_EP_XFER_MASK      0x03u
#define USB_EP_DIR_IN         0x80u

/* Common interface classes */
#define USB_CLASS_HID         0x03u
#define USB_CLASS_VENDOR      0xFFu

/* HID (boot protocol) */
#define USB_HID_SUBCLASS_BOOT 0x01u
#define USB_HID_PROTO_KEYBOARD 0x01u
#define USB_HID_REQ_SET_PROTOCOL 0x0Bu
#define USB_HID_REQ_SET_IDLE     0x0Au
#define USB_HID_PROTO_BOOT       0x00u

/* RTL8153B (device@2 "usbbda,8153") */
#define USB_VID_REALTEK       0x0BDAu
#define USB_PID_RTL8153       0x8153u

/* ------------------------------------------------------------------ */
/* Representation of an enumerated endpoint                           */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  address;        /* bEndpointAddress (dir + number) */
    uint8_t  type;           /* USB_EP_XFER_* */
    uint16_t max_packet;     /* wMaxPacketSize */
    uint8_t  interval;       /* bInterval (interrupt) */
} usb_endpoint_t;

#define USB_MAX_ENDPOINTS   8u

/* ------------------------------------------------------------------ */
/* Abstract HCD interface (implemented by hcd_xhci / hcd_ehci)        */
/* ------------------------------------------------------------------ */
struct usb_device;    /* forward */

typedef struct usb_hcd_ops {
    const char *name;

    /* Initializes the controller (reset, rings/queues, host mode). */
    usb_status_t (*init)(void);

    /* Root port reset + connection detection. Returns the detected speed
     * in *speed. USB_ENOTCONN if nothing is plugged in. */
    usb_status_t (*port_reset)(usb_speed_t *speed);

    /* Allocates the HCD resources for a freshly detected device
     * (xHCI slot / address). Must be called before any transfer. */
    usb_status_t (*device_alloc)(struct usb_device *dev);

    /* Releases a device's HCD resources. */
    void         (*device_free)(struct usb_device *dev);

    /* CONTROL transfer on endpoint 0 (SETUP + data + status).
     * 'data' may be NULL if setup->wLength == 0. */
    usb_status_t (*control)(struct usb_device *dev, const usb_setup_t *setup,
                            void *data, uint16_t len);

    /* BULK transfer (IN or OUT per bit7 of ep_addr). Returns the number
     * of bytes actually transferred in *xferred (may be NULL). */
    usb_status_t (*bulk)(struct usb_device *dev, uint8_t ep_addr,
                         void *buf, uint32_t len, uint32_t *xferred);

    /* INTERRUPT IN transfer (HID keyboard, RTL link). */
    usb_status_t (*int_in)(struct usb_device *dev, uint8_t ep_addr,
                           void *buf, uint32_t len, uint32_t *xferred);

    /* Informs the HCD of the endpoints to configure after SET_CONFIGURATION
     * (xHCI: Configure Endpoint; EHCI: allocate the QHs). Optional. */
    usb_status_t (*configure_eps)(struct usb_device *dev);
} usb_hcd_ops_t;

/* ------------------------------------------------------------------ */
/* Enumerated USB device (filled by usb_core)                         */
/* ------------------------------------------------------------------ */
typedef struct usb_device {
    const usb_hcd_ops_t *hcd;    /* HCD that drives this device */
    void        *hcd_priv;       /* HCD context (xHCI slot, EHCI QH...) */
    uint8_t      address;        /* assigned USB address (1..127) */
    usb_speed_t  speed;
    uint16_t     max_packet0;    /* max EP0 size (8/64/512) */

    usb_device_descriptor_t dev_desc;
    uint8_t      config_value;   /* active bConfigurationValue */

    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS];
    uint8_t        num_endpoints;

    /* Detected class (for driver dispatch) */
    uint8_t      if_class;
    uint8_t      if_subclass;
    uint8_t      if_protocol;
} usb_device_t;

#endif /* RTOS_DRIVERS_USB_H */
