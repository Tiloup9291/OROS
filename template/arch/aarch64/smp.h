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
 * smp.h — Secondary core SMP boot
 *
 * Wakeup core 1..3 with PSCI (CPU_ON, SMC call to ATF present on
 * the board). Each secondary core enters a common entry point (secondary_entry
 * in start.S) then call secondary_main() in C.
 */
#ifndef RTOS_ARCH_SMP_H
#define RTOS_ARCH_SMP_H

#include <stdint.h>

/* Number of cores (cf config.h CFG_NUM_CORES = 4). */

/* Wakeup core 'core' (1..3) and boot it.
 * Return 0 if PSCI call succeed, else PSCI error code (<0). */
int smp_start_core(uint32_t core);

/* Wakeup all secondary cores (1..N-1). Return number of core
 * really started (excluding core 0). */
uint32_t smp_start_all(void);

/* ID of current core (MPIDR_EL1.Aff0). */
static inline uint32_t smp_core_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

/* Online cores counter (incremented by each core at boot). */
uint32_t smp_online_count(void);

/* Called in C by each secondary core (from start.S secondary_entry). */
void secondary_main(void);

/* Allows the secondary cores to start their schedulers..
 * Must be called by core 0 only AFTER all partition threads have been created. */
void smp_release_schedulers(void);

/* Shutdown system through PSCI SYSTEM_OFF (HVC in QEMU, SMC/ATF on
 * hardware). Used to cleanly terminate the demo unde QEMU. Never come back.
 * DON'T call on real hardware if the system must remain active. */
void smp_system_off(void) __attribute__((noreturn));

void smp_system_reset(void) __attribute__((noreturn));


#endif /* RTOS_ARCH_SMP_H */

