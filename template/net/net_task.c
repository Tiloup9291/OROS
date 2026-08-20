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
 * net_task.c — PERMANENT services loop of Core2 (IO_SOFT).
 *
 * PRODUCTION version. Init chain (xHCI + RTL8153B enumeration + r8152 + lwIP
 * static-IP netif + telnet:23 server + SSH:22 server) then an INFINITE
 * supervisor loop which, on every pass:
 *   - injects the RTL8153B frames into lwIP + advances the lwIP timers,
 *   - advances the SSH state machine,
 *   - serves the UART shell console,
 *   - polls the USB-A keyboard service (HOT-PLUG) -> shell,
 *   - drains klog (logs of the RT cores) and the inter-core mailbox,
 *   - watches the Ethernet LINK (cable HOT-PLUG) and only reports CHANGES.
 *
 * CONSOLE DISCIPLINE: init status lines + errors + link transitions ONLY.
 * No heartbeat, nothing periodic (the console stays available for the shell).
 *
 * DEGRADED MODE (no xHCI / no RTL8153B, e.g. QEMU): the network is skipped and
 * the loop runs LOCALLY (klog + mailbox + UART shell + keyboard). The system
 * therefore stays administrable through the serial console.
 *
 * The remote shell reads the ecat_diag snapshot published CONTINUOUSLY by
 * Core0 -> the `ecat` command reflects the EtherCAT state in REAL TIME.
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
#include "../drivers/usb/kbd_service.h"
#include "../arch/aarch64/timer.h"
#include "../kernel/klog.h"
#include "../kernel/mailbox.h"

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

/* Ethernet link check period (cable hot-plug), in ms. */
#define NET_LINK_CHECK_MS   500u

/* Global netif (a single one, RTL8153B). */
static struct netif  s_netif;
static r8152_dev_t   s_rt;          /* USB-Ethernet device (persistent) */

static void net_delay_us(uint32_t us)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks(us);
    while (timer_now_ticks() < end)
        __asm__ volatile("nop");
}

/* ------------------------------------------------------------------ */
/* Supervisor common to both modes (with or without network)           */
/* ------------------------------------------------------------------ */
/* Messages received per source core (readable while debugging). */
volatile uint64_t g_mbx_recv[CFG_NUM_CORES];

/*
 * Non-network part of the Core2 loop, executed on EVERY pass:
 *   - drains klog (traces produced by the RT cores; the ONLY path allowed
 *     for them to print something),
 *   - consumes the inter-core mailbox,
 *   - serves the UART shell console,
 *   - polls the USB-A keyboard service (hot-plug scan / report reads).
 * Fully non-blocking, and silent unless a driver reports a transition.
 */
static void core2_supervise(void)
{
    klog_drain_to_uart();

    uint32_t src;
    uint64_t msg;
    while (mailbox_recv_any(&src, &msg)) {
        if (src < CFG_NUM_CORES)
            g_mbx_recv[src]++;
    }

    uart_shell_poll();
    (void)kbd_service_poll();
}

/*
 * Degraded mode: no USB-Ethernet available. We do NOT go idle (that would
 * kill the console and the keyboard): the local supervisor keeps running
 * forever. Never returns.
 */
static void net_local_loop(void)
{
    uart_shell_start();
    printf("[net] local console mode (no USB-Ethernet) : shell UART + "
           "USB-A keyboard active.\n");

    for (;;) {
        core2_supervise();
        net_delay_us(200);
    }
}

void net_task_entry(void *arg)
{
    (void)arg;

    /* --- xHCI + RTL8153B enumeration ---
     * Any failure here = no IP network: we fall back to the LOCAL console
     * loop (shell + keyboard stay alive), we never go idle. */
    usb_status_t st = xhci_init();
    if (st == USB_ENODEV) {
        printf("[net] no xHCI controller : IP network unavailable.\n");
        net_local_loop();
    }
    if (st != USB_OK) {
        printf("[net] ERROR: xhci_init failed (code %d) : IP network unavailable.\n",
               (int)st);
        net_local_loop();
    }
    usb_core_set_hcd(&xhci_hcd_ops);

    static usb_device_t dev;
    st = usb_enumerate(&xhci_hcd_ops, &dev);
    if (st != USB_OK) {
        printf("[net] ERROR: USB enumeration failed (code %d) : "
               "IP network unavailable.\n", (int)st);
        net_local_loop();
    }

    int is_rtl = (dev.dev_desc.idVendor == USB_VID_REALTEK &&
                  dev.dev_desc.idProduct == USB_PID_RTL8153);
    if (!is_rtl) {
        printf("[net] ERROR: device %04X:%04X is not the RTL8153B : "
               "IP network unavailable.\n",
               dev.dev_desc.idVendor, dev.dev_desc.idProduct);
        net_local_loop();
    }

    /* --- r8152 driver: MAC + hardware init (does NOT wait for the link) --- */
    st = r8152_probe(&s_rt, &dev);
    if (st != USB_OK) {
        printf("[net] ERROR: r8152_probe failed (code %d) : "
               "IP network unavailable.\n", (int)st);
        net_local_loop();
    }
    printf("[net] RTL8153B ready (%s) MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
           (dev.speed == USB_SPEED_SUPER) ? "SuperSpeed" : "USB2",
           s_rt.mac[0], s_rt.mac[1], s_rt.mac[2],
           s_rt.mac[3], s_rt.mac[4], s_rt.mac[5]);

    /* Cable state at boot: read ONCE, non-blocking. An absent cable is NOT an
     * error: the netif is brought up anyway and the permanent loop will detect
     * the plug (hot-plug). */
    int link = r8152_link_status(&s_rt);

    /* --- lwIP: init + static IP netif --- */
    lwip_init();

    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw,   NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_GW_ADDR3);

    if (netif_r8152_add(&s_netif, &s_rt, &ip, &mask, &gw) != ERR_OK) {
        printf("[net] ERROR: netif_r8152_add failed : IP network unavailable.\n");
        net_local_loop();
    }
    /* The interface is administratively UP; the LINK follows the cable. */
    netif_set_up(&s_netif);
    if (link)
        netif_set_link_up(&s_netif);
    else
        netif_set_link_down(&s_netif);

    printf("[net] netif UP : IP=%u.%u.%u.%u mask=255.255.255.0 gw=%u.%u.%u.%u "
           "(cable %s)\n",
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_GW_ADDR3,
           link ? "connected, link UP" : "absent, link DOWN (hot-plug armed)");

    /* --- Remote shell TCP server (telnet-like port 23) --- */
    int sh = tcp_shell_start();
    if (sh == 0)
        printf("[net] TCP shell listening on port %u (telnet %u.%u.%u.%u %u)\n",
               (unsigned)TCP_SHELL_PORT,
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
               (unsigned)TCP_SHELL_PORT);
    else
        printf("[net] ERROR: TCP shell startup failed (code %d).\n", sh);

    /* --- SSH server (wolfSSH port 22) --- */
    int ss = ssh_server_start();
    if (ss == 0) {
        printf("[net] SSH server listening on port %u (ssh %s@%u.%u.%u.%u  password: %s)\n",
               (unsigned)SSH_SERVER_PORT, SSH_USER,
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3, SSH_PASS);
        /* SFTP rides on the SAME port/handshake/credentials as the shell: the
         * client just requests the "sftp" subsystem instead of a shell. */
        printf("[net] SFTP enabled on the same port %u (sftp %s@%u.%u.%u.%u), "
               "confined to %s\n",
               (unsigned)SSH_SERVER_PORT, SSH_USER,
               NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
               SSH_SFTP_ROOT);
    }
    else
        printf("[net] ERROR: SSH server startup failed (code %d).\n", ss);

    /* --- Unified UART shell console: the SAME interpreter as
     * telnet/SSH, accessible over the serial port. Banner + 1st prompt. --- */
    uart_shell_start();
    printf("[net] shell available on UART, telnet:%u and ssh:%u "
           "(type 'help').\n",
           (unsigned)TCP_SHELL_PORT, (unsigned)SSH_SERVER_PORT);

    /* --- PERMANENT main loop: lwIP RX/timers + SSH + shell + keyboard
     *     + Ethernet cable hot-plug. No periodic message. --- */
    uint64_t next_link = timer_now_ticks() +
                         timer_us_to_ticks((uint64_t)NET_LINK_CHECK_MS * 1000ull);

    for (;;) {
        /* Inject the frames received from the RTL8153B into lwIP (ARP/ICMP/IP/TCP).
         * Only useful when the link is up (RX is empty otherwise). */
        int rx_now = link ? netif_r8152_poll(&s_netif) : 0;

        /* Advance the lwIP timers (ARP, TCP, ...). */
        sys_check_timeouts();

        /* Advance the SSH state machine (handshake / shell exchange). */
        ssh_server_poll();

        /* klog + mailbox + UART shell + USB-A keyboard (hot-plug). */
        core2_supervise();

        /* --- Ethernet cable HOT-PLUG: periodic non-blocking read, and a
         * message ONLY on a state change. --- */
        if (timer_now_ticks() >= next_link) {
            next_link = timer_now_ticks() +
                        timer_us_to_ticks((uint64_t)NET_LINK_CHECK_MS * 1000ull);
            int now_link = r8152_link_status(&s_rt);
            if (now_link != link) {
                link = now_link;
                if (link) {
                    netif_set_link_up(&s_netif);
                    printf("[net] Ethernet link UP (cable connected).\n");
                } else {
                    netif_set_link_down(&s_netif);
                    printf("[net] Ethernet link DOWN (cable disconnected).\n");
                }
            }
        }

        /* Reactivity: RX traffic in progress OR active SSH session -> loop
         * back immediately; otherwise wait a bit (USB savings). */
        if (!((rx_now > 0) || ssh_server_session_active()))
            net_delay_us(20);
    }
}
