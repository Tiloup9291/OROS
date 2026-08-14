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
 * net_demo.c — Demo: lwIP IP stack over the USB-Ethernet RTL8153B.
 *
 * Complete chain (Core2, IO_SOFT):
 *   1. xhci_init() + usb_enumerate() -> RTL8153B device (0BDA:8153)
 *   2. r8152_probe() (MAC, HW init, RE|TE) + r8152_link_wait()
 *   3. lwip_init() + netif_r8152_add() (static IP) + netif_set_up()
 *   4. ~60 s loop: netif_r8152_poll() (RX) + sys_check_timeouts() (timers) ->
 *      responds to ARP and PING (ICMP echo) sent by a PC on the network.
 *
 * Success on: `ping <ip_board>` OK from a PC plugged into the same
 * network as the RTL8153B (RJ45 cable on the 2nd port). ICMP/ARP counters +
 * lwIP link displayed on the UART.
 *
 * On QEMU (-DMMU_QEMU): no xHCI -> the demo self-ignores cleanly.
 */
#include <stdio.h>
#include <string.h>

#include "net_demo.h"

#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/stats.h"

#include "netif_r8152.h"
#include "tcp_shell.h"
#include "ssh_server.h"
#include "net_shell.h"


#include "../drivers/usb/usb_core.h"
#include "../drivers/usb/hcd_xhci.h"
#include "../drivers/usb/class_r8152.h"
#include "../drivers/usb/usb.h"
#include "../arch/aarch64/timer.h"

/* ------------------------------------------------------------------ */
/* Static IP addressing. TO ADJUST to the test network.               */
/* Default: board = 192.168.1.50/24, gateway = 192.168.1.1.           */
/* ------------------------------------------------------------------ */
#ifndef NET_IP_ADDR0
#define NET_IP_ADDR0   192
#define NET_IP_ADDR1   168
#define NET_IP_ADDR2   1
#define NET_IP_ADDR3   50
#endif
#ifndef NET_GW_ADDR3
#define NET_GW_ADDR3   1
#endif

/* Network test window (ping + TCP shell). Lengthened to leave time
 * to open a shell session and type commands. */
#ifndef NET_DEMO_WINDOW_S
#define NET_DEMO_WINDOW_S   180u
#endif

/* Global netif (a single one, RTL8153B). */
static struct netif s_netif;

static void net_delay_us(uint32_t us)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks(us);
    while (timer_now_ticks() < end)
        __asm__ volatile("nop");
}

void net_demo_run(void)
{
    printf("\n===== DEMO (lwIP + IP on USB-Ethernet RTL8153B) =====\n");

    /* --- xHCI + RTL8153B enumeration --- */
    usb_status_t st = xhci_init();
    if (st == USB_ENODEV) {
        printf("[net] no xHCI controller (QEMU) : Demo ignored.\n");
        return;
    }
    if (st != USB_OK) {
        printf("[net] failed to init xHCI (code %d) : Demo aborted.\n", (int)st);
        return;
    }
    usb_core_set_hcd(&xhci_hcd_ops);

    static usb_device_t dev;
    st = usb_enumerate(&xhci_hcd_ops, &dev);
    if (st != USB_OK) {
        printf("[net] USB enumeration failed (code %d) : Demo aborted.\n",
               (int)st);
        return;
    }

    int is_rtl = (dev.dev_desc.idVendor == USB_VID_REALTEK &&
                  dev.dev_desc.idProduct == USB_PID_RTL8153);
    if (!is_rtl) {
        printf("[net] listed device %04X:%04X is not the RTL8153B : "
               "Demo aborted.\n",
               dev.dev_desc.idVendor, dev.dev_desc.idProduct);
        return;
    }
    printf("[net] RTL8153B listed (%04X:%04X, %s).\n",
           dev.dev_desc.idVendor, dev.dev_desc.idProduct,
           (dev.speed == USB_SPEED_SUPER) ? "SuperSpeed" : "USB2");

    /* --- r8152 driver: MAC + init + link --- */
    static r8152_dev_t rt;
    st = r8152_probe(&rt, &dev);
    if (st != USB_OK) {
        printf("[net] r8152_probe failed (code %d) : Demo aborted.\n", (int)st);
        return;
    }
    printf("[net] MAC = %02X:%02X:%02X:%02X:%02X:%02X\n",
           rt.mac[0], rt.mac[1], rt.mac[2], rt.mac[3], rt.mac[4], rt.mac[5]);

    st = r8152_link_wait(&rt, 8000);
    if (st != USB_OK)
        printf("[net] ATTENTION : Ethernet link DOWN (cable ?) — we are mounting anyway.\n");
    else
        printf("[net] Ethernet link UP.\n");

    /* ---lwIP: init + static IP netif --- */
    lwip_init();

    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw,   NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_GW_ADDR3);

    if (netif_r8152_add(&s_netif, &rt, &ip, &mask, &gw) != ERR_OK) {
        printf("[net] netif_r8152_add failed: Demo aborted.\n");
        return;
    }
    netif_set_link_up(&s_netif);
    netif_set_up(&s_netif);

    printf("[net] netif UP : IP=%u.%u.%u.%u  mask=%u.%u.%u.%u  gw=%u.%u.%u.%u\n",
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
           255, 255, 255, 0,
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_GW_ADDR3);
    /* --- Remote shell TCP server (telnet-like port 23) --- */
    int sh = tcp_shell_start();
    if (sh == 0)
        printf("[net] TCP shell listening on port %u "
               "(telnet %u.%u.%u.%u %u  or  nc %u.%u.%u.%u %u)\n",
               (unsigned)TCP_SHELL_PORT,
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
               (unsigned)TCP_SHELL_PORT,
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
               (unsigned)TCP_SHELL_PORT);
    else
        printf("[net] WARNING : TCP shell startup failed (code %d).\n", sh);

    /* --- SSH server (wolfSSH port 22) --- */
    int ss = ssh_server_start();
    if (ss == 0)
        printf("[net] SSH server listening on port %u "
               "(ssh %s@%u.%u.%u.%u  — password: %s)\n",
               (unsigned)SSH_SERVER_PORT, SSH_USER,
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3, SSH_PASS);
    else
        printf("[net] WARNING : SSH server startup failed (code %d).\n", ss);

    printf("[net] === Test : `ping %u.%u.%u.%u` then `telnet %u.%u.%u.%u %u` "
           "(window %u s) ===\n",
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
           (unsigned)TCP_SHELL_PORT, NET_DEMO_WINDOW_S);

    /* --- lwIP main-loop: RX polling + timers, ~180 s --- */
    uint64_t deadline = timer_now_ticks() +
                        timer_us_to_ticks((uint64_t)NET_DEMO_WINDOW_S * 1000000ull);
    uint64_t next_beat = timer_now_ticks();
    uint32_t total_rx = 0;

    while (timer_now_ticks() < deadline) {
        /* Inject the frames received from the RTL8153B into lwIP (ARP/ICMP/IP).
         * We track how many frames were processed THIS round to adapt the
         * pace: as long as there is traffic, we don't wait. */
        int rx_now = netif_r8152_poll(&s_netif);
        total_rx += (uint32_t)rx_now;

        /* Advance the lwIP timers (ARP, TCP, ...). */
        sys_check_timeouts();

        /* Advance the SSH state machine (handshake / shell exchange).
         * ssh_advance() now drains ALL pending packets. */
        ssh_server_poll();

        /* Is an SSH session active? If so, stay very reactive (the
         * interactive throughput depends on the polling frequency). Same if
         * we just received frames (burst in progress). */
        int busy = (rx_now > 0) || ssh_server_session_active();


        /* Activity heartbeat every ~10 s (visible progress). */
        if (timer_now_ticks() >= next_beat) {
            next_beat = timer_now_ticks() + timer_us_to_ticks(10000000ull);
#if LWIP_STATS
            printf("[net] ...enabled (RX frames received=%lu, "
                   "ICMP in=%lu/out=%lu, ARP recv=%lu)\n",
                   (unsigned long)total_rx,
                   (unsigned long)lwip_stats.icmp.recv,
                   (unsigned long)lwip_stats.icmp.xmit,
                   (unsigned long)lwip_stats.etharp.recv);
#else
            printf("[net] ...enabled (RX frames received=%lu)\n",
                   (unsigned long)total_rx);
#endif
        }

        /* Adaptive pace: at rest we wait (20 us) so as not to hammer the USB
         * uselessly; as soon as there is traffic or an active session, we
         * loop back IMMEDIATELY (minimal interactive latency).
         */
        if (!busy)
            net_delay_us(20);
    }


    /* --- summary --- */
#if LWIP_STATS
    unsigned long icmp_in  = (unsigned long)lwip_stats.icmp.recv;
    unsigned long icmp_out = (unsigned long)lwip_stats.icmp.xmit;
    unsigned long arp_recv = (unsigned long)lwip_stats.etharp.recv;
    int ping_ok = (icmp_in > 0 && icmp_out > 0);
    printf("\n[net][result] RX frames=%lu ; ARP recv=%lu ; "
           "ICMP echo in=%lu / reply out=%lu\n",
           (unsigned long)total_rx, arp_recv, icmp_in, icmp_out);
    if (ping_ok)
        printf(">>> Demo: lwIP OK — PING answered (ICMP echo). OK. <<<\n");
    else
        printf(">>> Demo: stack mounted but no PING received "
               "(check cable/IP/PC on same subnet). <<<\n");
#else
    printf("\n>>> Demo: lwIP stack mounted (RX frames=%lu). <<<\n",
           (unsigned long)total_rx);
#endif

    /* --- summary: TCP server + remote shell --- */
    uint32_t sessions = tcp_shell_sessions();
    uint32_t commands = tcp_shell_commands();
    printf("\n[net][result] TCP shell : sessions=%lu, executed commands=%lu\n",
           (unsigned long)sessions, (unsigned long)commands);
    if (sessions > 0 && commands > 0)
        printf(">>> Demo: TCP server + remote shell OK "
               "(session + received commands). <<<\n");
    else
        printf(">>> Demo: TCP server listening but no session/command "
               "(test `telnet %u.%u.%u.%u %u` during window). <<<\n",
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
               (unsigned)TCP_SHELL_PORT);

    /* --- summary: SSH server (wolfSSH) --- */
    uint32_t ssh_sess = ssh_server_sessions();
    uint32_t ssh_auth = ssh_server_auth_ok();
    uint32_t ssh_cmd  = ssh_server_commands();
    printf("\n[net][result] SSH server: sessions=%lu, auth OK=%lu, "
           "executed commands=%lu\n",
           (unsigned long)ssh_sess, (unsigned long)ssh_auth,
           (unsigned long)ssh_cmd);
    if (ssh_auth > 0 && ssh_cmd > 0)
        printf(">>> Demo: SSH server OK — login + remote encrypted shell. "
               "Demo COMPLETE. <<<\n");
    else if (ssh_sess > 0)
        printf(">>> Demo: SSH connection received but no authenticated shell "
               "(check user/password %s/%s). <<<\n", SSH_USER, SSH_PASS);
    else
        printf(">>> Demo: SSH server listening but no session "
               "(test `ssh %s@%u.%u.%u.%u` during window). <<<\n",
               SSH_USER, NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);
}
