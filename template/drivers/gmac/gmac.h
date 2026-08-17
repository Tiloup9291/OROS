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
 * gmac.h — Raw L2 GMAC driver (Synopsys DWMAC 1000) for RK3328
 *
 * The RK3328 exposes a "legacy" GMAC `gmac2io@ff540000` = Synopsys DWMAC 1000
 * (NOT the DWMAC QoS/`dwc_eth_qos`). It is wired on the Orange Pi R1 Plus LTS
 * board to a Motorcomm YT8531C PHY (MDIO addr 0, `rgmii-id` mode, reset gpio1
 * PC2 active low). This port is DEDICATED to the EtherCAT master: raw L2
 * traffic (EtherType 0x88A4), no IP stack.
 *
 * Purposes: send/receive RAW Ethernet frames in POLLING (no GMAC
 * IRQ), independently of EtherCAT — testable alone.
 *   - DWMAC 1000 init (DMA reset, MAC filter, ring TX/RX descriptors);
 *   - Rockchip glue (RGMII mode via GRF mac_con);
 *   - MDIO + YT8531C PHY (reset, ID, autoneg/link-up, speed/duplex);
 *   - raw L2 API: gmac_init / gmac_link_up / gmac_send / gmac_poll_recv.
 *
 * Sources (offsets/bits QUOTED, never deduced):
 *   - Registers/descriptors/init: U-Boot drivers/net/designware.c + .h
 *     (struct eth_mac_regs @0x0000, eth_dma_regs @0x1000, struct dmamacdescr).
 *   - RGMII/clock glue RK3328: U-Boot drivers/net/gmac_rockchip.c
 *     (rk3328_gmac_set_to_rgmii / rk3328_gmac_fix_mac_speed) + grf_rk3328.h
 *     (mac_con[0]=GRF+0x900, mac_con[1]=+0x904).
 *   - PHY: U-Boot drivers/net/phy/motorcomm.c (MDIO clause-22).
 *   - Board: DT rk3328-orangepi-r1-plus-lts.dts (@ff540000, IRQ SPI24=INTID56,
 *     phy@0, rgmii-id, reset gpio1 PC2 active low 15ms/50ms).
 *
 * On QEMU 'virt' there is no RK3328 GMAC: the functions neutralize themselves
 * (GMAC_ENODEV) under -DMMU_QEMU.
 *
 * Independent of Linux: MMIO register access only.
 */
#ifndef RTOS_DRIVERS_GMAC_H
#define RTOS_DRIVERS_GMAC_H

#include <stdint.h>

/* GIC INTID (SPI+32) of the GMAC: DT interrupts = GIC_SPI 24 -> 24+32 = 56.
 * Note: the GMAC RX IRQ stays DISABLED (synchronous polling). */
#define GMAC_IRQ         56u

/* Max size of a handled Ethernet frame (with header, without hardware FCS). */
#define GMAC_FRAME_MAX   1536u

/* Return codes. */
typedef enum {
    GMAC_OK       = 0,
    GMAC_ENODEV   = -1,   /* no controller (QEMU) */
    GMAC_ENOLINK  = -2,   /* no link (PHY down) */
    GMAC_ETIMEOUT = -3,   /* timeout (DMA reset, MDIO, TX) */
    GMAC_EIO      = -4,   /* transfer error */
    GMAC_EINVAL   = -5,   /* invalid parameter */
    GMAC_EAGAIN   = -6,   /* nothing to receive (RX empty) — for poll_recv */
} gmac_status_t;

/* Information about the detected PHY / link. */
typedef struct {
    uint32_t phy_id;      /* OUI+model (MII registers 2/3) */
    uint32_t phy_addr;    /* MDIO address (0 on this board) */
    int      link;        /* 1 = link up */
    int      speed;       /* 10 / 100 / 1000 (Mbit/s) */
    int      duplex;      /* 1 = full duplex */
    uint8_t  mac[6];      /* MAC address used by the MAC */
} gmac_info_t;

/* Initializes the GMAC: RGMII glue (GRF), DWMAC DMA reset, descriptors, MDIO,
 * reset + probe YT8531C PHY, link-up wait (up to ~3 s), speed config.
 * 'mac' (6 bytes) = MAC address to program (if NULL, a fixed local MAC is
 * used). Fills *info if not NULL. Returns GMAC_OK if ready for L2 traffic. */
gmac_status_t gmac_init(const uint8_t mac[6], gmac_info_t *info);

/* Returns 1 if the PHY link is active (re-reads PHY BMSR). */
int gmac_link_up(void);

/* Emits a raw Ethernet frame (len bytes, header + payload, WITHOUT FCS — added
 * by the MAC) through the TX ring in POLLING (waits until the descriptor is
 * re-owned by the CPU). Returns GMAC_OK or an error code. */
gmac_status_t gmac_send(const void *frame, uint32_t len);

/* Retrieves a frame received through the RX ring in POLLING (non-blocking).
 * Copies at most 'bufsz' bytes into buf, writes the received length in *out_len.
 * Returns GMAC_OK if a frame was retrieved, GMAC_EAGAIN if RX is empty. */
gmac_status_t gmac_poll_recv(void *buf, uint32_t bufsz, uint32_t *out_len);

/* Copies the current MAC address (6 bytes) into out. */
void gmac_get_mac(uint8_t out[6]);

/* Returns the negotiated link speed (10/100/1000 Mbit/s), 0 if no link/QEMU.
 * Remembered during gmac_init() (PHY read at link-up). */
int gmac_get_link_speed(void);


/* Raw MDIO clause-22 access (debug / PHY). Returns -1 on timeout. */
int gmac_mdio_read(uint32_t phy_addr, uint32_t reg);
int gmac_mdio_write(uint32_t phy_addr, uint32_t reg, uint16_t val);

/* Hot re-applies the YT8531 PHY RGMII delays (TX board debug):
 * tx_code/rx_code = 0..15 (×150 ps), tx_invert = 0/1 (inverts tx_clk).
 * Used by the demo to SWEEP the TX delay (RX already works). QEMU no-op. */
void gmac_phy_set_rgmii_tuning(uint32_t tx_code, uint32_t rx_code, int tx_invert);

/* TX diagnostics: dump DMA/MAC registers + last TX descriptor status + MMC
 * counter of actually transmitted frames. Helps locate a TX stall (is the MAC
 * really transmitting?). QEMU no-op. */
void gmac_tx_diag(void);

#endif /* RTOS_DRIVERS_GMAC_H */
