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
 * ecat_diag.c — Shared EtherCAT diagnostic snapshot
 *               + WCET histogram bounds.
 *
 * A single static instance, published by the EtherCAT partition (producer)
 * and consulted by the remote shell (consumer). See ecat_diag.h.
 */
#include "ecat_diag.h"
#include <string.h>

/* Aligned on a cache line (64 B): Core0 (EtherCAT) is the SOLE PRODUCER,
 * Core2 (shell) is the CONSUMER. Both cores have an active MMU (shared
 * Normal WB memory, same Inner Shareable coherence domain): `volatile`
 * fields + atomic scalar access are sufficient
 * (no dc cvac required between two cores with active MMU). The alignment
 * avoids false-sharing with neighboring data. */
static ecat_diag_t s_diag __attribute__((aligned(64)));

/* Upper bounds (µs) of the wake-up jitter histogram buckets.
 * The last bucket ([N-1]) captures everything >= the last bound.
 * Choice: fine granularity near 0 (the nominal case, jitter << µs),
 * then wider buckets to spot a potential disturbance. */
const uint32_t ecat_wcet_bucket_us[ECAT_WCET_BUCKETS] = {
    1u,    /* [0-1)   µs */
    2u,    /* [1-2)   µs */
    5u,    /* [2-5)   µs */
    10u,   /* [5-10)  µs */
    20u,   /* [10-20) µs */
    50u,   /* [20-50) µs */
    100u,  /* [50-100)µs */
    0u,    /* [>=100] µs — last bucket: everything else */
};

void ecat_diag_reset(void)
{
    memset((void *)&s_diag, 0, sizeof(s_diag));
    /* proc_min must start at +inf so that the 1st sample lowers it. */
    s_diag.proc_min_cyc = (uint64_t)-1;
}

ecat_diag_t *ecat_diag_get(void)
{
    return &s_diag;
}
