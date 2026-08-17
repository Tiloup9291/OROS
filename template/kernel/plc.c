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
 * plc.c — Cyclic execution engine, programmable-logic-controller style
 *
 * Fixed-period scan loop, based on the system counter (CNTPCT).
 * Each cycle:
 *   1. read_inputs()            (sampling — "process image")
 *   2. run the due cyclic tasks (run-to-completion, no preemption)
 *   3. write_outputs()          (apply outputs)
 *   4. measure the duration + jitter, detect overruns
 *   5. wait for the next tick (deadline = start + n*T_cycle)
 *
 * This engine is deterministic: no preemption between the steps of a cycle.
 * It will equip Core 0/1 as needed.
 *
 * Independent of Linux.
 */

#include "plc.h"
#include "../arch/aarch64/timer.h"

typedef struct {
    plc_task_fn  fn;
    void        *arg;
    uint32_t     every;      /* run every 'every' cycles */
    const char  *name;
} plc_task_t;

static plc_task_t  g_tasks[CFG_MAX_CYCLIC_TASKS];
static uint32_t    g_ntasks;
static uint32_t    g_period_us;
static uint64_t    g_period_ticks;
static plc_io_fn   g_read_inputs;
static plc_io_fn   g_write_outputs;
static plc_stats_t g_stats;

void plc_init(uint32_t period_us, plc_io_fn read_inputs, plc_io_fn write_outputs)
{
    g_ntasks        = 0;
    g_period_us     = period_us;
    g_period_ticks  = timer_us_to_ticks(period_us);
    g_read_inputs   = read_inputs;
    g_write_outputs = write_outputs;

    g_stats.cycles_done      = 0;
    g_stats.overruns         = 0;
    g_stats.last_exec_ticks  = 0;
    g_stats.max_exec_ticks   = 0;
    g_stats.last_jitter_ticks = 0;
    g_stats.max_jitter_ticks  = 0;
}

int plc_register_task(plc_task_fn fn, void *arg, uint32_t every_cycles,
                      const char *name)
{
    if (g_ntasks >= CFG_MAX_CYCLIC_TASKS)
        return -1;
    if (every_cycles == 0)
        every_cycles = 1;

    g_tasks[g_ntasks].fn    = fn;
    g_tasks[g_ntasks].arg   = arg;
    g_tasks[g_ntasks].every = every_cycles;
    g_tasks[g_ntasks].name  = name;
    g_ntasks++;
    return 0;
}

void plc_get_stats(plc_stats_t *out)
{
    if (out) *out = g_stats;
}

/* Runs a complete scan cycle. 'deadline' is the current theoretical tick;
 * it is advanced by g_period_ticks. Returns the new deadline. */
static uint64_t plc_one_cycle(uint64_t deadline)
{
    uint64_t cycle_start = timer_now_ticks();

    /* Jitter = deviation between the actual start and the theoretical tick. */
    uint64_t jitter = (cycle_start > deadline) ? (cycle_start - deadline) : 0;
    g_stats.last_jitter_ticks = jitter;
    if (jitter > g_stats.max_jitter_ticks)
        g_stats.max_jitter_ticks = jitter;

    /* 1. Read inputs (process image). */
    if (g_read_inputs)
        g_read_inputs();

    /* 2. Run the due cyclic tasks (run-to-completion). */
    for (uint32_t i = 0; i < g_ntasks; i++) {
        if ((g_stats.cycles_done % g_tasks[i].every) == 0)
            g_tasks[i].fn(g_tasks[i].arg);
    }

    /* 3. Write outputs. */
    if (g_write_outputs)
        g_write_outputs();

    /* 4. Measure the execution duration. */
    uint64_t exec = timer_now_ticks() - cycle_start;
    g_stats.last_exec_ticks = exec;
    if (exec > g_stats.max_exec_ticks)
        g_stats.max_exec_ticks = exec;

    g_stats.cycles_done++;

    /* Next theoretical tick. */
    deadline += g_period_ticks;

    /* Overrun detection: the work exceeded the period. */
    uint64_t now = timer_now_ticks();
    if (now > deadline) {
        g_stats.overruns++;
        deadline = now;    /* resync to avoid accumulating the lag */
    } else {
        /* 5. Wait for the next tick (deterministic busy-wait). */
        while (timer_now_ticks() < deadline)
            ;
    }
    return deadline;
}

void plc_run(plc_stats_t *stats)
{
    uint64_t deadline = timer_now_ticks();
    for (;;) {
        deadline = plc_one_cycle(deadline);
        if (stats)
            *stats = g_stats;
    }
}

void plc_run_bounded(uint64_t n_cycles, plc_stats_t *stats)
{
    uint64_t deadline = timer_now_ticks();
    for (uint64_t k = 0; k < n_cycles; k++)
        deadline = plc_one_cycle(deadline);
    if (stats)
        *stats = g_stats;
}
