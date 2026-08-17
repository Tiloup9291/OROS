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
 * class_r8152.c — RTL8153B USB-Ethernet class driver
 *
 * Built ON TOP of the usb_core layer (control + bulk), so CONTROLLER-INDEPENDENT.
 * Algorithm extracted from u-boot drivers/usb/eth/r8152.c + r8152.h
 * (offsets/bits/sequences QUOTED, code rewritten).
 *
 * Register access model (u-boot get_registers/set_registers):
 *   - control IN  : bmRequestType=0xC0 (vendor,device,in), bRequest=0x05,
 *                   wValue=index, wIndex=type (MCU_TYPE_PLA/USB), data[size].
 *   - control OUT : bmRequestType=0x40, bRequest=0x05, wValue=index,
 *                   wIndex=type|byte_enable, data[size].
 * generic_ocp_read/write encapsulate aligned 32-bit block read/write.
 * ocp_read_word/ocp_write_word handle word access (dword-aligned + shift).
 * ocp_reg_read/write access OCP MII registers (base 0xB000 via
 * PLA_OCP_GPHY_BASE).
 *
 * RTL8153B init sequence (RTL_VER_08/09): r8153b_init + r8153_first_init +
 * rtl8153_enable (RE|TE + RX mode). Link via PLA_PHYSTATUS (LINK_STATUS).
 * TX/RX: each frame is prefixed with a Realtek tx_desc/rx_desc.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "class_r8152.h"
#include "usb_core.h"
#include "../../arch/aarch64/timer.h"

/* ------------------------------------------------------------------ */
/* Registers / constants (QUOTED from u-boot r8152.h)                 */
/* ------------------------------------------------------------------ */
/* Vendor requests */
#define RTL8152_REQ_GET_REGS   0x05u
#define RTL8152_REQ_SET_REGS   0x05u
#define RTL8152_REQT_READ      0xC0u   /* vendor | device | dir IN  */
#define RTL8152_REQT_WRITE     0x40u   /* vendor | device | dir OUT */

/* MCU type (high wIndex) */
#define MCU_TYPE_PLA           0x0100u
#define MCU_TYPE_USB           0x0000u

/* Byte-enable */
#define BYTE_EN_DWORD          0xFFu
#define BYTE_EN_WORD           0x33u
#define BYTE_EN_BYTE           0x11u
#define BYTE_EN_SIX_BYTES      0x3Fu
#define BYTE_EN_START_MASK     0x0Fu
#define BYTE_EN_END_MASK       0xF0u

/* PLA registers */
#define PLA_IDR                0xC000u
#define PLA_RCR                0xC010u
#define PLA_RMS                0xC016u
#define PLA_RXFIFO_CTRL0       0xC0A0u
#define PLA_RXFIFO_CTRL1       0xC0A4u
#define PLA_RXFIFO_CTRL2       0xC0A8u
#define PLA_FMC                0xC0B4u
#define PLA_TCR0               0xE610u
#define PLA_TCR1               0xE612u
#define PLA_MTPS               0xE615u
#define PLA_TXFIFO_CTRL        0xE618u
#define PLA_RSTTALLY           0xE800u
#define BIST_CTRL              0xE810u
#define PLA_CR                 0xE813u
#define PLA_CRWECR             0xE81Cu
#define PLA_OOB_CTRL           0xE84Fu
#define PLA_MISC_1             0xE85Au
#define PLA_OCP_GPHY_BASE      0xE86Cu
#define PLA_SFF_STS_7          0xE8DEu
#define PLA_PHYSTATUS          0xE908u
#define PLA_BOOT_CTRL          0xE004u
#define PLA_CPCR               0xE854u
#define PLA_EEEP_CR            0xE080u
#define PLA_MAC_PWR_CTRL2      0xE0CAu
#define PLA_MAC_PWR_CTRL3      0xE0CCu

/* USB registers */
#define USB_USB_CTRL           0xD406u
#define USB_RX_BUF_TH          0xD40Cu
#define USB_RX_EARLY_TIMEOUT   0xD42Cu
#define USB_RX_EARLY_SIZE      0xD42Eu
#define USB_RX_EXTRA_AGGR_TMR  0xD432u
#define USB_UPT_RXDMA_OWN      0xD437u
#define USB_BMU_RESET          0xD4B0u
#define USB_UPS_CTRL           0xD800u
#define USB_POWER_CUT          0xD80Au
#define USB_MISC_0             0xD81Au
#define USB_MSC_TIMER          0xCBFCu
#define USB_LPM_CONFIG         0xCFD8u
#define USB_PM_CTRL_STATUS     0xD432u   /* (RTL8153A) */
#define USB_U2P3_CTRL          0xB460u

/* OCP (MII) registers */
#define OCP_BASE_MII           0xA400u
#define OCP_PHY_STATUS         0xA420u
#define OCP_POWER_CFG          0xA430u
#define OCP_ALDPS_CONFIG       0x2010u

/* Bits */
#define RCR_AAP                0x00000001u
#define RCR_APM                0x00000002u
#define RCR_AM                 0x00000004u
#define RCR_AB                 0x00000008u
#define RCR_ACPT_ALL           (RCR_AAP | RCR_APM | RCR_AM | RCR_AB)

#define PLA_CR_RST             0x10u
#define PLA_CR_RE              0x08u
#define PLA_CR_TE              0x04u

#define BIST_CTRL_SW_RESET     (0x10u << 24)
#define FMC_FCR_MCU_EN         0x0001u
#define RXDY_GATED_EN          0x0008u
#define NOW_IS_OOB             0x80u
#define MCU_BORW_EN            0x4000u
#define RE_INIT_LL             0x8000u
#define LINK_LIST_READY        0x02u
#define TCR0_AUTO_FIFO         0x0080u
#define TALLY_RESET            0x0001u
#define AUTOLOAD_DONE          0x0002u
#define RX_AGG_DISABLE         0x0010u
#define RX_ZERO_EN             0x0080u
#define CRWECR_NORMAL          0x00u
#define CRWECR_CONFIG          0xC0u
#define OWN_UPDATE             (1u << 0)
#define OWN_CLEAR              (1u << 1)
#define BMU_RESET_EP_IN        0x01u
#define BMU_RESET_EP_OUT       0x02u
#define POWER_CUT              0x0100u
#define RESUME_INDICATE        0x0001u
#define PWR_EN                 0x0001u
#define PHASE2_EN              0x0008u
#define PCUT_STATUS            0x0001u
#define MAC_CLK_SPDWN_EN       (1u << 15)
#define PLA_MCU_SPDWN_EN       (1u << 14)
#define LPM_U1U2_EN            (1u << 0)
#define U2P3_ENABLE            0x0001u
#define EEEP_CR_EEEP_TX        0x0002u
#define CPCR_RX_VLAN           0x0040u
#define PHY_STAT_MASK          0x0007u
#define PHY_STAT_LAN_ON        3u
#define PHY_STAT_PWRDN         5u
#define OCP_PHY_STATUS_ADDR    0xA420u
#define EN_ALDPS               0x0004u

/* PLA_PHYSTATUS bits (enum rtl_register_content) */
#define LINK_STATUS            0x02u

/* Version: PLA_TCR1 mask */
#define VERSION_MASK           0x7CF0u

/* Frame sizes */
#define R8152_ETH_FRAME_LEN    1514u
#define CRC_SIZE               4u
#define RTL8153_RMS            (R8152_ETH_FRAME_LEN + CRC_SIZE)
#define RTL8152_AGG_BUF_SZ     2048u
#define MTPS_JUMBO             (12u * 1024u / 64u)

/* RX FIFO thresholds (normal) */
#define RXFIFO_THR1_NORMAL     0x00080002u
#define RXFIFO_THR2_NORMAL     0x00A0u
#define RXFIFO_THR3_NORMAL     0x0110u
#define TXFIFO_THR_NORMAL2     0x01000008u

/* Detected RTL versions (subset) */
#define RTL_VER_UNKNOWN        0u
#define RTL_VER_08             8u    /* RTL8153B */
#define RTL_VER_09             9u

/* tx_desc opts1 (Realtek) */
#define TX_FS                  (1u << 31)
#define TX_LS                  (1u << 30)
/* rx_desc opts1 */
#define RX_LEN_MASK            0x7FFFu

#define R8152_WAIT_TIMEOUT     2000

/* Realtek descriptors (little-endian). */
struct tx_desc { uint32_t opts1; uint32_t opts2; };
struct rx_desc { uint32_t opts1; uint32_t opts2; uint32_t opts3;
                 uint32_t opts4; uint32_t opts5; uint32_t opts6; };

/* ------------------------------------------------------------------ */
/* DMA-aligned buffers (Normal coherent memory, MMU active — see HCD) */
/* ------------------------------------------------------------------ */
static uint8_t g_ctl_buf[64]  __attribute__((aligned(64)));  /* control regs */
static uint8_t g_tx_buf[RTL8152_AGG_BUF_SZ] __attribute__((aligned(64)));
static uint8_t g_rx_buf[RTL8152_AGG_BUF_SZ] __attribute__((aligned(64)));

static void mdelay(uint32_t ms)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks((uint64_t)ms * 1000u);
    while (timer_now_ticks() < end)
        __asm__ volatile("nop");
}

/* ================================================================== */
/* Register access via vendor control transfers */
/* ================================================================== */
/* get_registers : control IN. wValue=index, wIndex=type. */
static int rtl_get_regs(r8152_dev_t *rt, uint16_t index, uint16_t type,
                        uint16_t size, void *data)
{
    if (size > sizeof(g_ctl_buf))
        return -1;
    usb_status_t st = usb_control(rt->dev,
        RTL8152_REQT_READ, RTL8152_REQ_GET_REGS, index, type,
        g_ctl_buf, size);
    if (st != USB_OK)
        return -1;
    memcpy(data, g_ctl_buf, size);
    return 0;
}

/* set_registers : control OUT. wValue=index, wIndex=type (with byte-enable). */
static int rtl_set_regs(r8152_dev_t *rt, uint16_t index, uint16_t type,
                        uint16_t size, const void *data)
{
    if (size > sizeof(g_ctl_buf))
        return -1;
    memcpy(g_ctl_buf, data, size);
    usb_status_t st = usb_control(rt->dev,
        RTL8152_REQT_WRITE, RTL8152_REQ_SET_REGS, index, type,
        g_ctl_buf, size);
    return (st == USB_OK) ? 0 : -1;
}

/* generic_ocp_read: reads 'size' bytes (multiple of 4, index aligned 4). */
static int generic_ocp_read(r8152_dev_t *rt, uint16_t index, uint16_t size,
                            void *data, uint16_t type)
{
    uint16_t burst = 64;
    uint8_t *p = (uint8_t *)data;
    if ((size & 3) || !size || (index & 3))
        return -1;
    while (size) {
        uint16_t tx = (size < burst) ? size : burst;
        if (rtl_get_regs(rt, index, type, tx, p) < 0)
            return -1;
        index += tx; p += tx; size -= tx;
    }
    return 0;
}

/* generic_ocp_write: writes 'size' bytes with start/end byte-enable
 * u-boot generic_ocp_write. */
static int generic_ocp_write(r8152_dev_t *rt, uint16_t index, uint16_t byteen,
                             uint16_t size, const void *data, uint16_t type)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t burst = 512;
    if ((size & 3) || !size || (index & 3))
        return -1;

    uint16_t byteen_start = byteen & BYTE_EN_START_MASK;
    uint16_t byteen_end   = byteen & BYTE_EN_END_MASK;
    uint16_t be = byteen_start | (byteen_start << 4);
    if (rtl_set_regs(rt, index, type | be, 4, p) < 0)
        return -1;
    index += 4; p += 4; size -= 4;

    if (size) {
        size -= 4;
        while (size) {
            uint16_t tx = (size < burst) ? size : burst;
            if (rtl_set_regs(rt, index, type | BYTE_EN_DWORD, tx, p) < 0)
                return -1;
            index += tx; p += tx; size -= tx;
        }
        be = byteen_end | (byteen_end >> 4);
        if (rtl_set_regs(rt, index, type | be, 4, p) < 0)
            return -1;
    }
    return 0;
}

static int pla_ocp_read(r8152_dev_t *rt, uint16_t index, uint16_t size, void *d)
{ return generic_ocp_read(rt, index, size, d, MCU_TYPE_PLA); }
static int pla_ocp_write(r8152_dev_t *rt, uint16_t index, uint16_t be,
                         uint16_t size, const void *d)
{ return generic_ocp_write(rt, index, be, size, d, MCU_TYPE_PLA); }

/* ocp_read_dword / word / byte — typed access (dword-aligned + shift). */
static uint32_t ocp_read_dword(r8152_dev_t *rt, uint16_t type, uint16_t index)
{
    uint32_t data = 0;
    generic_ocp_read(rt, index, 4, &data, type);
    return data;   /* the device is little-endian, like AArch64 -> no swap needed */
}
static void ocp_write_dword(r8152_dev_t *rt, uint16_t type, uint16_t index,
                            uint32_t data)
{
    generic_ocp_write(rt, index, BYTE_EN_DWORD, 4, &data, type);
}
static uint16_t ocp_read_word(r8152_dev_t *rt, uint16_t type, uint16_t index)
{
    uint8_t shift = index & 2;
    uint16_t base = index & ~3u;
    uint32_t data = 0;
    generic_ocp_read(rt, base, 4, &data, type);
    data >>= (shift * 8);
    return (uint16_t)(data & 0xFFFF);
}
static void ocp_write_word(r8152_dev_t *rt, uint16_t type, uint16_t index,
                           uint32_t data)
{
    uint32_t mask = 0xFFFF;
    uint16_t byen = BYTE_EN_WORD;
    uint8_t shift = index & 2;
    data &= mask;
    if (index & 2) {
        byen <<= shift;
        mask <<= (shift * 8);
        data <<= (shift * 8);
        index &= ~3u;
    }
    (void)mask;
    generic_ocp_write(rt, index, byen, 4, &data, type);
}
static uint8_t ocp_read_byte(r8152_dev_t *rt, uint16_t type, uint16_t index)
{
    uint8_t shift = index & 3;
    uint16_t base = index & ~3u;
    uint32_t data = 0;
    generic_ocp_read(rt, base, 4, &data, type);
    data >>= (shift * 8);
    return (uint8_t)(data & 0xFF);
}
static void ocp_write_byte(r8152_dev_t *rt, uint16_t type, uint16_t index,
                           uint32_t data)
{
    uint32_t mask = 0xFF;
    uint16_t byen = BYTE_EN_BYTE;
    uint8_t shift = index & 3;
    data &= mask;
    if (index & 3) {
        byen <<= shift;
        mask <<= (shift * 8);
        data <<= (shift * 8);
        index &= ~3u;
    }
    (void)mask;
    generic_ocp_write(rt, index, byen, 4, &data, type);
}

/* ocp_reg_read/write — OCP MII registers (base 0xB000 via GPHY_BASE). */
static uint16_t ocp_reg_read(r8152_dev_t *rt, uint16_t addr)
{
    uint16_t ocp_base = addr & 0xF000u;
    if (ocp_base != rt->ocp_base) {
        ocp_write_word(rt, MCU_TYPE_PLA, PLA_OCP_GPHY_BASE, ocp_base);
        rt->ocp_base = ocp_base;
    }
    uint16_t ocp_index = (addr & 0x0FFFu) | 0xB000u;
    return ocp_read_word(rt, MCU_TYPE_PLA, ocp_index);
}
static void ocp_reg_write(r8152_dev_t *rt, uint16_t addr, uint16_t data)
{
    uint16_t ocp_base = addr & 0xF000u;
    if (ocp_base != rt->ocp_base) {
        ocp_write_word(rt, MCU_TYPE_PLA, PLA_OCP_GPHY_BASE, ocp_base);
        rt->ocp_base = ocp_base;
    }
    uint16_t ocp_index = (addr & 0x0FFFu) | 0xB000u;
    ocp_write_word(rt, MCU_TYPE_PLA, ocp_index, data);
}

/* Waits until a masked bit reaches the desired state by polling a register 
 * OCP (dword). Returns 0 if OK, -1 if timeout. (u-boot r8152_wait_for_bit). */
static int wait_for_bit(r8152_dev_t *rt, uint16_t type, uint16_t index,
                        uint32_t mask, int set, unsigned timeout)
{
    while (timeout--) {
        uint32_t val = ocp_read_dword(rt, type, index);
        if (!set) val = ~val;
        if ((val & mask) == mask) return 0;
        mdelay(1);
    }
    return -1;
}

/* ================================================================== */
/* Init sequences (adapted from u-boot) */
/* ================================================================== */

static void reset_packet_filter(r8152_dev_t *rt)
{
    uint32_t d = ocp_read_word(rt, MCU_TYPE_PLA, PLA_FMC);
    d &= ~FMC_FCR_MCU_EN;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_FMC, d);
    d |= FMC_FCR_MCU_EN;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_FMC, d);
}

static void nic_reset(r8152_dev_t *rt)
{
    uint32_t d = ocp_read_dword(rt, MCU_TYPE_PLA, BIST_CTRL);
    d |= BIST_CTRL_SW_RESET;
    ocp_write_dword(rt, MCU_TYPE_PLA, BIST_CTRL, d);
    wait_for_bit(rt, MCU_TYPE_PLA, BIST_CTRL, BIST_CTRL_SW_RESET, 0,
                 R8152_WAIT_TIMEOUT);
}

static void rxdy_gated_en(r8152_dev_t *rt, int enable)
{
    uint32_t d = ocp_read_word(rt, MCU_TYPE_PLA, PLA_MISC_1);
    if (enable) d |= RXDY_GATED_EN; else d &= ~RXDY_GATED_EN;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_MISC_1, d);
}

static void reset_bmu(r8152_dev_t *rt)
{
    uint8_t d = ocp_read_byte(rt, MCU_TYPE_USB, USB_BMU_RESET);
    d &= ~(BMU_RESET_EP_IN | BMU_RESET_EP_OUT);
    ocp_write_byte(rt, MCU_TYPE_USB, USB_BMU_RESET, d);
    d |= BMU_RESET_EP_IN | BMU_RESET_EP_OUT;
    ocp_write_byte(rt, MCU_TYPE_USB, USB_BMU_RESET, d);
}

static void reinit_ll(r8152_dev_t *rt)
{
    /* Wait for empty FIFO, reinit link-list (SFF_STS_7 RE_INIT_LL). */
    uint32_t d = ocp_read_word(rt, MCU_TYPE_PLA, PLA_SFF_STS_7);
    d |= RE_INIT_LL;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_SFF_STS_7, d);
    wait_for_bit(rt, MCU_TYPE_PLA, PLA_SFF_STS_7, LINK_LIST_READY, 1,
                 R8152_WAIT_TIMEOUT);
}

static void set_rx_mode(r8152_dev_t *rt)
{
    /* Accept broadcast/multicast/physical match. */
    uint32_t d = ocp_read_dword(rt, MCU_TYPE_PLA, PLA_RCR);
    d |= RCR_APM | RCR_AM | RCR_AB;
    ocp_write_dword(rt, MCU_TYPE_PLA, PLA_RCR, d);
}

/* r8153_first_init : reset + reconfig FIFO (subset u-boot). */
static void r8153_first_init(r8152_dev_t *rt)
{
    rxdy_gated_en(rt, 1);

    uint32_t d = ocp_read_dword(rt, MCU_TYPE_PLA, PLA_RCR);
    d &= ~RCR_ACPT_ALL;
    ocp_write_dword(rt, MCU_TYPE_PLA, PLA_RCR, d);

    nic_reset(rt);
    reset_bmu(rt);

    d = ocp_read_byte(rt, MCU_TYPE_PLA, PLA_OOB_CTRL);
    d &= ~NOW_IS_OOB;
    ocp_write_byte(rt, MCU_TYPE_PLA, PLA_OOB_CTRL, d);

    d = ocp_read_word(rt, MCU_TYPE_PLA, PLA_SFF_STS_7);
    d &= ~MCU_BORW_EN;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_SFF_STS_7, d);

    reinit_ll(rt);

    /* RX max size + TX max packet size. */
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_RMS, RTL8153_RMS);
    ocp_write_byte(rt, MCU_TYPE_PLA, PLA_MTPS, MTPS_JUMBO);

    d = ocp_read_word(rt, MCU_TYPE_PLA, PLA_TCR0);
    d |= TCR0_AUTO_FIFO;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_TCR0, d);

    nic_reset(rt);

    /* Normal RX/TX FIFO thresholds. */
    ocp_write_dword(rt, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, RXFIFO_THR1_NORMAL);
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_NORMAL);
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_NORMAL);
    ocp_write_dword(rt, MCU_TYPE_PLA, PLA_TXFIFO_CTRL, TXFIFO_THR_NORMAL2);

    /* Disable RX aggregation (we read frame by frame). */
    d = ocp_read_word(rt, MCU_TYPE_USB, USB_USB_CTRL);
    d &= ~(RX_AGG_DISABLE | RX_ZERO_EN);
    ocp_write_word(rt, MCU_TYPE_USB, USB_USB_CTRL, d);
}

static void tally_reset(r8152_dev_t *rt)
{
    uint32_t d = ocp_read_word(rt, MCU_TYPE_PLA, PLA_RSTTALLY);
    d |= TALLY_RESET;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_RSTTALLY, d);
}

static void power_cut_en(r8152_dev_t *rt, int enable)
{
    uint32_t d = ocp_read_word(rt, MCU_TYPE_USB, USB_POWER_CUT);
    if (enable) d |= PWR_EN | PHASE2_EN; else d &= ~(PWR_EN | PHASE2_EN);
    ocp_write_word(rt, MCU_TYPE_USB, USB_POWER_CUT, d);
    d = ocp_read_word(rt, MCU_TYPE_USB, USB_MISC_0);
    d &= ~PCUT_STATUS;
    ocp_write_word(rt, MCU_TYPE_USB, USB_MISC_0, d);
}

static void u1u2_en(r8152_dev_t *rt, int enable)
{
    uint32_t d = ocp_read_word(rt, MCU_TYPE_USB, USB_LPM_CONFIG);
    if (enable) d |= LPM_U1U2_EN; else d &= ~LPM_U1U2_EN;
    ocp_write_word(rt, MCU_TYPE_USB, USB_LPM_CONFIG, d);
}

static void u2p3_en(r8152_dev_t *rt, int enable)
{
    uint32_t d = ocp_read_word(rt, MCU_TYPE_USB, USB_U2P3_CTRL);
    if (enable) d |= U2P3_ENABLE; else d &= ~U2P3_ENABLE;
    ocp_write_word(rt, MCU_TYPE_USB, USB_U2P3_CTRL, d);
}

static void disable_aldps(r8152_dev_t *rt)
{
    uint16_t d = ocp_reg_read(rt, OCP_ALDPS_CONFIG);
    d &= ~0x8000u /* ENPWRSAVE */;
    ocp_reg_write(rt, OCP_ALDPS_CONFIG, d);
    mdelay(20);
}

/* r8153b_init: RTL8153B init sequence (RTL_VER_08/09). */
static void r8153b_init(r8152_dev_t *rt)
{
    disable_aldps(rt);
    u1u2_en(rt, 0);

    wait_for_bit(rt, MCU_TYPE_PLA, PLA_BOOT_CTRL, AUTOLOAD_DONE, 1,
                 R8152_WAIT_TIMEOUT);

    for (int i = 0; i < R8152_WAIT_TIMEOUT; i++) {
        uint16_t st = ocp_reg_read(rt, OCP_PHY_STATUS) & PHY_STAT_MASK;
        if (st == PHY_STAT_LAN_ON || st == PHY_STAT_PWRDN)
            break;
        mdelay(1);
    }

    u2p3_en(rt, 0);

    /* MSC timer. */
    ocp_write_word(rt, MCU_TYPE_USB, USB_MSC_TIMER, 0x0FFF);
    power_cut_en(rt, 0);

    /* MAC clock speed down. */
    uint32_t d = ocp_read_word(rt, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2);
    d |= MAC_CLK_SPDWN_EN;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2, d);

    d = ocp_read_word(rt, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3);
    d &= ~PLA_MCU_SPDWN_EN;
    ocp_write_word(rt, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3, d);

    /* RX aggregation off. */
    d = ocp_read_word(rt, MCU_TYPE_USB, USB_USB_CTRL);
    d &= ~(RX_AGG_DISABLE | RX_ZERO_EN);
    ocp_write_word(rt, MCU_TYPE_USB, USB_USB_CTRL, d);

    tally_reset(rt);
}

/* rtl_enable : reset filter + RE|TE + rx_mode (u-boot rtl_enable). */
static int rtl_enable(r8152_dev_t *rt)
{
    reset_packet_filter(rt);

    uint8_t d = ocp_read_byte(rt, MCU_TYPE_PLA, PLA_CR);
    d |= PLA_CR_RE | PLA_CR_TE;
    ocp_write_byte(rt, MCU_TYPE_PLA, PLA_CR, d);

    /* RTL8153B: indicate an RX aggregation change. */
    ocp_write_byte(rt, MCU_TYPE_USB, USB_UPT_RXDMA_OWN, OWN_UPDATE | OWN_CLEAR);

    rxdy_gated_en(rt, 0);
    set_rx_mode(rt);
    return 0;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

static void r8152_get_version(r8152_dev_t *rt)
{
    uint16_t tcr = ocp_read_word(rt, MCU_TYPE_PLA, PLA_TCR1) & VERSION_MASK;
    /* Table (u-boot r8152_versions): the RTL8153B known on this board has a tcr
     * in the RTL_VER_08/09 family. Known values are recognized;
     * otherwise assume RTL_VER_09 (recent 8153B) because it's the expected device. */
    switch (tcr) {
    case 0x6000: rt->version = RTL_VER_08; break;   /* RTL8153B */
    case 0x7000: rt->version = RTL_VER_09; break;   /* RTL8153B variant */
    default:     rt->version = RTL_VER_09; break;
    }
    printf("[r8152] version tcr=0x%04x -> RTL_VER_%02u\n",
           (unsigned)tcr, (unsigned)rt->version);
}

static int r8152_read_mac(r8152_dev_t *rt)
{
    uint8_t mac8[8] = {0};
    if (pla_ocp_read(rt, PLA_IDR, 8, mac8) < 0)
        return -1;
    memcpy(rt->mac, mac8, R8152_MAC_LEN);
    return 0;
}

usb_status_t r8152_probe(r8152_dev_t *rt, usb_device_t *dev)
{
    memset(rt, 0, sizeof(*rt));
    rt->dev = dev;
    rt->ocp_base = 0xFFFF;   /* force the first OCP base set */

    /* Locate the endpoints (bulk IN/OUT + interrupt IN). */
    const usb_endpoint_t *bin  = usb_find_endpoint(dev, USB_EP_XFER_BULK, 1);
    const usb_endpoint_t *bout = usb_find_endpoint(dev, USB_EP_XFER_BULK, 0);
    const usb_endpoint_t *iin  = usb_find_endpoint(dev, USB_EP_XFER_INT,  1);
    if (!bin || !bout) {
        printf("[r8152] missing bulk endpoints (in=%p out=%p)\n",
               (void *)bin, (void *)bout);
        return USB_EINVAL;
    }
    rt->ep_in  = bin->address;
    rt->ep_out = bout->address;
    rt->ep_int = iin ? iin->address : 0;
    printf("[r8152] endpoints : bulk IN 0x%02x, bulk OUT 0x%02x, int IN 0x%02x\n",
           rt->ep_in, rt->ep_out, rt->ep_int);

    /* Version + MAC. */
    r8152_get_version(rt);
    if (r8152_read_mac(rt) < 0) {
        printf("[r8152] MAC read (PLA_IDR) failed\n");
        return USB_EIO;
    }
    printf("[r8152] MAC = %02X:%02X:%02X:%02X:%02X:%02X\n",
           rt->mac[0], rt->mac[1], rt->mac[2],
           rt->mac[3], rt->mac[4], rt->mac[5]);

    /* Hardware init sequence (RTL8153B). */
    r8153b_init(rt);
    r8153_first_init(rt);

    /* Enable RE|TE + RX mode. */
    rtl_enable(rt);

    printf("[r8152] init OK (RE|TE active)\n");
    return USB_OK;
}

usb_status_t r8152_link_wait(r8152_dev_t *rt, uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    for (;;) {
        uint8_t sp = ocp_read_byte(rt, MCU_TYPE_PLA, PLA_PHYSTATUS);
        if (sp & LINK_STATUS) {
            rt->link_up = 1;
            printf("[r8152] LINK UP (PHYSTATUS=0x%02x)\n", (unsigned)sp);
            return USB_OK;
        }
        if (elapsed >= timeout_ms)
            break;
        mdelay(50);
        elapsed += 50;
    }
    rt->link_up = 0;
    printf("[r8152] link DOWN (timeout %lu ms)\n", (unsigned long)timeout_ms);
    return USB_ETIMEOUT;
}

/* NON-BLOCKING link read (Ethernet cable hot-plug). One vendor control
 * transfer, no wait, no log: the caller (net_task) reports the CHANGES only. */
int r8152_link_status(r8152_dev_t *rt)
{
    uint8_t sp = ocp_read_byte(rt, MCU_TYPE_PLA, PLA_PHYSTATUS);
    rt->link_up = (sp & LINK_STATUS) ? 1u : 0u;
    return (int)rt->link_up;
}

usb_status_t r8152_send(r8152_dev_t *rt, const void *frame, uint32_t len)
{
    if (len == 0 || len > R8152_ETH_FRAME_LEN)
        return USB_EINVAL;
    if (sizeof(struct tx_desc) + len > sizeof(g_tx_buf))
        return USB_EINVAL;

    /* Prefix the frame with a Realtek tx_desc: opts1 = len | FS | LS. */
    struct tx_desc *td = (struct tx_desc *)g_tx_buf;
    td->opts1 = (len & 0x3FFFFu) | TX_FS | TX_LS;
    td->opts2 = 0;
    memcpy(g_tx_buf + sizeof(struct tx_desc), frame, len);

    uint32_t total = (uint32_t)sizeof(struct tx_desc) + len;
    uint32_t xfer = 0;
    usb_status_t st = rt->dev->hcd->bulk(rt->dev, rt->ep_out,
                                         g_tx_buf, total, &xfer);
    if (st != USB_OK) {
        printf("[r8152] TX failed (st=%d)\n", (int)st);
        return st;
    }
    return USB_OK;
}

usb_status_t r8152_recv(r8152_dev_t *rt, void *buf, uint32_t buf_cap,
                        uint32_t *out_len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    uint32_t xfer = 0;
    usb_status_t st = rt->dev->hcd->bulk(rt->dev, rt->ep_in,
                                         g_rx_buf, sizeof(g_rx_buf), &xfer);
    if (st != USB_OK)
        return st;
    if (xfer < sizeof(struct rx_desc))
        return USB_ETIMEOUT;   /* nothing useful */

    struct rx_desc *rd = (struct rx_desc *)g_rx_buf;
    uint32_t plen = (rd->opts1 & RX_LEN_MASK);
    if (plen < CRC_SIZE)
        return USB_ETIMEOUT;
    plen -= CRC_SIZE;                 /* remove Ethernet CRC */
    if (plen > buf_cap)
        plen = buf_cap;
    if (plen > xfer - sizeof(struct rx_desc))
        plen = xfer - sizeof(struct rx_desc);

    memcpy(buf, g_rx_buf + sizeof(struct rx_desc), plen);
    if (out_len) *out_len = plen;
    return USB_OK;
}
