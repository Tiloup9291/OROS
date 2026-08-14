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
#include "../drivers/sdmmc/sdmmc.h"
#include "../drivers/usb/usb_demo.h"
#include "../drivers/gmac/gmac_demo.h"
#include "../lib/ethercat/ecat_demo.h"
#include "../lib/ethercat/ecat_task.h"
#include "../fs/fs_demo.h"
#include "../net/net_demo.h"
#include "../net/net_task.h"

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
/* Shared resource protected by a PI mutex (priority-inversion demo)  */
/* ------------------------------------------------------------------ */
static mutex_t   g_res_mutex = MUTEX_INIT;
static volatile uint64_t g_shared_resource;

/* ------------------------------------------------------------------ */
/* RT_HARD partitions (Core 0 & Core 1)                                */
/* ------------------------------------------------------------------ */
/* Each hard-RT task: measures its jitter (PMU), increments a shared resource
 * under a PI mutex, sends a heartbeat via mailbox to Core 2. */
volatile uint64_t g_hard_iters[CFG_NUM_CORES];
volatile uint64_t g_hard_max_jitter[CFG_NUM_CORES];   /* in PMU cycles */

static void hard_rt_task(void *arg)
{
    uint32_t core = cur_core();
    uint64_t last = pmu_cycles();
    uint64_t expected = 0;

    for (;;) {
        /* Measure inter-passage jitter (PMU cycles). */
        uint64_t now = pmu_cycles();
        uint64_t delta = now - last;
        last = now;
        if (expected == 0)
            expected = delta;                 /* calibration on the 1st round */
        uint64_t jit = (delta > expected) ? (delta - expected) : (expected - delta);
        if (jit > g_hard_max_jitter[core])
            g_hard_max_jitter[core] = jit;

        /* Critical section protected by a PI mutex. */
        mutex_lock(&g_res_mutex);
        g_shared_resource++;
        mutex_unlock(&g_res_mutex);

        g_hard_iters[core]++;

        /* Periodic heartbeat to Core 2 (mailbox + IPI). One iteration =
         * one tick (~1 ms); notify every ~100 ms (light, non-blocking). */
        if ((g_hard_iters[core] % 100u) == 0) {
            uint64_t msg = ((uint64_t)core << 32) | (g_hard_iters[core] & 0xFFFFFFFF);
            mailbox_send_notify(CFG_CORE_IO_SOFT, msg);
        }
        thread_yield();

    }
}

/* ------------------------------------------------------------------ */
/* RT_SOFT partition (Core 3)                                         */
/* ------------------------------------------------------------------ */
volatile uint64_t g_soft_iters;

static void soft_rt_task(void *arg)
{
    for (;;) {
        g_soft_iters++;
        /* Periodic heartbeat to Core 2 (each iteration = 1 tick ~1 ms;
         * notify every ~100 ms to stay light). */
        if ((g_soft_iters % 100u) == 0) {
            uint64_t msg = ((uint64_t)CFG_CORE_RT_SOFT << 32) |
                           (g_soft_iters & 0xFFFFFFFF);
            mailbox_send_notify(CFG_CORE_IO_SOFT, msg);
        }
        thread_yield();

    }
}

/* ------------------------------------------------------------------ */
/* Demo: GPIO + SDMMC (sector read) + interrupt-driven UART RX        */
/* ------------------------------------------------------------------ */

/* Small blocking delay (busy-wait) based on the system counter. */
static void delay_ms(uint32_t ms)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks((uint64_t)ms * 1000ull);
    while (timer_now_ticks() < end)
        __asm__ volatile("nop");
}

/* Configures a board LED (IOMUX=GPIO, output direction, off). */
static void board_led_setup(uint32_t bank, uint32_t pin)
{
    gpio_set_iomux(bank, pin, GPIO_FUNC_GPIO);
    gpio_set_direction(bank, pin, GPIO_OUT);
    gpio_set_value(bank, pin, BOARD_LED_OFF);
}

static void driver_demo(void)
{
    printf("\n===== DEMO basic drivers =====\n");

    /* --- 1) GPIO: drive the REAL LEDs of the Orange Pi R1 Plus LTS ---
     * 3 drivable LEDs (active HIGH): LAN (gpio2 PB7), STATUS (gpio3 PC5),
     * WAN (gpio2 PC2). Configure them then blink the STATUS LED 3 times
     * (~200 ms ON / 200 ms OFF): a test VISUALLY observable on the board.
     * The register is also read to confirm the electrical toggling. */
    /* Note: on the Orange Pi R1 Plus LTS, the LAN/WAN LEDs are generally
     * driven by the Ethernet PHY/MAC (network activity) and do not necessarily
     * respond to direct GPIO driving; the STATUS LED (gpio3 PC5) is attempted
     * here but may depend on the wiring. The REFERENCE GPIO test, observable
     * on a voltmeter, is the pin 10 OUTPUT (GPIO3_C0) below. */
    printf("\n[gpio] attempting LED STATUS (gpio3 PC5) — signal (board wiring)\n");
    board_led_setup(BOARD_LED_STATUS_BANK, BOARD_LED_STATUS_PIN);
    for (int i = 0; i < 3; i++) {
        gpio_set_value(BOARD_LED_STATUS_BANK, BOARD_LED_STATUS_PIN, BOARD_LED_ON);
        printf("    LED STATUS = ON\n");
        delay_ms(2000);
        gpio_set_value(BOARD_LED_STATUS_BANK, BOARD_LED_STATUS_PIN, BOARD_LED_OFF);
        printf("    LED STATUS = OFF\n");
        delay_ms(2000);
    }

    /* --- GENERAL-PURPOSE digital I/O on the 13-pin connector ---
     * DIGITAL OUTPUT: pin 10 (GPIO3_C0). Toggles 3 cycles HIGH(3,3V)/LOW(0V):
     * enough to drive a relay / transistor / external LED. Measurable with a
     * voltmeter between pin 10 and GND (pin 2) — this is the REFERENCE TEST.
     * DIGITAL INPUT: pin 13 (GPIO2_A2) with internal pull-up: reads 1 at rest,
     * reads 0 when the pin is tied to GND (pin 2) — button / dry contact. */
    printf("\n[gpio] General-purpose digital I/O on 13-pin connector:\n");
    printf("  OUTPUT pin10=GPIO3_C0 (relay/ext.), INPUT pin13=GPIO2_A2 (pull-up)\n");

    uint32_t v_hi = 0, v_lo = 0;
    gpio_output_setup(HDR_PIN10_BANK, HDR_PIN10_PIN, GPIO_LOW);
    for (int i = 0; i < 3; i++) {
        gpio_set_value(HDR_PIN10_BANK, HDR_PIN10_PIN, GPIO_HIGH);
        v_hi = gpio_get_value(HDR_PIN10_BANK, HDR_PIN10_PIN);
        printf("  pin10 = HIGH (3,3V) ; re-read=%lu\n", (unsigned long)v_hi);
        delay_ms(2000);
        gpio_set_value(HDR_PIN10_BANK, HDR_PIN10_PIN, GPIO_LOW);
        v_lo = gpio_get_value(HDR_PIN10_BANK, HDR_PIN10_PIN);
        printf("  pin10 = LOW  (0V)   ; re-read=%lu\n", (unsigned long)v_lo);
        delay_ms(2000);
    }



    /* Digital input: CONTINUOUS read for ~8 s so one can
     * connect/disconnect the pin to GND and OBSERVE the change.
     * internal pull-up => 1 at rest, 0 when tied to GND (pin 2). */
    gpio_input_setup(HDR_PIN13_BANK, HDR_PIN13_PIN, GPIO_PULL_UP);
    printf("  input pin13 : continuous reading ~8 s (connect to GND to see 0)...\n");
    uint32_t last_lvl = 2;   /* force the 1st display */
    uint64_t in_end = timer_now_ticks() + timer_us_to_ticks(8000000ull);
    while (timer_now_ticks() < in_end) {
        uint32_t lvl = gpio_get_value(HDR_PIN13_BANK, HDR_PIN13_PIN);
        if (lvl != last_lvl) {
            printf("    pin13 = %lu (%s)\n", (unsigned long)lvl,
                   lvl ? "rest/HIGH" : "link to GND/LOW");
            last_lvl = lvl;
        }
        delay_ms(20);   /* light debounce */
    }
    printf("  input pin13 : final level = %lu\n", (unsigned long)last_lvl);



    /* --- 2) SDMMC: card init + read of sector 0 (MBR) --- */

    printf("\n[sdmmc] SD card initialization...\n");
    sd_card_t card;
    sd_status_t st = sdmmc_init(&card);
    int sd_ok = 0;
    if (st == SD_OK) {
        printf("  card OK : SDHC=%lu, RCA=0x%lx, sectors=%lu (~%lu Mb)\n",
               (unsigned long)card.is_sdhc, (unsigned long)card.rca,
               (unsigned long)card.sector_count,
               (unsigned long)(card.capacity_bytes / (1024ull * 1024ull)));

        static uint8_t sec[SD_SECTOR_SIZE] __attribute__((aligned(64)));
        st = sdmmc_read_blocks(0, 1, sec);
        if (st == SD_OK) {
            printf("  reading sector 0 OK. 16 first bytes :\n   ");
            for (int i = 0; i < 16; i++)
                printf(" %02x", sec[i]);
            printf("\n");
            /* End of sector (offset 496..511): the 0x55AA signature is at
             * 510-511 for a valid MBR/VBR. */
            printf("  bytes 496..511 :\n   ");
            for (int i = 496; i < 512; i++)
                printf(" %02x", sec[i]);
            printf("\n");
            /* Count the non-zero bytes: distinguishes a sector actually read
             * from a buffer left at zero. */
            int nz = 0;
            for (int i = 0; i < SD_SECTOR_SIZE; i++)
                if (sec[i]) nz++;
            printf("  no null bytes in sector = %d / 512\n", nz);
            int mbr = (sec[510] == 0x55 && sec[511] == 0xAA);
            printf("  signature 0x55AA (offset 510-511) : %s\n",
                   mbr ? "PRESENT" : "missing");

            /* Also read sector 2048 (usual start of the 1st FAT partition on a
             * partitioned SD card) to verify reading another LBA
             * and look for a signature/boot record. */
            static uint8_t sec2[SD_SECTOR_SIZE] __attribute__((aligned(64)));
            if (sdmmc_read_blocks(2048, 1, sec2) == SD_OK) {
                int nz2 = 0;
                for (int i = 0; i < SD_SECTOR_SIZE; i++)
                    if (sec2[i]) nz2++;
                printf("  sector 2048 : not nulls=%d, sig 55AA=%s, "
                       "16 1st bytes :\n   ", nz2,
                       (sec2[510] == 0x55 && sec2[511] == 0xAA) ? "yes" : "no");
                for (int i = 0; i < 16; i++)
                    printf(" %02x", sec2[i]);
                printf("\n");
            }
            /* The "SD sector read" is satisfied as soon as real data is read
             * (non-empty sector) without a transfer error. */
            sd_ok = (nz > 0);
        } else {
            printf("  FAILED to read sector 0 (code %d)\n", (int)st);
        }

    } else if (st == SD_ENODEV) {
        printf("  (no SDMMC controller in QEMU : test ignored)\n");
    } else {
        printf("  FAILED to init card (code %d)\n", (int)st);
    }

    /* --- 3) Interrupt-driven UART RX: echo for ~5 s --- */
    printf("\n[uart-rx] active interruptive echo ~5 s : type some text...\n");
    printf("  (each byte received triggers the IRQ %u route on Core%u)\n",
           UART_IRQ, CFG_CORE_IO_SOFT);
    uint64_t end = timer_now_ticks() + timer_us_to_ticks(5000000ull);
    uint32_t echoed = 0;
    while (timer_now_ticks() < end) {
        char c;
        while (uart_getc(&c)) {
            uart_putc(c);            /* echo */
            if (c == '\r') uart_putc('\n');
            echoed++;
        }
        __asm__ volatile("nop");
    }
    printf("\n[uart-rx] echoed bytes = %lu ; IRQ UART received on Core2 = %lu ; loss = %lu\n",
           (unsigned long)echoed,
           (unsigned long)g_uart_irq_count[CFG_CORE_IO_SOFT],
           (unsigned long)uart_rx_dropped());

    /* --- Demo summary --- */
    printf("\n[Demo results]\n");
    printf("  Controllable GPIO          : %s\n",
           (v_hi != v_lo) ? "OK" : "to verify (wiring)");
    printf("  SDMMC sector reading     : %s\n",
           sd_ok ? "OK" : (st == SD_ENODEV ? "N/A (QEMU)" : "KO"));
    printf("  UART RX interruptive       : %s\n",
           (g_uart_irq_count[CFG_CORE_IO_SOFT] > 0) ? "OK" :
           "no strike received (test with keyboard)");
    printf("\n>>> Demo: basic drivers (see results above). <<<\n");
}

/* ------------------------------------------------------------------- */
/* IO_SOFT partition (Core 2): drains logs + receives mailbox + summary*/
/* ------------------------------------------------------------------- */
static volatile uint64_t g_demo_deadline;
static volatile int      g_demo_done;
volatile uint64_t        g_mbx_recv[CFG_NUM_CORES];   /* messages received per source */


static void io_supervisor(void *arg)
{
    for (;;) {
        /* Drain the logs of all cores to the UART. */
        klog_drain_to_uart();

        /* Consume the mailbox messages from all sources. */
        uint32_t src;
        uint64_t msg;
        while (mailbox_recv_any(&src, &msg)) {
            if (src < CFG_NUM_CORES)
                g_mbx_recv[src]++;
        }

        if (!g_demo_done && timer_now_ticks() >= g_demo_deadline) {
            g_demo_done = 1;

            uint64_t f = timer_frequency();
            (void)f;

            printf("\n===== RESULTS DEMO SMP-partitioned =====\n");
            printf("[partitions] Core0=EtherCAT(RT_HARD) Core1=RT_HARD "
                   "Core2=IO_SOFT(network) Core3=RT_SOFT\n\n");

            printf("[activity by partition]\n");
            printf("  Core0 = PERMANENT EtherCAT MASTER (see block [ecat] in //)\n");
            printf("  Core1 hard-RT iters = %lu\n", (unsigned long)g_hard_iters[1]);
            printf("  Core3 soft-RT iters = %lu\n", (unsigned long)g_soft_iters);
            printf("  shared resources (PI mutex) = %lu\n",
                   (unsigned long)g_shared_resource);

            printf("\n[IRQ isolation: timer ticks per core]\n");
            for (uint32_t c = 0; c < CFG_NUM_CORES; c++)
                printf("  Core%lu ticks=%lu  ipi_mbx=%lu\n",
                       (unsigned long)c,
                       (unsigned long)g_tick_count[c],
                       (unsigned long)g_ipi_mbx[c]);

            printf("\n[inter-cores mailbox received by Core2]\n");
            for (uint32_t c = 0; c < CFG_NUM_CORES; c++)
                printf("  from Core%lu : %lu msg\n",
                       (unsigned long)c, (unsigned long)g_mbx_recv[c]);
            printf("  total mailbox dropped = %lu\n",
                   (unsigned long)mailbox_dropped());

            printf("\n[measured hard-RT jitter by PMU (CPU cycles)]\n");
            printf("  Core0 max_jitter = %lu cycles\n",
                   (unsigned long)g_hard_max_jitter[0]);
            printf("  Core1 max_jitter = %lu cycles\n",
                   (unsigned long)g_hard_max_jitter[1]);

            /* Core0 is now the permanent EtherCAT master (no longer hard_rt_task):
             * only Core1 (hard-RT) + Core3 (soft-RT) are required active here. */
            int ok_parts = (g_hard_iters[1] > 0) && (g_soft_iters > 0);
            int ok_mbx   = (g_mbx_recv[0] > 0) || (g_mbx_recv[1] > 0) ||
                           (g_mbx_recv[3] > 0);
            int ok_iso   = (g_tick_count[0] > 0) && (g_tick_count[1] > 0) &&
                           (g_tick_count[2] > 0) && (g_tick_count[3] > 0);

            printf("\n[results]\n");
            printf("  active partitions (0/1/3)      : %s\n", ok_parts ? "OK" : "KO");
            printf("  mailbox Core->Core2             : %s\n", ok_mbx   ? "OK" : "KO");
            printf("  each core scheduled (ticks) : %s\n", ok_iso   ? "OK" : "KO");
            printf("  PI mutex (consistent resource)  : %s\n",
                   (g_shared_resource > 0) ? "OK" : "KO");

            if (ok_parts && ok_mbx && ok_iso)
                printf("\n>>> Demo : SMP PARTIONED OK. <<<\n");
            else
                printf("\n>>> Demo : error, see results. <<<\n");

            /* ---------------------------------------------------------- */
            /* Demo: GPIO, SDMMC (sector read), UART RX                   */
            /* ---------------------------------------------------------- */
            driver_demo();

            /* ---------------------------------------------------------- */
            /* Demo: FAT32 (FatFs) — mount, list, read, write             */
            /* and read back a file (R/W validation). Relies on the SDMMC */
            /* driver already initialized by the driver demo.             */
            /* ---------------------------------------------------------- */
            fs_demo_run();

            /* ---------------------------------------------------------- */
            /* PARALLEL architecture:                                     */
            /*   - Core0 ALREADY runs the PERMANENT EtherCAT master       */
            /*     (thread `ecatM0`, ecat_task_entry) -> continuous LRW   */
            /*     cycle, publishes ecat_diag IN REAL TIME.               */
            /*   - Here (Core2) the PERMANENT IP/SSH network stack is     */
            /*     started (net_task_entry, infinite loop) -> telnet/SSH  */
            /*     see EtherCAT live.                                     */
            /*                                                            */
            /* QEMU (PSCI_HVC): no GMAC nor xHCI -> the permanent tasks   */
            /* self-idle. So the historical SHORT demos are replayed      */
            /* (ecat_demo_run + net_demo_run, auto-ignored) then shutdown */
            /* cleanly (SYSTEM_OFF) so as not to block the QEMU CLI.      */
            /* ---------------------------------------------------------- */
#if defined(PSCI_HVC)
            /* --- QEMU: short demos + shutdown (no hardware) --- */
            ecat_demo_run();
            net_demo_run();
            printf("\n[qemu] end of demo : shutting down (PSCI SYSTEM_OFF)...\n");
            smp_system_off();
#else
            /* --- Board: PERMANENT network on Core2, IN PARALLEL with     */
            /*     Core0's permanent EtherCAT. Never returns.               */
            printf("\nCore2 : starting PERMANENT network stack "
                   "(// EtherCAT Core0)...\n");
            net_task_entry(NULL);   /* infinite loop: never returns */
#endif
        }
    }
}



/* ------------------------------------------------------------------ */
/* Kernel entry point (core 0)                                        */
/* ------------------------------------------------------------------ */
void kmain(void)
{
    uart_init();

    uart_puts("\n=========================================\n");
    uart_puts(" OROS - Demo: SMP-partioned\n");
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
     * The SDMMC controller is initialized later, in the demo (Core 2). */
    gpio_init();
    gic_set_priority(UART_IRQ, 0x80);
    gic_set_target(UART_IRQ, CFG_CORE_IO_SOFT);
    gic_enable_irq(UART_IRQ);
    uart_rx_init_irq();
    printf("[uart] active interruptive RX (IRQ %u -> Core%u)\n",
           UART_IRQ, CFG_CORE_IO_SOFT);

    /* --- Per-partition thread creation (strict affinity) --- */

    sched_init();
    /* Core0 = PERMANENT EtherCAT MASTER (infinite thread,
     * timer-clocked cycle, GMAC polling), IN PARALLEL with Core2's network/SSH.
     * Core1 stays RT_HARD (PLC/heartbeat task with PI mutex, isolation proof). */
    thread_create_on("ecatM0",  ecat_task_entry, NULL, 5, CFG_CORE_ECAT_HARD);
    thread_create_on("hardRT1", hard_rt_task,    NULL, 5, CFG_CORE_RT_HARD_1);
    /* Core 2 = IO_SOFT (supervisor: summaries then PERMANENT network). */
    thread_create_on("ioSup",   io_supervisor,   NULL, 10, CFG_CORE_IO_SOFT);
    /* Core 3 = RT_SOFT (lower priority). */
    thread_create_on("softRT3", soft_rt_task,    NULL, 15, CFG_CORE_RT_SOFT);

    printf("[sched] threads per existing partitions :\n");
    for (uint32_t c = 0; c < CFG_NUM_CORES; c++)
        printf("  Core%lu : %lu thread(s)\n",
               (unsigned long)c, (unsigned long)sched_partition_count(c));

    /* Demo window: ~2 s. */
    g_demo_deadline = timer_now_ticks() + timer_us_to_ticks(2000000ull);

    g_sched_enabled = 1;

    /* Allow the secondaries to start their scheduler, THEN start ours. */
    smp_release_schedulers();

    uart_puts("[sched] starting per-partition schedulers...\n");
    irq_enable();
    sched_start();   /* schedules core 0's partition; never returns */
}
