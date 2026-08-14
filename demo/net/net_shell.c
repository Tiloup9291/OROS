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
 * net_shell.c — Remote shell command interpreter
 *               + `wcet` command (WCET validation).
 *
 * Parses a command line and produces a text response via an output callback
 * (transport-agnostic). Used by the telnet-like TCP server (tcp_shell.c),
 * the SSH server (ssh_server.c) and the UART console (uart_shell.c).
 * No dependency on lwIP at the parsing level: the shell is pure text.
 *
 * Commands:
 *   help / ?            list of commands
 *   uptime             time elapsed since boot
 *   net                lwIP stack state (IP, MAC, stats)
 *   stats              lwIP ICMP/ARP/TCP counters
 *   ecat               EtherCAT master diagnostics (shared snapshot)
 *   ecat slaves        EtherCAT slave details
 *   wcet               WCET campaign (EtherCAT cycle, wake-up jitter, histogram)
 *   echo <text>        returns the text
 *   ver                version / phase
 *   quit / exit        closes the session
 */
#include "net_shell.h"
#include "ecat_diag.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "lwip/stats.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "../arch/aarch64/timer.h"

/* ------------------------------------------------------------------ */
/* Output helpers                                                     */
/* ------------------------------------------------------------------ */
static void sh_puts(shell_out_fn out, void *ctx, const char *s)
{
    out(ctx, s, strlen(s));
}

/* Limited printf-like (local buffer) to the output callback. */
static void sh_printf(shell_out_fn out, void *ctx, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    out(ctx, buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Commands                                                           */
/* ------------------------------------------------------------------ */
static void cmd_help(shell_out_fn out, void *ctx)
{
    sh_puts(out, ctx,
        "Available commands :\r\n"
        "  help, ?        this help\r\n"
        "  ver            version\r\n"
        "  uptime         time from boot\r\n"
        "  net            status of IP stack (IP, MAC)\r\n"
        "  stats          network counter (ICMP/ARP/TCP)\r\n"
        "  ecat           EtherCAT master diagnostic\r\n"
        "  ecat slaves    EtherCAT slaves details\r\n"
        "  wcet           WCET validation (EtherCAT cycle, wake-up jitter, histogram)\r\n"
        "  echo <text>    return text\r\n"
        "  quit, exit     quit session\r\n");
}

static void cmd_ver(shell_out_fn out, void *ctx)
{
    sh_puts(out, ctx,
        "OROS (Orange Pi R1 Plus LTS)\r\n"
        "  UART/TCP/SSH unified shell; Core0 permanent EtherCAT // Core2 network\r\n");
}

static void cmd_uptime(shell_out_fn out, void *ctx)
{
    uint64_t ticks = timer_now_ticks();
    uint64_t us    = timer_ticks_to_us(ticks);
    uint64_t s     = us / 1000000ull;
    uint64_t ms    = (us / 1000ull) % 1000ull;
    uint64_t h     = s / 3600ull;
    uint64_t m     = (s / 60ull) % 60ull;
    uint64_t sec   = s % 60ull;
    sh_printf(out, ctx, "uptime : %lluh %02llum %02llus %03llums (%llu us)\r\n",
              (unsigned long long)h, (unsigned long long)m,
              (unsigned long long)sec, (unsigned long long)ms,
              (unsigned long long)us);
}

static void cmd_net(shell_out_fn out, void *ctx)
{
    struct netif *nif = netif_default;
    if (!nif) {
        sh_puts(out, ctx, "net : no netif active.\r\n");
        return;
    }
    const ip4_addr_t *ip   = netif_ip4_addr(nif);
    const ip4_addr_t *mask = netif_ip4_netmask(nif);
    const ip4_addr_t *gw   = netif_ip4_gw(nif);

    sh_printf(out, ctx, "netif  : %c%c%u  %s%s\r\n",
              nif->name[0], nif->name[1], (unsigned)nif->num,
              netif_is_up(nif) ? "UP" : "DOWN",
              netif_is_link_up(nif) ? " (link up)" : " (link down)");
    sh_printf(out, ctx, "IP     : %u.%u.%u.%u\r\n",
              ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip));
    sh_printf(out, ctx, "mask   : %u.%u.%u.%u\r\n",
              ip4_addr1(mask), ip4_addr2(mask), ip4_addr3(mask), ip4_addr4(mask));
    sh_printf(out, ctx, "gw     : %u.%u.%u.%u\r\n",
              ip4_addr1(gw), ip4_addr2(gw), ip4_addr3(gw), ip4_addr4(gw));
    sh_printf(out, ctx, "MAC    : %02X:%02X:%02X:%02X:%02X:%02X  MTU=%u\r\n",
              nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
              nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5],
              (unsigned)nif->mtu);
}

static void cmd_stats(shell_out_fn out, void *ctx)
{
#if LWIP_STATS
    sh_printf(out, ctx, "ICMP : recv=%lu xmit=%lu\r\n",
              (unsigned long)lwip_stats.icmp.recv,
              (unsigned long)lwip_stats.icmp.xmit);
    sh_printf(out, ctx, "ARP  : recv=%lu xmit=%lu\r\n",
              (unsigned long)lwip_stats.etharp.recv,
              (unsigned long)lwip_stats.etharp.xmit);
    sh_printf(out, ctx, "TCP  : recv=%lu xmit=%lu drop=%lu\r\n",
              (unsigned long)lwip_stats.tcp.recv,
              (unsigned long)lwip_stats.tcp.xmit,
              (unsigned long)lwip_stats.tcp.drop);
    sh_printf(out, ctx, "IP   : recv=%lu xmit=%lu\r\n",
              (unsigned long)lwip_stats.ip.recv,
              (unsigned long)lwip_stats.ip.xmit);
#else
    sh_puts(out, ctx, "stats : LWIP_STATS disabled.\r\n");
#endif
}

static const char *al_state_str(uint32_t al)
{
    switch (al) {
    case 1:  return "INIT";
    case 2:  return "PREOP";
    case 4:  return "SAFEOP";
    case 8:  return "OP";
    default: return "?";
    }
}

static void cmd_ecat(shell_out_fn out, void *ctx, const char *arg)
{
    ecat_diag_t *d = ecat_diag_get();

    if (!d->valid) {
        sh_puts(out, ctx,
            "ecat : no diagnostic available.\r\n"
            "       (EtherCAT master is not yet running, or no slave/GMAC)\r\n");
        return;
    }

    if (arg && strcmp(arg, "slaves") == 0) {
        sh_printf(out, ctx, "slaves: online=%lu operationnal=%lu\r\n",
                  (unsigned long)d->slaves_online, (unsigned long)d->slaves_op);
        sh_printf(out, ctx, "  slave0 : AL=%s (0x%02lX) vendor=0x%08lX product=0x%08lX\r\n",
                  al_state_str(d->al_state), (unsigned long)d->al_state,
                  (unsigned long)d->vendor_id, (unsigned long)d->product_code);
        sh_printf(out, ctx, "  DI=0x%04lX  DO=0x%04lX  WKC=%lu\r\n",
                  (unsigned long)d->di, (unsigned long)d->do_,
                  (unsigned long)d->last_wkc);
        return;
    }

    sh_printf(out, ctx, "EtherCAT master: %s (GMAC link %lu Mbit/s)\r\n",
              d->master_up ? "active" : "inactive",
              (unsigned long)d->link_mbps);
    sh_printf(out, ctx, "slaves        : online=%lu  OP=%lu  AL(slave0)=%s\r\n",
              (unsigned long)d->slaves_online, (unsigned long)d->slaves_op,
              al_state_str(d->al_state));
    sh_printf(out, ctx, "process data    : DI=0x%04lX  DO=0x%04lX  WKC=%lu\r\n",
              (unsigned long)d->di, (unsigned long)d->do_,
              (unsigned long)d->last_wkc);
    sh_printf(out, ctx, "cycles          : total=%lu  WKC>0=%lu  WKC=0=%lu  overruns=%lu\r\n",
              (unsigned long)d->cycles_total, (unsigned long)d->wkc_ok,
              (unsigned long)d->wkc_zero, (unsigned long)d->overruns);
    sh_printf(out, ctx, "jitter max cycle: %llu CPU cycles (PMU)\r\n",
              (unsigned long long)d->jit_max_cycles);
    sh_puts(out, ctx, "  ('ecat slaves' for slaves details, 'wcet' for WCET validation)\r\n");
}

/* Converts CPU (PMU) cycles into nanoseconds according to cpu_hz (0 if unknown). */
static uint64_t cyc_to_ns(uint64_t cyc, uint64_t cpu_hz)
{
    return (cpu_hz > 0) ? (cyc * 1000000000ull) / cpu_hz : 0u;
}

/* Converts Generic Timer ticks into nanoseconds according to tfreq. */
static uint64_t ticks_to_ns(uint64_t ticks, uint64_t tfreq)
{
    return (tfreq > 0) ? (ticks * 1000000000ull) / tfreq : 0u;
}

/*
 * cmd_wcet — WCET VALIDATION report.
 *
 * Displays, for Core0's hard-RT EtherCAT cycle measured at the PMU IN
 * PARALLEL with Core2's network/USB/SSH load:
 *   - the cycle PROCESSING time (min / avg / max) in CPU cycles and in ns;
 *   - the WAKE-UP JITTER (max / avg) in ns/µs (isolation indicator);
 *   - the wake-up jitter HISTOGRAM;
 *   - a VERDICT (WCET margin vs period, overruns).
 */
static void cmd_wcet(shell_out_fn out, void *ctx)
{
    ecat_diag_t *d = ecat_diag_get();

    if (!d->valid || !d->master_up) {
        sh_puts(out, ctx,
            "wcet : EtherCAT master inactive (no slave/GMAC).\r\n"
            "       The WCET validation need the permanent EtherCAT cycle (Core0).\r\n");
        return;
    }

    uint64_t cpu_hz    = d->cpu_hz;
    uint64_t n         = d->wcet_samples;
    uint64_t period_us = d->cycle_us;

    sh_puts(out, ctx,
        "===== WCET VALIDATION - EtherCAT hard-RT cycle (Core0) =====\r\n");
    sh_printf(out, ctx,
        "cycle period   : %llu us   (warm-up ignored : %lu cycles)\r\n",
        (unsigned long long)period_us, (unsigned long)d->warmup_cycles);
    sh_printf(out, ctx,
        "samples    : %llu measured cycles   |   overruns : %lu\r\n",
        (unsigned long long)n, (unsigned long)d->overruns);
    if (cpu_hz)
        sh_printf(out, ctx, "CPU frequency   : %llu MHz (PMU calibrated)\r\n",
                  (unsigned long long)(cpu_hz / 1000000ull));
    else
        sh_puts(out, ctx, "CPU frequency   : not calibrated (ns unavailable)\r\n");

    /* (a) cycle processing time */
    uint64_t p_min = d->proc_min_cyc;
    uint64_t p_max = d->proc_max_cyc;
    uint64_t p_avg = (n > 0) ? d->proc_sum_cyc / n : 0u;
    sh_puts(out, ctx, "\r\n-- cycle processing time (CPU load) --\r\n");
    sh_printf(out, ctx, "  min : %8llu cyc  (%8llu ns)\r\n",
              (unsigned long long)p_min, (unsigned long long)cyc_to_ns(p_min, cpu_hz));
    sh_printf(out, ctx, "  moy : %8llu cyc  (%8llu ns)\r\n",
              (unsigned long long)p_avg, (unsigned long long)cyc_to_ns(p_avg, cpu_hz));
    sh_printf(out, ctx, "  MAX : %8llu cyc  (%8llu ns)  <= WCET processing\r\n",
              (unsigned long long)p_max, (unsigned long long)cyc_to_ns(p_max, cpu_hz));

    /* Margin: processing WCET vs period. */
    if (cpu_hz && period_us) {
        uint64_t wcet_ns   = cyc_to_ns(p_max, cpu_hz);
        uint64_t period_ns = period_us * 1000ull;
        uint64_t used_pmille = (period_ns > 0) ? (wcet_ns * 1000ull) / period_ns : 0u;
        sh_printf(out, ctx,
            "  cycle load: %llu.%llu %% of the period (%llu ns / %llu ns)\r\n",
            (unsigned long long)(used_pmille / 10ull),
            (unsigned long long)(used_pmille % 10ull),
            (unsigned long long)wcet_ns, (unsigned long long)period_ns);
    }

    /* (b) wake-up jitter (isolation against Core2). */
    uint64_t tfreq = timer_frequency();
    uint64_t w_max_ns    = ticks_to_ns(d->wake_max_ticks, tfreq);
    uint64_t w_avg_ticks = (n > 0) ? d->wake_sum_ticks / n : 0u;
    uint64_t w_avg_ns    = ticks_to_ns(w_avg_ticks, tfreq);
    sh_puts(out, ctx, "\r\n-- wake-up jitter (isolation vs Core2 load) --\r\n");
    sh_printf(out, ctx, "  moy : %llu ns   |   MAX : %llu ns (%llu us)\r\n",
              (unsigned long long)w_avg_ns,
              (unsigned long long)w_max_ns,
              (unsigned long long)(w_max_ns / 1000ull));

    /* Wake-up jitter histogram. */
    sh_puts(out, ctx, "  histogram (wake-up jitter) :\r\n");
    for (unsigned i = 0; i < ECAT_WCET_BUCKETS; i++) {
        char range[24];
        if (i == 0)
            snprintf(range, sizeof(range), "[0-%lu) us",
                     (unsigned long)ecat_wcet_bucket_us[0]);
        else if (i == ECAT_WCET_BUCKETS - 1u)
            snprintf(range, sizeof(range), ">=%lu us",
                     (unsigned long)ecat_wcet_bucket_us[i - 1]);
        else
            snprintf(range, sizeof(range), "[%lu-%lu) us",
                     (unsigned long)ecat_wcet_bucket_us[i - 1],
                     (unsigned long)ecat_wcet_bucket_us[i]);
        sh_printf(out, ctx, "    %-12s : %lu\r\n",
                  range, (unsigned long)d->wake_hist[i]);
    }

    /* Verdict. */
    sh_puts(out, ctx, "\r\n-- verdict --\r\n");
    if (d->overruns == 0)
        sh_puts(out, ctx, "  overruns=0 : no period overrun. ");
    else
        sh_printf(out, ctx, "  overruns=%lu : overruns observed. ",
                  (unsigned long)d->overruns);
    if (cpu_hz && period_us &&
        cyc_to_ns(d->proc_max_cyc, cpu_hz) < period_us * 1000ull)
        sh_puts(out, ctx, "WCET processing < period : OK.\r\n");
    else
        sh_puts(out, ctx, "\r\n");
}

/* ------------------------------------------------------------------ */
/* Line parsing                                                       */
/* ------------------------------------------------------------------ */
/* Splits `line` into a head (command) and the rest (arguments). Returns a
 * pointer to the 1st argument (string, possibly empty) and terminates the
 * command word in `cmd` with '\0'. `cmd` must be at least as large as `line`. */
static const char *split_cmd(const char *line, char *cmd, size_t cmd_sz)
{
    /* skip leading spaces */
    while (*line == ' ' || *line == '\t') line++;

    size_t i = 0;
    while (line[i] && line[i] != ' ' && line[i] != '\t' && i < cmd_sz - 1) {
        cmd[i] = line[i];
        i++;
    }
    cmd[i] = '\0';

    const char *rest = line + i;
    while (*rest == ' ' || *rest == '\t') rest++;
    return rest;
}

int net_shell_exec(const char *line, shell_out_fn out, void *ctx)
{
    char cmd[64];
    const char *arg = split_cmd(line, cmd, sizeof(cmd));

    if (cmd[0] == '\0') {
        /* empty line: nothing to do */
        return 0;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help(out, ctx);
    } else if (strcmp(cmd, "ver") == 0 || strcmp(cmd, "version") == 0) {
        cmd_ver(out, ctx);
    } else if (strcmp(cmd, "uptime") == 0) {
        cmd_uptime(out, ctx);
    } else if (strcmp(cmd, "net") == 0) {
        cmd_net(out, ctx);
    } else if (strcmp(cmd, "stats") == 0) {
        cmd_stats(out, ctx);
    } else if (strcmp(cmd, "ecat") == 0) {
        cmd_ecat(out, ctx, arg);
    } else if (strcmp(cmd, "wcet") == 0) {
        cmd_wcet(out, ctx);
    } else if (strcmp(cmd, "echo") == 0) {
        sh_puts(out, ctx, arg);
        sh_puts(out, ctx, "\r\n");
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        sh_puts(out, ctx, "bye.\r\n");
        return 1;
    } else {
        sh_printf(out, ctx, "unknown command: '%s' (type 'help')\r\n", cmd);
    }
    return 0;
}

void net_shell_welcome(shell_out_fn out, void *ctx)
{
    sh_puts(out, ctx,
        "\r\n"
        "=================================================\r\n"
        " OROS - remote shell\r\n"
        " Type 'help' to display the list of commands.\r\n"
        "=================================================\r\n");
    net_shell_prompt(out, ctx);
}

void net_shell_prompt(shell_out_fn out, void *ctx)
{
    sh_puts(out, ctx, "oros> ");
}
