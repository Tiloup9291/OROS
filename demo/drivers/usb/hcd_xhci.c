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
 * hcd_xhci.c — xHCI/DWC3 HCD (USB 3.0) for RK3328
 *
 * Implements the usb_hcd_ops_t interface (usb.h) on top of the controller
 * standard xHCI @0xFF600000 (usbdrd3). Works in POLLING mode (event ring
 * polled, no IRQ) for a simple and deterministic bring-up.
 *
 * SOURCES (offsets/bits QUOTED, not derived):
 *   - Spec Intel "eXtensible Host Controller Interface for USB" 1.2 :
 *       - Host Controller Capability Registers (CAPLENGTH, HCSPARAMS1,
 *            DBOFF, RTSOFF, HCCPARAMS1)
 *       - Host Controller Operational Registers (USBCMD, USBSTS, CRCR,
 *            DCBAAP, CONFIG, PORTSC)
 *       - Host Controller Runtime Registers (IMAN, IMOD, ERSTSZ, ERSTBA,
 *            ERDP)
 *       - Data Structures (TRB 16o, Command/Event Ring, ERST, Device/Input
 *            Context, DCBAA)
 *   - u-boot drivers/usb/host/xhci.c / xhci-mem.c (init sequence, sizes)
 *   - DWC3 glue (Linux drivers/usb/dwc3/core.h) : GCTL/GUSB* if reprogramming is required.
 *
 * Bring-up note: U-Boot already initialized the USB3 PHY, CRU clocks/resets
 * and put DWC3 in host mode. We start from this state:
 * we (re)do a clean xHCI HC reset and program our structures. If the
 * controller does not respond (registers at 0xFFFFFFFF / bad CAPLENGTH), return
 * USB_ENODEV.
 *
 * On QEMU (-DMMU_QEMU): no MMIO -> xhci_init() returns USB_ENODEV, the
 * demo stays harmless (like gpio/sdmmc).
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "hcd_xhci.h"
#include "usb_core.h"
#include "../../arch/aarch64/timer.h"

/* ================================================================== */
/* QEMU neutralization                                                 */
/* ================================================================== */
#if defined(MMU_QEMU)

usb_status_t xhci_init(void) { return USB_ENODEV; }

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
static usb_status_t q_cfg_eps(usb_device_t *d) { (void)d; return USB_ENODEV; }

const usb_hcd_ops_t xhci_hcd_ops = {
    .name = "xhci(qemu-stub)",
    .init = q_init, .port_reset = q_port_reset,
    .device_alloc = q_dev_alloc, .device_free = q_dev_free,
    .control = q_control, .bulk = q_bulk, .int_in = q_int_in,
    .configure_eps = q_cfg_eps,
};


#else /* ============================= BOARD RK3328 ============================= */

/* ------------------------------------------------------------------ */
/* 32-bit MMIO access                                                   */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(uintptr_t a)
{
    return *(volatile uint32_t *)a;
}
static inline void wr32(uintptr_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}
static inline void wr64(uintptr_t a, uint64_t v)
{
    /* xHCI 64-bit registers: write low then high. */
    *(volatile uint32_t *)a       = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(a + 4) = (uint32_t)(v >> 32);
}

/* ------------------------------------------------------------------ */
/* xHCI registers — offsets/bits (xHCI spec 1.2)                        */
/* ------------------------------------------------------------------ */
/* Capability registers (base = XHCI_BASE) */
#define XHCI_CAPLENGTH        0x00  /* [7:0] length; [31:16] HCIVERSION */
#define XHCI_HCSPARAMS1       0x04  /* [7:0] MaxSlots; [31:24] MaxPorts */
#define XHCI_HCSPARAMS2       0x08
#define XHCI_HCCPARAMS1       0x10  /* bit0 AC64; bit2 CSZ (context size 64) */
#define XHCI_DBOFF            0x14  /* Doorbell array offset (aligned 4) */
#define XHCI_RTSOFF           0x18  /* Runtime registers offset (aligned 32) */

/* Operational registers (base = XHCI_BASE + CAPLENGTH) */
#define XHCI_OP_USBCMD        0x00
#define XHCI_OP_USBSTS        0x04
#define XHCI_OP_PAGESIZE      0x08
#define XHCI_OP_DNCTRL        0x14
#define XHCI_OP_CRCR          0x18  /* 64 bits : Command Ring Control */
#define XHCI_OP_DCBAAP        0x30  /* 64 bits : Device Context Base Addr Array */
#define XHCI_OP_CONFIG        0x38
#define XHCI_OP_PORTSC(p)     (0x400 + (0x10 * (p)))   /* port p (0-based) */

/* USBCMD bits */
#define USBCMD_RS             (1u << 0)   /* Run/Stop */
#define USBCMD_HCRST          (1u << 1)   /* Host Controller Reset */
#define USBCMD_INTE           (1u << 2)   /* Interrupter Enable */

/* USBSTS bits */
#define USBSTS_HCH            (1u << 0)   /* HC Halted */
#define USBSTS_CNR            (1u << 11)  /* Controller Not Ready */

/* PORTSC bits */
#define PORTSC_CCS           (1u << 0)    /* Current Connect Status */
#define PORTSC_PED           (1u << 1)    /* Port Enabled/Disabled */
#define PORTSC_PR            (1u << 4)    /* Port Reset */
#define PORTSC_PP            (1u << 9)    /* Port Power */
#define PORTSC_PRC           (1u << 21)   /* Port Reset Change */
#define PORTSC_CSC           (1u << 17)   /* Connect Status Change */
#define PORTSC_SPEED_SHIFT   10
#define PORTSC_SPEED_MASK    (0xFu << 10) /* Port Speed (PSI) */
/* Change bits are RW1C; preserve PED/PP when writing. */
#define PORTSC_RW1C          (PORTSC_PRC | PORTSC_CSC)

/* Runtime registers (base = XHCI_BASE + RTSOFF), Interrupter 0 */
#define XHCI_RT_IMAN         0x20
#define XHCI_RT_IMOD         0x24
#define XHCI_RT_ERSTSZ       0x28
#define XHCI_RT_ERSTBA       0x30   /* 64 bits */
#define XHCI_RT_ERDP         0x38   /* 64 bits : Event Ring Dequeue Ptr */
#define IMAN_IP              (1u << 0)   /* Interrupt Pending (RW1C) */
#define IMAN_IE              (1u << 1)   /* Interrupt Enable */
#define ERDP_EHB             (1u << 3)   /* Event Handler Busy (RW1C) */

/* ------------------------------------------------------------------ */
/* TRB (Transfer Request Block), 16 octets                            */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;    /* [15:10] TRB type ; bit0 Cycle */
} xhci_trb_t;

/* TRB types */
#define TRB_NORMAL           1
#define TRB_SETUP_STAGE      2
#define TRB_DATA_STAGE       3
#define TRB_STATUS_STAGE     4
#define TRB_LINK             6
#define TRB_ENABLE_SLOT      9
#define TRB_ADDRESS_DEVICE   11
#define TRB_CONFIGURE_EP     12
#define TRB_TRANSFER_EVENT   32
#define TRB_CMD_COMPLETION   33
#define TRB_PORT_STATUS_EVENT 34
#define TRB_TYPE_SHIFT       10
#define TRB_TYPE(t)          ((uint32_t)(t) << TRB_TYPE_SHIFT)
#define TRB_TYPE_GET(c)      (((c) >> TRB_TYPE_SHIFT) & 0x3Fu)
#define TRB_CYCLE            (1u << 0)
#define TRB_IOC              (1u << 5)   /* Interrupt On Completion */
#define TRB_IDT              (1u << 6)   /* Immediate Data (setup stage) */
#define TRB_ISP              (1u << 2)   /* Interrupt on Short Packet */
/* Completion code (status[31:24] of an event TRB) */
#define TRB_CC(status)       (((status) >> 24) & 0xFFu)
#define CC_SUCCESS           1
#define CC_SHORT_PACKET      13

/* Data stage direction (control[16]) */
#define TRB_DIR_IN           (1u << 16)

/* ------------------------------------------------------------------ */
/* Contexts. We work with a context size of 32 bytes if CSZ=0,
 * 64 if CSZ=1. We allocate 64 bytes to cover both cases (padding).  */
/* ------------------------------------------------------------------ */
#define CTX_BYTES            64u    /* worst case (CSZ=1) */

/* Slot + EP contexts are blocks of 8 dwords (32B) or 16 dwords (64B).
 * We manipulate them via dword offsets taking g_ctx_stride into account. */

/* ------------------------------------------------------------------ */
/* Ring sizes                                                           */
/* ------------------------------------------------------------------ */
#define CMD_RING_TRBS        16u
#define EVENT_RING_TRBS      16u
#define XFER_RING_TRBS       16u

/* ------------------------------------------------------------------ */
/* Global HCD state (a single controller, a single device)  */
/* ------------------------------------------------------------------ */
static uintptr_t g_op;        /* base of the Operational Registers */
static uintptr_t g_rt;        /* base of the Runtime Registers */
static uintptr_t g_db;        /* base of the Doorbells */
static uint32_t  g_max_slots;
static uint32_t  g_max_ports;
static uint32_t  g_ctx_stride;/* 32 or 64 (CSZ) */


/* DMA structures (aligned, coherent Normal memory — MMU active). */
static uint64_t  g_dcbaa[64]           __attribute__((aligned(64)));
/* Scratchpad: array of pointers (DCBAA[0]) + 4 KiB buffers (PAGESIZE).
 * Required if HCSPARAMS2 Max Scratchpad Buffers > 0 (common on DWC3/RK3328).
 * Without this, the HC may never complete Enable Slot (silent event ring). */
#define MAX_SCRATCHPADS      64u
static uint64_t  g_scratch_arr[MAX_SCRATCHPADS] __attribute__((aligned(64)));
static uint8_t   g_scratch_buf[MAX_SCRATCHPADS][4096] __attribute__((aligned(4096)));

static xhci_trb_t g_cmd_ring[CMD_RING_TRBS] __attribute__((aligned(64)));
static xhci_trb_t g_evt_ring[EVENT_RING_TRBS] __attribute__((aligned(64)));
static uint64_t  g_erst[2]             __attribute__((aligned(64))); /* 1 segment: base, size */
static uint8_t   g_dev_ctx[CTX_BYTES * 32] __attribute__((aligned(64))); /* device context (slot+eps) */
static uint8_t   g_in_ctx[CTX_BYTES * 33]  __attribute__((aligned(64))); /* input context (icc+slot+eps) */
static xhci_trb_t g_ep0_ring[XFER_RING_TRBS] __attribute__((aligned(64)));

/* Transfer rings of the "data" endpoints (bulk/interrupt).
 * One ring per DCI (Device Context Index). DCI = ep_num*2 + (dir_in?1:0),
 * DCI 0/1 reserved for slot/EP0 -> we index from 2 to 31. The RTL8153B uses
 * bulk IN ep1 (DCI 3), bulk OUT ep2 (DCI 4), int IN ep3 (DCI 7). We allocate
 * an array of rings indexed by DCI (32 entries) but only a few of them
 * are used. */
#define XHCI_MAX_DCI         32u
static xhci_trb_t g_ep_ring[XHCI_MAX_DCI][XFER_RING_TRBS]
                                    __attribute__((aligned(64)));
static uint32_t  g_ep_cycle[XHCI_MAX_DCI];
static uint32_t  g_ep_idx[XHCI_MAX_DCI];
static uint8_t   g_ep_active[XHCI_MAX_DCI];   /* 1 if the ring is configured */

/* Cycle bits + current indices. */
static uint32_t  g_cmd_cycle = 1;
static uint32_t  g_cmd_idx;
static uint32_t  g_evt_cycle = 1;
static uint32_t  g_evt_idx;
static uint32_t  g_ep0_cycle = 1;
static uint32_t  g_ep0_idx;
static uint8_t   g_slot_id;


/* ------------------------------------------------------------------ */
/* Memory barriers (DMA structures in Normal WB, MMU active)             */
/* ------------------------------------------------------------------ */
static inline void dmb(void) { __asm__ volatile("dmb ish" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy"  ::: "memory"); }

/* ------------------------------------------------------------------ */
/* DMA cache coherence (CRUCIAL) — the xHCI accesses rings/contexts via
 * DMA, but our RAM is mapped Normal WB CACHEABLE (mmu.c RK3328). We must
 * therefore:
 *   - CLEAN (dc cvac) what the CPU writes and the HC reads (cmd/ep0 ring,
 *     contexts, DCBAA, ERST) → push to the Point of Coherency;
 *   - INVALIDATE (dc ivac) what the HC writes and the CPU reads (event ring)
 *     → force a re-read from RAM, not from the stale cache.
 * Symptom without this: USBSTS.EINT=1 (event posted in RAM by the HC) but our
 * evt_ring[0] read = 0 (stale CPU cache) -> Enable Slot "cc=-1" (timeout). */
static inline void cache_clean(const void *addr, uint32_t size)
{
    uintptr_t p = (uintptr_t)addr & ~63UL;
    uintptr_t end = (uintptr_t)addr + size;
    for (; p < end; p += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static inline void cache_invalidate(void *addr, uint32_t size)
{
    uintptr_t p = (uintptr_t)addr & ~63UL;
    uintptr_t end = (uintptr_t)addr + size;
    for (; p < end; p += 64)
        __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}


static void udelay(uint32_t us)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks(us);
    while (timer_now_ticks() < end)
        __asm__ volatile("nop");
}

/* ------------------------------------------------------------------ */
/* Init of the RK3328 USB2 PHY (usb2phy_grf) — take the ports out of suspend */
/* ------------------------------------------------------------------ */
/* Symptom without this: xHCI up, PORTSC=0x280 (PP=1, CCS=0, PLS=5 RxDetect):
 * the PHY stays in suspend -> no connection detected (internal RTL8153B included).
 *
 * Offsets/values QUOTED from the source — Linux/u-boot
 * phy-rockchip-inno-usb2.c, rk3328_phy_cfgs (RK3328 patch 2017-03):
 *   usb2phy_grf @ 0xFF450000 ; reg base 0x100.
 *   OTG-PORT  phy_sus = { off 0x100, bits[15:0], disable=0, enable=0x1d1 }
 *   HOST-PORT phy_sus = { off 0x104, bits[15:0], disable=0, enable=0x1d1 }
 * "disable suspend" = write 0 into bits[15:0] → PHY powered/active.
 * Rockchip GRF writes: write-mask in [31:16] → (0xFFFF<<16)|val. */
#define USB2PHY_GRF_BASE     0xFF450000UL
#define U2PHY_OTG_SUS        (USB2PHY_GRF_BASE + 0x100)
#define U2PHY_HOST_SUS       (USB2PHY_GRF_BASE + 0x104)
#define U2PHY_CLKOUT_CTL     (USB2PHY_GRF_BASE + 0x108)  /* clkout_ctl {4,4} */

static void usb2phy_init(void)
{
    /* Take OTG and HOST out of suspend: bits[15:0]=0 with write-mask 0xFFFF. */
    wr32(U2PHY_OTG_SUS,  (0xFFFFu << 16) | 0x0000u);
    wr32(U2PHY_HOST_SUS, (0xFFFFu << 16) | 0x0000u);
    /* Enable the PHY clock output (clkout_ctl bit4: 0=enable). */
    wr32(U2PHY_CLKOUT_CTL, (1u << (4 + 16)) | (0u << 4));
    dsb();
    udelay(2000);   /* let the PHY stabilize */
}

/* ------------------------------------------------------------------ */
/* AUTONOMOUS bring-up of the USB3 controller (CRU + DWC3) — replaces `usb start` */
/* ------------------------------------------------------------------ */
/* GOAL: wake up the DWC3+PHY USB3 combo OURSELVES, without depending on the
 * U-Boot `usb start` command. Sources:
 *   - RK3328 CRU: base 0xFF440000; softrst_con[12] @0x300 (16 resets/reg,
 *     write-mask [31:16]); clkgate_con[29] @0x200. IDs (rk3328-cru.h):
 *       SRST_USB3GRF=79, SRST_USB3OTG=120, SRST_USB3OTG_UTMI=124,
 *       SRST_USB3PHY_U2=125, SRST_USB3PHY_U3=126, SRST_USB3PHY_PIPE=127.
 *   - DWC3 core (linux/usb/dwc3.h): "glue" block @ base+0xC100.
 *       GCTL       0xC110 : bit11 CoreSoftReset ; PrtCapDir bits[13:12] (HOST=1)
 *                           bit0 DsblClkGtng, bit3 DisScramble, [5:4] ScaleDown
 *       GSNPSID    0xC120 : [31:16] must be 0x5533 (U3) or 0x3331
 *       GUSB2PHYCFG(0) 0xC200 : bit31 PHYSOFTRST ; bit6 SUSPHY
 *       GUSB3PIPECTL(0)0xC2C0 : bit31 PHYSOFTRST ; bit17 SUSPHY
 * NOTE: on RK3328, usbdrd3 uses the USB2 utmi PHY (already woken by
 * usb2phy_init) for USB2, + an internal USB3 pipe. The DT has NO separate USB3
 * PHY driver -> the wake-up is limited to: deassert CRU resets + dwc3_core_init +
 * PrtCapDir=HOST. If GSNPSID is invalid (clock absent), we log and return
 * -1: the caller will keep the `usb start` fallback as backup. */
#define CRU_BASE             0xFF440000UL
#define CRU_SOFTRST_CON(n)   (CRU_BASE + 0x300 + 4u * (n))   /* n = 0..11 */
#define CRU_CLKGATE_CON(n)   (CRU_BASE + 0x200 + 4u * (n))   /* n = 0..28 */

/* DWC3 glue (base = XHCI_BASE + 0xC100 ; absolute offsets from XHCI_BASE). */
#define DWC3_GCTL            (XHCI_BASE + 0xC110)
#define DWC3_GSNPSID         (XHCI_BASE + 0xC120)
#define DWC3_GUSB2PHYCFG0    (XHCI_BASE + 0xC200)
#define DWC3_GUSB3PIPECTL0   (XHCI_BASE + 0xC2C0)
#define DWC3_GHWPARAMS1      (XHCI_BASE + 0xC144)
#define GCTL_CORESOFTRESET   (1u << 11)
#define GCTL_PRTCAPDIR_MASK  (3u << 12)
#define GCTL_PRTCAP_HOST     (1u << 12)   /* PrtCapDir = HOST(1) << 12 */
#define GCTL_DSBLCLKGTNG     (1u << 0)
#define GCTL_DISSCRAMBLE     (1u << 3)
#define GCTL_SCALEDOWN_MASK  (3u << 4)
#define GUSB2_PHYSOFTRST     (1u << 31)
#define GUSB3_PHYSOFTRST     (1u << 31)
#define GSNPSID_MASK         0xFFFF0000u

/* Deassert (take out of reset) a given SRST_*: softrst_con[id/16], bit id%16,
 * write-mask in [31:16]. Writing 0 into the bit = deassert. */
static void cru_deassert(uint32_t srst_id)
{
    uint32_t reg = srst_id / 16u;
    uint32_t bit = srst_id % 16u;
    wr32(CRU_SOFTRST_CON(reg), (1u << (bit + 16)) | (0u << bit));
}

/* Returns 0 if the DWC3/PHY USB3 combo is awake (valid GSNPSID), -1 otherwise. */
static int usb3_bring_up(void)
{
    /* 1) Take the whole USB3 subsystem out of reset (CRU). The order follows
     * Linux/u-boot: GRF, PHY (U2/U3/PIPE), UTMI, then OTG (the controller). */
    cru_deassert(79);   /* SRST_USB3GRF        */
    cru_deassert(125);  /* SRST_USB3PHY_U2     */
    cru_deassert(126);  /* SRST_USB3PHY_U3     */
    cru_deassert(127);  /* SRST_USB3PHY_PIPE   */
    cru_deassert(158);  /* SRST_USB3PHY_OTG_P  */
    cru_deassert(159);  /* SRST_USB3PHY_PIPE_P */
    cru_deassert(124);  /* SRST_USB3OTG_UTMI   */
    cru_deassert(120);  /* SRST_USB3OTG        */
    dsb();
    udelay(2000);

    /* 2) Check that the DWC3 responds (clocks OK) via GSNPSID. */
    uint32_t id = rd32(DWC3_GSNPSID);
    if ((id & GSNPSID_MASK) != 0x55330000u &&
        (id & GSNPSID_MASK) != 0x33310000u) {
        printf("[xhci] DWC3 GSNPSID=0x%08lx invalid → USB3 PHY not ready "
               "(keeping `usb start`)\n", (unsigned long)id);
        return -1;
    }
    printf("[xhci] DWC3 GSNPSID=0x%08lx OK\n", (unsigned long)id);

    /* 3) dwc3_core_init (linux/u-boot): soft-reset core + PHY, set GCTL. */
    /* 3a) Put the core in reset. */
    wr32(DWC3_GCTL, rd32(DWC3_GCTL) | GCTL_CORESOFTRESET);
    /* 3b) Assert USB2 + USB3 PHY resets. */
    wr32(DWC3_GUSB3PIPECTL0, rd32(DWC3_GUSB3PIPECTL0) | GUSB3_PHYSOFTRST);
    wr32(DWC3_GUSB2PHYCFG0,  rd32(DWC3_GUSB2PHYCFG0)  | GUSB2_PHYSOFTRST);
    dsb();
    udelay(100000);   /* 100 ms (like u-boot mdelay(100)) */
    /* 3c) Deassert the PHY resets. */
    wr32(DWC3_GUSB3PIPECTL0, rd32(DWC3_GUSB3PIPECTL0) & ~GUSB3_PHYSOFTRST);
    wr32(DWC3_GUSB2PHYCFG0,  rd32(DWC3_GUSB2PHYCFG0)  & ~GUSB2_PHYSOFTRST);
    dsb();
    udelay(100000);
    /* 3d) Take the core out of reset. */
    wr32(DWC3_GCTL, rd32(DWC3_GCTL) & ~GCTL_CORESOFTRESET);

    /* 3e) Set GCTL: clear ScaleDown/DisScramble, handle clock gating. */
    uint32_t reg = rd32(DWC3_GCTL);
    reg &= ~GCTL_SCALEDOWN_MASK;
    reg &= ~GCTL_DISSCRAMBLE;
    /* Power-opt clock: if HWPARAMS1 en_pwropt==1, allow clock gating. */
    uint32_t hw1 = rd32(DWC3_GHWPARAMS1);
    if (((hw1 >> 24) & 0x3u) == 1u /* EN_PWROPT_CLK */)
        reg &= ~GCTL_DSBLCLKGTNG;
    wr32(DWC3_GCTL, reg);

    /* 4) dwc3_set_mode(HOST): PrtCapDir = HOST. */
    reg = rd32(DWC3_GCTL);
    reg &= ~GCTL_PRTCAPDIR_MASK;
    reg |= GCTL_PRTCAP_HOST;
    wr32(DWC3_GCTL, reg);
    dsb();
    udelay(10000);
    printf("[xhci] DWC3 core init + HOST mode OK (GCTL=0x%08lx)\n",
           (unsigned long)rd32(DWC3_GCTL));
    return 0;
}


/* ------------------------------------------------------------------ */
/* Context access (stride 32 or 64)                                    */
/* ------------------------------------------------------------------ */
static inline uint32_t *ctx_dword(uint8_t *base, uint32_t ctx_index, uint32_t dw)
{
    return (uint32_t *)(base + ctx_index * g_ctx_stride) + dw;
}


/* ------------------------------------------------------------------ */
/* Command ring: posts a command TRB + doorbell 0                      */
/* ------------------------------------------------------------------ */
static void cmd_push(uint64_t param, uint32_t status, uint32_t ctrl_type_flags)
{
    xhci_trb_t *t = &g_cmd_ring[g_cmd_idx];
    t->param_lo = (uint32_t)(param & 0xFFFFFFFFu);
    t->param_hi = (uint32_t)(param >> 32);
    t->status   = status;
    uint32_t ctrl = ctrl_type_flags;
    if (g_cmd_cycle) ctrl |= TRB_CYCLE; else ctrl &= ~TRB_CYCLE;
    t->control = ctrl;
    dsb();

    g_cmd_idx++;
    if (g_cmd_idx >= CMD_RING_TRBS - 1) {
        /* Link TRB at the end of the ring (toggle cycle). */
        xhci_trb_t *lnk = &g_cmd_ring[CMD_RING_TRBS - 1];
        lnk->param_lo = (uint32_t)((uintptr_t)g_cmd_ring & 0xFFFFFFFFu);
        lnk->param_hi = (uint32_t)((uint64_t)(uintptr_t)g_cmd_ring >> 32);
        lnk->status   = 0;
        lnk->control  = TRB_TYPE(TRB_LINK) | (1u << 1) /*Toggle Cycle*/ |
                        (g_cmd_cycle ? TRB_CYCLE : 0);
        dsb();
        g_cmd_idx = 0;
        g_cmd_cycle ^= 1;
    }
    /* Publish the command ring in RAM so the HC (DMA) sees it. */
    cache_clean(g_cmd_ring, sizeof(g_cmd_ring));
    /* Doorbell 0 (host controller command). */
    wr32(g_db + 0, 0);
    dsb();
}


/* Acknowledges the current event: advances the dequeue pointer (toggle cycle
 * at the end of the ring) THEN rewrites ERDP = address of the NEW dequeue |
 * EHB. Conforms to u-boot xhci_acknowledge_event. */
static void evt_acknowledge(void)
{
    g_evt_idx++;
    if (g_evt_idx >= EVENT_RING_TRBS) {
        g_evt_idx = 0;
        g_evt_cycle ^= 1;                 /* end of the event ring -> toggle */
    }
    /* ERDP = address of the next TRB to consume | EHB (Event Handler Busy). */
    uint64_t erdp = (uint64_t)(uintptr_t)&g_evt_ring[g_evt_idx] | ERDP_EHB;
    wr64(g_rt + XHCI_RT_ERDP, erdp);
    dsb();
}

/* Waits for an event TRB of type 'want' on the event ring (polling). Returns
 * the completion code, and copies the TRB into *out if non-NULL. -1 on timeout.
 */
static int evt_wait(uint32_t want_type, xhci_trb_t *out, uint32_t timeout_ms)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks((uint64_t)timeout_ms * 1000u);
    for (;;) {
        xhci_trb_t *e = &g_evt_ring[g_evt_idx];
        /* The HC writes the event via DMA in RAM: invalidate our cache to read
         * the real value (otherwise we read a stale 0 → timeout). */
        cache_invalidate(e, sizeof(*e));
        uint32_t ctrl = e->control;
        dmb();

        /* "event ready" = the TRB's cycle bit == our software cycle_state. */
        if ((ctrl & TRB_CYCLE) == g_evt_cycle) {
            uint32_t type = TRB_TYPE_GET(ctrl);
            int cc = (int)TRB_CC(e->status);
            xhci_trb_t copy = *e;

            if (type == want_type || want_type == 0) {
                /* Expected event: acknowledge it (advance dequeue + ERDP) then
                 * return it. */
                evt_acknowledge();
                if (out) *out = copy;
                return cc;
            }

            /* UNEXPECTED event (e.g. Port Status Change): acknowledge + continue. */
            evt_acknowledge();
            continue;
        }
        if (timer_now_ticks() >= end)
            return -1;
    }
}


/* ------------------------------------------------------------------ */
/* EP0 transfer ring: enqueues a TRB (WITHOUT doorbell nor cycle of the 1st). */
/*                                                                       */
/* IMPORTANT (u-boot xhci_ctrl_tx / giveback_first_trb algorithm:        */
/* we must NOT give the 1st TRB (SETUP)                                  */
/* to the HC before having written ALL the TRBs of the TD. Trick: we     */
/* write the 1st TRB with the INVERTED cycle bit (the HC sees it as      */
/* "not ready"), we enqueue data+status with the current cycle, THEN we  */
/* restore the cycle of the 1st TRB (giveback) and ring the doorbell.    */
/* 'is_first' = SETUP.                                                   */
static xhci_trb_t *g_ep0_first;    /* pointer to the 1st TRB of the TD */
static uint32_t    g_ep0_first_cycle;

static void ep0_push(uint64_t param, uint32_t status, uint32_t ctrl, int is_first)
{
    xhci_trb_t *t = &g_ep0_ring[g_ep0_idx];
    t->param_lo = (uint32_t)(param & 0xFFFFFFFFu);
    t->param_hi = (uint32_t)(param >> 32);
    t->status   = status;

    uint32_t cyc = g_ep0_cycle;
    if (is_first) {
        /* 1st TRB: we note the location + the CORRECT cycle, but we write
         * the INVERTED cycle so the HC does not consume it yet. */
        g_ep0_first       = t;
        g_ep0_first_cycle = cyc;
        cyc ^= 1;
    }
    if (cyc) ctrl |= TRB_CYCLE; else ctrl &= ~TRB_CYCLE;
    t->control = ctrl;

    g_ep0_idx++;
    if (g_ep0_idx >= XFER_RING_TRBS - 1) {
        xhci_trb_t *lnk = &g_ep0_ring[XFER_RING_TRBS - 1];
        lnk->param_lo = (uint32_t)((uintptr_t)g_ep0_ring & 0xFFFFFFFFu);
        lnk->param_hi = (uint32_t)((uint64_t)(uintptr_t)g_ep0_ring >> 32);
        lnk->status   = 0;
        lnk->control  = TRB_TYPE(TRB_LINK) | (1u << 1) |
                        (g_ep0_cycle ? TRB_CYCLE : 0);
        g_ep0_idx = 0;
        g_ep0_cycle ^= 1;
    }
}

/* Ends the TD: restores the cycle of the 1st TRB (the HC can now read the
 * whole TD), publishes the ring in RAM and rings the EP0 doorbell. */
static void ep0_giveback_and_ring(void)
{
    if (g_ep0_first) {
        if (g_ep0_first_cycle) g_ep0_first->control |= TRB_CYCLE;
        else                   g_ep0_first->control &= ~TRB_CYCLE;
        g_ep0_first = NULL;
    }
    cache_clean(g_ep0_ring, sizeof(g_ep0_ring));
    dsb();
    /* Slot doorbell: target = 1 (Control EP0). */
    wr32(g_db + 4u * g_slot_id, 1);
    dsb();
}


/* ================================================================== */
/* HCD Interface                                                       */
/* ================================================================== */

static usb_status_t xhci_hc_init(void)
{
    /* Capability base: read CAPLENGTH to find the Operational regs. */
    uint32_t caplen_ver = rd32(XHCI_BASE + XHCI_CAPLENGTH);
    if (caplen_ver == 0xFFFFFFFFu || (caplen_ver & 0xFF) == 0)
        return USB_ENODEV;              /* no controller / dead MMIO */

    /* 0) Wake up the USB2 PHY (take OTG+HOST out of suspend). Without this, the
     * port stays PLS=5 (RxDetect) and no connection appears (CCS=0). */
    usb2phy_init();

    /* 0.2) AUTONOMOUS USB3 BRING-UP (CRU deassert reset + DWC3 core init + HOST
     * mode) — replaces the U-Boot `usb start` crutch. Non-blocking: if the
     * PHY/DWC3 combo does not respond (invalid GSNPSID), we continue anyway
     * (the user can keep `usb start` as a fallback for that build). */
    (void)usb3_bring_up();



    uint8_t caplen = caplen_ver & 0xFF;
    g_op = XHCI_BASE + caplen;
    g_rt = XHCI_BASE + (rd32(XHCI_BASE + XHCI_RTSOFF) & ~0x1Fu);
    g_db = XHCI_BASE + (rd32(XHCI_BASE + XHCI_DBOFF)  & ~0x3u);

    uint32_t hcs1 = rd32(XHCI_BASE + XHCI_HCSPARAMS1);
    g_max_slots = hcs1 & 0xFF;
    g_max_ports = (hcs1 >> 24) & 0xFF;

    uint32_t hcc1 = rd32(XHCI_BASE + XHCI_HCCPARAMS1);
    g_ctx_stride = (hcc1 & (1u << 2)) ? 64u : 32u;   /* CSZ */

    /* 1) Stop then reset the HC. */
    uint32_t cmd = rd32(g_op + XHCI_OP_USBCMD);
    cmd &= ~USBCMD_RS;
    wr32(g_op + XHCI_OP_USBCMD, cmd);
    /* wait for HCHalted */
    for (int i = 0; i < 1000; i++) {
        if (rd32(g_op + XHCI_OP_USBSTS) & USBSTS_HCH) break;
        udelay(100);
    }
    wr32(g_op + XHCI_OP_USBCMD, rd32(g_op + XHCI_OP_USBCMD) | USBCMD_HCRST);
    for (int i = 0; i < 1000; i++) {
        if (!(rd32(g_op + XHCI_OP_USBCMD) & USBCMD_HCRST) &&
            !(rd32(g_op + XHCI_OP_USBSTS) & USBSTS_CNR))
            break;
        udelay(1000);
    }
    if (rd32(g_op + XHCI_OP_USBSTS) & USBSTS_CNR)
        return USB_ETIMEOUT;

    /* Post-reset trace: after a successful HCRST, HCH must be 1 (HC halted)
     * and CNR 0. If HCH=0 here, the reset did NOT happen (HC still running
     * from U-Boot) → U-Boot's event ring stays active and our events don't
     * arrive in our ring (symptom evt[0]=0 despite EINT=1). */
    printf("[xhci] post-reset : USBSTS=0x%08lx USBCMD=0x%08lx\n",
           (unsigned long)rd32(g_op + XHCI_OP_USBSTS),
           (unsigned long)rd32(g_op + XHCI_OP_USBCMD));

    /* Clear the RW1C bits of USBSTS (EINT, PCD, etc.) to start clean. */
    wr32(g_op + XHCI_OP_USBSTS, rd32(g_op + XHCI_OP_USBSTS));

    /* 2) Program MaxSlotsEn in CONFIG. */
    wr32(g_op + XHCI_OP_CONFIG, g_max_slots);


    /* 3) DCBAA (Device Context Base Address Array) + SCRATCHPAD.
     * HCSPARAMS2: Max Scratchpad Buffers = {bits[25:21] hi} << 5 | {bits[31:27] lo}.
     * If > 0, DCBAA[0] MUST point to an array of N pointers to N pages
     * of PAGESIZE (the HC uses it as working memory). A common omission that
     * prevents Enable Slot from completing (silent event ring). */
    memset(g_dcbaa, 0, sizeof(g_dcbaa));
    uint32_t hcs2 = rd32(XHCI_BASE + XHCI_HCSPARAMS2);
    uint32_t max_sp = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (max_sp > MAX_SCRATCHPADS) max_sp = MAX_SCRATCHPADS;
    if (max_sp > 0) {
        memset(g_scratch_arr, 0, sizeof(g_scratch_arr));
        for (uint32_t i = 0; i < max_sp; i++)
            g_scratch_arr[i] = (uint64_t)(uintptr_t)g_scratch_buf[i];
        g_dcbaa[0] = (uint64_t)(uintptr_t)g_scratch_arr;
        cache_clean(g_scratch_arr, sizeof(g_scratch_arr));
        printf("[xhci] scratchpad : %lu buffers allocated (DCBAA[0])\n",
               (unsigned long)max_sp);
    }
    cache_clean(g_dcbaa, sizeof(g_dcbaa));
    dsb();
    wr64(g_op + XHCI_OP_DCBAAP, (uint64_t)(uintptr_t)g_dcbaa);


    /* 4) Command Ring: CRCR = base | RCS(1). */
    memset(g_cmd_ring, 0, sizeof(g_cmd_ring));
    g_cmd_idx = 0; g_cmd_cycle = 1;
    dsb();
    wr64(g_op + XHCI_OP_CRCR, (uint64_t)(uintptr_t)g_cmd_ring | 1u /*RCS*/);

    /* 5) Event Ring (single segment) + ERST + Interrupter 0.
     * EXACT u-boot ORDER (xhci_mem_init):
     *   ERDP (erst_dequeue) → ERST_SIZE → ERST_BASE LAST.
     * Writing ERST_BASE "attaches" the event ring to the interrupter; that's
     * why it comes last. */
    memset(g_evt_ring, 0, sizeof(g_evt_ring));
    g_evt_idx = 0; g_evt_cycle = 1;
    g_erst[0] = (uint64_t)(uintptr_t)g_evt_ring;   /* ring segment base */
    g_erst[1] = EVENT_RING_TRBS;                   /* segment size (low), high=0 */
    cache_clean(g_erst, sizeof(g_erst));
    cache_clean(g_evt_ring, sizeof(g_evt_ring));
    dsb();
    /* ERDP = address of the 1st event TRB (no EHB at init). */
    wr64(g_rt + XHCI_RT_ERDP, (uint64_t)(uintptr_t)g_evt_ring);
    /* ERST_SIZE: preserve [31:16], set nb segments = 1 in [15:0]. */
    { uint32_t sz = rd32(g_rt + XHCI_RT_ERSTSZ) & 0xFFFF0000u;
      wr32(g_rt + XHCI_RT_ERSTSZ, sz | 1u); }
    /* ERST_BASE LAST. */
    wr64(g_rt + XHCI_RT_ERSTBA, (uint64_t)(uintptr_t)g_erst);

    /* DNCTRL = 0: avoids spurious Device Notification Events that would
     * pollute the event ring (u-boot xhci_mem_init). */
    wr32(g_op + XHCI_OP_DNCTRL, 0);


    /* 5b) ENABLE Interrupter 0 (IMAN.IE=1) — CRUCIAL: on many xHCI
     * controllers, if the interrupter is not "enabled", the HC does NOT WRITE
     * the Event TRBs into the event ring -> no Command Completion (symptom
     * Enable Slot cc=-1). u-boot arms IE even if it polls. IMOD=0
     * (no moderation). We leave USBCMD.INTE off (no CPU IRQ, polling). */
    wr32(g_rt + XHCI_RT_IMOD, 0);
    wr32(g_rt + XHCI_RT_IMAN, rd32(g_rt + XHCI_RT_IMAN) | IMAN_IE);
    dsb();

    /* 6) Start the HC (Run). CPU-level polling (USBCMD.INTE off). */
    wr32(g_op + XHCI_OP_USBCMD, rd32(g_op + XHCI_OP_USBCMD) | USBCMD_RS);

    for (int i = 0; i < 1000; i++) {
        if (!(rd32(g_op + XHCI_OP_USBSTS) & USBSTS_HCH)) break;
        udelay(100);
    }
    printf("[xhci] HC up : slots=%lu ports=%lu ctx=%lu op=+0x%lx rt=+0x%lx db=+0x%lx\n",
           (unsigned long)g_max_slots, (unsigned long)g_max_ports,
           (unsigned long)g_ctx_stride,
           (unsigned long)(g_op - XHCI_BASE), (unsigned long)(g_rt - XHCI_BASE),
           (unsigned long)(g_db - XHCI_BASE));

    /* 7) POWER all root ports (PORTSC.PP=1). Some controllers
     * (DWC3/RK3328) start with PP=0: without PP, no connection is detected
     * (CCS stays 0). We arm PP then leave time for power-on (debounce).
     * We also dump PORTSC of each port for diagnostics. */
    for (uint32_t p = 0; p < g_max_ports; p++) {
        uintptr_t pr = g_op + XHCI_OP_PORTSC(p);
        uint32_t sc = rd32(pr);
        sc &= ~PORTSC_RW1C;                 /* do not re-clear the changes */
        wr32(pr, sc | PORTSC_PP);
    }
    dsb();
    udelay(20000);                          /* ~20 ms power stabilization */
    for (uint32_t p = 0; p < g_max_ports; p++) {
        uint32_t sc = rd32(g_op + XHCI_OP_PORTSC(p));
        printf("[xhci] PORTSC[%lu]=0x%08lx (CCS=%lu PED=%lu PP=%lu spd=%lu)\n",
               (unsigned long)p, (unsigned long)sc,
               (unsigned long)((sc & PORTSC_CCS) ? 1 : 0),
               (unsigned long)((sc & PORTSC_PED) ? 1 : 0),
               (unsigned long)((sc & PORTSC_PP)  ? 1 : 0),
               (unsigned long)((sc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT));
    }
    return USB_OK;
}


/* Finds the first port with a connected device; resets it; returns the
 * speed. (Internal RTL8153B: the SuperSpeed port must be connected.) */
static usb_status_t xhci_port_reset(usb_speed_t *speed)
{
    /* Wait for a port to present a connection (CCS=1). The internal RTL8153B
     * (USB link wired to the SoC) may take time to establish after power-on;
     * we scan ALL ports (SS + USB2) with a wide timeout. */
    int found = -1;
    for (int tries = 0; tries < 150 && found < 0; tries++) {   /* ~1.5 s max */
        for (uint32_t p = 0; p < g_max_ports; p++) {
            uint32_t sc = rd32(g_op + XHCI_OP_PORTSC(p));
            if (sc & PORTSC_CCS) { found = (int)p; break; }
        }
        if (found < 0)
            udelay(10000);                  /* 10 ms between scans */
    }
    if (found < 0)
        return USB_ENOTCONN;
    printf("[xhci] connection detected on port %d after waiting\n", found);


    uintptr_t pr = g_op + XHCI_OP_PORTSC((uint32_t)found);
    /* Write PR=1 (preserve PP, clear RW1C changes). */
    uint32_t sc = rd32(pr);
    sc &= ~PORTSC_RW1C;                 /* do not re-clear by mistake */
    wr32(pr, (sc & ~PORTSC_PED) | PORTSC_PR | PORTSC_PP);
    /* wait for Port Reset Change. */
    for (int i = 0; i < 500; i++) {
        sc = rd32(pr);
        if (sc & PORTSC_PRC) break;
        udelay(1000);
    }
    /* clear PRC/CSC (RW1C). */
    wr32(pr, (rd32(pr) & ~PORTSC_PED) | PORTSC_PRC | PORTSC_CSC | PORTSC_PP);
    udelay(2000);

    sc = rd32(pr);
    if (!(sc & PORTSC_PED))
        return USB_EIO;                 /* port not enabled after reset */

    uint32_t psi = (sc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
    /* Standard xHCI PSI: 1=FS 2=LS 3=HS 4=SS (default Speed IDs). */
    switch (psi) {
    case 1: *speed = USB_SPEED_FULL;  break;
    case 2: *speed = USB_SPEED_LOW;   break;
    case 3: *speed = USB_SPEED_HIGH;  break;
    case 4: *speed = USB_SPEED_SUPER; break;
    default: *speed = USB_SPEED_SUPER; break;   /* RTL8153B = SS */
    }
    printf("[xhci] port %d connected, PED ok, PSI=%lu\n", found, (unsigned long)psi);
    return USB_OK;
}

/* Enable Slot + Address Device (builds the input context: slot + EP0). */
static usb_status_t xhci_device_alloc(usb_device_t *dev)
{
    xhci_trb_t ev;
    int cc;

    /* --- Enable Slot --- */
    cmd_push(0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    cc = evt_wait(TRB_CMD_COMPLETION, &ev, 1000);
    if (cc != CC_SUCCESS) {
        /* Diagnostic: HC state + rings to understand the absence of event. */
        printf("[xhci] Enable Slot failed cc=%d\n", cc);
        printf("  USBSTS=0x%08lx USBCMD=0x%08lx CRCR_lo=0x%08lx IMAN=0x%08lx\n",
               (unsigned long)rd32(g_op + XHCI_OP_USBSTS),
               (unsigned long)rd32(g_op + XHCI_OP_USBCMD),
               (unsigned long)rd32(g_op + XHCI_OP_CRCR),
               (unsigned long)rd32(g_rt + XHCI_RT_IMAN));
        printf("  evt_idx=%lu evt_cycle=%lu evt[0].st=0x%08lx evt[0].ctrl=0x%08lx\n",
               (unsigned long)g_evt_idx, (unsigned long)g_evt_cycle,
               (unsigned long)g_evt_ring[0].status,
               (unsigned long)g_evt_ring[0].control);
        printf("  &evt_ring=0x%08lx ERDP_lo=0x%08lx ERSTBA_lo=0x%08lx &erst=0x%08lx erst[0]=0x%08lx\n",
               (unsigned long)(uintptr_t)g_evt_ring,
               (unsigned long)rd32(g_rt + XHCI_RT_ERDP),
               (unsigned long)rd32(g_rt + XHCI_RT_ERSTBA),
               (unsigned long)(uintptr_t)g_erst,
               (unsigned long)(uint32_t)g_erst[0]);
        printf("  &cmd_ring=0x%08lx cmd[0].ctrl=0x%08lx  DCBAAP_lo=0x%08lx\n",
               (unsigned long)(uintptr_t)g_cmd_ring,
               (unsigned long)g_cmd_ring[0].control,
               (unsigned long)rd32(g_op + XHCI_OP_DCBAAP));
        return USB_EIO;

    }

    g_slot_id = (uint8_t)((ev.control >> 24) & 0xFF);
    if (g_slot_id == 0)
        return USB_EIO;

    /* --- Input Context: Input Control Context (dword0/1) + Slot + EP0 --- */
    memset(g_in_ctx, 0, sizeof(g_in_ctx));
    memset(g_dev_ctx, 0, sizeof(g_dev_ctx));

    /* Input Control Context = ctx index 0. Add flags: A0 (slot) + A1 (EP0). */
    *ctx_dword(g_in_ctx, 0, 1) = (1u << 0) | (1u << 1);   /* Add Context flags */

    /* Slot Context = ctx index 1 (in the input context). */
    uint32_t *slot = ctx_dword(g_in_ctx, 1, 0);
    /* dword0: Route String=0, Speed [23:20], Context Entries=1 [31:27]. */
    uint32_t speed_id;
    switch (dev->speed) {
    case USB_SPEED_FULL:  speed_id = 1; break;
    case USB_SPEED_LOW:   speed_id = 2; break;
    case USB_SPEED_HIGH:  speed_id = 3; break;
    default:              speed_id = 4; break;  /* SS */
    }
    slot[0] = (speed_id << 20) | (1u << 27);   /* Context Entries = 1 */
    /* dword1: Root Hub Port Number [23:16] = real port (1-based). */
    uint32_t rhport = 1;
    for (uint32_t p = 0; p < g_max_ports; p++) {
        if (rd32(g_op + XHCI_OP_PORTSC(p)) & PORTSC_CCS) { rhport = p + 1; break; }
    }
    slot[1] = (rhport << 16);

    /* EP0 Context = ctx index 2 (in the input context). */
    uint32_t *ep0 = ctx_dword(g_in_ctx, 2, 0);
    /* dword1: EP Type = Control(4) [5:3], MaxPacketSize [31:16], CErr=3 [2:1]. */
    ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)dev->max_packet0 << 16);
    /* dword2/3: TR Dequeue Pointer | DCS(1). */
    memset(g_ep0_ring, 0, sizeof(g_ep0_ring));
    g_ep0_idx = 0; g_ep0_cycle = 1;
    uint64_t trdp = (uint64_t)(uintptr_t)g_ep0_ring | 1u; /* DCS=1 */
    ep0[2] = (uint32_t)(trdp & 0xFFFFFFFFu);
    ep0[3] = (uint32_t)(trdp >> 32);

    /* Publish input context + device context + DCBAA in RAM (the HC reads
     * them via DMA). The device context will be FILLED by the HC -> we will
     * invalidate it after Address Device to reread it if needed. */
    g_dcbaa[g_slot_id] = (uint64_t)(uintptr_t)g_dev_ctx;
    cache_clean(g_in_ctx, sizeof(g_in_ctx));
    cache_clean(g_dev_ctx, sizeof(g_dev_ctx));
    cache_clean(g_dcbaa, sizeof(g_dcbaa));
    cache_clean(g_ep0_ring, sizeof(g_ep0_ring));
    dsb();


    /* --- Address Device --- */
    cmd_push((uint64_t)(uintptr_t)g_in_ctx, 0,
             TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)g_slot_id << 24));
    cc = evt_wait(TRB_CMD_COMPLETION, &ev, 1000);
    if (cc != CC_SUCCESS) {
        printf("[xhci] Address Device failed cc=%d\n", cc);
        return USB_EIO;
    }
    dev->hcd_priv = (void *)(uintptr_t)g_slot_id;
    printf("[xhci] slot %u addressed OK (rhport=%lu)\n",
           g_slot_id, (unsigned long)rhport);
    return USB_OK;
}

static void xhci_device_free(usb_device_t *dev)
{
    (void)dev;
    /* a single device; no Disable Slot for now. */
}

/* Control transfer (SETUP + optional DATA + STATUS) via EP0. */
static usb_status_t xhci_control(usb_device_t *dev, const usb_setup_t *setup,
                                 void *data, uint16_t len)
{
    (void)dev;
    xhci_trb_t ev;
    int cc;
    int in = (setup->bmRequestType & USB_DIR_IN) ? 1 : 0;

    /* xHCI: the standard SET_ADDRESS must NOT be issued on EP0 — the
     * controller addresses the device via the "Address Device" command
     * (already done in xhci_device_alloc). Issuing a real SET_ADDRESS control
     * causes a TRB Error (cc=5). We therefore treat it as a NO-OP success
     * (like u-boot, which intercepts USB_REQ_SET_ADDRESS and calls
     * xhci_address_device). */
    if ((setup->bmRequestType & 0x60) == 0x00 /* standard type */ &&
        setup->bRequest == 0x05 /* USB_REQ_SET_ADDRESS */) {
        return USB_OK;
    }

    /* We build the whole TD (SETUP [+DATA] +STATUS) BEFORE giving it to the HC
     * (the 1st TRB is written with an inverted cycle, ep0_push/is_first). */


    /* If data IN: invalidate the buffer BEFORE (the HC will write into it), so
     * as not to reintroduce stale cache lines after the transfer. */
    if (len && data && in)
        cache_invalidate(data, len);
    else if (len && data)
        cache_clean(data, len);        /* data OUT: publish what we send */

    /* --- SETUP stage (Immediate Data, 8 bytes) — 1st TRB of the TD --- */
    uint64_t setup_data;
    memcpy(&setup_data, setup, 8);
    uint32_t setup_ctrl = TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT;
    /* xHCI 1.0+: Transfer Type (TRT) control[17:16]: 3=IN data, 2=OUT data,
     * 0=no-data. Our HC is 1.10 → we set it. */
    if (len && in)       setup_ctrl |= (3u << 16);
    else if (len)        setup_ctrl |= (2u << 16);
    /* status = TRB transfer length = 8 (Interrupter target 0). */
    ep0_push(setup_data, 8, setup_ctrl, /*is_first=*/1);

    /* --- DATA stage (optional) --- */
    if (len && data) {
        uint32_t data_ctrl = TRB_TYPE(TRB_DATA_STAGE);
        if (in) data_ctrl |= TRB_DIR_IN | TRB_ISP; /* ISP only for IN */
        /* status = TRB length (bits[16:0]); TD size=0 (last/single data
         * TRB for a control ≤ maxpkt), interrupter target 0. */
        ep0_push((uint64_t)(uintptr_t)data, (uint32_t)len, data_ctrl, /*is_first=*/0);
    }

    /* --- STATUS stage --- : if data IN -> status OUT (dir=0); else status IN. */
    uint32_t status_ctrl = TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC;
    if (!(len && in))  status_ctrl |= TRB_DIR_IN;
    ep0_push(0, 0, status_ctrl, /*is_first=*/0);

    /* Give the TD to the HC (restores the SETUP cycle) + doorbell. */
    ep0_giveback_and_ring();

    /* Wait for the Transfer Event(s). There can be 1 event per TRB with
     * IOC/ISP (short data IN + status). We loop until the STATUS succeeds. */
    int seen_data_short = 0;
    for (int k = 0; k < 3; k++) {
        cc = evt_wait(TRB_TRANSFER_EVENT, &ev, 1000);
        if (cc < 0)
            return USB_ETIMEOUT;
        if (cc == CC_SHORT_PACKET) { seen_data_short = 1; continue; }
        if (cc == CC_SUCCESS)      break;
        /* Failure: trace the cc + the setup for diagnostics (rare once the
         * stack is validated). */
        printf("[xhci] control cc=%d (bmRT=0x%02x bReq=0x%02x wVal=0x%04x len=%u)\n",
               cc, setup->bmRequestType, setup->bRequest, setup->wValue,
               (unsigned)len);
        return USB_EIO;
    }


    (void)seen_data_short;

    /* Data IN: reinvalidate the buffer so usb_core reads what the HC wrote in
     * RAM (not the stale cache). */
    if (len && data && in)
        cache_invalidate(data, len);
    dmb();
    return USB_OK;
}


/* ================================================================== */
/* bulk/interrupt endpoints (Configure Endpoint + transfers)          */
/* ================================================================== */

/* DCI (Device Context Index) of an endpoint from its bEndpointAddress.
 * u-boot xhci_get_ep_index: ep_index (0-based) = ep_num*2 - (in?0:1)
 * for non-control; DCI = ep_index + 1 (index in the device context).
 *   bulk IN  ep1 (0x81) → ep_index=2 → DCI=3
 *   bulk OUT ep2 (0x02) → ep_index=3 → DCI=4
 *   int  IN  ep3 (0x83) → ep_index=6 → DCI=7                             */
static uint32_t ep_addr_to_dci(uint8_t ep_addr)
{
    uint32_t num = ep_addr & 0x0Fu;
    uint32_t in  = (ep_addr & USB_EP_DIR_IN) ? 1u : 0u;
    uint32_t ep_index = (num * 2u) - (in ? 0u : 1u);
    return ep_index + 1u;               /* DCI */
}

/* xHCI endpoint type (ep_info2 [5:3]) from the USB type + direction.
 * xHCI codes (u-boot xhci.h):
 *   ISOC_OUT=1 BULK_OUT=2 INT_OUT=3 CTRL=4 ISOC_IN=5 BULK_IN=6 INT_IN=7.
 * u-boot formula: ep_type = (bmAttributes & 3) | (dir_in << 2).           */
static uint32_t ep_xhci_type(uint8_t usb_type, int dir_in)
{
    return (uint32_t)(usb_type & 0x3u) | (dir_in ? (1u << 2) : 0u);
}

/* Enqueues a NORMAL TRB on an endpoint's ring (DCI) and rings its doorbell,
 * then waits for the Transfer Event. A single TRB per transfer (our RTL
 * frames are ≤ 2 KB, well below a TRB's 64 KB limit -> no chaining).
 * Returns USB_OK / USB_ETIMEOUT / USB_EIO; *xferred = bytes actually
 * transferred (len - residual from the completion). Algo: u-boot xhci_bulk_tx +
 * giveback_first_trb (adapted to bulk).            */
static usb_status_t ep_xfer(uint32_t dci, uint8_t ep_addr, void *buf,
                            uint32_t len, uint32_t *xferred, int is_in)
{
    if (dci >= XHCI_MAX_DCI || !g_ep_active[dci])
        return USB_EINVAL;

    xhci_trb_t *ring = g_ep_ring[dci];
    uint32_t idx = g_ep_idx[dci];
    uint32_t cyc = g_ep_cycle[dci];

    /* DMA cache coherence of the data buffer. */
    if (len && buf) {
        if (is_in) cache_invalidate(buf, len);
        else       cache_clean(buf, len);
    }

    /* Write the NORMAL TRB. As for EP0, we first write with the INVERTED
     * cycle (the HC does not consume it), then restore it just before the
     * doorbell (single-TRB -> immediate "giveback"). */
    xhci_trb_t *t = &ring[idx];
    t->param_lo = (uint32_t)((uintptr_t)buf & 0xFFFFFFFFu);
    t->param_hi = (uint32_t)((uint64_t)(uintptr_t)buf >> 32);
    /* status: TRB Transfer Length [16:0], TD Size=0, Interrupter Target 0. */
    t->status   = (len & 0x1FFFFu);
    uint32_t ctrl = TRB_TYPE(TRB_NORMAL) | TRB_IOC;
    if (is_in) ctrl |= TRB_ISP;         /* interrupt on short packet (IN) */
    /* inverted cycle while writing, then restored. */
    t->control = ctrl | (cyc ? 0u : TRB_CYCLE);
    dsb();
    /* giveback: set the correct cycle bit. */
    if (cyc) t->control |= TRB_CYCLE; else t->control &= ~TRB_CYCLE;

    /* Advance the enqueue + handle the link TRB at the end of the ring. */
    idx++;
    if (idx >= XFER_RING_TRBS - 1) {
        xhci_trb_t *lnk = &ring[XFER_RING_TRBS - 1];
        lnk->param_lo = (uint32_t)((uintptr_t)ring & 0xFFFFFFFFu);
        lnk->param_hi = (uint32_t)((uint64_t)(uintptr_t)ring >> 32);
        lnk->status   = 0;
        lnk->control  = TRB_TYPE(TRB_LINK) | (1u << 1) /*Toggle Cycle*/ |
                        (cyc ? TRB_CYCLE : 0);
        idx = 0;
        cyc ^= 1;
    }
    g_ep_idx[dci] = idx;
    g_ep_cycle[dci] = cyc;

    cache_clean(ring, sizeof(g_ep_ring[dci]));
    dsb();

    /* Slot doorbell, target = endpoint's DCI. */
    wr32(g_db + 4u * g_slot_id, dci);
    dsb();

    /* Wait for the Transfer Event (IOC). */
    xhci_trb_t ev;
    int cc = evt_wait(TRB_TRANSFER_EVENT, &ev, 2000);
    if (cc < 0)
        return USB_ETIMEOUT;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        printf("[xhci] ep 0x%02x xfer cc=%d\n", ep_addr, cc);
        return USB_EIO;
    }

    /* Bytes transferred = len - residual (residual = status[23:0] of the event). */
    uint32_t residual = ev.status & 0xFFFFFFu;
    uint32_t done = (len >= residual) ? (len - residual) : len;
    if (is_in && buf)
        cache_invalidate(buf, len);
    if (xferred) *xferred = done;
    dmb();
    return USB_OK;
}

static usb_status_t xhci_bulk(usb_device_t *dev, uint8_t ep_addr,
                              void *buf, uint32_t len, uint32_t *xferred)
{
    (void)dev;
    int is_in = (ep_addr & USB_EP_DIR_IN) ? 1 : 0;
    return ep_xfer(ep_addr_to_dci(ep_addr), ep_addr, buf, len, xferred, is_in);
}

static usb_status_t xhci_int_in(usb_device_t *dev, uint8_t ep_addr,
                                void *buf, uint32_t len, uint32_t *xferred)
{
    (void)dev;
    return ep_xfer(ep_addr_to_dci(ep_addr), ep_addr, buf, len, xferred, 1);
}

/* Configure Endpoint: builds the input context with an ep context per
 * enumerated endpoint (bulk/int), allocates their transfer rings, then issues
 * the CONFIGURE_ENDPOINT command. Called by usb_core BEFORE SET_CONFIGURATION.
 * Algo: u-boot xhci_set_configuration + xhci_init_ep_contexts_if.          */
static usb_status_t xhci_configure_eps(usb_device_t *dev)
{
    if (g_slot_id == 0)
        return USB_EIO;

    /* Start from a clean input context (we do NOT reset the slot/EP0 rings:
     * they are already active on the HC side; we just copy the slot ctx +
     * add the new endpoints). */
    memset(g_in_ctx, 0, sizeof(g_in_ctx));

    uint32_t *icc = ctx_dword(g_in_ctx, 0, 0);
    /* Input Control Context: add_flags dword1, drop_flags dword0. */
    icc[0] = 0;                             /* drop flags */
    uint32_t add_flags = (1u << 0);         /* A0 = Slot Context (mandatory) */
    uint32_t max_dci = 1;                   /* at least EP0 (DCI 1) */

    /* Slot Context (ctx index 1): copy speed + rhport (like device_alloc)
     * and set Context Entries to the largest configured DCI. */
    uint32_t *slot = ctx_dword(g_in_ctx, 1, 0);
    uint32_t speed_id;
    switch (dev->speed) {
    case USB_SPEED_FULL:  speed_id = 1; break;
    case USB_SPEED_LOW:   speed_id = 2; break;
    case USB_SPEED_HIGH:  speed_id = 3; break;
    default:              speed_id = 4; break;  /* SS */
    }
    uint32_t rhport = 1;
    for (uint32_t p = 0; p < g_max_ports; p++) {
        if (rd32(g_op + XHCI_OP_PORTSC(p)) & PORTSC_CCS) { rhport = p + 1; break; }
    }
    slot[1] = (rhport << 16);

    /* For each enumerated data endpoint: build its ep context + its ring. */
    for (uint8_t i = 0; i < dev->num_endpoints; i++) {
        const usb_endpoint_t *ep = &dev->endpoints[i];
        if (ep->type != USB_EP_XFER_BULK && ep->type != USB_EP_XFER_INT)
            continue;
        int is_in = (ep->address & USB_EP_DIR_IN) ? 1 : 0;
        uint32_t dci = ep_addr_to_dci(ep->address);
        if (dci >= XHCI_MAX_DCI)
            continue;

        add_flags |= (1u << dci);           /* Add Context flag for this DCI */
        if (dci > max_dci) max_dci = dci;

        /* Allocate/initialize the endpoint's transfer ring. */
        memset(g_ep_ring[dci], 0, sizeof(g_ep_ring[dci]));
        g_ep_idx[dci]   = 0;
        g_ep_cycle[dci] = 1;
        g_ep_active[dci] = 1;

        /* EP context = ctx index (dci + 1) in the input context (icc=0, slot=1,
         * EP0=2, epN=DCI+1). */
        uint32_t *epc = ctx_dword(g_in_ctx, dci + 1u, 0);
        /* dword0: Interval/Mult/MaxESIT-hi. For SS interrupt, bInterval is
         * an exponent (2^(bInterval-1) microframes); u-boot computes it via
         * xhci_get_endpoint_interval. For a bring-up we set the raw
         * (bounded) interval for INT and 0 for bulk. */
        uint32_t interval = 0;
        if (ep->type == USB_EP_XFER_INT) {
            interval = ep->interval ? (ep->interval - 1u) : 0u;
            if (interval > 15u) interval = 15u;
        }
        epc[0] = (interval & 0xFFu) << 16;
        /* dword1: EP Type [5:3], MaxPacketSize [31:16], CErr=3 [2:1],
         * MaxBurst [15:8] (0 here: 1 packet per burst, enough for bring-up). */
        uint32_t ep_type = ep_xhci_type(ep->type, is_in);
        epc[1] = (ep_type << 3) | (3u << 1) |
                 ((uint32_t)ep->max_packet << 16);
        /* dword2/3: TR Dequeue Pointer | DCS(1). */
        uint64_t trdp = (uint64_t)(uintptr_t)g_ep_ring[dci] | 1u;
        epc[2] = (uint32_t)(trdp & 0xFFFFFFFFu);
        epc[3] = (uint32_t)(trdp >> 32);
        /* dword4: Average TRB Length (≥ 8 recommended; we set the max packet). */
        uint32_t *epc4 = ctx_dword(g_in_ctx, dci + 1u, 4);
        epc4[0] = ep->max_packet ? ep->max_packet : 8u;

        cache_clean(g_ep_ring[dci], sizeof(g_ep_ring[dci]));
    }

    /* Context Entries = largest DCI (slot ctx dword0 [31:27]). */
    slot[0] = (speed_id << 20) | (max_dci << 27);
    icc[1] = add_flags;                     /* add_flags dword1 */

    cache_clean(g_in_ctx, sizeof(g_in_ctx));
    dsb();

    /* Issue CONFIGURE_ENDPOINT (slot in control[31:24]). */
    cmd_push((uint64_t)(uintptr_t)g_in_ctx, 0,
             TRB_TYPE(TRB_CONFIGURE_EP) | ((uint32_t)g_slot_id << 24));
    xhci_trb_t ev;
    int cc = evt_wait(TRB_CMD_COMPLETION, &ev, 1000);
    if (cc != CC_SUCCESS) {
        printf("[xhci] Configure Endpoint failed cc=%d (add=0x%08lx maxdci=%lu)\n",
               cc, (unsigned long)add_flags, (unsigned long)max_dci);
        return USB_EIO;
    }
    printf("[xhci] Configure Endpoint OK (add=0x%08lx maxdci=%lu)\n",
           (unsigned long)add_flags, (unsigned long)max_dci);
    return USB_OK;
}

const usb_hcd_ops_t xhci_hcd_ops = {
    .name         = "xhci-rk3328",
    .init         = xhci_hc_init,
    .port_reset   = xhci_port_reset,
    .device_alloc = xhci_device_alloc,
    .device_free  = xhci_device_free,
    .control      = xhci_control,
    .bulk         = xhci_bulk,
    .int_in       = xhci_int_in,
    .configure_eps = xhci_configure_eps,   /* bulk/int endpoints */
};


usb_status_t xhci_init(void)
{
    return xhci_hc_init();
}

#endif /* MMU_QEMU */
