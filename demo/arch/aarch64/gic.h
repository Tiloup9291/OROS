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
 * gic.h — GIC-400 (GICv2) : interrupts controller
 */
#ifndef RTOS_ARCH_GIC_H
#define RTOS_ARCH_GIC_H

#include <stdint.h>

/* Initialize distributor (GICD) and CPU interface (GICC) of core 0. */
void gic_init(void);

/* Initialize CPU interface (GICC) + local SGI/PPI of current
 * core. To call for EACH secondary core (GICC is banked per core). */
void gic_init_cpu(void);


/* Enable / disable an interrupt by its INTID. */
void gic_enable_irq(uint32_t intid);
void gic_disable_irq(uint32_t intid);

/* Set priority (0 = highest) of an interrupt. */
void gic_set_priority(uint32_t intid, uint8_t prio);

/* Acknowledge pending IRQ : read IAR, return INTID. */
uint32_t gic_acknowledge(void);

/* Signal end of processing (EOIR) of the INTID. */
void gic_end_of_interrupt(uint32_t intid);

/* Send a single IPI/SGI to a target core. */
void gic_send_sgi(uint32_t sgi_id, uint32_t target_core);

/* Route a SPI (INTID >= 32) to a single target core.
 * Write corresponding ITARGETSR field (mask 1<<core). Used to
 * isolate I/O IRQ on Core 2 (Hard-RT cores doesn't receive them). */
void gic_set_target(uint32_t intid, uint32_t target_core);

/* IDs of SGIs used as inter-core IPIs. */
#define IPI_RESCHED   0u    /* Reschedule request for target core */
#define IPI_MAILBOX   1u    /* "mailbox not empty" notification */


/* Special INTID : "spurious" (no actual IRQ). */
#define GIC_SPURIOUS   1023u

#endif /* RTOS_ARCH_GIC_H */
