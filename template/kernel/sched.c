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
 * sched.c — Per-partition SMP scheduler with strict affinity
 *
 * Model:
 *   - Global TCB table; each TCB carries its affinity core (fixed).
 *   - ONE logical run-queue per core: each core schedules ONLY the
 *     threads whose tcb.core == its id (no migration -> determinism).
 *   - g_current[core] = index of the current thread on this core.
 *   - Fixed priorities (0 = highest), round-robin at equal priority.
 *
 * Preemption is driven by the timer IRQ (per core) and by IPIs
 * (SGI IPI_RESCHED), via sched_on_tick() called from irq_handler.
 *
 * Each thread starts through a fake "trap frame" built on its stack;
 * at the first eret, the CPU jumps to its entry point, IRQs enabled.
 *
 * Independent of Linux.
 */

#include "thread.h"
#include "config.h"

/* First thread start (context.S). */
extern void cpu_start_first(uint64_t sp) __attribute__((noreturn));

/* Current core ID (MPIDR_EL1.Aff0). */
static inline uint32_t cur_core(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

/* ---- Global scheduler state ---- */
static tcb_t   g_tcb[CFG_MAX_THREADS];
static uint8_t g_stacks[CFG_MAX_THREADS][CFG_THREAD_STACK_SIZE]
                   __attribute__((aligned(16)));
/* volatile: the table can be extended while the scheduler is already
 * running (Core2 creates its service threads after sched_start()). */
static volatile uint32_t g_nthreads;

/* Current thread PER CORE (index in g_tcb, or 0xFFFFFFFF = none). */
static volatile uint32_t g_current[CFG_NUM_CORES];
#define NO_THREAD   0xFFFFFFFFu

/* SPSR for a thread starting in EL1h (IRQs enabled). */
#define SPSR_EL1H_IRQON   0x5u

/* Trampoline: wraps a thread entry to catch an eventual return. */
static void thread_trampoline(thread_entry_t entry, void *arg)
{
    entry(arg);
    /* A thread must not return: block it cleanly. */
    uint32_t core = cur_core();
    for (;;) {
        uint32_t id = g_current[core];
        if (id != NO_THREAD)
            g_tcb[id].state = THREAD_BLOCKED;
        thread_yield();
    }
}

void sched_init(void)
{
    g_nthreads = 0;
    for (uint32_t i = 0; i < CFG_MAX_THREADS; i++)
        g_tcb[i].state = THREAD_UNUSED;
    for (uint32_t c = 0; c < CFG_NUM_CORES; c++)
        g_current[c] = NO_THREAD;
}

int thread_create_on(const char *name, thread_entry_t entry, void *arg,
                     uint32_t priority, uint32_t core)
{
    if (g_nthreads >= CFG_MAX_THREADS || core >= CFG_NUM_CORES)
        return -1;

    /* The slot is filled BEFORE g_nthreads is incremented: a thread may be
     * created while the scheduler is already running (Core2 service
     * threads). Publishing the counter too early would expose to
     * pick_next() a TCB whose SP is not built yet -> branch to sp=0. */
    uint32_t id = g_nthreads;
    tcb_t *t = &g_tcb[id];

    t->id         = id;
    t->name       = name;
    t->priority   = priority;
    t->base_prio  = priority;
    t->core       = core;
    t->state      = THREAD_UNUSED;   /* not schedulable yet: see below */
    t->stack_base = g_stacks[id];
    t->stack_size = CFG_THREAD_STACK_SIZE;

    /* 16-aligned stack top. */
    uint64_t sp_top = (uint64_t)(t->stack_base + t->stack_size);
    sp_top &= ~0xFUL;

    /* Build the initial trap frame. */
    sp_top -= TRAPFRAME_SIZE;
    uint64_t *tf = (uint64_t *)sp_top;
    for (uint32_t i = 0; i < TRAPFRAME_SIZE / 8; i++)
        tf[i] = 0;

    tf[0] = (uint64_t)entry;   /* x0 -> trampoline arg0 */
    tf[1] = (uint64_t)arg;     /* x1 -> trampoline arg1 */
    tf[TF_OFF_ELR / 8]  = (uint64_t)thread_trampoline;
    tf[TF_OFF_SPSR / 8] = SPSR_EL1H_IRQON;

    t->sp = sp_top;

    /* Publish: the TCB is complete, mark it schedulable THEN extend the
     * table (barrier to enforce ordering against the other cores and the
     * scheduling IRQ). */
    t->state = THREAD_READY;
    __asm__ volatile("dmb ish" ::: "memory");
    g_nthreads = id + 1u;

    return (int)id;
}

int thread_create(const char *name, thread_entry_t entry, void *arg,
                  uint32_t priority)
{
    return thread_create_on(name, entry, arg, priority, 0);
}

/* Selects the next highest-priority ready thread IN the partition of core
 * 'core' (round-robin at equal priority, starting after 'from'). */
static uint32_t pick_next(uint32_t core, uint32_t from)
{
    uint32_t best = NO_THREAD;
    uint32_t best_prio = 0xFFFFFFFFu;

    if (g_nthreads == 0)
        return NO_THREAD;

    for (uint32_t k = 1; k <= g_nthreads; k++) {
        uint32_t i = (from + k) % g_nthreads;
        if (g_tcb[i].core != core)
            continue;
        if (g_tcb[i].state == THREAD_READY || g_tcb[i].state == THREAD_RUNNING) {
            if (g_tcb[i].priority < best_prio) {
                best_prio = g_tcb[i].priority;
                best = i;
            }
        }
    }
    return best;
}

void sched_start(void)
{
    uint32_t core = cur_core();

    /* Pick the first highest-priority ready thread of THIS partition. */
    uint32_t best = NO_THREAD;
    uint32_t best_prio = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < g_nthreads; i++) {
        if (g_tcb[i].core == core && g_tcb[i].state == THREAD_READY &&
            g_tcb[i].priority < best_prio) {
            best_prio = g_tcb[i].priority;
            best = i;
        }
    }

    if (best == NO_THREAD) {
        /* No task on this core: stay idle (wfi) without crashing. */
        for (;;)
            __asm__ volatile("wfi");
    }

    g_current[core] = best;
    g_tcb[best].state = THREAD_RUNNING;
    cpu_start_first(g_tcb[best].sp);
    for (;;) { }
}

uint32_t thread_self(void)
{
    uint32_t id = g_current[cur_core()];
    return (id == NO_THREAD) ? 0 : id;
}

uint64_t sched_on_tick(uint64_t sp_current)
{
    uint32_t core = cur_core();
    uint32_t cur  = g_current[core];

    /* No thread on this core: nothing to schedule. */
    if (cur == NO_THREAD)
        return sp_current;

    /* Save the current context. */
    g_tcb[cur].sp = sp_current;
    if (g_tcb[cur].state == THREAD_RUNNING)
        g_tcb[cur].state = THREAD_READY;

    /* Pick the next in the partition. */
    uint32_t next = pick_next(core, cur);
    if (next == NO_THREAD)
        next = cur;                    /* no one else: stay */

    g_current[core] = next;
    g_tcb[next].state = THREAD_RUNNING;
    return g_tcb[next].sp;
}

void thread_yield(void)
{
    /* Rely on the timer tick to preempt; wfi waits for the next one. */
    __asm__ volatile("wfi");
}

uint32_t sched_partition_count(uint32_t core)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nthreads; i++)
        if (g_tcb[i].core == core &&
            (g_tcb[i].state == THREAD_READY || g_tcb[i].state == THREAD_RUNNING))
            n++;
    return n;
}

/* ---- PI mutex support ---- */
uint32_t thread_get_priority(uint32_t id)
{
    return (id < g_nthreads) ? g_tcb[id].priority : 0xFFFFFFFFu;
}

void thread_set_effective_priority(uint32_t id, uint32_t prio)
{
    if (id < g_nthreads && prio < g_tcb[id].priority)
        g_tcb[id].priority = prio;    /* boost only (lower value) */
}

void thread_restore_priority(uint32_t id)
{
    if (id < g_nthreads)
        g_tcb[id].priority = g_tcb[id].base_prio;
}

uint32_t thread_core_of(uint32_t id)
{
    return (id < g_nthreads) ? g_tcb[id].core : 0;
}
