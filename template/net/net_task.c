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
 * net_task.c — PERMANENT services of Core2 (IO_SOFT).
 *
 * PRODUCTION version. Init chain (xHCI + RTL8153B enumeration + r8152 + lwIP
 * static-IP netif + telnet:23 server + SSH:22 server), then Core2 is split
 * into THREE permanent threads of equal priority (round-robin, 1 ms tick):
 *   - io2_net  : injects the RTL8153B frames into lwIP, advances the lwIP
 *                timers and the SSH state machine, polls the cable link;
 *   - io2_logs : drains klog (logs of the RT cores) with a bounded budget;
 *   - io2_svc  : inter-core mailbox, UART shell console, USB-A keyboard
 *                service (HOT-PLUG) -> shell.
 *
 * RATIONALE: everything used to run in a SINGLE loop, so the blocking UART
 * log drain (or a costly SSH handshake) stalled every other service and made
 * remote sessions very laggy. Splitting bounds the latency of each service.
 *
 * OWNERSHIP RULES (the split is only safe because of these):
 *   - lwIP is NO_SYS=1 (not thread safe): only io2_net calls into it;
 *   - the xHCI controller and the RTL8153B (bulk RX/TX *and* the control
 *     transfers of r8152_link_status) belong to io2_net ONLY. The xHCI
 *     driver keeps its rings/doorbells/event ring in unprotected globals,
 *     so a second thread touching the same controller corrupts transfers;
 *   - io2_svc drives the USB-A keyboard on the *other* controllers
 *     (EHCI/OHCI), whose state is separate -> no conflict with io2_net.
 *
 * CONSOLE DISCIPLINE: init status lines + errors + link transitions ONLY.
 * No heartbeat, nothing periodic (the console stays available for the shell).
 *
 * DEGRADED MODE (no xHCI / no RTL8153B, e.g. QEMU): the network is skipped,
 * the three threads still run (io2_net idles). The system therefore stays
 * administrable through the serial console.
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
#include "../kernel/thread.h"

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
/* Core2 split into 3 threads (IO_SOFT, equal priority):                */
/*   - io2_net  : r8152 RX -> lwIP + timeouts + SSH (network only)      */
/*   - io2_logs : budgeted klog drain (isolates the slow UART logs)     */
/*   - io2_svc  : mailbox + UART shell + USB keyboard + link hot-plug   */
/* At equal priority the preemptive scheduler (1 ms tick) round-robins  */
/* them: no thread can starve the others, so a costly SSH handshake or  */
/* a burst of logs can no longer freeze the whole supervisor.           */
/* ------------------------------------------------------------------ */

/* Network state shared between the threads. */
static volatile int s_net_ok;   /* 1 = network initialized + servers started */
static volatile int s_link;     /* current Ethernet link state               */

/* Messages received per source core (readable while debugging). */
volatile uint64_t g_mbx_recv[CFG_NUM_CORES];

/* Max number of log messages emitted per pass of the log thread. The UART
 * is blocking (~11.5 KB/s): bounding the drain bounds the time this thread
 * can hold the CPU. */
#ifndef KLOG_DRAIN_BUDGET
#define KLOG_DRAIN_BUDGET  32u
#endif

/* Idle delays (us) of the loops: avoid burning the CPU when there is
 * nothing to do. */
#define SVC_LOOP_DELAY_US    200u
#define NET_IDLE_DELAY_US     20u

/* Consumes the inter-core mailbox (non-blocking). */
static void core2_drain_mailbox(void)
{
    uint32_t src;
    uint64_t msg;
    while (mailbox_recv_any(&src, &msg)) {
        if (src < CFG_NUM_CORES)
            g_mbx_recv[src]++;
    }
}

/*
 * io2_net thread — network ONLY. It does nothing else, so a network burst
 * (or a costly SSH handshake) can no longer block the logs nor the console.
 * With no network (degraded mode) the thread stays armed and sleeps.
 */
static void net_rx_loop(void *arg)
{
    (void)arg;
    uint64_t next_link = timer_now_ticks() +
                         timer_us_to_ticks((uint64_t)NET_LINK_CHECK_MS * 1000ull);

    for (;;) {
        if (!s_net_ok) {
            net_delay_us(SVC_LOOP_DELAY_US);
            continue;
        }

        int rx_now = s_link ? netif_r8152_poll(&s_netif) : 0;
        sys_check_timeouts();
        ssh_server_poll();

        /* Ethernet cable HOT-PLUG. This poll issues a USB CONTROL transfer,
         * so it MUST happen in this thread: the xHCI driver keeps its rings,
         * doorbells and event ring in globals with no locking, and mixing it
         * with the bulk RX/TX of another thread corrupts both transfers
         * (symptom: link flapping UP/DOWN and dead RX/TX).
         * Reported ONLY on a state change. */
        if (timer_now_ticks() >= next_link) {
            next_link = timer_now_ticks() +
                        timer_us_to_ticks((uint64_t)NET_LINK_CHECK_MS * 1000ull);
            int now_link = r8152_link_status(&s_rt);
            if (now_link != s_link) {
                s_link = now_link;
                if (s_link) {
                    netif_set_link_up(&s_netif);
                    printf("[net] Ethernet link UP (cable connected).\n");
                } else {
                    netif_set_link_down(&s_netif);
                    printf("[net] Ethernet link DOWN (cable disconnected).\n");
                }
            }
        }

        if (!((rx_now > 0) || ssh_server_session_active()))
            net_delay_us(NET_IDLE_DELAY_US);
    }
}

/*
 * io2_logs thread — drains the klog rings to the UART with a TIGHT BUDGET,
 * so it can never freeze the network (~32 messages per pass). */
static void net_klog_loop(void *arg)
{
    (void)arg;
    uint32_t emitted;
    for (;;) {
        (void)klog_drain_budgeted(KLOG_DRAIN_BUDGET, &emitted);
        net_delay_us(SVC_LOOP_DELAY_US);
    }
}

/*
 * io2_svc thread — services other than network and klog: inter-core
 * mailbox, UART console, USB-A keyboard (EHCI/OHCI). It must never touch
 * lwIP nor the xHCI/RTL8153B, which belong to io2_net. */
static void net_svc_loop(void *arg)
{
    (void)arg;

    for (;;) {
        core2_drain_mailbox();
        uart_shell_poll();
        (void)kbd_service_poll();
        net_delay_us(SVC_LOOP_DELAY_US);
    }
}

/*
 * Starts the 3 Core2 threads (equal priority). Used in both modes (with or
 * without network). Never returns.
 */
static void core2_start_threads(void)
{
    (void)thread_create_on("io2_net",  net_rx_loop,  NULL, 10, CFG_CORE_IO_SOFT);
    (void)thread_create_on("io2_logs", net_klog_loop, NULL, 10, CFG_CORE_IO_SOFT);
    (void)thread_create_on("io2_svc",  net_svc_loop,  NULL, 10, CFG_CORE_IO_SOFT);

    /* The init thread is done: it must NOT return to its caller, otherwise
     * the initialization sequence would continue and be replayed. Park it
     * for good; the scheduler then switches to the 3 service threads
     * created above. */
    for (;;)
        thread_yield();
}

/*
 * Degraded mode: no USB-Ethernet available (no xHCI / no RTL8153B, e.g.
 * QEMU). We do NOT go idle (that would kill the console and the keyboard):
 * the UART console is started, then the 3 Core2 threads run (the network
 * thread stays armed and sleeping). Never returns.
 */
static void core2_degraded(void)
{
    uart_shell_start();
    printf("[net] local console mode (no USB-Ethernet) : shell UART + "
           "USB-A keyboard active.\n");

    s_link   = 0;
    s_net_ok = 0;
    core2_start_threads();
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
        core2_degraded();
    }
    if (st != USB_OK) {
        printf("[net] ERROR: xhci_init failed (code %d) : IP network unavailable.\n",
               (int)st);
        core2_degraded();
    }
    usb_core_set_hcd(&xhci_hcd_ops);

    static usb_device_t dev;
    st = usb_enumerate(&xhci_hcd_ops, &dev);
    if (st != USB_OK) {
        printf("[net] ERROR: USB enumeration failed (code %d) : "
               "IP network unavailable.\n", (int)st);
        core2_degraded();
    }

    int is_rtl = (dev.dev_desc.idVendor == USB_VID_REALTEK &&
                  dev.dev_desc.idProduct == USB_PID_RTL8153);
    if (!is_rtl) {
        printf("[net] ERROR: device %04X:%04X is not the RTL8153B : "
               "IP network unavailable.\n",
               dev.dev_desc.idVendor, dev.dev_desc.idProduct);
        core2_degraded();
    }

    /* --- r8152 driver: MAC + hardware init (does NOT wait for the link) --- */
    st = r8152_probe(&s_rt, &dev);
    if (st != USB_OK) {
        printf("[net] ERROR: r8152_probe failed (code %d) : "
               "IP network unavailable.\n", (int)st);
        core2_degraded();
    }
    printf("[net] RTL8153B ready (%s) MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
           (dev.speed == USB_SPEED_SUPER) ? "SuperSpeed" : "USB2",
           s_rt.mac[0], s_rt.mac[1], s_rt.mac[2],
           s_rt.mac[3], s_rt.mac[4], s_rt.mac[5]);

    /* Cable state at boot: read ONCE, non-blocking. An absent cable is NOT
     * an error: the netif is brought up anyway and the service thread will
     * detect the plug (hot-plug). */
    s_link = r8152_link_status(&s_rt);

    /* --- lwIP: init + static IP netif --- */
    lwip_init();

    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw,   NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_GW_ADDR3);

    if (netif_r8152_add(&s_netif, &s_rt, &ip, &mask, &gw) != ERR_OK) {
        printf("[net] ERROR: netif_r8152_add failed : IP network unavailable.\n");
        core2_degraded();
    }
    /* The interface is administratively UP; the LINK follows the cable. */
    netif_set_up(&s_netif);
    if (s_link)
        netif_set_link_up(&s_netif);
    else
        netif_set_link_down(&s_netif);

    printf("[net] netif UP : IP=%u.%u.%u.%u mask=255.255.255.0 gw=%u.%u.%u.%u "
           "(cable %s)\n",
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_IP_ADDR3,
           NET_IP_ADDR0, NET_IP_ADDR1, NET_IP_ADDR2, NET_GW_ADDR3,
           s_link ? "connected, link UP" : "absent, link DOWN (hot-plug armed)");

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

    /* --- Network and servers ready: arm the network thread. --- */
    s_net_ok = 1;   /* the Ethernet link (s_link) was determined above */

    /* --- Hand over to the scheduler: the 3 Core2 threads (network / logs /
     * services) share the core in round-robin. This init thread is parked
     * inside core2_start_threads() and never returns. --- */
    core2_start_threads();
}
