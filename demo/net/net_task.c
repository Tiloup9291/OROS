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
 * net_task.c — PERMANENT IP/SSH network stack on Core2.
 *
 * "Infinite thread" version of the demo (net_demo.c). Same init chain
 * (xHCI + RTL8153B enumeration + r8152 + lwIP static IP netif + telnet:23
 * server + SSH:22 server), but the polling loop is INFINITE (no deadline):
 * the board remains a permanently administrable network node, IN PARALLEL
 * with Core0's permanent EtherCAT master.
 *
 * The remote shell reads the ecat_diag snapshot published CONTINUOUSLY by
 * Core0 -> the `ecat` command reflects the EtherCAT state in REAL TIME.
 *
 * QEMU (-DMMU_QEMU): xhci_init -> ENODEV -> the task reports it and goes idle
 * (the rest of the system continues).
 */
#include <stdio.h>
#include <string.h>

#include "net_task.h"
#include "../kernel/config.h"

#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/stats.h"

#include "netif_r8152.h"
#include "tcp_shell.h"
#include "ssh_server.h"
#include "net_shell.h"
#include "uart_shell.h"

#include "../drivers/usb/usb_core.h"
#include "../drivers/usb/hcd_xhci.h"
#include "../drivers/usb/class_r8152.h"
#include "../drivers/usb/usb.h"
#include "../arch/aarch64/timer.h"

/* ------------------------------------------------------------------ */
/* Static IP addressing (same as net_demo.c). TO ADJUST to the network. */
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

/* Global netif (a single one, RTL8153B). */
static struct netif s_netif;

static void net_delay_us(uint32_t us)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks(us);
    while (timer_now_ticks() < end)
        __asm__ volatile("nop");
}

/* Goes idle without blocking the rest of the system (no network available). */
static void net_task_idle(void)
{
    for (;;)
        __asm__ volatile("wfi");
}

void net_task_entry(void *arg)
{
    (void)arg;

    printf("\n===== Demo: IP/SSH PERMANENT stack on Core%u "
           "(// EtherCAT Core%u) =====\n",
           (unsigned)CFG_CORE_IO_SOFT, (unsigned)CFG_CORE_ECAT_HARD);

    /* --- xHCI + RTL8153B enumeration --- */
    usb_status_t st = xhci_init();
    if (st == USB_ENODEV) {
        printf("[net] no xHCI controller (QEMU) : network task idle.\n");
        net_task_idle();
    }
    if (st != USB_OK) {
        printf("[net] failed to init xHCI (code %d) : network task idle.\n", (int)st);
        net_task_idle();
    }
    usb_core_set_hcd(&xhci_hcd_ops);

    static usb_device_t dev;
    st = usb_enumerate(&xhci_hcd_ops, &dev);
    if (st != USB_OK) {
        printf("[net] USB enumeration failed (code %d) : network task idle.\n",
               (int)st);
        net_task_idle();
    }

    int is_rtl = (dev.dev_desc.idVendor == USB_VID_REALTEK &&
                  dev.dev_desc.idProduct == USB_PID_RTL8153);
    if (!is_rtl) {
        printf("[net] device %04X:%04X is not the RTL8153B : network task idle.\n",
               dev.dev_desc.idVendor, dev.dev_desc.idProduct);
        net_task_idle();
    }
    printf("[net] RTL8153B listed (%04X:%04X, %s).\n",
           dev.dev_desc.idVendor, dev.dev_desc.idProduct,
           (dev.speed == USB_SPEED_SUPER) ? "SuperSpeed" : "USB2");

    /* --- r8152 driver: MAC + init + link --- */
    static r8152_dev_t rt;
    st = r8152_probe(&rt, &dev);
    if (st != USB_OK) {
        printf("[net] r8152_probe failed (code %d) : network task idle.\n", (int)st);
        net_task_idle();
    }
    printf("[net] MAC = %02X:%02X:%02X:%02X:%02X:%02X\n",
           rt.mac[0], rt.mac[1], rt.mac[2], rt.mac[3], rt.mac[4], rt.mac[5]);

    st = r8152_link_wait(&rt, 8000);
    if (st != USB_OK)
        printf("[net] WARNING : Ethernet link DOWN (cable ?) — we are mounting anyway.\n");
    else
        printf("[net] Ethernet link UP.\n");

    /* --- lwIP: init + static IP netif --- */
    lwip_init();

    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw,   NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_GW_ADDR3);

    if (netif_r8152_add(&s_netif, &rt, &ip, &mask, &gw) != ERR_OK) {
        printf("[net] netif_r8152_add failed : network task idle.\n");
        net_task_idle();
    }
    netif_set_link_up(&s_netif);
    netif_set_up(&s_netif);

    printf("[net] netif UP : IP=%u.%u.%u.%u  mask=255.255.255.0  gw=%u.%u.%u.%u\n",
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_GW_ADDR3);

    /* --- Remote shell TCP server (telnet-like port 23) --- */
    int sh = tcp_shell_start();
    if (sh == 0)
        printf("[net] TCP shell listening on port %u (telnet %u.%u.%u.%u %u)\n",
               (unsigned)TCP_SHELL_PORT,
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
               (unsigned)TCP_SHELL_PORT);
    else
        printf("[net] WARNING : TCP shell startup failed (code %d).\n", sh);

    /* --- SSH server (wolfSSH port 22) --- */
    int ss = ssh_server_start();
    if (ss == 0)
        printf("[net] SSH server listening on port %u (ssh %s@%u.%u.%u.%u  password: %s)\n",
               (unsigned)SSH_SERVER_PORT, SSH_USER,
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3, SSH_PASS);
    else
        printf("[net] WARNING : SSH server startup failed (code %d).\n", ss);

    /* --- Unified UART shell console: the SAME interpreter as
     * telnet/SSH, accessible over the serial port. Banner + 1st prompt. --- */
    uart_shell_start();
    printf("[net] === UART shell CLI enabled (type 'help') — same shell as "
           "telnet/ssh ===\n");

    /* --- PERMANENT lwIP main-loop: RX polling + timers + SSH + UART --- */
    uint64_t next_beat = timer_now_ticks();
    uint64_t total_rx  = 0;

    for (;;) {
        /* Inject the frames received from the RTL8153B into lwIP (ARP/ICMP/IP/TCP). */
        int rx_now = netif_r8152_poll(&s_netif);
        total_rx += (uint32_t)rx_now;

        /* Advance the lwIP timers (ARP, TCP, ...). */
        sys_check_timeouts();

        /* Advance the SSH state machine (handshake / shell exchange). */
        ssh_server_poll();

        /* UART shell console: processes serial keystrokes (non-blocking). */
        uart_shell_poll();

        /* Reactivity: RX traffic in progress OR active SSH session -> loop
         * back immediately; otherwise wait a bit (USB savings). */
        int busy = (rx_now > 0) || ssh_server_session_active();

        /* Activity heartbeat every ~30 s (visible progress). */
        if (timer_now_ticks() >= next_beat) {
            next_beat = timer_now_ticks() + timer_us_to_ticks(30000000ull);
#if LWIP_STATS
            printf("[net] ...active (RX received=%llu, ICMP in=%lu/out=%lu, "
                   "sessions telnet=%lu ssh=%lu)\n",
                   (unsigned long long)total_rx,
                   (unsigned long)lwip_stats.icmp.recv,
                   (unsigned long)lwip_stats.icmp.xmit,
                   (unsigned long)tcp_shell_sessions(),
                   (unsigned long)ssh_server_sessions());
#else
            printf("[net] ...active (RX received=%llu)\n",
                   (unsigned long long)total_rx);
#endif
        }

        if (!busy)
            net_delay_us(20);
    }
}
