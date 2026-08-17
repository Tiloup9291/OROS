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
 * hcd_ehci.c — EHCI HCD (USB 2.0 High-Speed) for RK3328
 *
 * Implements the usb_hcd_ops_t interface (usb.h) on top of the standard EHCI
 * controller @0xFF5C0000 (usb_host0_ehci). Works in POLLING.
 *
 * SOURCES (offsets/bits QUOTED, not deduced):
 *   - EHCI 1.0 spec (Enhanced Host Controller Interface): Capability/Operational
 *     registers, PORTSC, QH (Queue Head), qTD (Queue Element Transfer Descriptor).
 *   - u-boot drivers/usb/host/ehci-hcd.c / ehci.h (init sequence, async, port reset).
 *   - Notes: docs/USB_EHCI_OHCI_NOTES.md §1.
 *
 * A plugged Low/Full-Speed keyboard is HANDED OFF to the OHCI companion
 * (PORTSC.PO=1). This HCD therefore enumerates mostly High-Speed devices; the
 * OHCI (hcd_ohci.c) takes over for LS/FS.
 *
 * On QEMU (-DMMU_QEMU): MMIO absent -> ehci_init() returns USB_ENODEV.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "hcd_ehci.h"
#include "usb_core.h"
#include "../../arch/aarch64/timer.h"

/* ================================================================== */
/* QEMU neutralization                                                 */
/* ================================================================== */
#if defined(MMU_QEMU)

usb_status_t ehci_init(void) { return USB_ENODEV; }
int ehci_port_ceded_to_companion(void) { return 0; }
int ehci_port_connected(void) { return 0; }

static usb_status_t q_init(void) { return USB_ENODEV; }
static usb_status_t q_port_reset(usb_speed_t *s) { (void)s; return USB_ENODEV; }
static usb_status_t q_dev_alloc(usb_device_t *d) { (void)d; return USB_ENODEV; }
static void         q_dev_free(usb_device_t *d)  { (void)d; }
static usb_status_t q_control(usb_device_t *d, const usb_setup_t *s,
                              void *b, uint16_t l)
{ (void)d; (void)s; (void)b; (void)l; return USB_ENODEV; }
static usb_status_t q_bulk(usb_device_t *d, uint8_t e, void *b, uint32_t l,
                           uint32_t *x)
{ (void)d; (void)e; (void)b; (void)l; (void)x; return USB_ENODEV; }
static usb_status_t q_int_in(usb_device_t *d, uint8_t e, void *b, uint32_t l,
                             uint32_t *x)
{ (void)d; (void)e; (void)b; (void)l; (void)x; return USB_ENODEV; }

const usb_hcd_ops_t ehci_hcd_ops = {
    .name = "ehci(qemu-stub)",
    .init = q_init, .port_reset = q_port_reset,
    .device_alloc = q_dev_alloc, .device_free = q_dev_free,
    .control = q_control, .bulk = q_bulk, .int_in = q_int_in,
    .configure_eps = NULL,
};

#else /* ========================= BOARD RK3328 =========================== */

/* ------------------------------------------------------------------ */
/* 32-bit MMIO access                                                  */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void dmb(void) { __asm__ volatile("dmb ish" ::: "memory"); }

/* DMA cache coherence (WB Normal RAM, MMU active — hcd_xhci.c). */
static inline void cache_clean(const void *addr, uint32_t size)
{
    uintptr_t p = (uintptr_t)addr & ~63UL, end = (uintptr_t)addr + size;
    for (; p < end; p += 64) __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static inline void cache_invalidate(void *addr, uint32_t size)
{
    uintptr_t p = (uintptr_t)addr & ~63UL, end = (uintptr_t)addr + size;
    for (; p < end; p += 64) __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(uint32_t us)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks(us);
    while (timer_now_ticks() < end) __asm__ volatile("nop");
}

/* ------------------------------------------------------------------ */
/* USB2 host PHY (shared) — take the host-port out of suspend         */
/* (u2phy_host @0xFF450104).                                          */
/* ------------------------------------------------------------------ */
#define USB2PHY_GRF_BASE     0xFF450000UL
#define U2PHY_HOST_SUS       (USB2PHY_GRF_BASE + 0x104)
static void usb2phy_host_init(void)
{
    wr32(U2PHY_HOST_SUS, (0xFFFFu << 16) | 0x0000u);   /* disable suspend */
    dsb();
    udelay(2000);
}

/* ------------------------------------------------------------------ */
/* EHCI registers (spec 1.0)                                           */
/* ------------------------------------------------------------------ */
/* Capability (base = EHCI_BASE) */
#define EHCI_CAPLENGTH        0x00   /* [7:0] length ; [31:16] HCIVERSION */
#define EHCI_HCSPARAMS        0x04   /* [3:0] N_PORTS ; bit4 PPC */
#define EHCI_HCCPARAMS        0x08   /* bit0 64-bit */

/* Operational (base = EHCI_BASE + CAPLENGTH) */
#define EHCI_OP_USBCMD        0x00
#define EHCI_OP_USBSTS        0x04
#define EHCI_OP_USBINTR       0x08
#define EHCI_OP_FRINDEX       0x0C
#define EHCI_OP_CTRLDSSEG     0x10
#define EHCI_OP_PERIODICBASE  0x14
#define EHCI_OP_ASYNCLISTADDR 0x18
#define EHCI_OP_CONFIGFLAG    0x40
#define EHCI_OP_PORTSC(p)     (0x44 + 4u * (p))

/* USBCMD bits */
#define CMD_RUN      (1u << 0)
#define CMD_HCRESET  (1u << 1)
#define CMD_PSE      (1u << 4)
#define CMD_ASE      (1u << 5)
#define CMD_IAAD     (1u << 6)
#define CMD_LRESET   (1u << 7)
/* USBSTS bits */
#define STS_HALT     (1u << 12)
/* CONFIGFLAG */
#define FLAG_CF      (1u << 0)
/* PORTSC bits */
#define PS_CS        (1u << 0)
#define PS_CSC       (1u << 1)
#define PS_PE        (1u << 2)
#define PS_PEC       (1u << 3)
#define PS_OCC       (1u << 5)
#define PS_PR        (1u << 8)
#define PS_LS_MASK   (3u << 10)
#define PS_PP        (1u << 12)
#define PS_PO        (1u << 13)   /* Port Owner (hand off to the companion) */
#define PS_CLEAR     (PS_OCC | PS_PEC | PS_CSC)
#define PS_IS_LOWSPEED(v) (((v) & PS_LS_MASK) == (1u << 10))

/* ------------------------------------------------------------------ */
/* qTD / QH (EHCI spec)                                               */
/* ------------------------------------------------------------------ */
#define QT_TERMINATE   1u
typedef struct __attribute__((packed, aligned(32))) {
    uint32_t qt_next;
    uint32_t qt_altnext;
    uint32_t qt_token;
    uint32_t qt_buffer[5];
    uint32_t qt_buffer_hi[5];
    uint32_t pad[3];               /* 32-B alignment */
} ehci_qtd_t;

/* qt_token bits */
#define QT_DT(x)         (((x) & 1u) << 31)
#define QT_TOTALBYTES(x) (((x) & 0x7FFFu) << 16)
#define QT_GET_TOTALBYTES(t) (((t) >> 16) & 0x7FFFu)
#define QT_IOC           (1u << 15)
#define QT_CERR(x)       (((x) & 3u) << 10)
#define QT_PID(x)        (((x) & 3u) << 8)
#define QT_PID_OUT       0u
#define QT_PID_IN        1u
#define QT_PID_SETUP     2u
#define QT_STATUS_ACTIVE 0x80u
#define QT_STATUS_HALTED 0x40u
#define QT_GET_STATUS(t) ((t) & 0xFFu)

typedef struct __attribute__((packed, aligned(32))) {
    uint32_t qh_link;
    uint32_t qh_endpt1;
    uint32_t qh_endpt2;
    uint32_t qh_curtd;
    /* overlay qTD */
    uint32_t qt_next;
    uint32_t qt_altnext;
    uint32_t qt_token;
    uint32_t qt_buffer[5];
    uint32_t qt_buffer_hi[5];
    uint32_t pad[2];
} ehci_qh_t;

#define QH_LINK_TERMINATE 1u
#define QH_LINK_TYPE_QH   (2u << 1)
/* qh_endpt1 */
#define QH_RL(x)      (((x) & 0xFu) << 28)
#define QH_C(x)       (((x) & 1u) << 27)
#define QH_MAXPKT(x)  (((x) & 0x7FFu) << 16)
#define QH_H(x)       (((x) & 1u) << 15)
#define QH_DTC(x)     (((x) & 1u) << 14)
#define QH_EPS(x)     (((x) & 3u) << 12)   /* 0=FS 1=LS 2=HS */
#define QH_ENDPT(x)   (((x) & 0xFu) << 8)
#define QH_DEVADDR(x) (((x) & 0x7Fu) << 0)
/* qh_endpt2 */
#define QH_MULT(x)    (((x) & 3u) << 30)

/* ------------------------------------------------------------------ */
/* Global state (one controller, one device)                          */
/* ------------------------------------------------------------------ */
static uintptr_t g_op;
static uint32_t  g_nports;
static int       g_ceded;      /* did the last port_reset hand over to the companion? */

/* Static aligned DMA structures. */
static ehci_qh_t  g_async_qh   __attribute__((aligned(64)));  /* reclaim head */
static ehci_qh_t  g_periodic_qh __attribute__((aligned(64)));
static uint32_t   g_periodic_list[1024] __attribute__((aligned(4096)));
static ehci_qh_t  g_qh         __attribute__((aligned(64)));  /* working QH */
static ehci_qtd_t g_qtd[4]     __attribute__((aligned(64)));  /* SETUP/DATA/ACK */
/* Page-aligned bounce buffer for control/int data (avoids spanning 4 KiB). */
static uint8_t    g_xfer_buf[4096] __attribute__((aligned(4096)));

/* Toggle per device (control EP0: reset at each SETUP). Interrupt: persistent. */
static uint8_t    g_toggle_int;

/* USB address of the device (0 before SET_ADDRESS). */
static uint8_t    g_dev_addr;

/* ------------------------------------------------------------------ */
/* Fills a data/setup qTD with page buffers.                            */
/* ------------------------------------------------------------------ */
static void qtd_set_buffer(ehci_qtd_t *td, void *data, uint32_t len)
{
    memset(td->qt_buffer, 0, sizeof(td->qt_buffer));
    memset(td->qt_buffer_hi, 0, sizeof(td->qt_buffer_hi));
    if (len == 0 || !data)
        return;
    uintptr_t b = (uintptr_t)data;
    td->qt_buffer[0] = (uint32_t)b;
    for (int i = 1; i < 5; i++)
        td->qt_buffer[i] = (uint32_t)((b + i * 0x1000u) & ~0xFFFu);
}

/* Waits for the end of a TD (status !ACTIVE). Returns 0 OK, -1 timeout, -2 halted. */
static int qtd_wait(ehci_qtd_t *last, uint32_t timeout_ms)
{
    uint64_t end = timer_now_ticks() +
                   timer_us_to_ticks((uint64_t)timeout_ms * 1000u);
    for (;;) {
        cache_invalidate(last, sizeof(*last));
        uint32_t tok = last->qt_token;
        dmb();
        if (!(QT_GET_STATUS(tok) & QT_STATUS_ACTIVE)) {
            if (QT_GET_STATUS(tok) & QT_STATUS_HALTED)
                return -2;
            return 0;
        }
        if (timer_now_ticks() >= end)
            return -1;
    }
}

/* ------------------------------------------------------------------ */
/* HCD interface                                                        */
/* ------------------------------------------------------------------ */

static usb_status_t ehci_hc_init(void)
{
    /* Read CAPLENGTH to find the Operational regs + detect absence. */
    uint32_t caplen_ver = rd32(EHCI_BASE + EHCI_CAPLENGTH);
    if (caplen_ver == 0xFFFFFFFFu || (caplen_ver & 0xFF) == 0)
        return USB_ENODEV;

    usb2phy_host_init();

    uint8_t caplen = caplen_ver & 0xFF;
    g_op = EHCI_BASE + caplen;
    uint32_t hcs = rd32(EHCI_BASE + EHCI_HCSPARAMS);
    g_nports = hcs & 0xF;
    if (g_nports == 0) g_nports = 1;

    /* 1) HC reset: USBCMD = (USBCMD & ~RUN) | RESET; wait for RESET=0. */
    uint32_t cmd = rd32(g_op + EHCI_OP_USBCMD);
    cmd = (cmd & ~CMD_RUN) | CMD_HCRESET;
    wr32(g_op + EHCI_OP_USBCMD, cmd);
    int ok = 0;
    for (int i = 0; i < 250; i++) {
        if (!(rd32(g_op + EHCI_OP_USBCMD) & CMD_HCRESET)) { ok = 1; break; }
        udelay(1000);
    }
    if (!ok) {
        printf("[ehci] HC reset timeout\n");
        return USB_ETIMEOUT;
    }

    /* 64-bit segment = 0 if supported. */
    if (rd32(EHCI_BASE + EHCI_HCCPARAMS) & 1u)
        wr32(g_op + EHCI_OP_CTRLDSSEG, 0);

    /* 2) Async reclaim head (circular QH list, H=1, HS, HALTED). */
    memset(&g_async_qh, 0, sizeof(g_async_qh));
    g_async_qh.qh_link  = (uint32_t)(uintptr_t)&g_async_qh | QH_LINK_TYPE_QH;
    g_async_qh.qh_endpt1 = QH_H(1) | QH_EPS(2 /*HS*/);
    g_async_qh.qt_next   = QT_TERMINATE;
    g_async_qh.qt_altnext = QT_TERMINATE;
    g_async_qh.qt_token  = QT_STATUS_HALTED;
    cache_clean(&g_async_qh, sizeof(g_async_qh));
    wr32(g_op + EHCI_OP_ASYNCLISTADDR, (uint32_t)(uintptr_t)&g_async_qh);

    /* 3) Periodic list: parent QH + 1024 entries pointing to it. */
    memset(&g_periodic_qh, 0, sizeof(g_periodic_qh));
    g_periodic_qh.qh_link   = QH_LINK_TERMINATE;
    g_periodic_qh.qt_next   = QT_TERMINATE;
    g_periodic_qh.qt_altnext = QT_TERMINATE;
    cache_clean(&g_periodic_qh, sizeof(g_periodic_qh));
    for (int i = 0; i < 1024; i++)
        g_periodic_list[i] = (uint32_t)(uintptr_t)&g_periodic_qh | QH_LINK_TYPE_QH;
    cache_clean(g_periodic_list, sizeof(g_periodic_list));
    wr32(g_op + EHCI_OP_PERIODICBASE, (uint32_t)(uintptr_t)g_periodic_list);
    dsb();

    /* 4) Start + CONFIGFLAG (take control of the ports). */
    cmd = rd32(g_op + EHCI_OP_USBCMD);
    cmd &= ~(CMD_LRESET | CMD_IAAD | CMD_PSE | CMD_ASE | CMD_HCRESET);
    cmd |= CMD_RUN;
    wr32(g_op + EHCI_OP_USBCMD, cmd);
    wr32(g_op + EHCI_OP_CONFIGFLAG, rd32(g_op + EHCI_OP_CONFIGFLAG) | FLAG_CF);
    udelay(5000);

    /* Power the ports (PP=1 if PPC). */
    if (hcs & (1u << 4)) {
        for (uint32_t p = 0; p < g_nports; p++) {
            uintptr_t pr = g_op + EHCI_OP_PORTSC(p);
            uint32_t sc = rd32(pr) & ~PS_CLEAR;
            wr32(pr, sc | PS_PP);
        }
        udelay(20000);
    }
    printf("[ehci] HC up : ports=%lu op=+0x%lx (HCIVERSION=0x%04lx)\n",
           (unsigned long)g_nports, (unsigned long)caplen,
           (unsigned long)(caplen_ver >> 16));
    for (uint32_t p = 0; p < g_nports; p++)
        printf("[ehci] PORTSC[%lu]=0x%08lx\n", (unsigned long)p,
               (unsigned long)rd32(g_op + EHCI_OP_PORTSC(p)));
    return USB_OK;
}

/* Detects a device on a port, resets it, returns the speed. If LS/FS →
 * hands over to the OHCI companion (PORTSC.PO=1) and returns USB_ENOTCONN
 * + the g_ceded flag. */
static usb_status_t ehci_port_reset(usb_speed_t *speed)
{
    g_ceded = 0;
    g_dev_addr = 0;

    int found = -1;
    for (int tries = 0; tries < 100 && found < 0; tries++) {
        for (uint32_t p = 0; p < g_nports; p++) {
            if (rd32(g_op + EHCI_OP_PORTSC(p)) & PS_CS) { found = (int)p; break; }
        }
        if (found < 0) udelay(10000);
    }
    if (found < 0)
        return USB_ENOTCONN;

    uintptr_t pr = g_op + EHCI_OP_PORTSC((uint32_t)found);
    uint32_t sc = rd32(pr);
    printf("[ehci] port connection %d PORTSC=0x%08lx\n", found, (unsigned long)sc);

    /* Low-Speed detected BEFORE reset → hand over directly to the companion. */
    if (PS_IS_LOWSPEED(sc)) {
        wr32(pr, (sc & ~PS_CLEAR) | PS_PO);
        dsb();
        g_ceded = 1;
        printf("[ehci] Low-Speed device -> ceding to OHCI companion\n");
        return USB_ENOTCONN;
    }

    /* Reset: PR=1, clear PE, wait ~50 ms, PR=0, wait for termination. */
    sc = rd32(pr) & ~PS_CLEAR;
    wr32(pr, (sc & ~PS_PE) | PS_PR);
    dsb();
    udelay(50000);
    sc = rd32(pr) & ~PS_CLEAR;
    wr32(pr, sc & ~PS_PR);
    dsb();
    for (int i = 0; i < 100; i++) {
        if (!(rd32(pr) & PS_PR)) break;
        udelay(200);
    }
    udelay(2000);

    sc = rd32(pr);
    /* After reset: if the port is NOT enabled but connected → FS device →
     * hand over to the companion. */
    if ((sc & (PS_PE | PS_CS)) == PS_CS) {
        wr32(pr, (sc & ~PS_CLEAR) | PS_PO);
        dsb();
        g_ceded = 1;
        printf("[ehci] Full-Speed device -> ceding to OHCI companion\n");
        return USB_ENOTCONN;
    }
    if (!(sc & PS_PE))
        return USB_EIO;

    *speed = USB_SPEED_HIGH;
    printf("[ehci] port %d reset OK, device High-Speed (PORTSC=0x%08lx)\n",
           found, (unsigned long)sc);
    return USB_OK;
}

int ehci_port_ceded_to_companion(void) { return g_ceded; }

/* NON-BLOCKING hot-plug probe: PORTSC.CS of every root port, no reset, no
 * log (called in a permanent polling loop). */
int ehci_port_connected(void)
{
    if (!g_op || !g_nports)
        return 0;
    for (uint32_t p = 0; p < g_nports; p++)
        if (rd32(g_op + EHCI_OP_PORTSC(p)) & PS_CS)
            return 1;
    return 0;
}

static usb_status_t ehci_device_alloc(usb_device_t *dev)
{
    /* EHCI: no hardware slot, addressing is done in software (SET_ADDRESS).
     * We remember the current address = 0 (default) until SET_ADDRESS. */
    g_dev_addr = 0;
    dev->hcd_priv = (void *)0;
    return USB_OK;
}

static void ehci_device_free(usb_device_t *dev) { (void)dev; }

/* Builds a QH for EP0 (control) or a given endpoint, runs the qTD chain,
 * waits for completion. dir_setup non-NULL = SETUP phase (control). */
static usb_status_t ehci_do_transfer(usb_device_t *dev, uint8_t ep_addr,
                                     uint8_t eps /*0FS 1LS 2HS*/, int is_control,
                                     const usb_setup_t *setup,
                                     void *data, uint32_t len, int dir_in,
                                     uint8_t *toggle, uint32_t *xferred,
                                     uint32_t timeout_ms)
{
    int ntd = 0;
    ehci_qtd_t *first, *last;
    uint32_t maxpkt = dev->max_packet0;
    uint8_t ep_num = ep_addr & 0x0F;

    if (!is_control) {
        /* endpoint data: real maxpkt */
        for (uint8_t i = 0; i < dev->num_endpoints; i++)
            if (dev->endpoints[i].address == ep_addr)
                maxpkt = dev->endpoints[i].max_packet;
    }

    memset(g_qtd, 0, sizeof(g_qtd));

    /* qTD chain. */
    if (is_control) {
        /* SETUP */
        ehci_qtd_t *s = &g_qtd[ntd++];
        memcpy(g_xfer_buf, setup, 8);
        cache_clean(g_xfer_buf, 8);
        s->qt_token = QT_DT(0) | QT_TOTALBYTES(8) | QT_CERR(3) |
                      QT_PID(QT_PID_SETUP) | QT_STATUS_ACTIVE;
        qtd_set_buffer(s, g_xfer_buf, 8);

        /* DATA (optional) */
        if (len && data) {
            ehci_qtd_t *d = &g_qtd[ntd];
            if (dir_in) {
                /* IN: the HC writes into g_xfer_buf */
            } else {
                memcpy(g_xfer_buf + 64, data, len);
                cache_clean(g_xfer_buf + 64, len);
            }
            d->qt_token = QT_DT(1) | QT_TOTALBYTES(len) | QT_CERR(3) |
                          QT_PID(dir_in ? QT_PID_IN : QT_PID_OUT) |
                          QT_STATUS_ACTIVE;
            qtd_set_buffer(d, g_xfer_buf + 64, len);
            g_qtd[ntd - 1].qt_next = (uint32_t)(uintptr_t)d;
            ntd++;
        }

        /* STATUS (opposite dir, len 0, IOC) */
        ehci_qtd_t *st = &g_qtd[ntd];
        st->qt_token = QT_DT(1) | QT_TOTALBYTES(0) | QT_CERR(3) | QT_IOC |
                       QT_PID((len && dir_in) ? QT_PID_OUT : QT_PID_IN) |
                       QT_STATUS_ACTIVE;
        st->qt_next = QT_TERMINATE;
        st->qt_altnext = QT_TERMINATE;
        g_qtd[ntd - 1].qt_next = (uint32_t)(uintptr_t)st;
        ntd++;
    } else {
        /* Bulk/interrupt transfer: 1 data qTD. */
        ehci_qtd_t *d = &g_qtd[ntd++];
        if (!dir_in && data) {
            memcpy(g_xfer_buf, data, len);
            cache_clean(g_xfer_buf, len);
        }
        d->qt_token = QT_DT(toggle ? *toggle : 0) | QT_TOTALBYTES(len) |
                      QT_CERR(3) | QT_IOC |
                      QT_PID(dir_in ? QT_PID_IN : QT_PID_OUT) | QT_STATUS_ACTIVE;
        qtd_set_buffer(d, g_xfer_buf, len);
        d->qt_next = QT_TERMINATE;
        d->qt_altnext = QT_TERMINATE;
    }

    /* Terminate the chains. */
    first = &g_qtd[0];
    last = &g_qtd[ntd - 1];
    for (int i = 0; i < ntd; i++) {
        if (g_qtd[i].qt_next == 0) g_qtd[i].qt_next = QT_TERMINATE;
        if (g_qtd[i].qt_altnext == 0) g_qtd[i].qt_altnext = QT_TERMINATE;
    }

    /* Working QH. */
    memset(&g_qh, 0, sizeof(g_qh));
    g_qh.qh_link = (uint32_t)(uintptr_t)&g_async_qh | QH_LINK_TYPE_QH;
    uint8_t c = (eps != 2 && ep_num == 0) ? 1 : 0;  /* LS/FS control via TT */
    g_qh.qh_endpt1 = QH_RL(8) | QH_C(c) | QH_MAXPKT(maxpkt) | QH_DTC(1) |
                     QH_EPS(eps) | QH_ENDPT(ep_num) | QH_DEVADDR(g_dev_addr);
    g_qh.qh_endpt2 = QH_MULT(1);
    g_qh.qt_next = (uint32_t)(uintptr_t)first;
    g_qh.qt_altnext = QT_TERMINATE;
    g_qh.qt_token = 0;   /* free overlay (not HALTED) */

    /* Publish everything in RAM. */
    cache_clean(g_qtd, sizeof(g_qtd));
    cache_clean(&g_qh, sizeof(g_qh));

    /* Insert the QH into the async list (async_qh -> g_qh -> async_qh). */
    g_async_qh.qh_link = (uint32_t)(uintptr_t)&g_qh | QH_LINK_TYPE_QH;
    cache_clean(&g_async_qh, sizeof(g_async_qh));
    dsb();

    /* Enable the async schedule. */
    wr32(g_op + EHCI_OP_USBCMD, rd32(g_op + EHCI_OP_USBCMD) | CMD_ASE);
    dsb();

    /* Wait for the last qTD. */
    int r = qtd_wait(last, timeout_ms);

    /* Remove our QH from the list (re-link async_qh to itself). */
    g_async_qh.qh_link = (uint32_t)(uintptr_t)&g_async_qh | QH_LINK_TYPE_QH;
    cache_clean(&g_async_qh, sizeof(g_async_qh));
    dsb();

    if (r == -1) return USB_ETIMEOUT;
    if (r == -2) return USB_ESTALL;

    /* Data IN: copy back from the bounce buffer. */
    uint32_t done = len;
    if (is_control) {
        cache_invalidate(g_qtd, sizeof(g_qtd));
        if (len && dir_in && data) {
            uint32_t rem = QT_GET_TOTALBYTES(g_qtd[1].qt_token);
            done = (len > rem) ? (len - rem) : 0;
            cache_invalidate(g_xfer_buf + 64, len);
            memcpy(data, g_xfer_buf + 64, done);
        }
    } else {
        cache_invalidate(last, sizeof(*last));
        uint32_t rem = QT_GET_TOTALBYTES(last->qt_token);
        done = (len > rem) ? (len - rem) : 0;
        if (dir_in && data) {
            cache_invalidate(g_xfer_buf, len);
            memcpy(data, g_xfer_buf, done);
        }
        if (toggle) *toggle ^= 1;   /* 1 TD packet -> simple toggle */
    }
    if (xferred) *xferred = done;
    return USB_OK;
}

static usb_status_t ehci_control(usb_device_t *dev, const usb_setup_t *setup,
                                 void *data, uint16_t len)
{
    int in = (setup->bmRequestType & USB_DIR_IN) ? 1 : 0;

    /* SET_ADDRESS: we perform the real control transfer at the CURRENT address
     * (g_dev_addr), then adopt the new address. */
    usb_status_t st = ehci_do_transfer(dev, 0x00, USB_SPEED_HIGH == dev->speed ? 2 :
                                       (dev->speed == USB_SPEED_LOW ? 1 : 0),
                                       /*is_control=*/1, setup, data, len, in,
                                       NULL, NULL, 1000);
    if (st == USB_OK &&
        (setup->bmRequestType & 0x60) == 0x00 &&
        setup->bRequest == USB_REQ_SET_ADDRESS) {
        g_dev_addr = (uint8_t)setup->wValue;
    }
    return st;
}

static usb_status_t ehci_bulk(usb_device_t *dev, uint8_t ep_addr,
                              void *buf, uint32_t len, uint32_t *xferred)
{
    static uint8_t tgl_out, tgl_in;
    int in = (ep_addr & USB_EP_DIR_IN) ? 1 : 0;
    uint8_t eps = (dev->speed == USB_SPEED_HIGH) ? 2 :
                  (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    return ehci_do_transfer(dev, ep_addr, eps, 0, NULL, buf, len, in,
                            in ? &tgl_in : &tgl_out, xferred, 2000);
}

static usb_status_t ehci_int_in(usb_device_t *dev, uint8_t ep_addr,
                                void *buf, uint32_t len, uint32_t *xferred)
{
    /* Interrupt IN via the async schedule (one-shot polling): simple and
     * sufficient to read a keyboard boot report. The toggle is persistent. */
    uint8_t eps = (dev->speed == USB_SPEED_HIGH) ? 2 :
                  (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    return ehci_do_transfer(dev, ep_addr, eps, 0, NULL, buf, len, 1,
                            &g_toggle_int, xferred, 50);
}

usb_status_t ehci_init(void) { return ehci_hc_init(); }

const usb_hcd_ops_t ehci_hcd_ops = {
    .name = "ehci",
    .init = ehci_init,
    .port_reset = ehci_port_reset,
    .device_alloc = ehci_device_alloc,
    .device_free = ehci_device_free,
    .control = ehci_control,
    .bulk = ehci_bulk,
    .int_in = ehci_int_in,
    .configure_eps = NULL,
};

#endif /* MMU_QEMU */
