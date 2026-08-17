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
 * wolf_port.c — Bare-metal port of wolfCrypt (RNG seed + time base).
 *               SSH server. See wolf_port.h.
 *
 * RNG seed: we mix TWO hardware sources available on the RK3328:
 *   - the PMU cycle counter (PMCCNTR_EL0, 1 CPU cycle resolution);
 *   - the ARM Generic Timer (CNTPCT_EL0).
 * These two counters, sampled in a loop with data-dependent delays, provide
 * usable jitter as a seed for wolfCrypt's DRBG (HASH-DRBG). We apply a
 * SplitMix64-style mixer to properly disperse the bits before delivering
 * them.
 *
 * WARNING: this seed is NOT a true TRNG (moderate entropy).
 * Sufficient to establish a demonstration SSH session; to be reinforced in
 * production.
 */
#include "wolf_port.h"

#include "../arch/aarch64/pmu.h"
#include "../arch/aarch64/timer.h"

/* SplitMix64 mixer (good bit dispersion, non-cryptographic). */
static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/*
 * Collects a 64-bit word of raw entropy by sampling the PMU and the
 * Generic Timer with variable micro-delays (the number of iterations
 * depends on the already-collected bits -> data-dependent jitter).
 */
static uint64_t collect_entropy_word(void)
{
    uint64_t acc = pmu_cycles() ^ (timer_now_ticks() << 1);

    for (int i = 0; i < 8; i++) {
        /* Small delay whose duration depends on the low bits of the
         * accumulator: introduces jitter measured by the counters. */
        volatile unsigned spin = (unsigned)((acc & 0x1F) + 3);
        while (spin--)
            __asm__ volatile("nop");

        uint64_t c = pmu_cycles();
        uint64_t t = timer_now_ticks();
        /* Non-linear mixing of the two counters. */
        acc ^= c + (t << 7) + (acc << 13);
        acc = (acc << 1) | (acc >> 63); /* rotation */
        acc ^= (c >> 3) ^ (t << 5);
    }
    return acc;
}

int wc_rtos_GenerateSeed(uint8_t *output, uint32_t sz)
{
    if (output == 0)
        return -1;

    uint64_t state = collect_entropy_word();

    uint32_t i = 0;
    while (i < sz) {
        /* Regularly re-collect fresh entropy (every 32 bytes) so as not to
         * unroll a simple deterministic PRNG over the whole seed. */
        if ((i & 0x1F) == 0)
            state ^= collect_entropy_word();

        uint64_t r = splitmix64(&state);

        for (int b = 0; b < 8 && i < sz; b++, i++) {
            output[i] = (uint8_t)(r & 0xFF);
            r >>= 8;
        }
    }
    return 0;
}

/*
 * Time base in seconds (monotonic since boot). Wired to XTIME via
 * WOLFSSL_USER_CURRTIME: wolfCrypt calls time(&t). We convert the Generic
 * Timer ticks to seconds.
 */
long wc_rtos_time(long *t)
{
    uint64_t us = timer_ticks_to_us(timer_now_ticks());
    long sec = (long)(us / 1000000ull);
    if (t)
        *t = sec;
    return sec;
}
