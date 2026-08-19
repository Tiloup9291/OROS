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
 *   help / ?                  list of commands
 *   shutdown                 power off the system (PSCI SYSTEM_OFF)
 *   reboot                   reboot the system (PSCI SYSTEM_RESET)
 *   uptime                   time elapsed since boot
 *   net                      lwIP stack state (IP, MAC, stats)
 *   stats                    lwIP ICMP/ARP/TCP counters
 *   ecat                     EtherCAT master diagnostics (shared snapshot)
 *   ecat slaves              EtherCAT slave details
 *   wcet                     WCET campaign (EtherCAT cycle, wake-up jitter, histogram)
 *   echo <text>              returns the text
 *   ls [-l] [path][/pattern] list FAT volume 0:
 *   cd <path>                change current directory
 *   pwd                      print working directory
 *   mkdir [-p] <path>        create a directory (option -p = create parents)
 *   rm [-r] [-f] <path>      remove file or empty directory (option -r recursive)
 *   cat <file>               print file content (-n : numbered lines; -A: show '$')
 *   mv [-f] <src> <dst>      rename/move file or dir (FAT volume 0:)
 *   cp                       copy file or directory tree (FAT volume 0:)
 *                            [-f] [-r] <src> <dst>
 *   touch [-c] <file>...     create empty files on FAT volume 0:
 *   ver                      version / phase
 *   quit / exit              closes the session
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
#include "../arch/aarch64/smp.h"


#include "ff.h"
#include "../fs/fs_mount.h"
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* cat helpers — buffered transform + flush to the transport.          */
/* ------------------------------------------------------------------ */
#define CAT_RBUF_SIZE  512u   /* chunk read from the file (f_read)     */
#define CAT_OBUF_SIZE  512u   /* output buffer (transformed bytes)     */

/* ---- Shell-level Current Working Directory -------------------------
 * FatFs is built with FF_FS_RPATH=0 (template/lib/fatfs/ffconf.h): its
 * f_chdir()/f_getcwd() are compiled OUT. We therefore track the CWD here
 * and always hand FatFs ABSOLUTE "0:/..." paths. ------------------ */
#define SHELL_PATH_MAX   256u   /* real bound: fits LFN(255)+"/"+drive */
#define SHELL_DRIVE      "0:"

static char s_cwd[SHELL_PATH_MAX];   /* the actual storage          */
char *CWD = s_cwd;             /* exported pointer -> s_cwd   */
size_t CWD_absolute_length = 1; /* strlen("/")                 */
static int s_cwd_ready = 0;

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
/* Output redirection  > / >>                                          */
/* ------------------------------------------------------------------ */
/* Wrapping descriptor: replaces (out, ctx) so every command output is
 * transparently written to a FatFs file instead of the transport. */
typedef struct {
    shell_out_fn base_out;    /* original transport write callback    */
    void        *base_ctx;    /* original transport context           */
    FIL          file;        /* FatFs file handle (target)           */
    int          failed;      /* 1 after a write error (drop output)  */
} redir_ctx_t;

/* The transport-agnostic "out" callback when a redirection is active.
 * Every sh_puts/sh_printf call in ALL commands lands here. */
static void sh_redir_out(void *c, const char *data, size_t len)
{
    redir_ctx_t *rd = (redir_ctx_t *)c;
    UINT         bw = 0;

    if (rd->failed || len == 0)
        return;

    if (f_write(&rd->file, data, (UINT)len, &bw) != FR_OK || bw != len)
        rd->failed = 1;                 /* drop the rest on error */
}

/*
 * Detects & strips a TRAILING Unix-style output redirection off `line`.
 *
 *   "ls"                    -> no redirect                  (returns 0)
 *   "ls > f"                -> cmd="ls", file="f", append=0  (returns 1)
 *   "ls >> f"               -> cmd="ls", file="f", append=1  (returns 1)
 *   "ls >f"  /  "ls >>f"    -> glued form, same semantics    (returns 1)
 *
 * Returns:
 *   -1 : syntax error (operator without a target, or trailing junk)
 *    0 : no redirection (`cmd` = copy of the whole line)
 *    1 : redirection parsed (`cmd` = command only, `file` = target,
 *        `*append` set)
 */
static int sh_parse_redir(const char *line, char *cmd, size_t cmd_sz, char *file,   size_t file_sz, int  *append)
{
    const char *p;
    size_t      op_pos    = (size_t)-1;  /* index of '>' that starts the op */
    int         op_append = 0;
    int         op_glued  = 0;
    const char *file_ptr  = NULL;
    size_t      file_len  = 0;

    /* Scan the tokens; remember the LAST redirection operator found. */
    p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *ts = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t tlen = (size_t)(p - ts);

        if (ts[0] == '>') {
            op_append = (tlen >= 2 && ts[1] == '>');
            op_pos    = (size_t)(ts - line);
            if (op_append) {
                if (tlen == 2)         { file_ptr = NULL; op_glued = 0; file_len = 0; }
                else                   { file_ptr = ts + 2; op_glued = 1; file_len = tlen - 2; }
            } else {
                if (tlen == 1)         { file_ptr = NULL; op_glued = 0; file_len = 0; }
                else                   { file_ptr = ts + 1; op_glued = 1; file_len = tlen - 1; }
            }
        }
    }

    if (op_pos == (size_t)-1) {
        /* No redirection: copy the whole line into cmd. */
        size_t n = strlen(line);
        if (n >= cmd_sz) n = cmd_sz - 1;
        memcpy(cmd, line, n);
        cmd[n] = '\0';
        return 0;
    }

    /* Determine the target file name. */
    if (op_glued) {
        if (file_len <= 0 || file_len >= file_sz)
            return -1;
        memcpy(file, file_ptr, file_len);
        file[file_len] = '\0';
    } else {
        const char *q = line + op_pos + (op_append ? 2 : 1);
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '\0') return -1;               /* operator, no target  */
        const char *f = q;
        while (*q && *q != ' ' && *q != '\t') q++;
        size_t flen = (size_t)(q - f);
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '\0') return -1;               /* extra operands: keep it deterministic */
        if (flen >= file_sz) return -1;
        memcpy(file, f, flen);
        file[flen] = '\0';
    }

    /* Rebuild the command = everything BEFORE op_pos, right-trimmed. */
    size_t clen = op_pos;
    while (clen > 0 && (line[clen-1] == ' ' || line[clen-1] == '\t')) clen--;
    if (clen >= cmd_sz) clen = cmd_sz - 1;
    memcpy(cmd, line, clen);
    cmd[clen] = '\0';

    *append = op_append;
    return 1;
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
        "  shutdown       power the system off (PSCI SYSTEM_OFF)\r\n"
        "  reboot         reboot the system (PSCI SYSTEM_RESET)\r\n"
        "  uptime         time from boot\r\n"
        "  net            status of IP stack (IP, MAC)\r\n"
        "  stats          network counter (ICMP/ARP/TCP)\r\n"
        "  ecat           EtherCAT master diagnostic\r\n"
        "  ecat slaves    EtherCAT slaves details\r\n"
        "  wcet           WCET validation (EtherCAT cycle, wake-up jitter, histogram)\r\n"
        "  echo <text>    return text\r\n"
        "  ls             list FAT volume 0: (ls, ls -l, ls *.txt)\r\n"
        "                 [-l] [path][/pattern]\r\n"
        "  cd <path>      change current directory (cd .., cd /)\r\n"
        "  pwd            print working directory\r\n"
        "  mkdir          create directory on FAT volume 0:\r\n"
        "                 [-p] <path>\r\n"
        "  rm             remove file/dir on FAT volume 0:\r\n"
        "                 [-r] [-f] <path>\r\n"
        "  cat <file>     concatenate and print files (cat -n, -A)\r\n"
        "  mv             rename or move file/dir on FAT volume 0:\r\n"
        "                 [-f] <src> <dst>\r\n"
        "  cp             copy file/dir on FAT volume 0:\r\n"
        "                 [-f] [-r] <src> <dst>\r\n"
        "  touch          create empty files on FAT volume 0:\r\n"
        "                 [-c] <file>...\r\n"
        "  quit, exit     quit session\r\n");
}

static void cmd_ver(shell_out_fn out, void *ctx)
{
    sh_puts(out, ctx,
        "OROS (Orange Pi R1 Plus LTS)\r\n"
        "  UART/TCP/SSH unified shell; Core0 permanent EtherCAT // Core2 network\r\n");
}

/* `shutdown` — clean system power-off through PSCI SYSTEM_OFF.
 * On QEMU (PSCI_HVC) this stops the VM and returns to the host shell.
 * On the board (SMC/ATF) it physically cuts the power. NEVER returns. */
static void cmd_shutdown(shell_out_fn out, void *ctx)
{
    sh_puts(out, ctx,
        "shutdown : system is going down NOW (PSCI SYSTEM_OFF)...\r\n");
    smp_system_off();   /* __attribute__((noreturn)) : never comes back */
}

static void cmd_reboot(shell_out_fn out, void *ctx)
{
    sh_puts(out, ctx,
        "reboot : system is rebooting (PSCI SYSTEM_RESET)...\r\n");
    smp_system_reset();   /* __attribute__((noreturn)) : never comes back */
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

/* Called (once) before any command that touches the FS. Resets to root. */
static void shell_cwd_init(void)
{
    if (s_cwd_ready)
        return;
    if (CWD == NULL)          /* defensive: keep the pointer valid */
        CWD = s_cwd;
    strcpy(CWD, "/");
    CWD_absolute_length = 1u;
    s_cwd_ready = 1;
}

/* Builds the absolute FatFs path ("0:/...") of a path operand relative to
 * CWD. Handles "", '.', '..', and absolute paths (leading '/'). */
static void shell_resolve(const char *rel, char *out, size_t sz)
{
    char   tmp[SHELL_PATH_MAX];
    size_t n = 0;

    /* 1) base = CWD for a relative path, empty for an absolute one */
    if (rel[0] != '/') {
        n = (size_t)CWD_absolute_length;
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, CWD, n);
        tmp[n] = '\0';
    } else {
        tmp[0] = '\0';
    }

    /* 2) append components, resolving '.' and '..' */
    while (*rel) {
        while (*rel == '/') rel++;
        if (*rel == '\0') break;
        const char *start = rel;
        while (*rel && *rel != '/') rel++;
        size_t clen = (size_t)(rel - start);

        if (clen == 1 && start[0] == '.') {
            ;                                   /* stay */
        } else if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 1 && tmp[n - 1] != '/') n--;   /* go up one level */
            if (n > 1) n--;
            if (n == 0) { tmp[0] = '/'; n = 1; }      /* clamp at root */
        } else {
            if (n > 0 && tmp[n - 1] != '/' && n + 1 < sizeof(tmp))
                tmp[n++] = '/';
            if (clen + 1 >= sizeof(tmp) - n) clen = sizeof(tmp) - n - 1;
            memcpy(tmp + n, start, clen);
            n += clen;
        }
        tmp[n] = '\0';
    }

    /* 3) guarantee "/..." form and prepend the drive */
    if (n == 0) { tmp[0] = '/'; tmp[1] = '\0'; n = 1; }
    snprintf(out, sz, "0:%s", tmp);
}

/* 1 if `s` contains a FatFs wildcard ('*' or '?'). */
static int fs_has_wildcard(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?')
            return 1;
    return 0;
}

/*
 * Splits the `ls` operand into an ABSOLUTE directory path (FatFs requires
 * "0:/..." because FF_FS_RPATH=0) and an optional wildcard pattern.
 *
 *   operand      ->  dir                      pattern
 *   --------------------  ------------------  -----------------
 *   "" or "/"    ->  "0:/"                    "" (root)
 *   "cfg"        ->  "0:/cfg"                 ""
 *   "cfg/sub"    ->  "0:/cfg/sub"             ""
 *   "*.txt"      ->  "0:/"                    "*.txt"
 *   "/cfg/*.conf"-> "0:/cfg"                  "*.conf"
 *
 * `dir`/`pat` are caller-provided buffers (must be >= dsz/psz bytes). The
 * pattern is COPIED into `pat` (never left pointing into a stack buffer).
 */
static void fs_split_dir_pattern(const char *operand,
                                 char *dir, size_t dsz,
                                 char *pat, size_t psz)
{
    char dirpart[SHELL_PATH_MAX];
    const char *last_slash = NULL, *p;
    size_t dlen;

    if (dsz) dir[0] = '\0';
    if (psz) pat[0] = '\0';

    if (fs_has_wildcard(operand)) {
        for (p = operand; *p; p++)
            if (*p == '/') last_slash = p;
        if (last_slash) {                         /* dir + pattern */
            dlen = (size_t)(last_slash - operand);
            if (dlen >= sizeof(dirpart)) dlen = sizeof(dirpart) - 1;
            memcpy(dirpart, operand, dlen);
            dirpart[dlen] = '\0';
            snprintf(pat, psz, "%s", last_slash + 1);
        } else {                                  /* wildcard at root */
            dirpart[0] = '\0';
            snprintf(pat, psz, "%s", operand);
        }
    } else {
        dlen = strlen(operand);                   /* whole = directory */
        if (dlen >= sizeof(dirpart)) dlen = sizeof(dirpart) - 1;
        memcpy(dirpart, operand, dlen);
        dirpart[dlen] = '\0';                     /* pattern stays ""  */
    }

    shell_resolve(dirpart, dir, dsz);             /* resolve vs CWD */
}

static void fs_fmt_time(char *out, size_t n, WORD ftime)
{
    unsigned h, mi;

    if (ftime == 0)
    {
        snprintf(out, n, "--:--");
        return;
    }
    h  = (ftime >> 11) & 0x1F;
    mi = (ftime >>  5) & 0x3F;
    snprintf(out, n, "%02u:%02u", h, mi);
}

/* FatFs FAT date (bit15-9=year+1980, 8-5=month, 4-0=day) -> "YYYY-MM-DD". */
static void fs_fmt_date(char *out, size_t n, WORD fdate)
{
    unsigned y, m, d;

    if (fdate == 0) /* no RTC -> fixed date in this port */
    {
        snprintf(out, n, "----/--/--");
        return;
    }
    y = (fdate >> 9) + 1980;
    m = (fdate >> 5) & 0x0F;
    d = (fdate >> 0) & 0x1F;
    snprintf(out, n, "%04u-%02u-%02u", y, m, d);
}

/* Human-readable size ("123 B", "1.5K", "12M"). Writes into `out`. */
static void fs_bytes_human(char *out, size_t n, FSIZE_t sz)
{
    static const char *units[] = { "B", "K", "M", "G", "T" };
    double   v = (double)sz;
    unsigned u = 0;

    while (v >= 1024.0 && u + 1 < (unsigned)(sizeof(units)/sizeof(units[0]))) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
    {
        snprintf(out, n, "%lu B", (unsigned long)sz);
    } else
    {
        snprintf(out, n, "%.1f%s", v, units[u]);
    }
}

/* Text for a FatFs FRESULT code. */
static const char *fs_fr_str(FRESULT r)
{
    switch (r) {
    case FR_OK:               return "OK";
    case FR_DISK_ERR:         return "DISK_ERR";
    case FR_INT_ERR:          return "INT_ERR";
    case FR_NOT_READY:        return "NOT_READY";
    case FR_NO_FILE:          return "NO_FILE";
    case FR_NO_PATH:          return "NO_PATH";
    case FR_INVALID_NAME:     return "INVALID_NAME";
    case FR_DENIED:           return "DENIED";
    case FR_EXIST:            return "EXIST";
    case FR_INVALID_OBJECT:   return "INVALID_OBJECT";
    case FR_WRITE_PROTECTED:  return "WRITE_PROTECTED";
    case FR_INVALID_DRIVE:    return "INVALID_DRIVE";
    case FR_NOT_ENABLED:      return "NOT_ENABLED";
    case FR_NO_FILESYSTEM:    return "NO_FILESYSTEM";
    case FR_MKFS_ABORTED:     return "MKFS_ABORTED";
    case FR_TIMEOUT:          return "TIMEOUT";
    case FR_LOCKED:           return "LOCKED";
    case FR_NOT_ENOUGH_CORE:  return "NOT_ENOUGH_CORE";
    case FR_TOO_MANY_OPEN_FILES: return "TOO_MANY_OPEN_FILES";
    case FR_INVALID_PARAMETER:   return "INVALID_PARAMETER";
    default:                  return "ERR";
    }
}

/*
 * Returns 1 (and prints a diagnostic) when the FAT volume "0:" is not
 * mounted, 0 otherwise. Because FatFs is ONLY usable from Core2 once the
 * volume is mounted (fs/fs_mount.c), every FatFs command starts with this.
 * Requires: #include "../fs/fs_mount.h"  (already part of Étape 1).
 */
static int fs_mount_guard(shell_out_fn out, void *ctx)
{
    if (!fs_mount_ready()) {
        sh_puts(out, ctx, "[fs] filesystem not mounted\r\n");
        return 1;
    }
    return 0;
}

/* Prints one directory entry (FILINFO) read by f_readdir/f_findnext. */
static void fs_print_entry(shell_out_fn out, void *ctx,
                           const FILINFO *fno, int long_fmt)
{
    const int   is_dir = (fno->fattrib & AM_DIR) ? 1 : 0;
    char        size_sz[16];
    char        dat[12], tim[6];

    if (!long_fmt) {
        /* simple mode: name, directories suffixed with '/' */
        sh_printf(out, ctx, "  %s%s\r\n",
                  fno->fname, is_dir ? "/" : "");
        return;
    }

    if (is_dir)
        snprintf(size_sz, sizeof(size_sz), "<DIR>");
    else
        fs_bytes_human(size_sz, sizeof(size_sz), fno->fsize);
    fs_fmt_date(dat, sizeof(dat), fno->fdate);
    fs_fmt_time(tim, sizeof(tim), fno->ftime);

    sh_printf(out, ctx, "  %c %11s %s %s  %s\r\n",
              is_dir ? 'd' : '-', size_sz, dat, tim, fno->fname);
}

static void cmd_ls(shell_out_fn out, void *ctx, const char *arg)
{
    DIR     dir;
    FILINFO fno;
    char    path[SHELL_PATH_MAX];
    char    pat[64];
    FRESULT r;
    int     long_fmt;
    unsigned long count;
    const char *p;

    /* 1) The FAT volume "0:" is only usable once mounted (Core2). */
    if (fs_mount_guard(out, ctx))
    {
        return;
    }

    /* 2) Parse the (optional) -l flag. */
    p = arg;
    while (*p == ' ' || *p == '\t') p++;
    long_fmt = 0;
    if (p[0] == '-' && p[1] == 'l')
    {
        long_fmt = 1;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
    }

    /* 3) Build absolute path + optional wildcard pattern. */
    fs_split_dir_pattern(p, path, sizeof(path), pat, sizeof(pat));

    count = 0;

    /* 4a) Wildcard request : use f_findfirst / f_findnext. */
    if (pat[0] != '\0')
    {
        r = f_findfirst(&dir, &fno, path, pat);
        if (r != FR_OK)
        {
            sh_printf(out, ctx, "[ls] find('%s', '%s') : %s\r\n",
                      path, pat, fs_fr_str(r));
            return;
        }
        while (r == FR_OK && fno.fname[0] != '\0')
        {
            fs_print_entry(out, ctx, &fno, long_fmt);
            count++;
            r = f_findnext(&dir, &fno);
        }
        f_closedir(&dir);

    /* 4b) Simple directory listing. */
    } else
    {
        r = f_opendir(&dir, path);
        if (r != FR_OK)
        {
            sh_printf(out, ctx, "[ls] cannot open '%s' : %s\r\n",
                      path, fs_fr_str(r));
            return;
        }
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0')
        {
            fs_print_entry(out, ctx, &fno, long_fmt);
            count++;
        }
        f_closedir(&dir);
    }

    /* 5) Summary line. */
    sh_printf(out, ctx, "%lu entr%s\r\n",
              count, (count == 1) ? "ie" : "ies");
}

/* cd <path> — change the shell current working directory.
 * FatFs is built with FF_FS_RPATH=0 (f_chdir compiled out), so we resolve
 * the operand against CWD, validate it via f_stat, then re-store the
 * canonical absolute path (without the "0:" drive prefix) in CWD/s_cwd.
 * mirror_valid CWD_absolute_length == strlen(CWD) is preserved for
 * shell_resolve(). */
static void cmd_cd(shell_out_fn out, void *ctx, const char *arg)
{
    char        path[SHELL_PATH_MAX];
    FILINFO     fno;
    const char *p = arg;
    size_t      len;
    FRESULT     r;

    /* 1) The FAT volume "0:" is only usable once mounted (Core2). */
    if (fs_mount_guard(out, ctx))
        return;

    /* 2) "cd" with no argument (or whitespace only) -> root. */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0')
        p = "/";

    /* 3) Wildcards have no meaning for cd. */
    if (fs_has_wildcard(p)) {
        sh_puts(out, ctx, "[cd] invalid path: wildcard not allowed.\r\n");
        return;
    }

    /* 4) Resolve vs CWD -> absolute "0:/..." (handles '', '.', '..'). */
    shell_resolve(p, path, sizeof(path));

    /* 5) Target must exist and be a directory.
     * FatFs f_stat("0:/") returns INVALID_NAME on a bare root path, so
     * the volume root is always accepted without validation. */
    r = FR_OK;
    if (strcmp(path, "0:/") != 0)
        r = f_stat(path, &fno);

    if (r != FR_OK) {
        sh_printf(out, ctx, "[cd] cannot access '%s' : %s\r\n",
                  path, fs_fr_str(r));
        return;
    }
    if (strcmp(path, "0:/") != 0 && !(fno.fattrib & AM_DIR)) {
        sh_printf(out, ctx, "[cd] '%s' is not a directory\r\n", path);
        return;
    }

    /* 6) Size bound on the stored path (WITHOUT the "0:" prefix). */
    len = strlen(path + 2);
    if (len >= SHELL_PATH_MAX) {
        sh_puts(out, ctx, "[cd] path too long\r\n");
        return;
    }

    /* 7) Store canonical absolute path + keep the invariant. */
    memcpy(s_cwd, path + 2, len + 1);   /* includes trailing '\0' */
    CWD                  = s_cwd;
    CWD_absolute_length  = len;

    /* Success is silent: the next (dynamic) prompt reflects the new CWD. */
}

/* pwd — print the shell current working directory. The CWD is tracked by
 * the shell itself (s_cwd/CWD), WITHOUT the "0:" drive prefix, matching
 * the convention shown in the dynamic prompt (net_shell_prompt). */
static void cmd_pwd(shell_out_fn out, void *ctx)
{
    shell_cwd_init();               /* CWD valid (root "/" by default)  */
    sh_printf(out, ctx, "%s\r\n", CWD);
}

/* mkdir [-p] <path> — create a directory on FAT volume "0:".
 * Follows the same conventions as cd/ls: the operand is resolved against CWD
 * and handed to FatFs as an ABSOLUTE "0:/..." path (FF_FS_RPATH=0), wildcards
 * are rejected. With -p, every missing intermediate component is created too
 * (f_mkdir is non-recursive; we loop over each component).
 */
static void cmd_mkdir(shell_out_fn out, void *ctx, const char *arg)
{
    char        path[SHELL_PATH_MAX];
    char        comp[SHELL_PATH_MAX];
    FRESULT     r;
    const char *p;
    int         parents = 0;

    /* 1) The FAT volume "0:" is only usable once mounted (Core2). */
    if (fs_mount_guard(out, ctx))
        return;

    /* 2) Parse the optional -p flag (parents). */
    p = arg;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == '-' && p[1] == 'p' &&
        (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
        parents = 1;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
    }

    /* 3) An empty operand is an error (like Unix mkdir). */
    if (*p == '\0') {
        sh_puts(out, ctx, "mkdir: missing operand\r\n");
        return;
    }

    /* 4) Wildcards have no meaning for mkdir. */
    if (fs_has_wildcard(p)) {
        sh_puts(out, ctx, "mkdir: invalid path: wildcard not allowed.\r\n");
        return;
    }

    /* 5) Resolve vs CWD -> absolute "0:/..." path (handles '', '.', '..'). */
    shell_resolve(p, path, sizeof(path));

    /* 6a) mkdir -p : create each missing component of the path. */
    if (parents) {
        size_t n = 2;                 /* running buffer = "0:" */
        size_t i = 2;                 /* skip "0:" */
        comp[0] = '0'; comp[1] = ':';
        while (path[i] == '/') i++;   /* skip leading '/' of the abs path */

        while (path[i]) {
            size_t start = i;
            while (path[i] && path[i] != '/') i++;
            if (i == start) {         /* consecutive '/' : nothing to add */
                i++;
                continue;
            }

            if (n > 2 && comp[n - 1] != '/')
                comp[n++] = '/';
            size_t clen = (size_t)(i - start);
            if (n + clen + 1 >= sizeof(comp)) {
                sh_puts(out, ctx, "mkdir: path too long\r\n");
                return;
            }
            memcpy(comp + n, path + start, clen);
            n += clen;
            comp[n] = '\0';

            r = f_mkdir(comp);
            if (r != FR_OK && r != FR_EXIST) {   /* allow already-exists */
                sh_printf(out, ctx, "mkdir: '%s' : %s\r\n",
                          comp + 2, fs_fr_str(r));
                return;
            }
        }
        sh_puts(out, ctx, "mkdir: OK\r\n");
        return;
    }

    /* 6b) mkdir <path> : create a single directory. */
    r = f_mkdir(path);
    if (r == FR_OK) {
        sh_printf(out, ctx, "mkdir: '%s' created\r\n", path + 2);
    } else if (r == FR_EXIST) {
        sh_printf(out, ctx, "mkdir: '%s' already exists\r\n", path + 2);
    } else if (r == FR_NO_PATH) {
        sh_printf(out, ctx, "mkdir: cannot create '%s' : %s (missing parent? try -p)\r\n",
                  path + 2, fs_fr_str(r));
    } else {
        sh_printf(out, ctx, "mkdir: '%s' : %s\r\n", path + 2, fs_fr_str(r));
    }
}

/* ------------------------------------------------------------------ */
/* rm [-r] [-f] <path> — remove a file or an (empty) directory on the  */
/* FAT volume "0:". Follows the same conventions as mkdir/cd/ls: the   */
/* operand is resolved against CWD and handed to FatFs as an ABSOLUTE  */
/* "0:/..." path (FF_FS_RPATH=0); wildcards are rejected.             */
/*   -r  recursive removal of a directory and its contents;           */
/*   -f  ignore nonexistent files (never report an error for them).   */
/* ------------------------------------------------------------------ */

/* Recursively deletes every file and sub-directory under the absolute
 * FatFs directory path (e.g. "0:/a/b"). FatFs f_unlink() only removes a
 * file or an EMPTY directory, so we walk the tree (post-order) and
 * remove each entry, then the directory itself. `path` must be ABSOLUTE
 * ("0:/..."), as required by FF_FS_RPATH=0. */
static FRESULT rm_tree(const char *path)
{
    DIR        dir;
    FILINFO    fno;
    char       sub[SHELL_PATH_MAX];
    FRESULT    r;

    r = f_opendir(&dir, path);
    if (r != FR_OK)
        return r;

    for (;;) {
        r = f_readdir(&dir, &fno);
        if (r != FR_OK || fno.fname[0] == '\0')
            break;

        /* defensive: skip "." / ".." (FatFs R0.15 filters them anyway) */
        if (fno.fname[0] == '.' &&
            (fno.fname[1] == '\0' ||
             (fno.fname[1] == '.' && fno.fname[2] == '\0')))
            continue;

        snprintf(sub, sizeof(sub), "%s/%s", path, fno.fname);

        if (fno.fattrib & AM_DIR) {
            r = rm_tree(sub);              /* recurse first            */
            if (r != FR_OK)
                break;
            r = f_unlink(sub);             /* sub-dir now empty        */
            if (r != FR_OK)
                break;
        } else {
            r = f_unlink(sub);             /* plain file               */
            if (r != FR_OK)
                break;
        }
    }

    f_closedir(&dir);
    return r;
}

static void cmd_rm(shell_out_fn out, void *ctx, const char *arg)
{
    char        path[SHELL_PATH_MAX];
    const char *p = arg;
    FILINFO     fno;
    FRESULT     r;
    int         force   = 0;
    int         recurse = 0;

    /* 1) The FAT volume "0:" is only usable once mounted (Core2). */
    if (fs_mount_guard(out, ctx))
        return;

    /* 2) Parse options: -f, -r, and combined forms (-rf, -fr). */
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '-' && p[1] != '\0' && p[1] != ' ' &&
            p[1] != '\t' && p[1] != '-') {
            const char *o;
            for (o = p + 1; *o && *o != ' ' && *o != '\t'; o++) {
                if      (*o == 'f') force   = 1;
                else if (*o == 'r') recurse = 1;
                else {
                    sh_printf(out, ctx, "rm: invalid option -- '%c'\r\n", *o);
                    return;
                }
            }
            while (*p && *p != ' ' && *p != '\t') p++;
        } else {
            break;
        }
    }
    while (*p == ' ' || *p == '\t') p++;

    /* 3) An empty operand is an error (like Unix rm). */
    if (*p == '\0') {
        sh_puts(out, ctx, "rm: missing operand\r\n");
        return;
    }

    /* 4) Wildcards have no meaning for a non-interactive rm. */
    if (fs_has_wildcard(p)) {
        sh_puts(out, ctx, "rm: wildcard not supported (give an explicit path)\r\n");
        return;
    }

    /* 5) Resolve vs CWD -> absolute "0:/..." path. */
    shell_resolve(p, path, sizeof(path));

    /* 6) Safety: never remove the volume root (would wipe the card). */
    if (strcmp(path, "0:/") == 0 || strcmp(path, "0:") == 0) {
        sh_puts(out, ctx, "rm: refusing to remove the filesystem root\r\n");
        return;
    }

    /* 7) The operand must exist (unless -f). */
    r = f_stat(path, &fno);
    if (r != FR_OK) {
        if (force)
            return;                            /* -f: silently ignore */
        sh_printf(out, ctx, "rm: cannot remove '%s': %s\r\n",
                  path + 2, fs_fr_str(r));
        return;
    }

    /* 8) Directory handling. */
    if (fno.fattrib & AM_DIR) {
        if (!recurse) {
            /* single empty directory */
            r = f_unlink(path);
            if (r == FR_DENIED) {
                sh_printf(out, ctx, "rm: cannot remove '%s': "
                          "directory not empty (use -r)\r\n", path + 2);
                return;
            }
            if (r != FR_OK) {
                sh_printf(out, ctx, "rm: '%s': %s\r\n", path + 2, fs_fr_str(r));
                return;
            }
            sh_printf(out, ctx, "rm: removed directory '%s'\r\n", path + 2);
            return;
        }

        /* recursive removal */
        r = rm_tree(path);
        if (r == FR_OK)
            r = f_unlink(path);
        if (r != FR_OK) {
            sh_printf(out, ctx, "rm: failed to remove '%s': %s\r\n",
                      path + 2, fs_fr_str(r));
            return;
        }
        sh_printf(out, ctx, "rm: removed '%s'\r\n", path + 2);
        return;
    }

    /* 9) Regular file. */
    r = f_unlink(path);
    if (r != FR_OK) {
        sh_printf(out, ctx, "rm: cannot remove '%s': %s\r\n",
                  path + 2, fs_fr_str(r));
    } else {
        sh_printf(out, ctx, "rm: removed '%s'\r\n", path + 2);
    }
}

/* Returns the basename of an absolute "0:/..." FatFs path into `out`.
 * "0:/a/b/file.txt" -> "file.txt", "0:/" -> "". */
static void fs_basename(const char *abs, char *out, size_t sz)
{
    const char *base = abs + 2;          /* skip "0:"                */
    const char *last = base;
    for (const char *p = base; *p; p++)
        if (*p == '/') last = p + 1;     /* char after the last '/' */
    size_t n = strlen(last);
    if (n >= sz) n = sz - 1;
    memcpy(out, last, n);
    out[n] = '\0';
}

/* mv [-f] <src> <dst> — rename/move a file or directory on FAT volume "0:".
 * Same conventions as rm/mkdir/cd/ls: operands resolved against CWD and
 * handed to FatFs as ABSOLUTE "0:/..." paths (FF_FS_RPATH=0); wildcards
 * rejected. Uses FatFs f_rename() (moves a file OR a directory, updating its
 * ".." entry) within the same volume. If <dst> is an existing directory the
 * object is moved INTO it keeping its basename (Unix-like). Without -f an
 * existing same-named object at <dst> is an error; with -f it is overwritten. */
static void cmd_mv(shell_out_fn out, void *ctx, const char *arg)
{
    char    src[SHELL_PATH_MAX], dst[SHELL_PATH_MAX];
    char    target[SHELL_PATH_MAX], base[SHELL_PATH_MAX];
    FILINFO fno;
    FRESULT r;
    BYTE    src_attrib;
    int     force = 0;
    const char *p = arg;

    /* 1) The FAT volume "0:" is only usable once mounted (Core2). */
    if (fs_mount_guard(out, ctx))
        return;

    /* 2) Parse the optional -f flag. */
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == '-' && p[1] == 'f' &&
        (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
        force = 1;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
    }

    /* 3) Exactly two operands: <src> <dst>. */
    if (*p == '\0') {
        sh_puts(out, ctx, "mv: missing source operand\r\n");
        return;
    }
    const char *src_s = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    size_t slen = (size_t)(p - src_s);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') {
        sh_puts(out, ctx, "mv: missing destination operand\r\n");
        return;
    }
    const char *dst_s = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    size_t dlen = (size_t)(p - dst_s);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\0') {
        sh_puts(out, ctx, "mv: too many operands\r\n");
        return;
    }

    if (slen >= sizeof src || dlen >= sizeof dst) {
        sh_puts(out, ctx, "mv: path too long\r\n");
        return;
    }
    memcpy(src, src_s, slen); src[slen] = '\0';
    memcpy(dst, dst_s, dlen); dst[dlen] = '\0';

    /* 4) Wildcards have no meaning here (same as rm/mkdir). */
    if (fs_has_wildcard(src) || fs_has_wildcard(dst)) {
        sh_puts(out, ctx, "mv: wildcard not supported (give explicit paths)\r\n");
        return;
    }

    /* 5) Resolve vs CWD -> absolute "0:/..." paths. */
    shell_resolve(src, src, sizeof(src));
    shell_resolve(dst, dst, sizeof(dst));

    /* 6) Safety: never move the volume root. */
    if (strcmp(src, "0:/") == 0 || strcmp(src, "0:") == 0) {
        sh_puts(out, ctx, "mv: refusing to move the filesystem root\r\n");
        return;
    }

    /* 7) The source must exist; remember whether it is a directory. */
    r = f_stat(src, &fno);
    if (r != FR_OK) {
        sh_printf(out, ctx, "mv: cannot stat '%s': %s\r\n",
                  src + 2, fs_fr_str(r));
        return;
    }
    src_attrib = fno.fattrib;

    /* 8) Destination semantics: an existing directory => "move into it". */
    r = f_stat(dst, &fno);
    if (r == FR_OK && (fno.fattrib & AM_DIR)) {
        fs_basename(src, base, sizeof(base));
        if (base[0] == '\0') {
            sh_puts(out, ctx, "mv: cannot move the volume root\r\n");
            return;
        }
        size_t d = strlen(dst);
        if (d + strlen(base) + 2 >= sizeof(target)) {
            sh_puts(out, ctx, "mv: destination path too long\r\n");
            return;
        }
        snprintf(target, sizeof(target), "%s%s%s",
                 dst, (dst[d - 1] == '/') ? "" : "/", base);
    } else {
        /* plain rename: dst is the new full name */
        snprintf(target, sizeof(target), "%s", dst);
    }

    /* 9) Refuse to move an object onto itself, or a directory into its own
     * subtree (FatFs would corrupt the directory chain). */
    if (strcmp(src, target) == 0) {
        sh_puts(out, ctx, "mv: source and destination are the same\r\n");
        return;
    }
    if ((src_attrib & AM_DIR) && strncmp(target, src, strlen(src)) == 0 &&
        target[strlen(src)] == '/') {
        sh_puts(out, ctx, "mv: cannot move a directory into itself\r\n");
        return;
    }

    /* 10) Handle an existing target (name collision). */
    r = f_stat(target, &fno);
    if (r == FR_OK) {
        if (!force) {
            sh_printf(out, ctx,
                      "mv: '%s' already exists (use -f to overwrite)\r\n",
                      target + 2);
            return;
        }
        r = f_unlink(target);            /* file, or empty directory */
        if (r != FR_OK) {
            sh_printf(out, ctx, "mv: cannot overwrite '%s': %s\r\n",
                      target + 2, fs_fr_str(r));
            return;
        }
    }

    /* 11) Perform the rename/move (same volume, file or directory). */
    r = f_rename(src, target);
    if (r != FR_OK) {
        sh_printf(out, ctx, "mv: cannot move '%s' to '%s': %s\r\n",
                  src + 2, target + 2, fs_fr_str(r));
        return;
    }

    sh_printf(out, ctx, "mv: '%s' -> '%s'\r\n", src + 2, target + 2);
}

/* ------------------------------------------------------------------ */
/* cp [-f] [-r] <src> <dst> — copy a file or a directory tree on the  */
/* FAT volume "0:". Same conventions as mv/rm/mkdir: operands resolved */
/* against CWD, absolute "0:/..." paths to FatFs (FF_FS_RPATH=0),     */
/* wildcards rejected.                                                 */
/*   -f  overwrite an existing destination (without it: error)         */
/*   -r  copy a directory recursively                                  */
/* ------------------------------------------------------------------ */

/* Copy a single file `src` -> `dst` (absolute "0:/..."). On failure    */
/* the partial destination is rolled back (f_unlink) so we never leave  */
/* a truncated file behind.                                              */
static FRESULT cp_file(const char *src, const char *dst)
{
    FIL      fi, fo;
    BYTE     buf[512];
    FRESULT  r;
    UINT     br, bw;

    r = f_open(&fi, src, FA_READ | FA_OPEN_EXISTING);
    if (r != FR_OK)
        return r;
    r = f_open(&fo, dst, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        f_close(&fi);
        return r;
    }

    for (;;) {
        br = 0;
        r = f_read(&fi, buf, sizeof buf, &br);
        if (r != FR_OK || br == 0)
            break;
        bw = 0;
        r = f_write(&fo, buf, br, &bw);
        if (r != FR_OK || bw < br) {   /* short write => disk full */
            if (r == FR_OK) r = FR_DISK_ERR;
            break;
        }
    }

    f_close(&fi);
    f_close(&fo);

    if (r != FR_OK)
        f_unlink(dst);                 /* rollback the partial copy */
    return r;
}

/* Recursively copy directory `src` (absolute "0:/...") into the NEW
 * directory `dst`, preserving names. `cnt` receives the number of
 * copied files (directories excluded, like the Unix cp summary). */
static FRESULT cp_tree(shell_out_fn out, void *ctx,
                       const char *src, const char *dst,
                       unsigned long *cnt)
{
    DIR      dir;
    FILINFO  fno;
    char     s[SHELL_PATH_MAX], d[SHELL_PATH_MAX];
    FRESULT  r;

    r = f_mkdir(dst);
    if (r != FR_OK && r != FR_EXIST)
        return r;

    r = f_opendir(&dir, src);
    if (r != FR_OK)
        return r;

    for (;;) {
        r = f_readdir(&dir, &fno);
        if (r != FR_OK || fno.fname[0] == '\0')
            break;

        if (fno.fname[0] == '.' &&
            (fno.fname[1] == '\0' ||
             (fno.fname[1] == '.' && fno.fname[2] == '\0')))
            continue;

        snprintf(s, sizeof s, "%s/%s", src, fno.fname);
        snprintf(d, sizeof d, "%s/%s", dst, fno.fname);

        if (fno.fattrib & AM_DIR) {
            r = cp_tree(out, ctx, s, d, cnt);
            if (r != FR_OK) {
                sh_printf(out, ctx, "cp: '%s': %s\r\n", s + 2, fs_fr_str(r));
                break;
            }
        } else {
            r = cp_file(s, d);
            if (r != FR_OK) {
                sh_printf(out, ctx, "cp: '%s' -> '%s': %s\r\n",
                          s + 2, d + 2, fs_fr_str(r));
                break;
            }
            (*cnt)++;
        }
    }

    f_closedir(&dir);
    return r;
}

static void cmd_cp(shell_out_fn out, void *ctx, const char *arg)
{
    char        src[SHELL_PATH_MAX], dst[SHELL_PATH_MAX];
    char        target[SHELL_PATH_MAX], base[SHELL_PATH_MAX];
    FILINFO     fno;
    FRESULT     r;
    BYTE        src_attrib;
    int         force = 0, recurse = 0;
    const char *p = arg;
    unsigned long cnt = 0;

    /* 1) The FAT volume "0:" is only usable once mounted (Core2). */
    if (fs_mount_guard(out, ctx))
        return;

    /* 2) Parse options: -f, -r and combined forms (-fr, -rf). */
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '-' && p[1] != '\0' && p[1] != ' ' &&
            p[1] != '\t' && p[1] != '-') {
            const char *o;
            for (o = p + 1; *o && *o != ' ' && *o != '\t'; o++) {
                if      (*o == 'f') force   = 1;
                else if (*o == 'r') recurse = 1;
                else {
                    sh_printf(out, ctx, "cp: invalid option -- '%c'\r\n", *o);
                    return;
                }
            }
            while (*p && *p != ' ' && *p != '\t') p++;
        } else {
            break;
        }
    }
    while (*p == ' ' || *p == '\t') p++;

    /* 3) Exactly two operands: <src> <dst>. */
    if (*p == '\0') {
        sh_puts(out, ctx, "cp: missing source operand\r\n");
        return;
    }
    const char *src_s = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    size_t slen = (size_t)(p - src_s);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') {
        sh_puts(out, ctx, "cp: missing destination operand\r\n");
        return;
    }
    const char *dst_s = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    size_t dlen = (size_t)(p - dst_s);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\0') {
        sh_puts(out, ctx, "cp: too many operands\r\n");
        return;
    }

    if (slen >= sizeof src || dlen >= sizeof dst) {
        sh_puts(out, ctx, "cp: path too long\r\n");
        return;
    }
    memcpy(src, src_s, slen); src[slen] = '\0';
    memcpy(dst, dst_s, dlen); dst[dlen] = '\0';

    /* 4) Wildcards have no meaning here (same as mv/rm/mkdir). */
    if (fs_has_wildcard(src) || fs_has_wildcard(dst)) {
        sh_puts(out, ctx, "cp: wildcard not supported (give explicit paths)\r\n");
        return;
    }

    /* 5) Resolve vs CWD -> absolute "0:/..." paths. */
    shell_resolve(src, src, sizeof(src));
    shell_resolve(dst, dst, sizeof(dst));

    /* 6) Safety: never copy the volume root. */
    if (strcmp(src, "0:/") == 0 || strcmp(src, "0:") == 0) {
        sh_puts(out, ctx, "cp: refusing to copy the filesystem root\r\n");
        return;
    }

    /* 7) The source must exist. */
    r = f_stat(src, &fno);
    if (r != FR_OK) {
        sh_printf(out, ctx, "cp: cannot stat '%s': %s\r\n",
                  src + 2, fs_fr_str(r));
        return;
    }
    src_attrib = fno.fattrib;

    /* 8) Destination semantics: an existing dir => "copy INTO it". */
    r = f_stat(dst, &fno);
    if (r == FR_OK && (fno.fattrib & AM_DIR)) {
        fs_basename(src, base, sizeof(base));
        if (base[0] == '\0') {
            sh_puts(out, ctx, "cp: cannot copy the volume root\r\n");
            return;
        }
        size_t d = strlen(dst);
        if (d + strlen(base) + 2 >= sizeof(target)) {
            sh_puts(out, ctx, "cp: destination path too long\r\n");
            return;
        }
        snprintf(target, sizeof(target), "%s%s%s",
                 dst, (dst[d - 1] == '/') ? "" : "/", base);
    } else {
        snprintf(target, sizeof(target), "%s", dst);
    }

    /* 9) Refuse to copy an object onto itself, or a directory into its
     * own subtree (would make `cp -r a a/b` expand forever). */
    if (strcmp(src, target) == 0) {
        sh_puts(out, ctx, "cp: source and destination are the same\r\n");
        return;
    }
    if ((src_attrib & AM_DIR) && strncmp(target, src, strlen(src)) == 0 &&
        target[strlen(src)] == '/') {
        sh_puts(out, ctx, "cp: cannot copy a directory into itself\r\n");
        return;
    }

    /* 10) Directory source: requires -r. */
    if (src_attrib & AM_DIR) {
        if (!recurse) {
            sh_puts(out, ctx, "cp: -r not specified; omitting directory\r\n");
            return;
        }
        /* An existing regular file at the target dir name is a conflict. */
        r = f_stat(target, &fno);
        if (r == FR_OK && !(fno.fattrib & AM_DIR)) {
            sh_printf(out, ctx, "cp: cannot overwrite non-directory '%s'\r\n",
                      target + 2);
            return;
        }

        r = cp_tree(out, ctx, src, target, &cnt);
        if (r != FR_OK) {
            sh_printf(out, ctx, "cp: copy of '%s' failed: %s\r\n",
                      src + 2, fs_fr_str(r));
            return;
        }
        sh_printf(out, ctx, "cp: '%s' -> '%s' : %lu file%s\r\n",
                  src + 2, target + 2, cnt, (cnt == 1) ? "" : "s");
        return;
    }

    /* 11) File source: one f_open/f_read/f_write copy. */
    r = f_stat(target, &fno);
    if (r == FR_OK && !force) {
        sh_printf(out, ctx, "cp: '%s' already exists (use -f to overwrite)\r\n",
                  target + 2);
        return;
    }

    r = cp_file(src, target);
    if (r != FR_OK) {
        sh_printf(out, ctx, "cp: cannot copy '%s' to '%s': %s\r\n",
                  src + 2, target + 2, fs_fr_str(r));
        return;
    }
    sh_printf(out, ctx, "cp: '%s' -> '%s'\r\n", src + 2, target + 2);
}

static void cat_flush(shell_out_fn out, void *ctx, char *ob, size_t *on)
{
    if (*on) {
        out(ctx, ob, *on);
        *on = 0;
    }
}

static void cat_out(shell_out_fn out, void *ctx,
                    char *ob, size_t *on, const char *data, size_t len)
{
    while (len) {
        size_t space = (size_t)CAT_OBUF_SIZE - *on;
        size_t take  = (len < space) ? len : space;
        memcpy(ob + *on, data, take);
        *on += take;
        data += take;
        len  -= take;
        if (*on == (size_t)CAT_OBUF_SIZE)
            cat_flush(out, ctx, ob, on);
    }
}

/* cat — concatenate file(s) to the shell output.
 * Usage:  cat [OPTION]... FILE...
 *   option -n : prefix each output line with its number ("%6lu\t")
 *   option -A / -e : mark the end of each line with '$'
 * Reads by f_read chunks (no line-length limit), resolves each path
 * against the shell CWD (FF_FS_RPATH=0 -> absolute "0:/..." only). */
static void cmd_cat(shell_out_fn out, void *ctx, const char *arg)
{
    const char *p    = arg;
    int         number  = 0;
    int         seol    = 0;   /* show '$' at end of line */
    char        ob[CAT_OBUF_SIZE];
    size_t      on = 0;

    /* 1) The FAT volume "0:" is only usable once mounted (Core2). */
    if (fs_mount_guard(out, ctx))
        return;

    /* 2) Parse options, then the file list. */
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '-') break;
        p++;                       /* skip '-' */
        while (*p && *p != ' ' && *p != '\t') {
            if      (*p == 'n')       number = 1;
            else if (*p == 'A' || *p == 'e') seol   = 1;
            else {
                sh_printf(out, ctx, "cat: invalid option -- '%c'\r\n", *p);
                return;
            }
            p++;
        }
    }

    if (*p == '\0') {
        sh_puts(out, ctx, "cat: missing operand\r\n");
        return;
    }

    unsigned long lineno  = 1;   /* 1-based line counter (-n)           */
    int           at_bol  = 1;   /* "at beginning of line" flag         */

    while (*p) {
        /* one file name (space/tab separated) */
        const char *cur = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t clen = (size_t)(p - cur);
        char   rel[SHELL_PATH_MAX], path[SHELL_PATH_MAX];
        if (clen >= sizeof rel) clen = sizeof rel - 1;
        memcpy(rel, cur, clen);
        rel[clen] = '\0';
        shell_resolve(rel, path, sizeof path);   /* -> absolute "0:/..." */

        {
            FIL      fp;
            BYTE     rbuf[CAT_RBUF_SIZE];
            FRESULT  r = f_open(&fp, path, FA_READ | FA_OPEN_EXISTING);
            if (r != FR_OK) {
                sh_printf(out, ctx, "cat: '%s': %s\r\n", path + 2, fs_fr_str(r));
                while (*p == ' ' || *p == '\t') p++;
                continue;
            }

            for (;;) {
                UINT br = 0;
                r = f_read(&fp, rbuf, (UINT)sizeof rbuf, &br);
                if (r != FR_OK) {
                    sh_printf(out, ctx, "cat: '%s': read error %s\r\n",
                              path + 2, fs_fr_str(r));
                    break;
                }
                if (br == 0) break;

                /* optional per-byte transform (line numbers / EOL marks) */
                for (UINT i = 0; i < br; i++) {
                    char b = (char)rbuf[i];
                    if (at_bol && number) {
                        char hdr[24];
                        int  n = snprintf(hdr, sizeof hdr, "%6lu\t", lineno);
                        cat_out(out, ctx, ob, &on, hdr, (size_t)n);
                        at_bol = 0;
                    }
                    if (b == '\n') {
                        if (seol) cat_out(out, ctx, ob, &on, "$", 1);
                        cat_out(out, ctx, ob, &on, "\n", 1);
                        at_bol = 1;
                        lineno++;
                    } else {
                        cat_out(out, ctx, ob, &on, &b, 1);
                    }
                }
            }
            f_close(&fp);
        }
        while (*p == ' ' || *p == '\t') p++;
    }

    cat_flush(out, ctx, ob, &on);
}

/* touch [-c] <file>... — create empty files on FAT volume "0:" (Unix touch,
 * idempotent: existing files are left untouched and are not an error).
 * Same conventions as mkdir/rm/mv/cp: each operand is resolved against the
 * CWD and handed to FatFs as an ABSOLUTE "0:/..." path (FF_FS_RPATH=0);
 * wildcards are rejected.
 *   -c   do NOT create missing files (missing files are silently ignored,
 *        like Unix touch -c).
 */
static void cmd_touch(shell_out_fn out, void *ctx, const char *arg)
{
    const char *p = arg;
    int         no_create = 0;

    /* 1) The FAT volume "0:" is only usable once mounted (Core2). */
    if (fs_mount_guard(out, ctx))
        return;

    /* 2) Parse the optional -c flag. */
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == '-' && p[1] == 'c' &&
        (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
        no_create = 1;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
    }

    /* 3) An empty operand is an error (like Unix touch). */
    if (*p == '\0') {
        sh_puts(out, ctx, "touch: missing file operand\r\n");
        return;
    }

    /* 4) Iterate over every <file> operand (space/tab separated). */
    while (*p) {
        const char *cur = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t clen = (size_t)(p - cur);
        char   rel[SHELL_PATH_MAX], path[SHELL_PATH_MAX];

        if (clen >= sizeof rel) clen = sizeof rel - 1;
        memcpy(rel, cur, clen);
        rel[clen] = '\0';
        while (*p == ' ' || *p == '\t') p++;

        /* 4a) Wildcards have no meaning for touch. */
        if (fs_has_wildcard(rel)) {
            sh_printf(out, ctx, "touch: '%s': wildcard not supported "
                      "(give an explicit path)\r\n", rel);
            continue;
        }

        /* 4b) Resolve vs CWD -> absolute "0:/..." path. */
        shell_resolve(rel, path, sizeof(path));

        /* 4c) Does the file already exist? */
        FILINFO fno;
        FRESULT r = f_stat(path, &fno);
        if (r == FR_OK) {
            /* Existing file: no-op (idempotent, Unix touch semantics).
             * No f_utime(): FatFs is built here with FF_USE_CHMOD=0 and there
             * is no RTC (FF_FS_NORTC=1); the timestamp would only ever be the
             * fixed 2026-01-01 anyway. */
            continue;
        }

        if (no_create) {
            /* -c : silently ignore a missing file. */
            continue;
        }

        /* 4d) File does not exist: create an empty one.
         *     (FR_NO_PATH = missing parent directory.) */
        if (r != FR_NO_FILE && r != FR_NO_PATH) {
            sh_printf(out, ctx, "touch: '%s': %s\r\n", path + 2, fs_fr_str(r));
            continue;
        }

        FIL fp;
        r = f_open(&fp, path, FA_CREATE_NEW | FA_WRITE);
        if (r != FR_OK) {
            sh_printf(out, ctx, "touch: cannot create '%s': %s\r\n",
                      path + 2, fs_fr_str(r));
            continue;
        }
        f_close(&fp);
    }
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
    char           cmd[256];
    char           cmdword[64];
    char           redir_file[SHELL_PATH_MAX];
    char           redir_abs[SHELL_PATH_MAX];
    int            append = 0;
    int            redir  = 0;
    int            rc     = 0;

    shell_out_fn   eff_out = out;          /* may be swapped to the file   */
    void          *eff_ctx = ctx;
    redir_ctx_t    rd;

    shell_cwd_init();

    /* 1) Detect & strip a trailing " > file" / " >> file" redirection. */
    {
        int pr = sh_parse_redir(line, cmd, sizeof cmd,
                                redir_file, sizeof redir_file, &append);
        if (pr < 0) {
            sh_puts(out, ctx, "syntax error: bad redirection\r\n");
            return 0;
        }
        redir = pr;
    }

    /* 2) Open the redirection target (needs the FAT volume mounted). */
    if (redir) {
        if (fs_mount_guard(out, ctx))
            return 0;

        shell_resolve(redir_file, redir_abs, sizeof redir_abs);

                BYTE   mode = FA_WRITE | (append ? FA_OPEN_APPEND : FA_CREATE_ALWAYS);
        FRESULT fr   = f_open(&rd.file, redir_abs, mode);
        if (fr != FR_OK) {
            sh_printf(out, ctx, "redir: cannot open '%s' (%s)\r\n",
                      redir_file, fs_fr_str(fr));
            return 0;
        }
        rd.base_out = out;
        rd.base_ctx = ctx;
        rd.failed   = 0;

        eff_out = sh_redir_out;             /* ALL commands now write to file */
        eff_ctx = &rd;
    }

    /* 3) Dispatch (identical list; only out/ctx are now eff_out/eff_ctx). */
    {
        const char *arg = split_cmd(cmd, cmdword, sizeof cmdword);

        if (cmdword[0] == '\0') {
            rc = 0;
        } else if (strcmp(cmdword, "help") == 0 || strcmp(cmdword, "?") == 0) {
            cmd_help(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "ver") == 0 || strcmp(cmdword, "version") == 0) {
            cmd_ver(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "shutdown") == 0) {
            cmd_shutdown(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "reboot") == 0) {
            cmd_reboot(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "uptime") == 0) {
            cmd_uptime(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "net") == 0) {
            cmd_net(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "stats") == 0) {
            cmd_stats(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "ecat") == 0) {
            cmd_ecat(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "wcet") == 0) {
            cmd_wcet(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "echo") == 0) {
            sh_puts(eff_out, eff_ctx, arg);
            sh_puts(eff_out, eff_ctx, "\r\n");
        } else if (strcmp(cmdword, "ls") == 0) {
            cmd_ls(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "cd") == 0) {
            cmd_cd(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "pwd") == 0) {
            cmd_pwd(eff_out, eff_ctx);
        } else if (strcmp(cmdword, "mkdir") == 0) {
            cmd_mkdir(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "rm") == 0) {
            cmd_rm(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "cat") == 0) {
            cmd_cat(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "mv") == 0) {
            cmd_mv(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "cp") == 0) {
            cmd_cp(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "touch") == 0) {
            cmd_touch(eff_out, eff_ctx, arg);
        } else if (strcmp(cmdword, "quit") == 0 || strcmp(cmdword, "exit") == 0) {
            sh_puts(eff_out, eff_ctx, "bye.\r\n");
            rc = 1;
        } else {
            sh_printf(eff_out, eff_ctx,
                      "unknown command: '%s' (type 'help')\r\n", cmdword);
        }
    }

    /* 4) Close the target file (flushes pending FatFs writes). */
    if (redir) {
        if (rd.failed)
            sh_puts(out, ctx, "redir: write error; output truncated\r\n");
        f_close(&rd.file);
    }

    return rc;
}


void net_shell_welcome(shell_out_fn out, void *ctx)
{
    sh_puts(out, ctx,
        "\r\n"
        "=================================================\r\n"
        " OROS - remote shell\r\n"
        " Type 'help' to display the list of commands.\r\n"
        "=================================================\r\n");
    shell_cwd_init();
    net_shell_prompt(out, ctx);
}

void net_shell_prompt(shell_out_fn out, void *ctx)
{
    shell_cwd_init();
    sh_printf(out, ctx, "oros@oros:%s> ", CWD);
}
