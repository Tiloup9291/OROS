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
 * gmac_demo.c — Demo: GMAC L2 raw (DWMAC1000 + PHY YT8531C).
 *
 * Tested:
 *   - link-up on the GMAC port;
 *   - TX of a raw frame (broadcast) emitted on the wire (observable with PC capture);
 *   - RX of frame(s) captured by POLLING (counter + dump).
 * No GMAC IRQ (pure polling).
 *
 * On QEMU: gmac_init returns GMAC_ENODEV → the demo self-ignores cleanly.
 */

#include "gmac_demo.h"
#include "gmac.h"
#include "timer.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Test EtherType (not IP, not EtherCAT): 0x88B5 = "Local Experimental 1". */
#define DEMO_ETHERTYPE   0x88B5u

static void __attribute__((unused)) hexdump_frame(const uint8_t *p, uint32_t len)

{
    uint32_t n = (len > 32) ? 32 : len;
    printf("      dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X "
           "type=%02X%02X\n",
           p[0], p[1], p[2], p[3], p[4], p[5],
           p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13]);
    printf("      [");
    for (uint32_t i = 0; i < n; i++) printf("%02X%s", p[i], (i + 1 < n) ? " " : "");
    printf("]\n");
}

void gmac_demo_run(void)
{
    printf("\n============================================================\n");
    printf(" Demo : GMAC L2 raw (DWMAC1000 + PHY YT8531C, POLLING)\n");
    printf("============================================================\n");

    /* Real board MAC (sticky: C0:74:2B:FC:33:A4). Used so that traffic has a
     * source consistent with the vendor (the RTL8153B USB is in ...:A5,
     * consecutive addresses). */
    static const uint8_t mac[6] = { 0xC0, 0x74, 0x2B, 0xFC, 0x33, 0xA4 };
    gmac_info_t info;

    gmac_status_t st = gmac_init(mac, &info);

    if (st != GMAC_OK) {
        if (st == GMAC_ENODEV)
            printf("[gmac] no GMAC controller (QEMU): demo ignored.\n");
        else if (st == GMAC_ENOLINK)
            printf("[gmac] init OK but NO link (cable?): TX/RX ignored.\n");
        else
            printf("[gmac] init FAILED (st=%d).\n", (int)st);
        return;
    }

    uint8_t my[6];
    gmac_get_mac(my);
    printf("[gmac] MAC=%02X:%02X:%02X:%02X:%02X:%02X  PHY id=0x%08lX  %d Mbit/s %s\n",
           my[0], my[1], my[2], my[3], my[4], my[5],
           (unsigned long)info.phy_id, info.speed, info.duplex ? "full" : "half");

    /* --- Build two test frames --- */
    /* Source/destination IP for the ARP request (adjust to your network if needed). */
    static const uint8_t ip_src[4]  = { 192, 168, 1, 123 };  /* IP "of" the board */
    static const uint8_t ip_dst[4]  = { 192, 168, 1, 5   };  /* IP to resolve (gateway) */

    /* (a) ARP broadcast request (visible with `tcpdump -e arp`, may trigger
     *     an ARP reply = end-to-end TX proof). RFC 826 format. */
    static uint8_t arp[42];
    memset(arp, 0, sizeof(arp));
    memset(&arp[0], 0xFF, 6);                    /* dst = broadcast */
    memcpy(&arp[6], my, 6);                       /* src = our MAC */
    arp[12] = 0x08; arp[13] = 0x06;               /* EtherType = ARP */
    arp[14] = 0x00; arp[15] = 0x01;               /* HTYPE = Ethernet */
    arp[16] = 0x08; arp[17] = 0x00;               /* PTYPE = IPv4 */
    arp[18] = 0x06;                               /* HLEN = 6 */
    arp[19] = 0x04;                               /* PLEN = 4 */
    arp[20] = 0x00; arp[21] = 0x01;               /* OPER = request */
    memcpy(&arp[22], my, 6);                      /* sender HW addr */
    memcpy(&arp[28], ip_src, 4);                  /* sender proto addr */
    /* target HW = 0 (unknown); target proto = IP to resolve */
    memcpy(&arp[38], ip_dst, 4);                  /* target proto addr */

    /* (b) Raw frame with experimental EtherType 0x88B5 (visible without filter). */
    static uint8_t raw[64];
    memset(raw, 0, sizeof(raw));
    memset(&raw[0], 0xFF, 6);
    memcpy(&raw[6], my, 6);
    raw[12] = (uint8_t)(DEMO_ETHERTYPE >> 8);
    raw[13] = (uint8_t)(DEMO_ETHERTYPE & 0xFF);
    const char *msg = "OROS DEMO GMAC RAW L2";
    memcpy(&raw[14], msg, strlen(msg));

    printf("[gmac] TX : ARP request (who-has %u.%u.%u.%u tell %u.%u.%u.%u)"
           " + frame 0x88B5\n",
           ip_dst[0], ip_dst[1], ip_dst[2], ip_dst[3],
           ip_src[0], ip_src[1], ip_src[2], ip_src[3]);
    printf("       (on a PC : `sudo tcpdump -i <if> -e arp or ether proto 0x88b5`)\n");

    /* ================================================================
     * AUTOMATIC SWEEP OF THE RGMII TX DELAY (like the phase sweep of
     * SDMMC). RX already works (RX delay validated at 1950 ps); it's
     * the TX direction that fails → we test several TX delay codes + both
     * polarities of tx_clk, looking for the one that causes an ARP REPLY
     * (proof that our TX actually goes out on the wire and is decoded by a
     * host). Converges in a SINGLE flash. RX_DELAY stays 13 (1950 ps, validated).
     * ================================================================ */
    static const uint8_t rx_code = 13;                 /* 1950 ps (RX validated) */
    static const uint8_t tx_codes[] = { 13, 8, 4, 0, 15, 11, 2 }; /* trials */
    static uint8_t rxbuf[GMAC_FRAME_MAX];
    uint32_t total_rx = 0, total_tx = 0;
    int found = 0; uint8_t good_tx = 0; int good_inv = 0;

    for (int inv = 0; inv <= 1 && !found; inv++) {
        for (unsigned ti = 0; ti < sizeof(tx_codes) && !found; ti++) {
            uint8_t txc = tx_codes[ti];
            gmac_phy_set_rgmii_tuning(txc, rx_code, inv);
            /* let the PHY stabilize after reconfig delay */
            uint64_t s = timer_now_ticks() + timer_us_to_ticks(50000);
            while (timer_now_ticks() < s) { __asm__ volatile("nop"); }

            /* Send ARP requests in burst + wait for a reply ~0.8 s. */
            uint32_t arp_reply = 0;
            uint64_t win = timer_now_ticks() + timer_us_to_ticks(800000);
            uint64_t next_tx = timer_now_ticks();
            while (timer_now_ticks() < win) {
                if (timer_now_ticks() >= next_tx) {
                    if (gmac_send(arp, sizeof(arp)) == GMAC_OK) total_tx++;
                    gmac_send(raw, 14 + (uint32_t)strlen(msg));
                    next_tx = timer_now_ticks() + timer_us_to_ticks(100000); /* 100 ms */
                }
                uint32_t len = 0;
                if (gmac_poll_recv(rxbuf, sizeof(rxbuf), &len) == GMAC_OK && len >= 14) {
                    total_rx++;
                    /* ARP reply THAT IS DESTINED TO US (RFC 826 format):
                     *   [12..13] EtherType=0x0806, [20..21] OPER=2 (reply),
                     *   [32..37] target HW = our MAC (the responder addresses us
                     *   its reply). NB: [38..41] = target IP (4 B), NOT a MAC —
                     *   the old memcmp(&rxbuf[38],my,6) was ALWAYS wrong,
                     *   hence "ARP replies=0" while TX worked (see tcpdump).
                     *   We also accept the case where Ethernet dst [0..5] = our MAC. */
                    if (len >= 42 && rxbuf[12] == 0x08 && rxbuf[13] == 0x06 &&
                        rxbuf[20] == 0x00 && rxbuf[21] == 0x02 &&
                        (memcmp(&rxbuf[32], my, 6) == 0 ||
                         memcmp(&rxbuf[0],  my, 6) == 0)) {
                        arp_reply++;
                    }
                }

            }
            printf("[gmac] trying TX delay=%2u (%4u ps) inv=%d : RX_total=%lu, "
                   "ARP replies=%lu\n",
                   txc, (unsigned)txc * 150u, inv,
                   (unsigned long)total_rx, (unsigned long)arp_reply);
            if (arp_reply > 0) { found = 1; good_tx = txc; good_inv = inv; }
        }
    }

    /* Low-level diagnostic: did the MAC REALLY transmit? (TX descriptor
     * status + DMA FSM + MMC counter of TX frames sent). */
    gmac_tx_diag();

    printf("------------------------------------------------------------\n");
    if (found) {
        gmac_phy_set_rgmii_tuning(good_tx, rx_code, good_inv);
        printf(">>> ⭐ TX PROVEN: ARP reply captured with TX delay=%u (%u ps) inv=%d.\n",
               good_tx, (unsigned)good_tx * 150u, good_inv);
        printf(">>> DEMO: GMAC raw L2 — link=UP, TX+RX PROVEN. <<<\n");
        printf(">>> (SET GMAC_TX_DELAY_CODE=%u, GMAC_TX_CLK_INVERT=%d in gmac.c.)\n",
               good_tx, good_inv);
    } else {
        printf(">>> RX OK (%lu frames), but NO ARP reply on all TX delays.\n",
               (unsigned long)total_rx);
        printf(">>> Check: (1) ip_src/ip_dst adapted to your network (an IP that replies);\n");
        printf(">>>            (2) on the PC side `tcpdump -e arp or ether proto 0x88b5`;\n");
        printf(">>>            (3) the switch does not block unknown MACs.\n");
        printf(">>> DEMO: RX valid; TX to confirm. <<<\n");
    }
    printf("============================================================\n\n");
}
