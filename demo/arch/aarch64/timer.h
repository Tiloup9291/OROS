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
 * timer.h — ARM Generic Timer (CNTP) — time base + tick
 */
#ifndef RTOS_ARCH_TIMER_H
#define RTOS_ARCH_TIMER_H

#include <stdint.h>

/* System counter frequency (CNTFRQ_EL0), in Hz. */
uint64_t timer_frequency(void);

/* Current physical counter (CNTPCT_EL0) — high resolution timestamp. */
uint64_t timer_now_ticks(void);

/* Ticks-to-microseconds conversion. */
uint64_t timer_ticks_to_us(uint64_t ticks);
uint64_t timer_us_to_ticks(uint64_t us);

/* Programs the timer to raise an IRQ in 'us' microseconds then start it. */
void timer_set_oneshot_us(uint32_t us);

/* Initializes the periodic timer to 'hz' interrupts/second.
 * The timer IRQ (PPI 30) must be enabled separately in the GIC. */
void timer_init_periodic(uint32_t hz);

/* Reloads the next periodic tick (to be called from the timer IRQ handler). */
void timer_ack_and_reload(void);

/* EL1 physical timer interrupt number (PPI). */
#define TIMER_IRQ_PPI   30u    /* CNTP (EL1 physical timer) = INTID 30 */

#endif /* RTOS_ARCH_TIMER_H */
