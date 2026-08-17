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
 * netif_r8152.c — lwIP network interface on top of the RTL8153B (r8152).
 *
 * lwIP bridge (NO_SYS=1) <-> USB-Ethernet r8152 driver (board-validated).
 *
 *   TX: lwIP calls low_level_output(netif, pbuf) -> we flatten the pbuf into a
 *       contiguous buffer (LWIP_NETIF_TX_SINGLE_PBUF=1: generally a single
 *       pbuf) then r8152_send().
 *   RX: netif_r8152_poll() (Core2 loop) -> non-blocking r8152_recv() -> if a
 *       frame arrives, we allocate a PBUF_POOL pbuf, copy it, then
 *       netif->input (= ethernet_input, wired by ethernet_input via netif_add).
 *
 * No IRQ: 100% polling reception (like the rest of the Core2 I/O stack).
 */
#include <string.h>

#include "netif_r8152.h"

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/snmp.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

/* Standard Ethernet MTU. */
#define NETIF_R8152_MTU     1500

/* Working buffers (a single r8152 netif, single-threaded Core2 -> static OK). */
static uint8_t s_tx_buf[NETIF_R8152_MTU + 64] __attribute__((aligned(64)));
static uint8_t s_rx_buf[NETIF_R8152_MTU + 64] __attribute__((aligned(64)));

/* ------------------------------------------------------------------ */
/* TX: lwIP -> r8152                                                  */
/* ------------------------------------------------------------------ */
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    r8152_dev_t *rt = (r8152_dev_t *)netif->state;

    if (p->tot_len > sizeof(s_tx_buf))
        return ERR_MEM;

    /* Flatten the pbuf chain into a contiguous buffer (complete Ethernet
     * frame, without CRC — the RTL8153B adds it). */
    u16_t len = pbuf_copy_partial(p, s_tx_buf, p->tot_len, 0);
    if (len != p->tot_len)
        return ERR_BUF;

    usb_status_t st = r8152_send(rt, s_tx_buf, len);

    MIB2_STATS_NETIF_ADD(netif, ifoutoctets, len);
    if (st != USB_OK) {
        LINK_STATS_INC(link.err);
        return ERR_IF;
    }
    LINK_STATS_INC(link.xmit);
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* netif init (netif_add callback)                                    */
/* ------------------------------------------------------------------ */
static err_t netif_r8152_lowinit(struct netif *netif)
{
    r8152_dev_t *rt = (r8152_dev_t *)netif->state;

#if LWIP_NETIF_HOSTNAME
    netif->hostname = "oros-rk3328";
#endif

    netif->name[0] = 'e';
    netif->name[1] = 'n';

    /* Outputs: IP -> ARP -> Ethernet -> r8152. */
    netif->output     = etharp_output;
    netif->linkoutput = low_level_output;

    /* MAC address (read by r8152_probe, PLA_IDR). */
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, rt->mac, ETH_HWADDR_LEN);

    netif->mtu = NETIF_R8152_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

    MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, 100000000);
    return ERR_OK;
}

err_t netif_r8152_add(struct netif *netif, r8152_dev_t *rt,
                      const ip4_addr_t *ip, const ip4_addr_t *mask,
                      const ip4_addr_t *gw)
{
    /* netif->input = ethernet_input: processes the Ethernet header then
     * dispatches ARP/IP. state = our r8152 device (retrieved in callbacks). */
    struct netif *n = netif_add(netif, ip, mask, gw, rt,
                                netif_r8152_lowinit, ethernet_input);
    if (n == NULL)
        return ERR_IF;

    netif_set_default(netif);
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* RX: r8152 → lwIP (polling)                                         */
/* ------------------------------------------------------------------ */
int netif_r8152_poll(struct netif *netif)
{
    r8152_dev_t *rt = (r8152_dev_t *)netif->state;
    int handled = 0;

    /* Drain all available frames without blocking (timeout 0). Bound the
     * count per call so as not to monopolize the Core2 thread. */
    for (int i = 0; i < 8; i++) {
        uint32_t len = 0;
        usb_status_t st = r8152_recv(rt, s_rx_buf, sizeof(s_rx_buf), &len, 0);
        if (st != USB_OK || len == 0)
            break;   /* nothing more to read this round */

        /* Allocate a pool pbuf and copy the received frame into it. */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p == NULL) {
            LINK_STATS_INC(link.memerr);
            LINK_STATS_INC(link.drop);
            MIB2_STATS_NETIF_INC(netif, ifindiscards);
            continue;
        }
        if (pbuf_take(p, s_rx_buf, (u16_t)len) != ERR_OK) {
            pbuf_free(p);
            LINK_STATS_INC(link.drop);
            continue;
        }

        LINK_STATS_INC(link.recv);
        MIB2_STATS_NETIF_ADD(netif, ifinoctets, len);

        /* netif->input = ethernet_input (set by netif_add). */
        if (netif->input(p, netif) != ERR_OK) {
            pbuf_free(p);
            LINK_STATS_INC(link.drop);
        } else {
            handled++;
        }
    }

    return handled;
}
