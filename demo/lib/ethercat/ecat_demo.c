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
 * ecat_demo.c — Demo: EtherCAT master (ecrt_*) on raw L2 GMAC.
 *
 * STATIC compiled PDO/SDO config (adding a slave = edit this table +
 * recompile), modeled after the reference example provided through Tiloup9291/YAEMAA:
 *   16-bit TOR I/O slave, vendor 0x0000_0B95 / product 0x0000_1500;
 *   output PDO 0x1600 -> entry 0x7000:00 (16 bits, DO);
 *   input PDO 0x1A00 -> entry 0x6000:00 (16 bits, DI).
 *
 * The real-time cycle reproduces Tiloup9291/YEAMAA's oobThread.c:
 *   receive -> domain_process -> EC_READ_U16(DI) -> EC_WRITE_U16(DO)
 *   -> application_time -> sync_reference_clock -> sync_slave_clocks
 *   -> domain_queue -> send.
 * Cycle jitter is measured via the PMU (WCET/jitter).
 *
 * Without a slave (or on QEMU): ecrt_request_master -> NULL or
 * master_activate < 0 -> the demo reports it cleanly and returns (no block).
 */

#include "ecat_demo.h"
#include "ecrt.h"
#include "timer.h"
#include "pmu.h"
#include "config.h"
#include "../../net/ecat_diag.h"   /* diag snapshot read by the shell */
#include "gmac.h"                    /* GMAC link speed for the diag */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ─── Slave config (identical to the YAEMAA example's ethercatPDOs.c) ─── */
#define SLAVE_VENDOR   0x00000B95u
#define SLAVE_PRODUCT  0x00001500u

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
    { 0, EC_DIR_OUTPUT, 0, NULL,          EC_WD_DISABLE },
    { 1, EC_DIR_INPUT,  0, NULL,          EC_WD_DISABLE },
    { 2, EC_DIR_OUTPUT, 1, slave_pdos_out, EC_WD_ENABLE  },
    { 3, EC_DIR_INPUT,  1, slave_pdos_in,  EC_WD_DISABLE },
    { 0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT } /* end marker */
};

/* Offsets filled by ecrt_domain_reg_pdo_entry_list. */
static unsigned int off_do;
static unsigned int off_di;

static ec_pdo_entry_reg_t domain_regs[] = {
    { 0, 0, SLAVE_VENDOR, SLAVE_PRODUCT, 0x7000, 0x00, &off_do, NULL },
    { 0, 0, SLAVE_VENDOR, SLAVE_PRODUCT, 0x6000, 0x00, &off_di, NULL },
    { 0 }   /* terminator */
};

void ecat_demo_run(void)
{
    printf("\n============================================================\n");
    printf(" DEMO: EtherCAT MASTER (ecrt_*) on GMAC L2 raw\n");
    printf("============================================================\n");
    printf("[ecat] expected slave : vendor=0x%08X product=0x%08X\n",
           SLAVE_VENDOR, SLAVE_PRODUCT);
    printf("[ecat] PDO : output 0x7000:00 (DO 16b) / input 0x6000:00 (DI 16b)\n");
    printf("[ecat] cycle = %u us (CFG_ECAT_CYCLE_US), synchronous polling.\n",
           (unsigned)CFG_ECAT_CYCLE_US);

    /* Diagnostic snapshot (read by the TCP shell). Populated as
     * execution progresses; remains consultable after the demo ends. */
    ecat_diag_t *dg = ecat_diag_get();
    ecat_diag_reset();
    dg->vendor_id    = SLAVE_VENDOR;
    dg->product_code = SLAVE_PRODUCT;
    dg->valid        = 1;

    /* --- 1. Reservation + static config (like Tiloup9291/YAEMAA's initThread) --- */
    ec_master_t *master = ecrt_request_master(0);
    if (!master) {
        printf("[ecat] no master (GMAC missing/QEMU or no link) : demo ignored.\n");
        printf("============================================================\n\n");
        return;
    }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) { printf("[ecat] create_domain failed.\n"); goto out; }

    ec_slave_config_t *sc = ecrt_master_slave_config(master, 0, 0,
                                                     SLAVE_VENDOR, SLAVE_PRODUCT);
    if (!sc) { printf("[ecat] slave_config failed.\n"); goto out; }

    if (ecrt_slave_config_pdos(sc, EC_END, slave_sync)) {
        printf("[ecat] config_pdos failed.\n"); goto out;
    }
    if (ecrt_domain_reg_pdo_entry_list(domain, domain_regs)) {
        printf("[ecat] reg_pdo_entry_list failed.\n"); goto out;
    }
    /* DC: cycle = EtherCAT period (assign_activate 0x0300 = SYNC0). */
    ecrt_slave_config_dc(sc, 0x0300, CFG_ECAT_CYCLE_US * 1000u, 0, 0, 0);

    /* --- 2. Activation: scan + ESM INIT→OP --- */
    if (ecrt_master_activate(master)) {
        printf("[ecat] activation FAILED: no slave reached OP.\n");
        printf(">>> Demo: master pipeline OK (build/logic), BUT no\n");
        printf(">>> slave in OP. Connect an EtherCAT slave in GMAC (RJ45)\n");
        printf(">>> port and restart. <<<\n");
        goto out;
    }

    uint8_t *pd = ecrt_domain_data(domain);
    if (!pd) { printf("[ecat] domain_data NULL.\n"); goto out; }

    ec_slave_config_state_t ss;
    ecrt_slave_config_state(sc, &ss);
    printf("[ecat] slave : online=%d operational=%d AL=0x%02X\n",
           ss.online, ss.operational, ss.al_state);

    /* Publish the activation state into the diag snapshot (shell). */
    dg->master_up     = 1;
    dg->link_mbps     = (uint32_t)gmac_get_link_speed();  /* GMAC link speed */
    dg->slaves_online = ss.online ? 1u : 0u;
    dg->slaves_op     = ss.operational ? 1u : 0u;
    dg->al_state      = ss.al_state;

    /* --- Direct FPRD probe + scan ---
     * Isolates the "DI always 0" cause BEFORE the cyclic loop. We first
     * emit an LRW cycle so the slave refreshes its SM, then we DIRECTLY
     * read its input memory via physical FPRD (and scan candidate
     * addresses). Compare with the logical DI read in the loop. */
    ecrt_domain_queue(domain);
    ecrt_master_send(master);
    ecrt_master_receive(master);
    printf("[ecat] --- probing inputs (direct FPRD + scanning) ---\n");
    ecrt_master_probe_input(master, 1);
    printf("[ecat] --------------------------------------------\n");


    /* --- 3. Real-time cycle loop --- */
    /* OUTPUTS test: we BLINK A SINGLE output (bit 0 = DO0) at 1 Hz
     * (1 s ON / 1 s OFF) to VISUALLY/multimeter-verify that this specific
     * output actually switches. On every tick of 1000 cycles (= 1 s @ 1 ms
     * cycle), we toggle bit 0 between 0x0001 (DO0 on) and 0x0000 (DO0 off). */
    const unsigned n_cycles = 20000;     /* ~20 s @1 ms (10 blinks) */
    const uint32_t cycle_us = CFG_ECAT_CYCLE_US;
    const unsigned blink_period = 1000000u / cycle_us; /* cycles for 1 s */
    const uint16_t OUT_BIT = 0x0001;     /* tested output bit (DO0) */
    uint16_t out_pattern = 0x0000;
    unsigned wkc_ok = 0, wkc_zero = 0, overruns = 0;
    uint64_t jit_min = ~0ull, jit_max = 0;



    uint64_t next = timer_now_ticks();
    for (unsigned c = 0; c < n_cycles; c++) {
        /* Wait for the cycle tick (fixed period). */
        next += timer_us_to_ticks(cycle_us);
        uint64_t t0 = timer_now_ticks();
        if (t0 > next) {
            overruns++;                  /* late cycle */
            next = t0;                   /* resync */
        } else {
            while (timer_now_ticks() < next) __asm__ volatile("nop");
        }

        uint64_t cyc0 = pmu_cycles();

        /* ===== EtherCAT cycle (like oobThread) ===== */
        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        /* Read the slave's inputs (DI). */
        uint16_t di = EC_READ_U16(pd + off_di);

        /* Output logic: 1 s ON / 1 s OFF BLINK of A SINGLE output
         * (DO0 = bit 0). ON = 0x0001 during the 1st second, OFF = 0x0000 the 2nd. */
        out_pattern = ((c / blink_period) & 1u) ? 0x0000u : OUT_BIT;
        EC_WRITE_U16(pd + off_do, out_pattern);



        ecrt_master_application_time(master,
                (uint64_t)timer_ticks_to_us(timer_now_ticks()) * 1000ull);
        ecrt_master_sync_reference_clock(master);
        ecrt_master_sync_slave_clocks(master);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);

        uint64_t cyc1 = pmu_cycles();
        uint64_t d = cyc1 - cyc0;
        if (d < jit_min) jit_min = d;
        if (d > jit_max) jit_max = d;

        /* Count the WKCs (proof of PDO exchange). */
        ec_domain_state_t ds;
        ecrt_domain_state(domain, &ds);
        if (ds.working_counter > 0) wkc_ok++;
        else                        wkc_zero++;

        /* Refresh the diag snapshot (read by the TCP shell). Atomic
         * scalar writes; the shell reads a consistent value per field. */
        dg->di            = di;
        dg->do_           = out_pattern;
        dg->last_wkc      = ds.working_counter;
        dg->cycles_total  = c + 1;
        dg->wkc_ok        = wkc_ok;
        dg->wkc_zero      = wkc_zero;
        dg->overruns      = overruns;
        dg->jit_max_cycles = jit_max;

        /* Periodic log (in the MIDDLE of each blink phase to clearly see
         * the alternation DO=0xFFFF ↔ 0x0000). */
        if ((c % blink_period) == (blink_period / 2)) {
            printf("[ecat] t=%2u s : DI=0x%04X DO=0x%04X (%s) WKC=%u\n",
                   c / blink_period, di, out_pattern,
                   out_pattern ? "OUTPUTS ON" : "outputs off",
                   ds.working_counter);
        }

    }

    printf("------------------------------------------------------------\n");
    printf("[ecat] cycles=%u : WKC>0=%u, WKC=0=%u, overruns=%u\n",
           n_cycles, wkc_ok, wkc_zero, overruns);
    printf("[ecat] cycle jitter (PMU) min=%llu max=%llu CPU cycles\n",
           (unsigned long long)jit_min, (unsigned long long)jit_max);

    if (wkc_ok > 0 && overruns == 0) {
        printf(">>> Demo: slave in OP, cyclic PDO STABLE (WKC>0),\n");
        printf(">>>    overrun=0. OK. <<<\n");
    } else if (wkc_ok > 0) {
        printf(">>> Demo: PDO exchanged (WKC>0) but %u overrun(s) — to be refined.\n",
               overruns);
    } else {
        printf(">>> Demo: slave in OP but WKC always 0 (FMMU/SM mapping\n");
        printf(">>>    to verify). <<<\n");
    }

out:
    ecrt_master_deactivate(master);
    ecrt_release_master(master);
    printf("============================================================\n\n");
}
