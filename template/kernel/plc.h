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
 * plc.h — Cyclic execution engine, programmable-logic-controller style
 *
 * PLC model: scan cycle at fixed period T_cycle:
 *   READ INPUTS -> RUN LOGIC -> WRITE OUTPUTS -> WAIT next tick.
 * Run-to-completion (no preemption between tasks of a cycle) -> determinism.
 */
#ifndef RTOS_PLC_H
#define RTOS_PLC_H

#include <stdint.h>
#include "config.h"

/* Optional hooks of a cycle (may be NULL). */
typedef void (*plc_io_fn)(void);        /* read inputs / write outputs */

/* A cyclic task routine. */
typedef void (*plc_task_fn)(void *arg);

/* Execution statistics of the engine (updated each cycle). */
typedef struct {
    uint64_t cycles_done;        /* number of executed cycles */
    uint64_t overruns;           /* number of T_cycle overruns */
    uint64_t last_exec_ticks;    /* last cycle execution duration (ticks) */
    uint64_t max_exec_ticks;     /* worst observed execution duration (ticks) */
    uint64_t last_jitter_ticks;  /* deviation from the theoretical tick (ticks) */
    uint64_t max_jitter_ticks;   /* worst observed jitter (ticks) */
} plc_stats_t;

/* Initializes the engine with a period (µs) and the I/O hooks. */
void plc_init(uint32_t period_us, plc_io_fn read_inputs, plc_io_fn write_outputs);

/* Registers a cyclic task (period = 'every' cycles). Returns 0 on success. */
int plc_register_task(plc_task_fn fn, void *arg, uint32_t every_cycles,
                      const char *name);

/* Runs the cyclic engine indefinitely (scan loop). Never returns.
 * 'stats' (if not NULL) is updated each cycle. */
void plc_run(plc_stats_t *stats) __attribute__((noreturn));

/* Bounded variant (test/bench): runs exactly 'n_cycles' cycles then
 * returns. Useful to validate jitter/overrun without an infinite loop. */
void plc_run_bounded(uint64_t n_cycles, plc_stats_t *stats);

/* Access to the latest stats (copy). */
void plc_get_stats(plc_stats_t *out);

#endif /* RTOS_PLC_H */
