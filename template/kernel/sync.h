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
 * sync.h — SMP synchronization primitives
 *
 * Spinlock based on ARMv8 atomic instructions (LDAXR/STXR) with
 * acquire/release barriers. Safe across cores (SMP), non-recursive.
 *
 * Note: requires an active MMU with "Normal Inner-Shareable" memory so that
 * the exclusive instructions work across cores — which is our case (mmu.c, SH_INNER).
 */
#ifndef RTOS_SYNC_H
#define RTOS_SYNC_H

#include <stdint.h>

typedef struct {
    volatile uint32_t lock;   /* 0 = free, 1 = held */
} spinlock_t;

#define SPINLOCK_INIT   { 0 }

/* Initializes a spinlock. */
static inline void spin_init(spinlock_t *s) { s->lock = 0; }

/* Takes the lock (active wait). Does it mask IRQs during the section? No:
 * the caller decides. For IRQ-safe usage, see spin_lock_irqsave later. */
void spin_lock(spinlock_t *s);

/* Tries to take the lock without blocking. Returns 1 if taken, 0 otherwise. */
int  spin_trylock(spinlock_t *s);

/* Releases the lock. */
void spin_unlock(spinlock_t *s);

#endif /* RTOS_SYNC_H */
