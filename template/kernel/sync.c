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
 * sync.c — SMP spinlock (ARMv8 LDAXR/STXR)
 *
 * Implementation using load-acquire / store-release exclusives + WFE/SEV to
 * reduce contention (low-power wait).
 *
 * Independent of Linux.
 */

#include "sync.h"

void spin_lock(spinlock_t *s)
{
    uint32_t tmp;
    __asm__ volatile(
        "   sevl\n"                 /* arm the event for the first wfe */
        "1: wfe\n"                  /* wait for an event (reduces contention) */
        "2: ldaxr   %w0, [%1]\n"    /* exclusive load-acquire */
        "   cbnz    %w0, 1b\n"      /* if taken (!=0), go back to waiting */
        "   stxr    %w0, %w2, [%1]\n" /* try to write 1 */
        "   cbnz    %w0, 2b\n"      /* if exclusive store failed, retry */
        : "=&r"(tmp)
        : "r"(&s->lock), "r"(1u)
        : "memory");
}

int spin_trylock(spinlock_t *s)
{
    uint32_t val, res;
    __asm__ volatile(
        "   ldaxr   %w0, [%2]\n"
        "   cbnz    %w0, 1f\n"      /* already taken -> failure */
        "   stxr    %w1, %w3, [%2]\n"
        "   b       2f\n"
        "1: mov     %w1, #1\n"      /* failure (non-zero marker) */
        "2:\n"
        : "=&r"(val), "=&r"(res)
        : "r"(&s->lock), "r"(1u)
        : "memory");
    /* Taken if val==0 (was free) AND res==0 (store succeeded). */
    return (val == 0 && res == 0) ? 1 : 0;
}

void spin_unlock(spinlock_t *s)
{
    __asm__ volatile(
        "   stlr    wzr, [%0]\n"    /* store-release 0 (releases) */
        "   sev\n"                  /* wake up the waiters (wfe) */
        :
        : "r"(&s->lock)
        : "memory");
}
