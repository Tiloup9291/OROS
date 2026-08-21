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
 * thread.h — Threads & per-partition SMP scheduler
 *
 * - single-core preemptive scheduler.
 * - PER-PARTITION scheduler with strict affinity:
 *   - one run-queue per core (partition);
 *   - each thread is pinned to a core (fixed affinity, NO migration);
 *   - each core schedules ONLY the threads of its partition;
 *   - fixed priorities (0 = highest), round-robin at equal priority.
 *
 * Strict affinity avoids non-deterministic cache/TLB disturbances
 * -> bounded WCET on the hard-RT cores (Core 0/1).
 */
#ifndef RTOS_THREAD_H
#define RTOS_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include "config.h"

/* Thread entry point. */
typedef void (*thread_entry_t)(void *arg);

/* Thread states. */
typedef enum {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
} thread_state_t;

/*
 * Trap frame layout pushed by vectors.S (must stay in sync):
 *   offset 0x00 : x0..x30 (31 regs) + 1 padding (xzr)  -> 32 * 8 = 256 bytes
 *   offset 0x100: ELR_EL1 (PC)                          -> +8
 *   offset 0x108: SPSR_EL1 (state)                      -> +8
 *   offset 0x110: v0..v31 (SIMD/NEON registers)         -> 32 * 16 = 512 B
 *   offset 0x310: FPCR (FP control word)                -> +8
 *   offset 0x318: FPSR (FP status word)                 -> +8
 *   total = 800 bytes
 *
 * The NEON registers are saved/restored on every context switch so that
 * several threads pinned to the same core can all use SIMD (e.g. NEON
 * memcpy, wolfSSL crypto) without corrupting each other.
 */
#define TRAPFRAME_SIZE     800u
#define TF_OFF_X0          0u
#define TF_OFF_X30         (30u * 8u)
#define TF_OFF_ELR         (32u * 8u)   /* 256 */
#define TF_OFF_SPSR        (33u * 8u)   /* 264 */
#define TF_OFF_NEON        (34u * 8u)                    /* 272: v0..v31   */
#define TF_OFF_FPCR        (TF_OFF_NEON + 32u * 16u)     /* 784 (8-aligned)*/
#define TF_OFF_FPSR        (TF_OFF_FPCR + 8u)            /* 792            */

/* Task control block (TCB). */
typedef struct tcb {
    uint64_t        sp;           /* saved SP (points to the trap frame) */
    thread_state_t  state;
    uint32_t        priority;     /* effective priority (0 = highest) */
    uint32_t        base_prio;    /* base priority (before PI inheritance) */
    uint32_t        id;
    uint32_t        core;         /* affinity core (partition) — fixed */
    uint8_t        *stack_base;   /* allocated stack base */
    size_t          stack_size;
    const char     *name;
} tcb_t;

/* Initializes the threads/scheduler subsystem (all partitions). */
void sched_init(void);

/* Creates a thread pinned to core 'core' (strict affinity). Fixed priority.
 * Returns the id (>=0) or -1 on failure. */
int thread_create_on(const char *name, thread_entry_t entry, void *arg,
                     uint32_t priority, uint32_t core);

/* Compat: creates a thread on core 0. */
int thread_create(const char *name, thread_entry_t entry, void *arg,
                  uint32_t priority);

/* Starts scheduling on the CURRENT core: switches to the highest-priority
 * ready thread of ITS partition. Never returns. Each core (0..3)
 * calls sched_start() after registering its threads. */
void sched_start(void) __attribute__((noreturn));

/* Voluntarily yields the CPU (triggers a reschedule at the next tick). */
void thread_yield(void);

/* Returns the id of the current thread on the CURRENT core. */
uint32_t thread_self(void);

/* Called by the IRQ handler (tick/IPI): decides a context switch on the
 * CURRENT core. Receives the saved SP, returns the SP to restore. */
uint64_t sched_on_tick(uint64_t sp_current);

/* Number of ready/running threads in core 'core' partition. */
uint32_t sched_partition_count(uint32_t core);

/* ---- Priority-inheritance mutex support (PI) ---- */
/* Current effective priority of thread 'id'. */
uint32_t thread_get_priority(uint32_t id);
/* Forces the effective priority of 'id' (PI boost). Does not overwrite base_prio. */
void     thread_set_effective_priority(uint32_t id, uint32_t prio);
/* Restores the effective priority of 'id' to its base. */
void     thread_restore_priority(uint32_t id);
/* Affinity core of thread 'id'. */
uint32_t thread_core_of(uint32_t id);

#endif /* RTOS_THREAD_H */
