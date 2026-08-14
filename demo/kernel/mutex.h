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
 * mutex.h — Priority-inheritance (PI) mutex & semaphores
 *
 * Deterministic synchronization primitives for real time:
 *
 *  - mutex_t  : mutual-exclusion lock with PRIORITY INHERITANCE (PI).
 *    If a high-priority thread waits for a mutex held by a low-priority
 *    thread, the latter sees its effective priority temporarily RAISED to the
 *    waiter's level -> avoids unbounded priority inversion
 *    (essential in hard-RT, NFR1).
 *
 *  - sem_t    : counting semaphore (count) with wait.
 *
 * Implementation: the internal structure is protected by an SMP spinlock
 * (sync.h). The wait is a short active wait + yield (suited to a partitioned
 * RTOS where critical sections are brief).
 *
 * Independent of Linux.
 */
#ifndef RTOS_MUTEX_H
#define RTOS_MUTEX_H

#include <stdint.h>
#include "sync.h"

/* "No owner" value. */
#define MUTEX_NO_OWNER   0xFFFFFFFFu

typedef struct {
    spinlock_t        guard;      /* protects the fields below (SMP) */
    volatile uint32_t owner;      /* id of the holding thread, or MUTEX_NO_OWNER */
    volatile uint32_t owner_prio; /* base priority of the holder (for PI) */
    volatile uint32_t held;       /* 0 = free, 1 = held */
} mutex_t;

#define MUTEX_INIT   { SPINLOCK_INIT, MUTEX_NO_OWNER, 0, 0 }

void mutex_init(mutex_t *m);

/* Takes the mutex (blocking). Applies priority inheritance to the holder if
 * the caller is more prioritary. */
void mutex_lock(mutex_t *m);

/* Tries to take the mutex without blocking. 1 if taken, 0 otherwise. */
int  mutex_trylock(mutex_t *m);

/* Releases the mutex (restores the holder's base priority). */
void mutex_unlock(mutex_t *m);

/* ---- Counting semaphore ---- */
typedef struct {
    spinlock_t        guard;
    volatile int32_t  count;
} sem_t;

#define SEM_INIT(n)   { SPINLOCK_INIT, (n) }

void sem_init(sem_t *s, int32_t initial);

/* P() / wait: decrements; blocks (active wait + yield) if count <= 0. */
void sem_wait(sem_t *s);

/* Tries without blocking. 1 if acquired, 0 otherwise. */
int  sem_trywait(sem_t *s);

/* V() / post: increments. */
void sem_post(sem_t *s);

/* Current value (diagnostic). */
int32_t sem_value(sem_t *s);

#endif /* RTOS_MUTEX_H */
