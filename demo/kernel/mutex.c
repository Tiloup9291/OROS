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
 * mutex.c — Priority-inheritance (PI) mutex & semaphores
 *
 * Priority inheritance (PI):
 *   When a thread H (high priority) requests a mutex held by L (low priority),
 *   we RAISE the effective priority of L to H's level as long as L holds the
 *   mutex. Thus L cannot be indefinitely preempted by intermediate-priority
 *   threads (unbounded priority inversion).
 *   On release, we restore L's base priority.
 *
 * The coherence of the fields (owner, held) is guaranteed by an SMP spinlock
 * (LDAXR/STXR). The involved threads may be on different cores;
 * the PI boost only really affects the scheduler of the holder's core, but
 * the priority write is visible through coherent memory (Normal IS).
 *
 * Independent of Linux.
 */

#include "mutex.h"
#include "thread.h"

void mutex_init(mutex_t *m)
{
    spin_init(&m->guard);
    m->owner = MUTEX_NO_OWNER;
    m->owner_prio = 0;
    m->held = 0;
}

int mutex_trylock(mutex_t *m)
{
    int got = 0;
    spin_lock(&m->guard);
    if (!m->held) {
        m->held = 1;
        m->owner = thread_self();
        m->owner_prio = thread_get_priority(m->owner);
        got = 1;
    }
    spin_unlock(&m->guard);
    return got;
}

void mutex_lock(mutex_t *m)
{
    uint32_t self = thread_self();
    uint32_t self_prio = thread_get_priority(self);

    for (;;) {
        spin_lock(&m->guard);
        if (!m->held) {
            /* Free: take it. */
            m->held = 1;
            m->owner = self;
            m->owner_prio = self_prio;
            spin_unlock(&m->guard);
            return;
        }

        /* Busy: apply priority inheritance to the holder if we are
         * more prioritary (lower numeric value). */
        uint32_t owner = m->owner;
        if (owner != MUTEX_NO_OWNER && self_prio < thread_get_priority(owner))
            thread_set_effective_priority(owner, self_prio);

        spin_unlock(&m->guard);

        /* Wait: yield the CPU to let the (boosted) holder run. */
        thread_yield();
    }
}

void mutex_unlock(mutex_t *m)
{
    spin_lock(&m->guard);
    uint32_t owner = m->owner;
    /* Restore the holder's base priority (end of inheritance). */
    if (owner != MUTEX_NO_OWNER)
        thread_restore_priority(owner);
    m->owner = MUTEX_NO_OWNER;
    m->held = 0;
    spin_unlock(&m->guard);
}

/* ------------------------------------------------------------------ */
/* Counting semaphores                                                */
/* ------------------------------------------------------------------ */
void sem_init(sem_t *s, int32_t initial)
{
    spin_init(&s->guard);
    s->count = initial;
}

int sem_trywait(sem_t *s)
{
    int ok = 0;
    spin_lock(&s->guard);
    if (s->count > 0) {
        s->count--;
        ok = 1;
    }
    spin_unlock(&s->guard);
    return ok;
}

void sem_wait(sem_t *s)
{
    for (;;) {
        if (sem_trywait(s))
            return;
        thread_yield();     /* blocked: yield until a post */
    }
}

void sem_post(sem_t *s)
{
    spin_lock(&s->guard);
    s->count++;
    spin_unlock(&s->guard);
}

int32_t sem_value(sem_t *s)
{
    return s->count;
}
