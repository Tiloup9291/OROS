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
 * gmac.c — Raw L2 GMAC driver (Synopsys DWMAC 1000) for RK3328
 *
 * Written in-house from the algorithm read in U-Boot (offsets/bits CITED,
 * never deduced) :
 *   - drivers/net/designware.c / designware.h: MAC/DMA registers, chained
 *     descriptors, DMA reset (DMAMAC_SRST), busmode/opmode init, TX/RX polling, MDIO.
 *   - drivers/net/gmac_rockchip.c : rk3328_gmac_set_to_rgmii (GRF mac_con[1] =
 *     PHY_INTF_SEL RGMII + delay enable ; mac_con[0] = delay values) and
 *     rk3328_gmac_fix_mac_speed (mac_con[1] bits 11-12 = clk_sel speed).
 *   - arch/arm/include/asm/arch-rockchip/grf_rk3328.h : mac_con[] at GRF+0x900.
 *   - DT rk3328-orangepi-r1-plus-lts.dts : @0xFF540000, PHY YT8531C @MDIO 0,
 *     rgmii-id, reset gpio1 PC2 active low (assert 15 ms, deassert 50 ms).
 *
 * Architecture choice (like the SDMMC driver): U-Boot has ALREADY
 * configured at boot the GMAC clocks (CRU SCLK_MAC2IO*, ACLK/PCLK) and the RGMII
 * source (clock_in_out="input" → clock comes from the PHY). We therefore INHERIT
 * the U-Boot clock and do NOT touch the CRU clock mux (switching it is risky).
 * We just (re)apply the GRF RGMII config (idempotent), then
 * perform OUR DMA reset + descriptors + MDIO/PHY + TX/RX. RX IRQ DISABLED.
 *
 * On QEMU (-DMMU_QEMU): no RK3328 GMAC → all functions return
 * GMAC_ENODEV (no MMIO access).
 */

#include "gmac.h"
#include "gpio.h"
#include "timer.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/* QEMU: complete neutralization                                      */
/* ================================================================== */
#if defined(MMU_QEMU)

gmac_status_t gmac_init(const uint8_t mac[6], gmac_info_t *info) {
    (void)mac; if (info) memset(info, 0, sizeof(*info));
    return GMAC_ENODEV;
}
int gmac_link_up(void) { return 0; }
gmac_status_t gmac_send(const void *f, uint32_t l) { (void)f; (void)l; return GMAC_ENODEV; }
gmac_status_t gmac_poll_recv(void *b, uint32_t s, uint32_t *o) {
    (void)b; (void)s; if (o) *o = 0; return GMAC_ENODEV;
}
void gmac_get_mac(uint8_t out[6]) { memset(out, 0, 6); }
int gmac_get_link_speed(void) { return 0; }
int gmac_mdio_read(uint32_t a, uint32_t r) { (void)a; (void)r; return -1; }
int gmac_mdio_write(uint32_t a, uint32_t r, uint16_t v) { (void)a; (void)r; (void)v; return -1; }
void gmac_phy_set_rgmii_tuning(uint32_t t, uint32_t r, int i) { (void)t; (void)r; (void)i; }
void gmac_tx_diag(void) { }


#else  /* ================= BOARD RK3328 ================= */

/* ------------------------------------------------------------------ */
/* Hardware addresses (DT)                                   */
/* ------------------------------------------------------------------ */
#define GMAC_BASE       0xFF540000UL              /* gmac2io */
#define GMAC_MAC        (GMAC_BASE + 0x0000UL)    /* eth_mac_regs */
#define GMAC_DMA        (GMAC_BASE + 0x1000UL)    /* eth_dma_regs (DW_DMA_BASE_OFFSET) */
#define GRF_BASE        0xFF100000UL
#define GRF_MAC_CON0    (GRF_BASE + 0x0900UL)     /* mac_con[0] : delays */
#define GRF_MAC_CON1    (GRF_BASE + 0x0904UL)     /* mac_con[1] : intf sel + clk sel + delay ena */
/* soc_con[] at GRF+0x400 (grf_rk3328.h: soc_con[11]). soc_con[4] = +0x410.
 * bit14 = EXTERNAL RGMII clock selection for SCLK_MAC2IO_EXT (clk_mac2io_ext). */
#define GRF_SOC_CON4    (GRF_BASE + 0x0410UL)


/* PHY reset: gpio1 RK_PC2, active low (DT eth_phy_reset_pin). */
#define PHY_RESET_BANK  GPIO_BANK1
#define PHY_RESET_PIN   GPIO_PIN(GPIO_GROUP_C, 2)
#define PHY_MDIO_ADDR   0u                        /* yt8531c: ethernet-phy@0 */

/* ------------------------------------------------------------------ */
/* MAC registers (offsets designware.h: struct eth_mac_regs @0x0000)   */
/* ------------------------------------------------------------------ */
#define MAC_CONF        0x00
#define MAC_FRAMEFILT   0x04
#define MAC_MIIADDR     0x10
#define MAC_MIIDATA     0x14
#define MAC_FLOWCTRL    0x18
#define MAC_VERSION     0x20
#define MAC_INTMASK     0x3C
#define MAC_ADDR0HI     0x40
#define MAC_ADDR0LO     0x44

/* MAC conf bits (designware.h). */
#define CONF_FRAMEBURST (1u << 21)
#define CONF_PORTSELECT (1u << 15)   /* MII_PORTSELECT : 1 = MII (10/100), 0 = GMII (1000) */
#define CONF_FES_100    (1u << 14)
#define CONF_DISABLERXOWN (1u << 13)
#define CONF_FULLDPLX   (1u << 11)
#define CONF_TXENABLE   (1u << 3)
#define CONF_RXENABLE   (1u << 2)

/* framefilt bits: RA = receive all (promiscuous mode) — useful to capture
 * raw L2 traffic / EtherCAT. bit31 = RA (Receive All). */
#define FILT_RECEIVE_ALL   (1u << 31)
#define FILT_PROMISC       (1u << 0)   /* PR : promiscuous */
#define FILT_PASS_ALL_MC   (1u << 4)   /* PM : pass all multicast */

/* MII address register (MDIO clause-22). */
#define MII_BUSY        (1u << 0)
#define MII_WRITE       (1u << 1)
#define MII_CLKRANGE_150_250M  (0x10u)   /* CR field (PCLK 150-250 MHz) */
#define MIIADDRSHIFT    11
#define MIIREGSHIFT     6
#define MII_REGMSK      (0x1Fu << 6)
#define MII_ADDRMSK     (0x1Fu << 11)

/* ------------------------------------------------------------------ */
/* DMA registers (offsets designware.h: struct eth_dma_regs @0x1000)   */
/* ------------------------------------------------------------------ */
#define DMA_BUSMODE     0x00
#define DMA_TXPOLL      0x04
#define DMA_RXPOLL      0x08
#define DMA_RXDESCLIST  0x0C
#define DMA_TXDESCLIST  0x10
#define DMA_STATUS      0x14
#define DMA_OPMODE      0x18
#define DMA_INTENABLE   0x1C

/* Bus mode bits. */
#define BUSMODE_SWR     (1u << 0)     /* DMAMAC_SRST */
#define BUSMODE_FIXEDBURST (1u << 16)
#define BUSMODE_PRIORXTX_41 (3u << 14)
#define GMAC_DMA_PBL    8u
#define BUSMODE_PBL     (GMAC_DMA_PBL << 8)

/* Op mode bits. */
#define OPMODE_STOREFWD (1u << 21)
#define OPMODE_FLUSHTX  (1u << 20)
#define OPMODE_TXSTART  (1u << 13)
#define OPMODE_RXSTART  (1u << 1)

#define DMA_POLL_DATA   0xFFFFFFFFu

/* Descriptor (struct dmamacdescr): status, cntl, addr, next. */
typedef struct {
    volatile uint32_t status;   /* txrx_status */
    volatile uint32_t cntl;     /* dmamac_cntl */
    volatile uint32_t addr;     /* dmamac_addr (buffer) */
    volatile uint32_t next;     /* dmamac_next  (chaining) */
} gmac_desc_t;

/* status/cntl bits ("normal" mode, NOT ALTDESCRIPTOR). */
#define DESC_TXSTS_OWNBYDMA   (1u << 31)
#define DESC_RXSTS_OWNBYDMA   (1u << 31)
#define DESC_RXSTS_ERROR      (1u << 15)
#define DESC_RXSTS_FRMLENMSK  (0x3FFFu << 16)
#define DESC_RXSTS_FRMLENSHFT 16
#define DESC_RXSTS_RXLAST     (1u << 8)
#define DESC_RXSTS_RXFIRST    (1u << 9)

#define DESC_TXCTRL_TXLAST    (1u << 30)
#define DESC_TXCTRL_TXFIRST   (1u << 29)
#define DESC_TXCTRL_TXCHAIN   (1u << 24)
#define DESC_TXCTRL_SIZE1MASK (0x7FFu << 0)

#define DESC_RXCTRL_RXCHAIN   (1u << 24)
#define DESC_RXCTRL_SIZE1MASK (0x7FFu << 0)

#define MAC_MAX_FRAME_SZ      1600u

/* ------------------------------------------------------------------ */
/* Descriptor rings + DMA buffers (static, 64-aligned)                 */
/* ------------------------------------------------------------------ */
#define TX_DESCR_NUM   8u
#define RX_DESCR_NUM   8u
#define ETH_BUFSIZE    2048u

static gmac_desc_t g_tx_desc[TX_DESCR_NUM] __attribute__((aligned(64)));
static gmac_desc_t g_rx_desc[RX_DESCR_NUM] __attribute__((aligned(64)));
static uint8_t     g_tx_buf[TX_DESCR_NUM][ETH_BUFSIZE] __attribute__((aligned(64)));
static uint8_t     g_rx_buf[RX_DESCR_NUM][ETH_BUFSIZE] __attribute__((aligned(64)));

static uint32_t g_tx_cur;
static uint32_t g_rx_cur;
static uint8_t  g_mac[6];
static int      g_link;
static int      g_speed;
static int      g_duplex;
static uint32_t g_last_tx_status;   /* last TX descriptor status (diag) */
static uint32_t g_tx_calls;         /* number of accepted gmac_send calls (diag) */

/* ------------------------------------------------------------------ */
/* MMIO + DMA cache + delay helpers (hcd_xhci.c / sdmmc.c)          */
/* ------------------------------------------------------------------ */
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void     wr32(uintptr_t a, uint32_t v) {
    *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory");
}

/* CLEAN (dc cvac): what the CPU writes and the GMAC DMA reads
 * (descriptors, TX buffers) -> push to the Point of Coherency. */
static inline void cache_clean(const void *addr, uint32_t size) {
    uintptr_t p = (uintptr_t)addr & ~63UL, end = (uintptr_t)addr + size;
    for (; p < end; p += 64) __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
/* INVALIDATE (dc ivac): what the GMAC DMA writes and the CPU reads
 * (RX descriptors, RX buffers) -> force a re-read from RAM. */
static inline void cache_invalidate(void *addr, uint32_t size) {
    uintptr_t p = (uintptr_t)addr & ~63UL, end = (uintptr_t)addr + size;
    for (; p < end; p += 64) __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(uint32_t us) {
    uint64_t d = timer_now_ticks() + timer_us_to_ticks(us);
    while (timer_now_ticks() < d) { __asm__ volatile("nop"); }
}
static void mdelay(uint32_t ms) { udelay(ms * 1000u); }

/* GRF/CRU write-mask: the upper 16 bits = write mask. */
static inline void grf_write(uintptr_t reg, uint32_t mask, uint32_t val) {
    wr32(reg, (mask << 16) | (val & mask));
}

/* ------------------------------------------------------------------ */
/* Clause-22 MDIO (designware.c dw_mdio_read/write)                    */
/* ------------------------------------------------------------------ */
int gmac_mdio_read(uint32_t phy_addr, uint32_t reg) {
    uint32_t miiaddr = ((phy_addr << MIIADDRSHIFT) & MII_ADDRMSK) |
                       ((reg << MIIREGSHIFT) & MII_REGMSK);
    wr32(GMAC_MAC + MAC_MIIADDR, miiaddr | MII_CLKRANGE_150_250M | MII_BUSY);
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(1000000);
    while (timer_now_ticks() < deadline) {
        if (!(rd32(GMAC_MAC + MAC_MIIADDR) & MII_BUSY))
            return (int)(rd32(GMAC_MAC + MAC_MIIDATA) & 0xFFFF);
        udelay(10);
    }
    return -1;
}

int gmac_mdio_write(uint32_t phy_addr, uint32_t reg, uint16_t val) {
    wr32(GMAC_MAC + MAC_MIIDATA, val);
    uint32_t miiaddr = ((phy_addr << MIIADDRSHIFT) & MII_ADDRMSK) |
                       ((reg << MIIREGSHIFT) & MII_REGMSK) | MII_WRITE;
    wr32(GMAC_MAC + MAC_MIIADDR, miiaddr | MII_CLKRANGE_150_250M | MII_BUSY);
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(1000000);
    while (timer_now_ticks() < deadline) {
        if (!(rd32(GMAC_MAC + MAC_MIIADDR) & MII_BUSY))
            return 0;
        udelay(10);
    }
    return -1;
}

/* Standard MII registers (clause-22). */
#define MII_BMCR      0x00
#define MII_BMSR      0x01
#define MII_PHYID1    0x02
#define MII_PHYID2    0x03
#define MII_ADVERTISE 0x04
#define MII_CTRL1000  0x09
#define MII_STAT1000  0x0A
#define BMCR_RESET    (1u << 15)
#define BMCR_ANENABLE (1u << 12)
#define BMCR_ANRESTART (1u << 9)
#define BMSR_LSTATUS  (1u << 2)
#define BMSR_ANEGCOMPLETE (1u << 5)
#define ADVERTISE_ALL 0x0DE1   /* 10/100 half+full + selector 802.3 */
#define CTRL1000_ADV  0x0300   /* 1000full + 1000half */

/* ------------------------------------------------------------------ */
/* Motorcomm YT8531 PHY: EXTENDED registers + internal RGMII delays    */
/* (motorcomm.c). The PHY is wired in `rgmii-id` → the RX/TX delays     */
/* are applied BY THE PHY (ext register 0xA003), NOT by the SoC.       */
/* Extended register access: PAGE_SELECT(0x1E)=addr, PAGE_DATA(0x1F)=data.*/
/* ------------------------------------------------------------------ */
#define YT_PHY_ID              0x4F51E91Bu   /* YT8531 */
#define YTPHY_PAGE_SELECT      0x1E
#define YTPHY_PAGE_DATA        0x1F
#define YT8531_CHIP_CONFIG_REG 0xA001
#define YT8531_CCR_RXC_DLY_EN  (1u << 8)
/* SyncE clock output of the PHY (motorcomm.c) — CRITICAL for TX 1000 in
 * `clock_in_out="input"` mode: the RK3328 SoC expects a 125 MHz RGMII
 * reference clock on its clkin pin, PROVIDED BY THE PHY. The board DTS
 * requires it: `motorcomm,clk-out-frequency-hz = <125000000>` + `keep-pll-enabled` +
 * `auto-sleep-disabled`. Without this 125 MHz output, the MAC has no GTX_CLK →
 * TX physically silent (txframecount_g=0) while RX works. */
#define YT8531_SYNCE_CFG_REG   0xA012
#define YT8531_SCR_SYNCE_ENABLE     (1u << 6)   /* enables the clock output */
#define YT8531_SCR_CLK_FRE_SEL_125M (1u << 4)   /* 1 = 125M, 0 = 25M */
#define YT8531_SCR_CLK_SRC_MASK     (7u << 1)   /* GENMASK(3,1) */
#define YT8531_SCR_CLK_SRC_PLL_125M (0u << 1)   /* source = PLL 125M */
#define YT8531_CLOCK_GATING_REG     0x000C
#define YT8531_CGR_RX_CLK_EN        (1u << 12)  /* keep-pll: RXC active without link (=0) */
#define YT8531_SLEEP_CONTROL1_REG   0x0027
#define YT8531_ESC1R_SLEEP_SW       (1u << 15)  /* auto-sleep (=0 to disable it) */
#define YT8531_RGMII_CONFIG1_REG 0xA003

#define YT8531_RC1R_TX_CLK_SEL_INV  (1u << 14)   /* inverse tx_clk_rgmii */
#define YT8531_RC1R_RX_DELAY_MASK   (0xFu << 10)  /* GENMASK(13,10) */
#define YT8531_RC1R_GE_TX_DELAY_MASK (0xFu << 0)  /* GENMASK(3,0) */
/* Delay value 1950 ps = code 13 (default tx/rx of DTS/u-boot). */
#define YT8531_RC1R_RGMII_1_950_NS  13u

/* Adjustable PHY RGMII settings for TX debug (RX validated at 1950 ps).
 * GMAC_TX_DELAY_CODE: 0..15 (×150 ps). GMAC_RX_DELAY_CODE: same.
 * GMAC_TX_CLK_INVERT: 1 = inverts tx_clk (frequent fix when RX is OK,
 * TX fails in RGMII). Adjustable here to iterate without changing the logic. */
#ifndef GMAC_TX_DELAY_CODE
#define GMAC_TX_DELAY_CODE   13u   /* 1950 ps */
#endif
#ifndef GMAC_RX_DELAY_CODE
#define GMAC_RX_DELAY_CODE   13u   /* 1950 ps */
#endif
#ifndef GMAC_TX_CLK_INVERT
#define GMAC_TX_CLK_INVERT   0u
#endif


/* Read/write of an extended PHY register (paged access). */
static int yt_read_ext(uint32_t addr, uint32_t reg) {
    if (gmac_mdio_write(addr, YTPHY_PAGE_SELECT, (uint16_t)reg) < 0) return -1;
    return gmac_mdio_read(addr, YTPHY_PAGE_DATA);
}
static int yt_write_ext(uint32_t addr, uint32_t reg, uint16_t val) {
    if (gmac_mdio_write(addr, YTPHY_PAGE_SELECT, (uint16_t)reg) < 0) return -1;
    return gmac_mdio_write(addr, YTPHY_PAGE_DATA, val);
}
static int yt_modify_ext(uint32_t addr, uint32_t reg, uint16_t mask, uint16_t set) {
    int cur = yt_read_ext(addr, reg);
    if (cur < 0) return -1;
    uint16_t v = (uint16_t)(((uint32_t)cur & ~(uint32_t)mask) | (set & mask));
    return yt_write_ext(addr, reg, v);
}

/* Configures the INTERNAL RGMII delays of the YT8531 PHY for `rgmii-id` mode
 * (RX + GE_TX delays = 1950 ps, RXC_DLY_EN=0), reproducing
 * ytphy_rgmii_clk_delay_config() from u-boot. Makes the driver autonomous (no
 * longer depends on the state left by U-Boot). */
static void yt8531_config_rgmii_id(uint32_t addr) {
    uint16_t val = (uint16_t)(
        (((uint32_t)GMAC_RX_DELAY_CODE << 10) & YT8531_RC1R_RX_DELAY_MASK) |
        (((uint32_t)GMAC_TX_DELAY_CODE <<  0) & YT8531_RC1R_GE_TX_DELAY_MASK));
#if GMAC_TX_CLK_INVERT
    val |= YT8531_RC1R_TX_CLK_SEL_INV;
#endif
    /* rgmii-id: RXC_DLY_EN must stay 0 (delay via RC1R, not via CCR). */
    yt_modify_ext(addr, YT8531_CHIP_CONFIG_REG, YT8531_CCR_RXC_DLY_EN, 0);
    yt_modify_ext(addr, YT8531_RGMII_CONFIG1_REG,
                  (uint16_t)(YT8531_RC1R_RX_DELAY_MASK | YT8531_RC1R_GE_TX_DELAY_MASK |
                             YT8531_RC1R_TX_CLK_SEL_INV),
                  val);
}

/* Enables the 125 MHz CLOCK OUTPUT of the YT8531 PHY (ext register SYNCE_CFG
 * 0xA012) + keeps the PLL active + disables auto-sleep. Reproduces the clock
 * part of `yt8531_config()` from u-boot, dictated by the board DTS:
 *   motorcomm,clk-out-frequency-hz = <125000000>  -> SYNCE_ENABLE|CLK_FRE_SEL_125M
 *                                                    |CLK_SRC=PLL_125M
 *   motorcomm,keep-pll-enabled                     -> CLOCK_GATING RX_CLK_EN=0
 *   motorcomm,auto-sleep-disabled                  -> SLEEP_CONTROL1 SLEEP_SW=0
 * -> In board mode `clock_in_out="input"`, this 125 MHz PHY output IS THE
 *   RGMII reference of the SoC: without it, no GTX_CLK → TX 1000 silent
 *   (txframecount_g=0), while RX (RXC recovered from the link) works. */
static void yt8531_config_clk_out(uint32_t addr) {
    /* 125 MHz on the clk-out pin: SYNCE + 125M + PLL 125M source. */
    yt_modify_ext(addr, YT8531_SYNCE_CFG_REG,
                  (uint16_t)(YT8531_SCR_SYNCE_ENABLE | YT8531_SCR_CLK_FRE_SEL_125M |
                             YT8531_SCR_CLK_SRC_MASK),
                  (uint16_t)(YT8531_SCR_SYNCE_ENABLE | YT8531_SCR_CLK_FRE_SEL_125M |
                             YT8531_SCR_CLK_SRC_PLL_125M));
    /* keep-pll-enabled: RXC active even without a cable (RX_CLK_EN=0). */
    yt_modify_ext(addr, YT8531_CLOCK_GATING_REG, YT8531_CGR_RX_CLK_EN, 0);
    /* auto-sleep-disabled: SLEEP_SW=0. */
    yt_modify_ext(addr, YT8531_SLEEP_CONTROL1_REG, YT8531_ESC1R_SLEEP_SW, 0);
}

/* Hot re-applies the PHY RGMII delays (TX debug: sweep). */
void gmac_phy_set_rgmii_tuning(uint32_t tx_code, uint32_t rx_code, int tx_invert) {

    uint32_t a = PHY_MDIO_ADDR;
    uint16_t val = (uint16_t)(
        ((rx_code << 10) & YT8531_RC1R_RX_DELAY_MASK) |
        ((tx_code <<  0) & YT8531_RC1R_GE_TX_DELAY_MASK));
    if (tx_invert) val |= YT8531_RC1R_TX_CLK_SEL_INV;
    yt_modify_ext(a, YT8531_CHIP_CONFIG_REG, YT8531_CCR_RXC_DLY_EN, 0);
    yt_modify_ext(a, YT8531_RGMII_CONFIG1_REG,
                  (uint16_t)(YT8531_RC1R_RX_DELAY_MASK | YT8531_RC1R_GE_TX_DELAY_MASK |
                             YT8531_RC1R_TX_CLK_SEL_INV),
                  val);
}

/* ------------------------------------------------------------------ */
/* Rockchip glue: RGMII mode via GRF (rk3328_gmac_set_to_rgmii)         */
/* ------------------------------------------------------------------ */
/* Default U-Boot RGMII delays (gmac_rockchip.c: tx=0x30, rx=0x10).
 * Note: the PHY is in "rgmii-id" (delays internal to the PHY); we nevertheless
 * keep the GRF config consistent with U-Boot (idempotent). */
#define RK3328_TX_DELAY  0x30u
#define RK3328_RX_DELAY  0x10u

static void rk3328_set_to_rgmii(void) {
    /* mac_con[1]: PHY_INTF_SEL = RGMII (bit4), RMII_MODE off (bit9).
     * Board mode = "rgmii-id": the RX/TX delays are applied BY THE PHY
     * (YT8531, ext register 0xA003). We therefore DISABLE the delays on the SoC
     * side (RX/TX clk delay enable = 0) to avoid a DOUBLE delay that corrupts
     * frames (symptom of test #1: link UP but TX not emitted / RX=0). GENMASK per
     * gmac_rockchip.c. */
    const uint32_t INTF_SEL_MASK = (7u << 4);      /* GENMASK(6,4) */
    const uint32_t INTF_SEL_RGMII = (1u << 4);
    const uint32_t RMII_MODE_MASK = (1u << 9);
    const uint32_t RXDLY_ENA = (1u << 1);
    const uint32_t TXDLY_ENA = (1u << 0);
    grf_write(GRF_MAC_CON1,
              INTF_SEL_MASK | RMII_MODE_MASK | RXDLY_ENA | TXDLY_ENA,
              INTF_SEL_RGMII /* SoC delays OFF (rgmii-id → PHY delays) */);
    (void)RK3328_RX_DELAY; (void)RK3328_TX_DELAY;

    /* SELECTION OF THE RGMII CLOCK SOURCE = EXTERNAL (clock_in_out="input").
     * ROOT CAUSE of missing TX (txframecount_g=0) in standalone boot: the RGMII
     * clock source was never selected by our driver. The board has
     * `clock_in_out="input"`: the 125 MHz RGMII reference clock is PROVIDED
     * BY THE YT8531 PHY (clkin pin), not generated internally by the CRU.
     *
     * In u-boot (clk_rk3328.c rk3328_gmac2io_(ext_)set_parent), selecting
     * `gmac_clkin` = setting TWO bits (each via GRF write-mask [31:16]):
     *   - mac_con[1] BIT(10) = 1  -> SCLK_MAC2IO_SRC ← external clkin
     *   - soc_con[4]  BIT(14) = 1 -> SCLK_MAC2IO_EXT ← external clkin
     * Without these bits, the MAC waits for an internal 125 MHz GTX_CLK (never
     * set in standalone boot) -> no TX frame physically emitted, while RX works
     * (RX clocked by the RXC clock that the PHY provides independently).
     * Idempotent (u-boot does the same if the GMAC was mounted before `go`). */
    grf_write(GRF_MAC_CON1, (1u << 10), (1u << 10));  /* external clkin (src) */
    grf_write(GRF_SOC_CON4, (1u << 14), (1u << 14));  /* external clkin (ext) */
}


/* MAC clock selection according to speed (rk3328_gmac_fix_mac_speed):
 * mac_con[1] bits 12:11: 0=125M(1000), 3=25M(100), 2=2.5M(10). */
static void rk3328_fix_mac_speed(int speed) {
    const uint32_t CLK_SEL_MASK = (3u << 11);   /* GENMASK(12,11) */
    uint32_t clk;
    if (speed == 10)       clk = (2u << 11);
    else if (speed == 100) clk = (3u << 11);
    else                   clk = (0u << 11);    /* 1000 */
    grf_write(GRF_MAC_CON1, CLK_SEL_MASK, clk);
}

/* ------------------------------------------------------------------ */
/* Descriptors: chained ring init (tx_descs_init/rx_descs_init)        */
/* ------------------------------------------------------------------ */
static void tx_descs_init(void) {
    for (uint32_t i = 0; i < TX_DESCR_NUM; i++) {
        g_tx_desc[i].addr   = (uint32_t)(uintptr_t)&g_tx_buf[i][0];
        g_tx_desc[i].next   = (uint32_t)(uintptr_t)&g_tx_desc[(i + 1) % TX_DESCR_NUM];
        g_tx_desc[i].cntl   = DESC_TXCTRL_TXCHAIN;
        g_tx_desc[i].status = 0;               /* CPU owner */
    }
    cache_clean(g_tx_desc, sizeof(g_tx_desc));
    wr32(GMAC_DMA + DMA_TXDESCLIST, (uint32_t)(uintptr_t)&g_tx_desc[0]);
    g_tx_cur = 0;
}

static void rx_descs_init(void) {
    /* Publish the RX buffers (zeros) before handing them to the DMA. */
    cache_clean(g_rx_buf, sizeof(g_rx_buf));
    for (uint32_t i = 0; i < RX_DESCR_NUM; i++) {
        g_rx_desc[i].addr   = (uint32_t)(uintptr_t)&g_rx_buf[i][0];
        g_rx_desc[i].next   = (uint32_t)(uintptr_t)&g_rx_desc[(i + 1) % RX_DESCR_NUM];
        g_rx_desc[i].cntl   = (MAC_MAX_FRAME_SZ & DESC_RXCTRL_SIZE1MASK) |
                              DESC_RXCTRL_RXCHAIN;
        g_rx_desc[i].status = DESC_RXSTS_OWNBYDMA;   /* DMA owner */
    }
    cache_clean(g_rx_desc, sizeof(g_rx_desc));
    wr32(GMAC_DMA + DMA_RXDESCLIST, (uint32_t)(uintptr_t)&g_rx_desc[0]);
    g_rx_cur = 0;
}

/* ------------------------------------------------------------------ */
/* HW address write (designware.c _dw_write_hwaddr)                    */
/* ------------------------------------------------------------------ */
static void write_hwaddr(const uint8_t *m) {
    uint32_t lo = m[0] | (m[1] << 8) | (m[2] << 16) | ((uint32_t)m[3] << 24);
    uint32_t hi = m[4] | (m[5] << 8);
    wr32(GMAC_MAC + MAC_ADDR0HI, hi);
    wr32(GMAC_MAC + MAC_ADDR0LO, lo);
}

/* ------------------------------------------------------------------ */
/* PHY reset (gpio1 PC2, active low) + probe/autoneg                   */
/* ------------------------------------------------------------------ */
static void phy_hard_reset(void) {
    gpio_output_setup(PHY_RESET_BANK, PHY_RESET_PIN, GPIO_LOW);  /* assert */
    mdelay(20);                                                  /* DT: 15 ms */
    gpio_set_value(PHY_RESET_BANK, PHY_RESET_PIN, GPIO_HIGH);    /* deassert */
    mdelay(60);                                                  /* DT: 50 ms */
}

/* Waits for link-up (autoneg) and determines speed/duplex via BMSR + 1000/adv. */
static int phy_startup(gmac_info_t *info) {
    uint32_t a = PHY_MDIO_ADDR;
    int id1 = gmac_mdio_read(a, MII_PHYID1);
    int id2 = gmac_mdio_read(a, MII_PHYID2);
    uint32_t phy_id = ((uint32_t)(id1 & 0xFFFF) << 16) | (uint32_t)(id2 & 0xFFFF);
    printf("[gmac] PHY @%u id=0x%08lX\n", (unsigned)a, (unsigned long)phy_id);

    /* Config of the PHY's INTERNAL RGMII delays (rgmii-id mode) if YT8531 — makes
     * the driver autonomous and avoids the double delay (SoC delays disabled). */
    if (phy_id == YT_PHY_ID) {
        /* 125 MHz clock output of the PHY (SYNCE) = RGMII reference of the SoC
         * (clock_in_out="input"). REQUIRED for TX 1000. */
        yt8531_config_clk_out(a);
        yt8531_config_rgmii_id(a);
        printf("[gmac] YT8531 rgmii-id : delays RX/TX=1950ps (RC1R=0x%04X, CCR=0x%04X, SYNCE=0x%04X)\n",
               (unsigned)(yt_read_ext(a, YT8531_RGMII_CONFIG1_REG) & 0xFFFF),
               (unsigned)(yt_read_ext(a, YT8531_CHIP_CONFIG_REG) & 0xFFFF),
               (unsigned)(yt_read_ext(a, YT8531_SYNCE_CFG_REG) & 0xFFFF));
    }


    /* Software reset + restart autoneg (advertises 10/100/1000). */
    gmac_mdio_write(a, MII_ADVERTISE, ADVERTISE_ALL);
    gmac_mdio_write(a, MII_CTRL1000, CTRL1000_ADV);
    gmac_mdio_write(a, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

    /* Wait for link-up (up to ~5 s). */
    int up = 0;
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(5000000);
    while (timer_now_ticks() < deadline) {
        int bmsr = gmac_mdio_read(a, MII_BMSR);
        (void)gmac_mdio_read(a, MII_BMSR); /* BMSR latched: reread */
        bmsr = gmac_mdio_read(a, MII_BMSR);
        if (bmsr >= 0 && (bmsr & BMSR_LSTATUS)) { up = 1; break; }
        mdelay(50);
    }
    if (!up) { printf("[gmac] PHY: no link\n"); return -1; }

    /* Speed/duplex determination via 1000BASE-T status + advertise results.
     * YT8531C also exposes a "specific status" register (0x11): we use it
     * to read resolved speed/duplex (bits 14:15 speed, 13 duplex, 10 link). */
    int spd = 1000, dpx = 1;
    int ssr = gmac_mdio_read(a, 0x11);
    if (ssr >= 0) {
        uint32_t s = (uint32_t)ssr;
        uint32_t sp = (s >> 14) & 3u;    /* 0=10,1=100,2=1000 */
        spd = (sp == 2) ? 1000 : (sp == 1) ? 100 : 10;
        dpx = (s & (1u << 13)) ? 1 : 0;
    } else {
        /* Fallback: 1000BASE-T status. */
        int st1000 = gmac_mdio_read(a, MII_STAT1000);
        if (st1000 >= 0 && (st1000 & 0x0800)) { spd = 1000; dpx = 1; }
        else {
            int adv = gmac_mdio_read(a, MII_ADVERTISE);
            int lpa = gmac_mdio_read(a, 0x05);
            int common = (adv & lpa) & 0xFFFF;
            if (common & 0x0100) { spd = 100; dpx = 1; }
            else if (common & 0x0080) { spd = 100; dpx = 0; }
            else if (common & 0x0040) { spd = 10; dpx = 1; }
            else { spd = 10; dpx = 0; }
        }
    }
    g_speed = spd; g_duplex = dpx; g_link = 1;
    if (info) { info->phy_id = phy_id; info->phy_addr = a; info->link = 1;
                info->speed = spd; info->duplex = dpx; }
    printf("[gmac] link UP : %d Mbit/s %s duplex\n", spd, dpx ? "full" : "half");
    return 0;
}

/* Applies the speed/duplex to the MAC (designware.c dw_adjust_link). */
static void mac_adjust_link(int speed, int duplex) {
    uint32_t conf = rd32(GMAC_MAC + MAC_CONF) | CONF_FRAMEBURST | CONF_DISABLERXOWN;
    if (speed != 1000) conf |= CONF_PORTSELECT; else conf &= ~CONF_PORTSELECT;
    if (speed == 100)  conf |= CONF_FES_100;   else conf &= ~CONF_FES_100;
    if (duplex)        conf |= CONF_FULLDPLX;   else conf &= ~CONF_FULLDPLX;
    wr32(GMAC_MAC + MAC_CONF, conf);
    rk3328_fix_mac_speed(speed);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
gmac_status_t gmac_init(const uint8_t mac[6], gmac_info_t *info) {
    static const uint8_t default_mac[6] = { 0x02, 0x00, 0x5A, 0xEC, 0xA7, 0x01 };
    memcpy(g_mac, mac ? mac : default_mac, 6);
    if (info) memset(info, 0, sizeof(*info));
    g_link = 0; g_speed = 0; g_duplex = 0;

    printf("[gmac] init DWMAC1000 @0x%08lX (ver=0x%08lX)\n",
           (unsigned long)GMAC_BASE, (unsigned long)rd32(GMAC_MAC + MAC_VERSION));

    /* 1) RGMII glue (GRF) — idempotent (U-Boot already did it). */
    rk3328_set_to_rgmii();

    /* 2) Hardware reset of the PHY (gpio1 PC2). */
    phy_hard_reset();

    /* 3) DMA reset (DMAMAC_SRST), we're not in MII → PORTSELECT cleared. */
    wr32(GMAC_DMA + DMA_BUSMODE, rd32(GMAC_DMA + DMA_BUSMODE) | BUSMODE_SWR);
    wr32(GMAC_MAC + MAC_CONF, rd32(GMAC_MAC + MAC_CONF) & ~CONF_PORTSELECT);
    {
        uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(3000000);
        while (rd32(GMAC_DMA + DMA_BUSMODE) & BUSMODE_SWR) {
            if (timer_now_ticks() >= deadline) {
                printf("[gmac] ERROR: DMA reset timeout\n");
                return GMAC_ETIMEOUT;
            }
            mdelay(1);
        }
    }

    /* 4) Reprogram the MAC (the reset cleared the address registers). */
    write_hwaddr(g_mac);

    /* 5) Promiscuous mode: capture ALL L2 traffic (useful for raw L2 / EtherCAT). */
    wr32(GMAC_MAC + MAC_FRAMEFILT, FILT_RECEIVE_ALL | FILT_PROMISC | FILT_PASS_ALL_MC);

    /* 6) TX/RX descriptors. */
    rx_descs_init();
    tx_descs_init();

    /* 7) Bus mode + op mode (store-and-forward), start TX/RX DMA. */
    wr32(GMAC_DMA + DMA_BUSMODE, BUSMODE_FIXEDBURST | BUSMODE_PRIORXTX_41 | BUSMODE_PBL);
    wr32(GMAC_DMA + DMA_OPMODE,
         rd32(GMAC_DMA + DMA_OPMODE) | OPMODE_FLUSHTX | OPMODE_STOREFWD);
    wr32(GMAC_DMA + DMA_OPMODE,
         rd32(GMAC_DMA + DMA_OPMODE) | OPMODE_RXSTART | OPMODE_TXSTART);

    /* 8) GMAC IRQ DISABLED (synchronous polling). */
    wr32(GMAC_DMA + DMA_INTENABLE, 0);
    wr32(GMAC_MAC + MAC_INTMASK, 0xFFFFFFFF);

    /* 9) Start the PHY (autoneg + link) and adjust the MAC. */
    if (phy_startup(info) != 0)
        return GMAC_ENOLINK;
    mac_adjust_link(g_speed, g_duplex);

    /* 10) Enable TX/RX at the MAC level. */
    wr32(GMAC_MAC + MAC_CONF, rd32(GMAC_MAC + MAC_CONF) | CONF_RXENABLE | CONF_TXENABLE);

    if (info) memcpy(info->mac, g_mac, 6);
    printf("[gmac] init OK (MAC=%02X:%02X:%02X:%02X:%02X:%02X, %d Mbit/s)\n",
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5], g_speed);
    return GMAC_OK;
}

int gmac_link_up(void) {
    if (!g_link) return 0;
    int bmsr = gmac_mdio_read(PHY_MDIO_ADDR, MII_BMSR);
    bmsr = gmac_mdio_read(PHY_MDIO_ADDR, MII_BMSR);
    return (bmsr >= 0 && (bmsr & BMSR_LSTATUS)) ? 1 : 0;
}

void gmac_get_mac(uint8_t out[6]) { memcpy(out, g_mac, 6); }

int gmac_get_link_speed(void) { return g_link ? g_speed : 0; }

#define ETH_ZLEN 60u

gmac_status_t gmac_send(const void *frame, uint32_t len) {
    if (!frame || len == 0 || len > ETH_BUFSIZE) return GMAC_EINVAL;
    if (!g_link) return GMAC_ENOLINK;

    uint32_t i = g_tx_cur;
    gmac_desc_t *d = &g_tx_desc[i];

    /* Check that the CPU owns the descriptor. */
    cache_invalidate(d, sizeof(*d));
    if (d->status & DESC_TXSTS_OWNBYDMA) {
        printf("[gmac] TX: descriptor busy (DMA owner)\n");
        return GMAC_EIO;
    }

    memcpy(g_tx_buf[i], frame, len);
    if (len < ETH_ZLEN) {                         /* minimum frame padding. */
        memset(g_tx_buf[i] + len, 0, ETH_ZLEN - len);
        len = ETH_ZLEN;
    }
    cache_clean(g_tx_buf[i], len);

    d->cntl = (d->cntl & ~DESC_TXCTRL_SIZE1MASK) |
              (len & DESC_TXCTRL_SIZE1MASK) |
              DESC_TXCTRL_TXLAST | DESC_TXCTRL_TXFIRST | DESC_TXCTRL_TXCHAIN;
    d->status = DESC_TXSTS_OWNBYDMA;
    cache_clean(d, sizeof(*d));

    g_tx_cur = (i + 1) % TX_DESCR_NUM;

    /* Start/restart the transmission (poll demand). */
    wr32(GMAC_DMA + DMA_TXPOLL, DMA_POLL_DATA);

    /* Poll-wait for the DMA to release the descriptor (WCET bounded ~10 ms). */
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(10000);
    for (;;) {
        cache_invalidate(d, sizeof(*d));
        if (!(d->status & DESC_TXSTS_OWNBYDMA)) {
            g_last_tx_status = d->status;  /* returned TX status (error bits) */
            g_tx_calls++;
            return GMAC_OK;
        }
        if (timer_now_ticks() >= deadline) {
            /* Diag: DMA STATUS (bits 20:17=TX FSM, 19:17 RX FSM), MAC CONF. */
            printf("[gmac] TX timeout : DMA_STATUS=0x%08lX MAC_CONF=0x%08lX desc.st=0x%08lX\n",
                   (unsigned long)rd32(GMAC_DMA + DMA_STATUS),
                   (unsigned long)rd32(GMAC_MAC + MAC_CONF),
                   (unsigned long)d->status);
            return GMAC_ETIMEOUT;
        }
    }
}

/* TX diagnostic: dump DMA/MAC/GRF registers + status of the last TX descriptor
 * + MMC counter of frames actually transmitted (txframecount_g @0x14C on
 * DWMAC 1000). Decodes the TX descriptor error bits (normal mode). */
void gmac_tx_diag(void) {
    uint32_t ts = g_last_tx_status;
    /* TX descriptor status bits (DWMAC1000 normal descriptor, RTL/DWC TRM) :
     *  bit0=deferred, bit1=underflow err, bit2=exc deferral, [6:3]=coll count,
     *  bit8=exc coll, bit9=late coll, bit10=no carrier, bit11=loss carrier,
     *  bit14=jabber timeout, bit15=ES (error summary). */
    printf("[gmac][diag] TX : sends=%lu, last_desc.status=0x%08lX%s%s%s%s%s%s%s\n",
           (unsigned long)g_tx_calls, (unsigned long)ts,
           (ts & (1u<<15)) ? " ES"        : "",
           (ts & (1u<<1))  ? " UNDERFLOW" : "",
           (ts & (1u<<8))  ? " EXC_COLL"  : "",
           (ts & (1u<<9))  ? " LATE_COLL" : "",
           (ts & (1u<<10)) ? " NO_CARRIER": "",
           (ts & (1u<<11)) ? " LOSS_CARR" : "",
           (ts & (1u<<14)) ? " JABBER"    : "");
    printf("[gmac][diag] DMA_STATUS=0x%08lX (TS=%lu RS=%lu) OPMODE=0x%08lX "
           "MAC_CONF=0x%08lX\n",
           (unsigned long)rd32(GMAC_DMA + DMA_STATUS),
           (unsigned long)((rd32(GMAC_DMA + DMA_STATUS) >> 20) & 7u),
           (unsigned long)((rd32(GMAC_DMA + DMA_STATUS) >> 17) & 7u),
           (unsigned long)rd32(GMAC_DMA + DMA_OPMODE),
           (unsigned long)rd32(GMAC_MAC + MAC_CONF));
    uint32_t mc1 = rd32(GRF_MAC_CON1);
    uint32_t sc4 = rd32(GRF_SOC_CON4);
    printf("[gmac][diag] GRF mac_con0=0x%08lX mac_con1=0x%08lX soc_con4=0x%08lX ; "
           "MMC txframecount_g(@0x14C)=%lu\n",
           (unsigned long)rd32(GRF_MAC_CON0),
           (unsigned long)mc1, (unsigned long)sc4,
           (unsigned long)rd32(GMAC_MAC + 0x14C));
    /* RGMII clock source: external (PHY clkin) if mac_con1 BIT10 AND
     * soc_con4 BIT14 = 1; otherwise internal (CRU) → 1000 GTX_CLK not guaranteed. */
    printf("[gmac][diag] RGMII clk src = %s (mac_con1.b10=%lu soc_con4.b14=%lu)\n",
           ((mc1 & (1u<<10)) && (sc4 & (1u<<14))) ? "EXTERNAL(PHY clkin)" : "INTERNAL(CRU)",
           (unsigned long)((mc1 >> 10) & 1u), (unsigned long)((sc4 >> 14) & 1u));
}


gmac_status_t gmac_poll_recv(void *buf, uint32_t bufsz, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!buf || bufsz == 0) return GMAC_EINVAL;

    uint32_t i = g_rx_cur;
    gmac_desc_t *d = &g_rx_desc[i];

    cache_invalidate(d, sizeof(*d));
    uint32_t status = d->status;
    if (status & DESC_RXSTS_OWNBYDMA)
        return GMAC_EAGAIN;                        /* RX empty */

    gmac_status_t ret = GMAC_OK;
    uint32_t frmlen = (status & DESC_RXSTS_FRMLENMSK) >> DESC_RXSTS_FRMLENSHFT;
    if (frmlen > 4) frmlen -= 4;                   /* remove the FCS (4 B) */

    if (status & DESC_RXSTS_ERROR) {
        ret = GMAC_EIO;
    } else {
        uint32_t n = (frmlen > bufsz) ? bufsz : frmlen;
        cache_invalidate(g_rx_buf[i], n);
        memcpy(buf, g_rx_buf[i], n);
        if (out_len) *out_len = n;
    }

    /* Return the descriptor to the DMA + advance. */
    d->status = DESC_RXSTS_OWNBYDMA;
    cache_clean(d, sizeof(*d));
    g_rx_cur = (i + 1) % RX_DESCR_NUM;

    /* Restart reception if the DMA had stopped (RX poll demand). */
    wr32(GMAC_DMA + DMA_RXPOLL, DMA_POLL_DATA);
    return ret;
}

#endif /* MMU_QEMU */
