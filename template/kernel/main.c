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
 * main.c — Kernel (RK3328): partitioned SMP + real-time primitives.
 *
 * Builds with MMU, GIC, timer, IRQ, preemptive
 * scheduler, PLC, lock-free logging and waking the 4 cores via PSCI.
 *
 * Features:
 *   - PER-PARTITION scheduler with strict affinity (one run-queue per core):
 *       Core 0/1 = RT_HARD, Core 2 = IO_SOFT (drains logs + receives mailbox),
 *       Core 3 = RT_SOFT.
 *   - Routing of I/O IRQs to Core 2 (gic_set_target): the hard-RT cores only
 *     receive THEIR scheduling timer (isolation verified by per-core IRQ
 *     counters).
 *   - Lock-free inter-core mailbox + notification IPI (SGI).
 *   - Priority-inheritance (PI) mutex + semaphores.
 *   - PMU: jitter/latency measurement of the hard-RT cores.
 *
 * It runs in EL1. Independent of Linux.
 */

#include <stdio.h>
#include <stdint.h>

#include "config.h"
#include "thread.h"
#include "klog.h"
#include "mailbox.h"
#include "mutex.h"
#include "../drivers/uart/uart.h"
#include "../drivers/gpio/gpio.h"
#include "../lib/ethercat/ecat_task.h"
#include "../app/app.h"

#include "../arch/aarch64/timer.h"

#include "../arch/aarch64/gic.h"
#include "../arch/aarch64/pmu.h"
#include "../arch/aarch64/smp.h"


/* Provided by mmu.c / vectors.S */
extern void mmu_enable(void);
extern char vector_table[];

/* ------------------------------------------------------------------ */
/* Low-level diagnostics                                              */
/* ------------------------------------------------------------------ */
static unsigned long current_el(void)
{
    unsigned long el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 0x3;
}

static void uart_puthex(unsigned long v)
{
    static const char hexd[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        uart_putc(hexd[(v >> i) & 0xF]);
}

static void install_vectors(void)
{
    __asm__ volatile("msr vbar_el1, %0" :: "r"(vector_table));
    __asm__ volatile("isb");
}

void exception_handler(unsigned long index, unsigned long esr,
                       unsigned long elr, unsigned long far)
{
    uart_puts("\n\n*** EXCEPTION (sync) ***\n");
    uart_puts("  vec index = "); uart_puthex(index); uart_puts("\n");
    uart_puts("  ESR       = "); uart_puthex(esr);   uart_puts("\n");
    uart_puts("  ELR       = "); uart_puthex(elr);   uart_puts("\n");
    uart_puts("  FAR       = "); uart_puthex(far);   uart_puts("\n");
    for (;;)
        __asm__ volatile("wfe");
}

static inline uint32_t cur_core(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Interrupts — SMP-aware handler (called by ALL cores)               */
/* ------------------------------------------------------------------ */
/* Per-core counters (IRQ isolation proof). */
volatile uint64_t g_tick_count[CFG_NUM_CORES];
volatile uint64_t g_ipi_mbx[CFG_NUM_CORES];
volatile uint64_t g_ipi_resched[CFG_NUM_CORES];
volatile uint64_t g_uart_irq_count[CFG_NUM_CORES];   /* UART RX IRQ per core */
static volatile int g_sched_enabled;


uint64_t irq_handler(uint64_t sp_saved)
{
    uint32_t core  = cur_core();
    uint32_t intid = gic_acknowledge();
    uint64_t sp_next = sp_saved;

    if (intid == TIMER_IRQ_PPI) {
        g_tick_count[core]++;
        timer_ack_and_reload();
        if (g_sched_enabled)
            sp_next = sched_on_tick(sp_saved);
    } else if (intid == IPI_MAILBOX) {
        /* "Mailbox not empty" notification: the receiver drains in its thread. */
        g_ipi_mbx[core]++;
    } else if (intid == IPI_RESCHED) {
        g_ipi_resched[core]++;
        if (g_sched_enabled)
            sp_next = sched_on_tick(sp_saved);
    } else if (intid == UART_IRQ) {
        /* Interrupt-driven RX: flush the FIFO into the software ring. */
        g_uart_irq_count[core]++;
        uart_rx_isr();
    }


    if (intid != GIC_SPURIOUS)
        gic_end_of_interrupt(intid);

    return sp_next;
}

static inline void irq_enable(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }

/* ------------------------------------------------------------------ */
/* Production application partitions                                  */
/* ------------------------------------------------------------------ */
/* The 4 partition loops live in app/ and lib/ethercat/ :
 *
 *   Core0 (RT_HARD) : ecat_task_entry()  — permanent EtherCAT master,
 *                     calls app_ecat_cycle() every cycle.
 *   Core1 (RT_HARD) : app_core1_entry()  — PLC scan engine (plc_run),
 *                     calls app_read_inputs/app_control/app_write_outputs.
 *   Core2 (IO_SOFT) : app_core2_entry()  — drivers_init() then the permanent
 *                     services loop (network/shell/keyboard/logs).
 *   Core3 (RT_SOFT) : app_core3_entry()  — soft periodic loop,
 *                     calls app_soft_periodic().
 *
 * There is NO demo window and no verdict any more: every loop is permanent.
 * The PI mutex, the mailbox and klog remain available to the application
 * (see app/app.h and API.md).
 */


/* ------------------------------------------------------------------ */
/* Kernel entry point (core 0)                                        */
/* ------------------------------------------------------------------ */
void kmain(void)
{
    uart_init();

    uart_puts("\n=========================================\n");
    uart_puts(" OROS - production (SMP partitioned)\n");
    uart_puts("=========================================\n");
    uart_puts("[boot] uart ok\n");

    uart_puts("[boot] CurrentEL = EL");
    uart_putc('0' + (char)current_el());
    uart_puts("\n");

    mmu_enable();
    uart_puts("[boot] MMU enabled\n");

    install_vectors();
    uart_puts("[boot] vector installed (VBAR)\n");

    klog_init();
    mailbox_init();
    printf("[boot] klog + mailbox ok\n");

    /* --- GIC + PMU + timer of core 0 --- */
    gic_init();
    pmu_init();
    gic_set_priority(TIMER_IRQ_PPI, 0x80);
    gic_enable_irq(TIMER_IRQ_PPI);

    printf("[timer] frequency = %lu Hz\n", (unsigned long)timer_frequency());
    timer_init_periodic(CFG_TICK_HZ);

    /* --- SMP wake-up of the 4 cores --- */
    uart_puts("[smp] waking-up secondary cores (PSCI CPU_ON)...\n");
    uint32_t started = smp_start_all();
    printf("[smp] CPU_ON signal OK for %lu core(s)\n",
           (unsigned long)started);

    /* Let the cores register + initialize (GIC/timer/PMU). */
    {
        uint64_t wait_until = timer_now_ticks() + timer_us_to_ticks(300000ull);
        while (timer_now_ticks() < wait_until)
            __asm__ volatile("nop");
    }
    klog_drain_to_uart();
    printf("[smp] online core = %lu / %u\n",
           (unsigned long)smp_online_count(), CFG_NUM_CORES);

    /* --- Routing of I/O IRQs to Core 2 (hard-RT isolation) ---
     * The future I/O SPIs (USB/GMAC/UART) will all be targeted at
     * Core 2. Here the SPI space is routed to Core 2 to prove the mechanism:
     * the hard-RT cores (0/1) will ONLY receive their timer (PPI, banked). */
    for (uint32_t intid = 32; intid < 224; intid++)
        gic_set_target(intid, CFG_CORE_IO_SOFT);
    printf("[irq] SPI I/O routed to Core%u (hard-RT isolation)\n",
           CFG_CORE_IO_SOFT);

    /* --- Basic drivers: GPIO + interrupt-driven UART RX ---
     * The UART SPI (INTID 89) is already routed to Core 2 by the loop
     * above. It is enabled in the distributor + the hardware RX is armed.
     * The other drivers (SDMMC/FAT, USB-A keyboard, USB-Ethernet) are brought
     * up by drivers_init() / net_task on Core 2 (see app/app_core2.c). */
    gpio_init();
    gic_set_priority(UART_IRQ, 0x80);
    gic_set_target(UART_IRQ, CFG_CORE_IO_SOFT);
    gic_enable_irq(UART_IRQ);
    uart_rx_init_irq();
    printf("[uart] active interruptive RX (IRQ %u -> Core%u)\n",
           UART_IRQ, CFG_CORE_IO_SOFT);

    /* --- Per-partition thread creation (strict affinity) --- */

    sched_init();
    /* Core0 = PERMANENT EtherCAT master (fixed cycle, GMAC polling) — calls
     *         app_ecat_cycle() every cycle. */
    thread_create_on("ecat0", ecat_task_entry,  NULL,  5, CFG_CORE_ECAT_HARD);
    /* Core1 = PLC scan engine (CFG_CYCLE_US) — app_read_inputs/app_control/
     *         app_write_outputs. */
    thread_create_on("plc1",  app_core1_entry,  NULL,  5, CFG_CORE_RT_HARD_1);
    /* Core2 = IO_SOFT supervisor: drivers_init() then the permanent services
     *         loop (lwIP/telnet/SSH, UART shell, USB-A keyboard, klog). */
    thread_create_on("io2",   app_core2_entry,  NULL, 10, CFG_CORE_IO_SOFT);
    /* Core3 = soft periodic loop — app_soft_periodic(). */
    thread_create_on("soft3", app_core3_entry,  NULL, 15, CFG_CORE_RT_SOFT);

    printf("[sched] threads per partition :\n");
    for (uint32_t c = 0; c < CFG_NUM_CORES; c++)
        printf("  Core%lu : %lu thread(s)\n",
               (unsigned long)c, (unsigned long)sched_partition_count(c));

    g_sched_enabled = 1;

    /* Allow the secondaries to start their scheduler, THEN start ours. */
    smp_release_schedulers();

    uart_puts("[sched] starting per-partition schedulers...\n");
    irq_enable();
    sched_start();   /* schedules core 0's partition; never returns */
}
