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
 * hcd_ohci.c — OHCI HCD (USB 1.1 Low/Full-Speed) for RK3328
 *
 * Implements the usb_hcd_ops_t interface (usb.h) on top of the standard OHCI
 * controller @0xFF5D0000 (usb_host0_ohci). Works in POLLING.
 *
 * This is the controller that enumerates a Low/Full-Speed USB KEYBOARD
 * (handed off by the EHCI).
 *
 * SOURCES (offsets/bits QUOTED, not deduced):
 *   - OHCI 1.0 spec (Open Host Controller Interface): HcControl/
 *     HcCommandStatus/HcRhPortStatus registers, ED (Endpoint Descriptor), TD
 *     (Transfer Descriptor), HCCA.
 *   - u-boot drivers/usb/host/ohci-hcd.c / ohci.h (reset/start, ED/TD, submit).
 *
 * On QEMU (-DMMU_QEMU): MMIO absent -> ohci_init() returns USB_ENODEV.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "hcd_ohci.h"
#include "usb_core.h"
#include "../../arch/aarch64/timer.h"

/* ================================================================== */
/* QEMU neutralization                                                 */
/* ================================================================== */
#if defined(MMU_QEMU)

usb_status_t ohci_init(void) { return USB_ENODEV; }
int ohci_port_connected(void) { return 0; }

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

const usb_hcd_ops_t ohci_hcd_ops = {
    .name = "ohci(qemu-stub)",
    .init = q_init, .port_reset = q_port_reset,
    .device_alloc = q_dev_alloc, .device_free = q_dev_free,
    .control = q_control, .bulk = q_bulk, .int_in = q_int_in,
    .configure_eps = NULL,
};

#else /* ========================= BOARD RK3328 =========================== */

/* ------------------------------------------------------------------ */
/* 32-bit MMIO access + barriers + DMA cache                           */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void dmb(void) { __asm__ volatile("dmb ish" ::: "memory"); }

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

/* Shared host PHY (already woken by EHCI/xHCI; we re-do it for robustness). */
#define USB2PHY_GRF_BASE   0xFF450000UL
#define U2PHY_HOST_SUS     (USB2PHY_GRF_BASE + 0x104)
static void usb2phy_host_init(void)
{
    wr32(U2PHY_HOST_SUS, (0xFFFFu << 16) | 0x0000u);
    dsb();
    udelay(2000);
}

/* ------------------------------------------------------------------ */
/* OHCI registers (spec 1.0, u-boot ohci.h — offsets = consecutive fields) */
/* ------------------------------------------------------------------ */
#define HC_REVISION       0x00
#define HC_CONTROL        0x04
#define HC_CMDSTATUS      0x08
#define HC_INTRSTATUS     0x0C
#define HC_INTRENABLE     0x10
#define HC_INTRDISABLE    0x14
#define HC_HCCA           0x18
#define HC_PERIODCURRENT  0x1C
#define HC_CTRLHEADED     0x20
#define HC_CTRLCURRENT    0x24
#define HC_BULKHEADED     0x28
#define HC_BULKCURRENT    0x2C
#define HC_DONEHEAD       0x30
#define HC_FMINTERVAL     0x34
#define HC_FMREMAINING    0x38
#define HC_FMNUMBER       0x3C
#define HC_PERIODICSTART  0x40
#define HC_LSTHRESH       0x44
#define HC_RHDESCRA       0x48
#define HC_RHDESCRB       0x4C
#define HC_RHSTATUS       0x50
#define HC_RHPORTSTATUS(p) (0x54 + 4u * (p))

/* HcControl bits */
#define CTRL_CBSR     (3u << 0)
#define CTRL_PLE      (1u << 2)
#define CTRL_IE       (1u << 3)
#define CTRL_CLE      (1u << 4)
#define CTRL_BLE      (1u << 5)
#define CTRL_HCFS     (3u << 6)
#define CTRL_IR       (1u << 8)
#define USB_RESET     (0u << 6)
#define USB_OPER      (2u << 6)
/* HcCommandStatus bits */
#define CMD_HCR       (1u << 0)   /* HostControllerReset */
#define CMD_CLF       (1u << 1)   /* ControlListFilled */
#define CMD_BLF       (1u << 2)   /* BulkListFilled */
#define CMD_OCR       (1u << 3)   /* OwnershipChangeRequest */
/* HcInterrupt bits */
#define INTR_WDH      (1u << 1)
#define INTR_MIE      (1u << 31)
/* Root hub port status */
#define RH_CCS        0x00000001
#define RH_PES        0x00000002
#define RH_PSS        0x00000004
#define RH_PRS        0x00000010
#define RH_PPS        0x00000100
#define RH_LSDA       0x00000200
#define RH_CSC        0x00010000
#define RH_PRSC       0x00100000

/* ------------------------------------------------------------------ */
/* ED / TD (OHCI spec, HW-only structures)                             */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t hwINFO;
    uint32_t hwTailP;
    uint32_t hwHeadP;
    uint32_t hwNextED;
} ohci_ed_t;

typedef struct __attribute__((packed, aligned(32))) {
    uint32_t hwINFO;
    uint32_t hwCBP;     /* Current Buffer Pointer */
    uint32_t hwNextTD;
    uint32_t hwBE;      /* Buffer End */
} ohci_td_t;

/* ED hwINFO */
#define ED_FA(x)    (((x) & 0x7Fu) << 0)
#define ED_EN(x)    (((x) & 0xFu) << 7)
#define ED_DIR_OUT  (1u << 11)   /* 0x800 */
#define ED_DIR_IN   (2u << 11)   /* 0x1000 */
#define ED_LOWSPEED (1u << 13)
#define ED_SKIP     (1u << 14)
#define ED_MPS(x)   (((x) & 0x7FFu) << 16)
/* TD hwINFO */
#define TD_CC_MASK   (0xFu << 28)
#define TD_CC_GET(i) (((i) >> 28) & 0xFu)
#define TD_T_DATA0   (0x2u << 24)
#define TD_T_DATA1   (0x3u << 24)
#define TD_T_TOGGLE  (0x0u << 24)
#define TD_R         (1u << 18)   /* buffer Rounding (short OK) */
#define TD_DP_SETUP  0u           /* DP = SETUP (bits[19:18]=00) */
#define TD_DP_OUT    (1u << 19)   /* 0x080000 (DP=01) */
#define TD_DP_IN     (2u << 19)   /* 0x100000 (DP=10) */
#define TD_CC_NOTACCESSED  0xF
#define TD_CC_NOERROR      0x0
#define TD_CC_STALL        0x4
#define TD_CC_DEVNOTRESP   0x5


/* ------------------------------------------------------------------ */
/* Global state + DMA structures                                      */
/* ------------------------------------------------------------------ */
static uintptr_t g_base;
static uint32_t  g_nports;
static uint8_t   g_dev_addr;
static uint8_t   g_low_speed;

/* HCCA (256 B, 256-aligned). */
static uint8_t   g_hcca[256] __attribute__((aligned(256)));
/* One ED + up to 4 TDs (SETUP/DATA/STATUS + dummy). */
static ohci_ed_t g_ed  __attribute__((aligned(16)));
static ohci_td_t g_td[4] __attribute__((aligned(32)));
/* Aligned bounce buffer. */
static uint8_t   g_buf[1024] __attribute__((aligned(64)));

/* Persistent interrupt toggle. */
static uint8_t   g_int_toggle;

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */
static usb_status_t ohci_hc_init(void)
{
    uint32_t rev = rd32(g_base + HC_REVISION);
    if (rev == 0xFFFFFFFFu || (rev & 0xFF) == 0)
        return USB_ENODEV;

    usb2phy_host_init();

    /* --- hc_reset --- */
    /* If IR (SMM owns) → request ownership. */
    if (rd32(g_base + HC_CONTROL) & CTRL_IR) {
        wr32(g_base + HC_CMDSTATUS, CMD_OCR);
        for (int i = 0; i < 50; i++) {
            if (!(rd32(g_base + HC_CONTROL) & CTRL_IR)) break;
            udelay(10000);
        }
    }
    wr32(g_base + HC_INTRDISABLE, INTR_MIE);
    wr32(g_base + HC_CONTROL, USB_RESET);          /* HcControl = 0 */
    wr32(g_base + HC_CMDSTATUS, CMD_HCR);          /* soft reset */
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        if (!(rd32(g_base + HC_CMDSTATUS) & CMD_HCR)) { ok = 1; break; }
        udelay(10);
    }
    if (!ok) {
        printf("[ohci] HC reset timeout\n");
        return USB_ETIMEOUT;
    }

    /* --- hc_start --- */
    memset(g_hcca, 0, sizeof(g_hcca));
    cache_clean(g_hcca, sizeof(g_hcca));
    wr32(g_base + HC_CTRLHEADED, 0);
    wr32(g_base + HC_BULKHEADED, 0);
    wr32(g_base + HC_HCCA, (uint32_t)(uintptr_t)g_hcca);

    uint32_t fmi = 0x2edf;
    wr32(g_base + HC_PERIODICSTART, (fmi * 9u) / 10u);
    fmi |= (((fmi - 210u) * 6u) / 7u) << 16;
    wr32(g_base + HC_FMINTERVAL, fmi);
    wr32(g_base + HC_LSTHRESH, 0x628);

    /* HcControl = CBSR|IE|PLE|CLE|BLE|USB_OPER. */
    wr32(g_base + HC_CONTROL,
         (CTRL_CBSR & 0x3u) | CTRL_IE | CTRL_PLE | CTRL_CLE | CTRL_BLE | USB_OPER);
    /* Disable + clear all interrupts (polling). */
    wr32(g_base + HC_INTRDISABLE, 0xFFFFFFFFu);
    wr32(g_base + HC_INTRSTATUS, 0x7FFFFFFFu);
    dsb();

    /* Number of ports (RhDescriptorA [7:0]). */
    uint32_t rha = rd32(g_base + HC_RHDESCRA);
    g_nports = rha & 0xFF;
    if (g_nports == 0 || g_nports > 15) g_nports = 1;

    /* Power all the ports (SET_POWER). */
    /* HcRhStatus LPSC (bit16) = Set Global Power. */
    wr32(g_base + HC_RHSTATUS, (1u << 16));
    for (uint32_t p = 0; p < g_nports; p++)
        wr32(g_base + HC_RHPORTSTATUS(p), RH_PPS);
    udelay(20000);

    printf("[ohci] HC up : rev=0x%02lx ports=%lu ctrl=0x%08lx\n",
           (unsigned long)(rev & 0xFF), (unsigned long)g_nports,
           (unsigned long)rd32(g_base + HC_CONTROL));
    for (uint32_t p = 0; p < g_nports; p++)
        printf("[ohci] RhPortStatus[%lu]=0x%08lx\n", (unsigned long)p,
               (unsigned long)rd32(g_base + HC_RHPORTSTATUS(p)));
    return USB_OK;
}

static usb_status_t ohci_port_reset(usb_speed_t *speed)
{
    g_dev_addr = 0;

    int found = -1;
    for (int tries = 0; tries < 100 && found < 0; tries++) {
        for (uint32_t p = 0; p < g_nports; p++) {
            if (rd32(g_base + HC_RHPORTSTATUS(p)) & RH_CCS) { found = (int)p; break; }
        }
        if (found < 0) udelay(10000);
    }
    if (found < 0)
        return USB_ENOTCONN;

    uintptr_t pr = g_base + HC_RHPORTSTATUS((uint32_t)found);
    uint32_t sc = rd32(pr);
    printf("[ohci] port connection %d status=0x%08lx\n", found, (unsigned long)sc);

    /* Enable + reset. */
    wr32(pr, RH_PES);              /* set port enable */
    udelay(2000);
    wr32(pr, RH_PRS);              /* set port reset */
    int rok = 0;
    for (int i = 0; i < 100; i++) {
        sc = rd32(pr);
        if (sc & RH_PRSC) { rok = 1; break; }   /* reset complete */
        udelay(2000);
    }
    (void)rok;
    wr32(pr, RH_PRSC);             /* clear reset change (RW1C) */
    wr32(pr, RH_CSC);              /* clear connect change */
    udelay(2000);

    sc = rd32(pr);
    if (!(sc & RH_PES)) {
        printf("[ohci] port %d not enabled after reset (0x%08lx)\n",
               found, (unsigned long)sc);
        return USB_EIO;
    }
    g_low_speed = (sc & RH_LSDA) ? 1 : 0;
    *speed = g_low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
    printf("[ohci] port %d reset OK, device %s (status=0x%08lx)\n",
           found, g_low_speed ? "Low-Speed" : "Full-Speed", (unsigned long)sc);
    /* Recovery delay (USB 2.0: the device needs a rest time AFTER
     * the reset before responding to the 1st control transfer. Without this
     * delay, the very first GET_DESCRIPTOR is STALLed/ignored. */
    udelay(50000);   /* 50 ms of margin (spec min 10 ms) */
    return USB_OK;
}


static usb_status_t ohci_device_alloc(usb_device_t *dev)
{
    (void)dev;
    g_dev_addr = 0;
    return USB_OK;
}
static void ohci_device_free(usb_device_t *dev) { (void)dev; }

/* Runs ONE transfer attempt (builds ED + TDs, issues, polls). */
static usb_status_t ohci_xfer_once(usb_device_t *dev, uint8_t ep_addr,
                                   int is_control, const usb_setup_t *setup,
                                   void *data, uint32_t len, int dir_in,
                                   uint8_t *toggle, uint32_t *xferred,
                                   uint32_t timeout_ms, int quiet)
{

    uint8_t ep_num = ep_addr & 0x0F;
    uint32_t maxpkt = dev->max_packet0;
    if (!is_control) {
        for (uint8_t i = 0; i < dev->num_endpoints; i++)
            if (dev->endpoints[i].address == ep_addr)
                maxpkt = dev->endpoints[i].max_packet;
    }
    if (len > sizeof(g_buf)) len = sizeof(g_buf);

    memset(g_td, 0, sizeof(g_td));
    int ntd = 0;   /* "real" TDs (excluding the final dummy) */

    /* ED (control: dir from TDs → ED_DIR = 0). */
    memset(&g_ed, 0, sizeof(g_ed));
    uint32_t edinfo = ED_FA(g_dev_addr) | ED_EN(ep_num) | ED_MPS(maxpkt);
    if (g_low_speed) edinfo |= ED_LOWSPEED;
    if (!is_control) edinfo |= dir_in ? ED_DIR_IN : ED_DIR_OUT;
    g_ed.hwINFO = edinfo;

    /* Build the TDs. A final "dummy" TD closes the queue (HeadP..TailP). */
    if (is_control) {
        /* SETUP */
        memcpy(g_buf, setup, 8);
        g_td[ntd].hwINFO = TD_CC_MASK | TD_DP_SETUP | TD_T_DATA0;
        g_td[ntd].hwCBP  = (uint32_t)(uintptr_t)g_buf;
        g_td[ntd].hwBE   = (uint32_t)(uintptr_t)(g_buf + 8 - 1);
        ntd++;
        /* DATA (optional) */
        if (len && data) {
            if (!dir_in) memcpy(g_buf + 16, data, len);
            g_td[ntd].hwINFO = TD_CC_MASK | TD_R | TD_T_DATA1 |
                               (dir_in ? TD_DP_IN : TD_DP_OUT);
            g_td[ntd].hwCBP  = (uint32_t)(uintptr_t)(g_buf + 16);
            g_td[ntd].hwBE   = (uint32_t)(uintptr_t)(g_buf + 16 + len - 1);
            ntd++;
        }
        /* STATUS: direction OPPOSITE to the DATA phase (USB 2.0).
         * - control-IN  (GET_DESCRIPTOR) → STATUS OUT
         * - control-OUT (SET_ADDRESS/SET_CONFIG, with or without data) → STATUS IN
         * The STATUS is always DATA1.
         */
        g_td[ntd].hwINFO = TD_CC_MASK | TD_T_DATA1 |
                           (dir_in ? TD_DP_OUT : TD_DP_IN);
        g_td[ntd].hwCBP  = 0;
        g_td[ntd].hwBE   = 0;
        ntd++;

    } else {
        /* bulk/interrupt: 1 data TD. */
        if (!dir_in && data) memcpy(g_buf, data, len);
        uint32_t t = toggle ? (*toggle ? TD_T_DATA1 : TD_T_DATA0) : TD_T_DATA0;
        g_td[ntd].hwINFO = TD_CC_MASK | TD_R | t |
                           (dir_in ? TD_DP_IN : TD_DP_OUT);
        g_td[ntd].hwCBP  = len ? (uint32_t)(uintptr_t)g_buf : 0;
        g_td[ntd].hwBE   = len ? (uint32_t)(uintptr_t)(g_buf + len - 1) : 0;
        ntd++;
    }

    /* Chain the TDs + final dummy. */
    for (int i = 0; i < ntd; i++)
        g_td[i].hwNextTD = (uint32_t)(uintptr_t)&g_td[i + 1];
    ohci_td_t *dummy = &g_td[ntd];
    dummy->hwINFO = 0;
    dummy->hwNextTD = 0;

    /* ED HeadP = 1st TD, TailP = dummy (the HC processes HeadP up to TailP). */
    g_ed.hwHeadP = (uint32_t)(uintptr_t)&g_td[0];
    g_ed.hwTailP = (uint32_t)(uintptr_t)dummy;
    g_ed.hwNextED = 0;

    cache_clean(g_buf, sizeof(g_buf));
    cache_clean(g_td, sizeof(g_td));
    cache_clean(&g_ed, sizeof(g_ed));
    dsb();

    /* Link the ED into the control list + ring CLF. */
    wr32(g_base + HC_CTRLHEADED, (uint32_t)(uintptr_t)&g_ed);
    wr32(g_base + HC_CTRLCURRENT, 0);
    dsb();
    wr32(g_base + HC_CMDSTATUS, CMD_CLF);
    dsb();

    /* Poll: the last real TD must be accessed (CC != NOTACCESSED). */
    ohci_td_t *lastreal = &g_td[ntd - 1];
    uint64_t end = timer_now_ticks() +
                   timer_us_to_ticks((uint64_t)timeout_ms * 1000u);
    int done_ok = 0, stalled = 0;
    for (;;) {
        cache_invalidate(g_td, sizeof(g_td));
        dmb();
        uint32_t cc = TD_CC_GET(lastreal->hwINFO);
        if (cc != TD_CC_NOTACCESSED) {
            /* Check all the real TDs. */
            for (int i = 0; i < ntd; i++) {
                uint32_t c = TD_CC_GET(g_td[i].hwINFO);
                if (c == TD_CC_STALL) { stalled = 1; break; }
                if (c != TD_CC_NOERROR && c != TD_CC_NOTACCESSED) { stalled = 1; break; }
            }
            done_ok = !stalled;
            break;
        }
        if (timer_now_ticks() >= end)
            break;
    }

    /* Unlink the ED (empty control head). */
    wr32(g_base + HC_CTRLHEADED, 0);
    dsb();

    if ((stalled || !done_ok) && !quiet) {
        /* Diagnostic: dump the CC of each TD + the ED/HC state to
         * understand which stage fails (SETUP/DATA/STATUS). */
        cache_invalidate(&g_ed, sizeof(g_ed));
        printf("[ohci] xfer %s (is_ctrl=%d ep=0x%02X len=%lu) : ",
               stalled ? "STALL" : "TIMEOUT", is_control, ep_addr,
               (unsigned long)len);
        for (int i = 0; i < ntd; i++)
            printf("TD%d.cc=%lu ", i, (unsigned long)TD_CC_GET(g_td[i].hwINFO));
        printf("| ED.hdr=0x%08lx tail=0x%08lx info=0x%08lx | CTRL=0x%08lx DONEHEAD=0x%08lx\n",
               (unsigned long)g_ed.hwHeadP, (unsigned long)g_ed.hwTailP,
               (unsigned long)g_ed.hwINFO,
               (unsigned long)rd32(g_base + HC_CONTROL),
               (unsigned long)rd32(g_base + HC_DONEHEAD));
    }

    if (stalled)
        return USB_ESTALL;
    if (!done_ok)
        return USB_ETIMEOUT;


    /* Compute what was transferred + copy back IN data. */

    uint32_t transferred = len;
    ohci_td_t *dtd = is_control ? (len && data ? &g_td[1] : NULL) : &g_td[0];
    if (dtd) {
        cache_invalidate(dtd, sizeof(*dtd));
        if (dtd->hwCBP == 0) {
            transferred = len;   /* all consumed */
        } else {
            uint32_t cur = dtd->hwCBP;
            uint32_t base = is_control ? (uint32_t)(uintptr_t)(g_buf + 16)
                                       : (uint32_t)(uintptr_t)g_buf;
            transferred = (cur > base) ? (cur - base) : 0;
        }
    }
    if (dir_in && data && transferred) {
        uint32_t off = is_control ? 16 : 0;
        cache_invalidate(g_buf, sizeof(g_buf));
        memcpy(data, g_buf + off, transferred);
    }
    if (!is_control && toggle) *toggle ^= 1;
    if (xferred) *xferred = transferred;
    return USB_OK;
}

/* Wrapper with RETRY: a Low-Speed device sometimes misses a transaction
 * (DEVNOTRESP/transient timeout, especially right after reset/SET_ADDRESS). We
 * replay the whole transfer up to 4 times with a small delay. We only dump the
 * diagnostic (quiet=0) on the LAST attempt. */
static usb_status_t ohci_do_transfer(usb_device_t *dev, uint8_t ep_addr,
                                     int is_control, const usb_setup_t *setup,
                                     void *data, uint32_t len, int dir_in,
                                     uint8_t *toggle, uint32_t *xferred,
                                     uint32_t timeout_ms)
{
    const int MAX_TRY = 4;
    usb_status_t st = USB_EIO;
    for (int t = 0; t < MAX_TRY; t++) {
        int last = (t == MAX_TRY - 1);
        st = ohci_xfer_once(dev, ep_addr, is_control, setup, data, len, dir_in,
                            toggle, xferred, timeout_ms, /*quiet=*/!last);
        if (st == USB_OK)
            return USB_OK;
        /* Real STALL (protocol): no point retrying. Timeout/DEVNOTRESP:
         * retry. We distinguish via the code — here we retry for all
         * transient failures (slow LS), except the very last round. */
        udelay(10000);   /* 10 ms rest before retry */
    }
    return st;
}

static usb_status_t ohci_control(usb_device_t *dev, const usb_setup_t *setup,
                                 void *data, uint16_t len)
{
    int in = (setup->bmRequestType & USB_DIR_IN) ? 1 : 0;
    usb_status_t st = ohci_do_transfer(dev, 0x00, 1, setup, data, len, in,
                                       NULL, NULL, 1000);

    if (st == USB_OK &&
        (setup->bmRequestType & 0x60) == 0x00 &&
        setup->bRequest == USB_REQ_SET_ADDRESS) {
        g_dev_addr = (uint8_t)setup->wValue;
        /* USB 2.0 : after SET_ADDRESS, the device has 2 ms to switch
         * to the new address. Without this delay, the next request
         * (GET_DESCRIPTOR at the new address) falls into DEVNOTRESP (cc=5). */
        udelay(5000);   /* 5 ms of margin */
    }
    return st;
}


static usb_status_t ohci_bulk(usb_device_t *dev, uint8_t ep_addr,
                              void *buf, uint32_t len, uint32_t *xferred)
{
    static uint8_t tgl_out, tgl_in;
    int in = (ep_addr & USB_EP_DIR_IN) ? 1 : 0;
    return ohci_do_transfer(dev, ep_addr, 0, NULL, buf, len, in,
                            in ? &tgl_in : &tgl_out, xferred, 2000);
}

static usb_status_t ohci_int_in(usb_device_t *dev, uint8_t ep_addr,
                                void *buf, uint32_t len, uint32_t *xferred)
{
    /* Interrupt IN via the control list (one-shot polling): sufficient to read
     * a keyboard boot report. The toggle is persistent. NO retry here: a NAK
     * (no key pressed) is normal and must not replay/toggle.
     * quiet=1: no dump (the "timeout" at rest is expected). */
    return ohci_xfer_once(dev, ep_addr, 0, NULL, buf, len, 1,
                          &g_int_toggle, xferred, 50, /*quiet=*/1);
}


usb_status_t ohci_init(void)
{
    g_base = OHCI_BASE;
    return ohci_hc_init();
}

/* NON-BLOCKING hot-plug probe: HcRhPortStatus.CCS of every root port, no
 * reset, no log (called in a permanent polling loop). */
int ohci_port_connected(void)
{
    if (!g_base || !g_nports)
        return 0;
    for (uint32_t p = 0; p < g_nports; p++)
        if (rd32(g_base + HC_RHPORTSTATUS(p)) & RH_CCS)
            return 1;
    return 0;
}

const usb_hcd_ops_t ohci_hcd_ops = {
    .name = "ohci",
    .init = ohci_init,
    .port_reset = ohci_port_reset,
    .device_alloc = ohci_device_alloc,
    .device_free = ohci_device_free,
    .control = ohci_control,
    .bulk = ohci_bulk,
    .int_in = ohci_int_in,
    .configure_eps = NULL,
};

#endif /* MMU_QEMU */
