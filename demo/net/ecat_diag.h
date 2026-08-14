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
 * ecat_diag.h — Shared EtherCAT diagnostics + WCET validation.
 *
 * We requires that the remote SHELL (Core2, on
 * the USB-Ethernet RTL8153B) be able to DIAGNOSE the EtherCAT master (which,
 * in production, runs on Core0). Since Core0 (EtherCAT) and Core2 (network/
 * shell) are DISTINCT partitions, communication goes through a lock-free
 * shared memory area: the master publishes a SNAPSHOT of its state (single
 * write by the producer), the shell READS it (consumer). This is the
 * counterpart of the planning's "EtherCAT diag via mailbox -> Core0", in its
 * simplest and most deterministic form (no request/response: continuous
 * publication).
 *
 * The EtherCAT partition (Core0) and the
 * network partition (Core2) run IN PARALLEL: the master (ecat_task.c)
 * publishes this snapshot on EVERY cycle, and the shell (net_shell.c) reads
 * it in real time.
 *
 * WCET VALIDATION: this snapshot also carries the EtherCAT cycle's
 * real-time statistics (CPU processing time, wake-up jitter, histogram),
 * which prove that Core2's network/USB load does not disturb Core0's
 * determinism.
 */
#ifndef RTOS_NET_ECAT_DIAG_H
#define RTOS_NET_ECAT_DIAG_H

#include <stdint.h>

/* Number of histogram "buckets" for the wake-up jitter (WCET validation). */
#define ECAT_WCET_BUCKETS   8u

/* Snapshot of the EtherCAT master's state, published by the EtherCAT
 * partition and read by the shell. All fields are scalars (atomic update). */
typedef struct {
    volatile uint32_t valid;          /* 0 = never populated, 1 = snapshot available */
    volatile uint32_t master_up;      /* 1 if a master has been reserved (GMAC OK) */
    volatile uint32_t link_mbps;      /* GMAC link speed (0/10/100/1000) */
    volatile uint32_t slaves_online;  /* number of slaves online */
    volatile uint32_t slaves_op;      /* number of slaves in OP */
    volatile uint32_t al_state;       /* AL state of the 1st slave (1/2/4/8) */
    volatile uint32_t di;             /* last read inputs (DI) (16 bits) */
    volatile uint32_t do_;            /* last written outputs (DO) (16 bits) */
    volatile uint32_t last_wkc;       /* last domain working counter */
    volatile uint32_t cycles_total;   /* number of cycles executed */
    volatile uint32_t wkc_ok;         /* cycles with WKC > 0 */
    volatile uint32_t wkc_zero;       /* cycles with WKC == 0 */
    volatile uint32_t overruns;       /* cycles that overran the period */
    volatile uint32_t vendor_id;      /* expected slave vendor id */
    volatile uint32_t product_code;   /* expected slave product code */
    volatile uint64_t jit_max_cycles; /* max cycle jitter (PMU cycles) — compat */

    /* ─── WCET validation — PMU measurements of the EtherCAT core (Core0) ─── */
    /* Calibrated CPU frequency (Hz): allows converting PMU cycles to ns.
     * 0 until calibration is done. */
    volatile uint64_t cpu_hz;
    volatile uint32_t cycle_us;       /* nominal cycle period (µs) */
    volatile uint32_t warmup_cycles;  /* cycles ignored at startup (warm-up) */
    volatile uint32_t wcet_samples;   /* number of cycles counted (post warm-up) */

    /* (a) EtherCAT cycle PROCESSING TIME (real CPU load, PMU cycles):
     *     receive -> process -> EC_READ/WRITE -> queue -> send. */
    volatile uint64_t proc_min_cyc;   /* min */
    volatile uint64_t proc_max_cyc;   /* max (= processing WCET) */
    volatile uint64_t proc_sum_cyc;   /* sum (for the average) */

    /* (b) WAKE-UP JITTER: absolute deviation between the expected cycle tick
     *     and the actual wake-up instant (Generic Timer ticks). This is THE
     *     isolation indicator against Core2's network/USB load. */
    volatile uint64_t wake_max_ticks; /* max wake-up jitter (timer ticks) */
    volatile uint64_t wake_sum_ticks; /* sum (for the average) */

    /* Wake-up jitter histogram (µs): buckets < increasing thresholds.
     * Bounds (µs): [0-1) [1-2) [2-5) [5-10) [10-20) [20-50) [50-100) [>=100] */
    volatile uint32_t wake_hist[ECAT_WCET_BUCKETS];
} ecat_diag_t;

/* Upper bounds (µs) of the histogram buckets (index 0..N-2; the last bucket
 * [N-1] captures everything else, >= the last bound). */
extern const uint32_t ecat_wcet_bucket_us[ECAT_WCET_BUCKETS];

/* Resets the snapshot (called by the EtherCAT partition at startup). */
void ecat_diag_reset(void);

/* Returns the shared snapshot (never NULL). The producer writes into it, the
 * consumer (shell) reads it. */
ecat_diag_t *ecat_diag_get(void);

#endif /* RTOS_NET_ECAT_DIAG_H */
