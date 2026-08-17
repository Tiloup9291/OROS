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
 * wolf_port.h — Bare-metal port of wolfSSL/wolfCrypt for the OROS RK3328
 *               (SSH server). Provides the hardware primitives
 *               that wolfCrypt requires in an OS-less environment:
 *   - RNG seed: wc_rtos_GenerateSeed() (wired to PMU + Generic Timer),
 *     referenced by CUSTOM_RAND_GENERATE_SEED in user_settings.h;
 *   - time base: wc_rtos_time() (monotonic seconds) for XTIME.
 *
 * WARNING: the seed's entropy comes from the PMU cycle counter +
 * the Generic Timer. This is sufficient for an SSH connectivity demo/POC,
 * but NOT a true TRNG. To be reinforced (dedicated entropy source) for
 * production use.
 */
#ifndef RTOS_NET_WOLF_PORT_H
#define RTOS_NET_WOLF_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generates `sz` bytes of seed for wolfCrypt's DRBG. Returns 0 on success
 * (wolfCrypt convention). Signature imposed by CUSTOM_RAND_GENERATE_SEED.
 */
int wc_rtos_GenerateSeed(uint8_t *output, uint32_t sz);

/*
 * Time base in seconds since boot (monotonic). Used by wolfCrypt's XTIME
 * hook (WOLFSSL_USER_CURRTIME). The return type follows time_t (long) for
 * wc_Time compatibility.
 */
long wc_rtos_time(long *t);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_NET_WOLF_PORT_H */
