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
 * ecat_task.c — PERMANENT EtherCAT master on Core0
 *               + WCET INSTRUMENTATION.
 *
 * PRODUCTION version. Static
 * PDO config (16DI/16DO slave, vendor 0x0B95 / product 0x1500), same ecrt_*
 * sequence (reserve -> domain -> slave_config -> pdos -> reg -> activate). The
 * DIFFERENCE: instead of a loop bounded to 20,000 cycles followed by a
 * return, we run CONTINUOUSLY (infinite loop) on the dedicated hard-RT core
 * (Core0), IN PARALLEL with Core2's network/SSH. The ecat_diag snapshot is
 * published on EVERY cycle -> the remote shell sees EtherCAT in REAL TIME.
 *
 * WCET VALIDATION — we measure via the PMU (CPU cycle counter) TWO
 * distinct indicators, published in ecat_diag and readable remotely via the
 * `wcet` shell command:
 *   (a) EtherCAT cycle PROCESSING TIME (receive->process->R/W->queue->send):
 *       min / max / average. This is the real CPU load per cycle; it MUST
 *       comfortably fit under the period (WCET margin).
 *   (b) WAKE-UP JITTER: deviation between the THEORETICAL cycle tick
 *       (absolute cadence) and the actual wake-up instant, in Generic Timer
 *       ticks + µs, with a HISTOGRAM. This is the ISOLATION indicator: if
 *       Core2 (network/USB/SSH) were disturbing Core0, this jitter would
 *       increase. We prove it stays bounded under load.
 * A WARM-UP phase (WCET_WARMUP cycles) is excluded from the stats so as not
 * to pollute the extremes with the very first exchanges (bus establishment).
 *
 * PMU cycles -> ns conversion: we CALIBRATE the CPU frequency at startup by
 * counting the PMU cycles elapsed over a known Generic Timer interval
 * (whose CNTFRQ frequency is exact). cpu_hz is published in ecat_diag.
 *
 * QEMU / no slave: ecrt_request_master -> NULL (GMAC ENODEV) -> the task
 * reports it and goes idle (wfi) without blocking the system.
 */

#include "ecat_task.h"
#include "ecrt.h"
#include "timer.h"
#include "pmu.h"
#include "config.h"
#include "klog.h"
#include "../../net/ecat_diag.h"   /* snapshot published for the shell (Core2) */
#include "gmac.h"                    /* GMAC link speed for the diag */
#include "../../app/app.h"           /* app_ecat_cycle(): application hook */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ─── Slave config (16DI/16DO). ADAPT to the real slave. ─── */
#define SLAVE_VENDOR   0x00000B95u
#define SLAVE_PRODUCT  0x00001500u

/* ─── WCET validation parameters ─── */
#define WCET_WARMUP    1000u   /* cycles ignored at startup (bus establishment) */

static const ec_pdo_entry_info_t slave_pdo_entries_out[] = {
    { 0x7000, 0x00, 16 },   /* DO: 16 output bits */
};
static const ec_pdo_entry_info_t slave_pdo_entries_in[] = {
    { 0x6000, 0x00, 16 },   /* DI: 16 input bits */
};

static const ec_pdo_info_t slave_pdos_out[] = {
    { 0x1600, 1, slave_pdo_entries_out },
};
static const ec_pdo_info_t slave_pdos_in[] = {
    { 0x1A00, 1, slave_pdo_entries_in },
};

static const ec_sync_info_t slave_sync[] = {
    { 0, EC_DIR_OUTPUT, 0, NULL,           EC_WD_DISABLE },
    { 1, EC_DIR_INPUT,  0, NULL,           EC_WD_DISABLE },
    { 2, EC_DIR_OUTPUT, 1, slave_pdos_out, EC_WD_ENABLE  },
    { 3, EC_DIR_INPUT,  1, slave_pdos_in,  EC_WD_DISABLE },
    { 0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT }   /* end marker */
};

/* Offsets filled by ecrt_domain_reg_pdo_entry_list. */
static unsigned int off_do;
static unsigned int off_di;

static ec_pdo_entry_reg_t domain_regs[] = {
    { 0, 0, SLAVE_VENDOR, SLAVE_PRODUCT, 0x7000, 0x00, &off_do, NULL },
    { 0, 0, SLAVE_VENDOR, SLAVE_PRODUCT, 0x6000, 0x00, &off_di, NULL },
    { 0 }   /* terminator */
};

/* Goes idle without blocking the rest of the system (no master available). */
static void ecat_task_idle(void)
{
    for (;;)
        __asm__ volatile("wfi");
}

/* Calibrates the CPU frequency (Hz) by counting the PMU cycles elapsed
 * during a known Generic Timer interval (whose CNTFRQ is exact). Returns 0
 * if the measurement is implausible (PMU unavailable). */
static uint64_t calibrate_cpu_hz(void)
{
    const uint64_t win_us = 20000u;             /* 20 ms measurement window */
    uint64_t t_end = timer_now_ticks() + timer_us_to_ticks(win_us);
    uint64_t c0 = pmu_cycles();
    while (timer_now_ticks() < t_end)
        __asm__ volatile("nop");
    uint64_t c1 = pmu_cycles();
    uint64_t dc = c1 - c0;
    /* cpu_hz = cycles / (win_us / 1e6) = cycles * 1e6 / win_us */
    uint64_t hz = (dc * 1000000ull) / win_us;
    /* Sanity guard: plausible range for a Cortex-A53 (0.3 - 3 GHz). */
    if (hz < 300000000ull || hz > 3000000000ull)
        return 0;
    return hz;
}

/* Places a wake-up jitter (µs) into the right histogram bucket. */
static void wcet_hist_add(ecat_diag_t *dg, uint32_t wake_us)
{
    unsigned i;
    for (i = 0; i < ECAT_WCET_BUCKETS - 1u; i++) {
        if (wake_us < ecat_wcet_bucket_us[i]) {
            dg->wake_hist[i]++;
            return;
        }
    }
    dg->wake_hist[ECAT_WCET_BUCKETS - 1u]++;   /* last bucket: everything else */
}

void ecat_task_entry(void *arg)
{
    (void)arg;

    /* RT RULE (Core0): never printf here (blocking UART -> false overruns).
     * All the traces go through the lock-free klog, drained by Core2. */
    klog_write("[ecat] permanent EtherCAT master starting on Core0");
    klog_write_u("[ecat] expected slave vendor=", SLAVE_VENDOR);
    klog_write_u("[ecat] expected slave product=", SLAVE_PRODUCT);
    klog_write_u("[ecat] cycle period (us)=", CFG_ECAT_CYCLE_US);

    /* Diagnostic snapshot (published continuously, read by the shell on Core2). */
    ecat_diag_t *dg = ecat_diag_get();
    ecat_diag_reset();
    dg->vendor_id    = SLAVE_VENDOR;
    dg->product_code = SLAVE_PRODUCT;
    dg->cycle_us     = CFG_ECAT_CYCLE_US;
    dg->warmup_cycles = WCET_WARMUP;
    dg->valid        = 1;

    /* CPU frequency calibration (to convert PMU cycles to ns). */
    uint64_t cpu_hz = calibrate_cpu_hz();
    dg->cpu_hz = cpu_hz;
    if (cpu_hz)
        klog_write_u("[ecat][wcet] CPU frequency (Hz)=", cpu_hz);
    else
        klog_write("[ecat][wcet] ERROR: no PMU calibration (WCET in cycles only)");

    /* --- 1. Reservation + static config --- */
    ec_master_t *master = ecrt_request_master(0);
    if (!master) {
        klog_write("[ecat] ERROR: no master (GMAC absent or no link) : idle");
        ecat_task_idle();
        return;   /* never reached */
    }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) { klog_write("[ecat] ERROR: create_domain failed"); ecat_task_idle(); }

    ec_slave_config_t *sc = ecrt_master_slave_config(master, 0, 0,
                                                     SLAVE_VENDOR, SLAVE_PRODUCT);
    if (!sc) { klog_write("[ecat] ERROR: slave_config failed"); ecat_task_idle(); }

    if (ecrt_slave_config_pdos(sc, EC_END, slave_sync)) {
        klog_write("[ecat] ERROR: config_pdos failed"); ecat_task_idle();
    }
    if (ecrt_domain_reg_pdo_entry_list(domain, domain_regs)) {
        klog_write("[ecat] ERROR: reg_pdo_entry_list failed"); ecat_task_idle();
    }
    ecrt_slave_config_dc(sc, 0x0300, CFG_ECAT_CYCLE_US * 1000u, 0, 0, 0);

    /* --- 2. Activation: scan + ESM INIT→OP --- */
    if (ecrt_master_activate(master)) {
        klog_write("[ecat] ERROR: activation failed (no slave in OP on GMAC)");
        klog_write("[ecat] master idle (Core2 services remain active)");
        ecat_task_idle();
    }

    uint8_t *pd = ecrt_domain_data(domain);
    if (!pd) { klog_write("[ecat] ERROR: domain_data NULL"); ecat_task_idle(); }

    ec_slave_config_state_t ss;
    ecrt_slave_config_state(sc, &ss);
    klog_write("[ecat] slave in OP : permanent cycle running");
    klog_write_u("[ecat] AL state=", ss.al_state);

    dg->master_up     = 1;
    dg->link_mbps     = (uint32_t)gmac_get_link_speed();
    dg->slaves_online = ss.online ? 1u : 0u;
    dg->slaves_op     = ss.operational ? 1u : 0u;
    dg->al_state      = ss.al_state;

    klog_write_u("[ecat][wcet] stats published continuously, warm-up cycles=",
                 WCET_WARMUP);

    /* --- 3. PERMANENT real-time cycle loop (never returns) --- */
    const uint32_t cycle_us = CFG_ECAT_CYCLE_US;
    uint16_t out_pattern = 0x0000;

    unsigned wkc_ok = 0, wkc_zero = 0, overruns = 0;
    uint64_t jit_max = 0;
    uint64_t jit_max_window = 0;          /* sliding max over the WCET window */
    uint64_t cycles = 0;

    /* WCET stats (local, copied into ecat_diag). */
    uint64_t proc_min = (uint64_t)-1, proc_max = 0, proc_sum = 0;
    uint64_t wake_max = 0, wake_sum = 0;
    uint32_t wcet_n = 0;

    /* Generic Timer ticks -> µs conversion for the jitter histogram. */
    const uint64_t tfreq = timer_frequency();   /* system counter Hz */

    /* Tolerance margin: we only count an "overrun" if the cycle REALLY
     * overran its period (delay > 1 period), not for the small resync
     * jitter (real processing = a few µs << 1 ms). */
    const uint64_t period_ticks = timer_us_to_ticks(cycle_us);
    uint64_t next = timer_now_ticks() + period_ticks;
    for (;;) {
        /* Wait for the cycle tick (fixed period, ABSOLUTE cadence). */
        uint64_t t0 = timer_now_ticks();
        uint64_t wake_off_ticks;   /* |wake-up - theoretical tick| deviation (jitter) */
        if (t0 >= next + period_ticks) {
            /* Delay of more than a full period = real overrun (the
             * previous cycle overran). We resync on the current instant. */
            overruns++;
            wake_off_ticks = t0 - next;    /* measured delay */
            next = t0 + period_ticks;
        } else {
            /* Normal case: wait for the tick, then schedule the next tick
             * at next+period (no drift: absolute cadence, not relative). */
            while (timer_now_ticks() < next) __asm__ volatile("nop");
            /* Wake-up jitter = deviation between the actual exit instant
             * from the wait loop and the theoretical tick `next`. */
            uint64_t tw = timer_now_ticks();
            wake_off_ticks = (tw > next) ? (tw - next) : 0u;
            next += period_ticks;
        }

        uint64_t cyc0 = pmu_cycles();

        /* ===== EtherCAT cycle (like oobThread) ===== */
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        uint16_t di = EC_READ_U16(pd + off_di);

        /* ===== APPLICATION hook (app/app_core0.c) =====
         * The process-data logic belongs to the programmer: inputs in,
         * outputs out. Must be WCET-bounded (see the `wcet` shell command). */
        out_pattern = app_ecat_cycle(di);
        EC_WRITE_U16(pd + off_do, out_pattern);

        ecrt_master_application_time(master,
                (uint64_t)timer_ticks_to_us(timer_now_ticks()) * 1000ull);
        ecrt_master_sync_reference_clock(master);
        ecrt_master_sync_slave_clocks(master);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);

        uint64_t cyc1 = pmu_cycles();
        uint64_t d = cyc1 - cyc0;              /* processing time (cycles) */
        if (d > jit_max)        jit_max = d;
        if (d > jit_max_window) jit_max_window = d;

        ec_domain_state_t ds;
        ecrt_domain_state(domain, &ds);
        if (ds.working_counter > 0) wkc_ok++;
        else                        wkc_zero++;

        cycles++;

        /* ===== WCET statistics (after warm-up) ===== */
        if (cycles > WCET_WARMUP) {
            wcet_n++;
            /* (a) processing time */
            if (d < proc_min) proc_min = d;
            if (d > proc_max) proc_max = d;
            proc_sum += d;
            /* (b) wake-up jitter */
            if (wake_off_ticks > wake_max) wake_max = wake_off_ticks;
            wake_sum += wake_off_ticks;
            /* wake-up jitter histogram, in µs */
            uint32_t wake_us = (tfreq > 0)
                ? (uint32_t)((wake_off_ticks * 1000000ull) / tfreq) : 0u;
            wcet_hist_add(dg, wake_us);
        }

        /* CONTINUOUS publication of the snapshot (read by the shell on Core2). */
        dg->di             = di;
        dg->do_            = out_pattern;
        dg->last_wkc       = ds.working_counter;
        dg->cycles_total   = (uint32_t)cycles;
        dg->wkc_ok         = wkc_ok;
        dg->wkc_zero       = wkc_zero;
        dg->overruns       = overruns;
        dg->jit_max_cycles = jit_max;
        /* WCET */
        dg->wcet_samples   = wcet_n;
        dg->proc_min_cyc   = (proc_min == (uint64_t)-1) ? 0u : proc_min;
        dg->proc_max_cyc   = proc_max;
        dg->proc_sum_cyc   = proc_sum;
        dg->wake_max_ticks = wake_max;
        dg->wake_sum_ticks = wake_sum;

        /* PRODUCTION: NO periodic log here.
         *
         * A printf on Core0 blocks on the UART (~10-15 ms at 115200 baud) and
         * would create a FALSE overrun on the next cycle. The state is
         * published CONTINUOUSLY in ecat_diag (snapshot above): it is read on
         * demand by the shell (`ecat`, `wcet` commands) from Core2. If Core0
         * really needs to trace an event, use the lock-free klog:
         *     klog_write("[ecat] event");   // drained by Core2
         */
        (void)jit_max_window;
    }
}
