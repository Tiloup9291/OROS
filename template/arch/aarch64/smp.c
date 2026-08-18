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
 * smp.c — Boot SMP through PSCI
 *
 * Wakeup core 1..3 with PSCI CPU_ON (SMC to board ATF, HVC in
 * QEMU). Each secondary core start at 'secondary_entry' (start.S) :
 * switch EL2->EL1, VBAR, clean stack (g_sec_sp[core]), Core's MMU activation
 * in assembler, then secondary_main() (C) where it register "online".
 *
 * Note (board RK3328) :
 *  - Secondary start in EL2 (ATF) -> switch EL2->EL1 needed.
 *  - Each core MMU must be enabled BEFORE first C code (else RAM
 *    "Device" -> C prologue / exclusive spinlock fault).
 *  - g_sec_sp[core] must be PUBLISHED AT POINT OF COHERENCY (dc cvac) by core
 *    0 BEFORE CPU_ON, else secondary read old value (0) -> SP=0
 *    -> Data Abort at 1st stack access.
 *  - DON'T touch CPUECTLR_EL1 (SMPEN) from EL1 : ATF already configured it
 *    and EL1 access is trapped (exception).
 *
 * Independent of Linux.
 */

#include "smp.h"
#include "../../kernel/config.h"
#include "../../kernel/klog.h"
#include "../../kernel/sync.h"
#include "../../kernel/thread.h"
#include "gic.h"
#include "timer.h"
#include "pmu.h"

/* MMU (mmu.c) : L1 table built by core 0 ; each secondary enables it
 * for itself through mmu_enable_cpu() (call in ASM from secondary_entry). */
extern void mmu_enable_cpu(void);

/* Complete EL1 vector table (vectors.S) — shared between all cores. */
extern char vector_table[];

/* Boot barrier : The primary core initializes the partitions BEFORE allowing the
 * secondary cores to start their schedulers (preventing a premature sched_start). */
static volatile uint32_t g_sched_go = 0;
void smp_release_schedulers(void) { g_sched_go = 1; __asm__ volatile("dsb sy; sev"); }


/* ---- PSCI (SMCCC) ---- */
#define PSCI_CPU_ON_AARCH64   0xC4000003u
#define PSCI_SYSTEM_OFF       0x84000008u
#define PSCI_SYSTEM_RESET     0x84000009u



/* Low level entry point for secondary cores (start.S). */
extern void secondary_entry(void);

/* Secondary cores stacks (one by core, core 0 used __stack_top). */
#define SEC_STACK_SIZE  (16u * 1024u)
static uint8_t g_sec_stacks[CFG_NUM_CORES][SEC_STACK_SIZE]
                   __attribute__((aligned(16)));

/* Top of stack address passed to the booting core (read by start.S). */
volatile uint64_t g_sec_sp[CFG_NUM_CORES];

/* Online cores counter (Incremented by each core, protected by a lock). */
static volatile uint32_t g_online = 1;   /* core 0 is already online */
static spinlock_t        g_online_lock = SPINLOCK_INIT;

/* PSCI call. Conduit : SMC (board, ATF EL3) / HVC (QEMU, -DPSCI_HVC). */
static int64_t psci_call(uint64_t fnid, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fnid;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
#if defined(PSCI_HVC)
    __asm__ volatile("hvc #0"
                     : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
#else
    __asm__ volatile("smc #0"
                     : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
#endif
    return (int64_t)x0;
}

int smp_start_core(uint32_t core)
{
    if (core == 0 || core >= CFG_NUM_CORES)
        return -1;

    /* Prepare this core's stack (aligned top). */
    uint64_t sp = (uint64_t)(g_sec_stacks[core] + SEC_STACK_SIZE);
    sp &= ~0xFUL;
    g_sec_sp[core] = sp;

    /* Publish g_sec_sp[core] AT POINT OF COHERENCY before waking up : core 0
     * write in its cache (Normal WB) ; secondary read it very early (before
     * its caches are coherent) and would otherwise see the old value (0)
     * -> SP=0 -> Data Abort. dc cvac + dsb ensure visibility. */
    __asm__ volatile("dc cvac, %0" :: "r"(&g_sec_sp[core]) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");

    /* Target MPIDR : on RK3328, Aff0 = core number (0..3). */
    uint64_t target_mpidr = core;

    /* PSCI CPU_ON : x1=target, x2=entry point (PA, identity-map VA==PA), x3=ctx. */
    int64_t ret = psci_call(PSCI_CPU_ON_AARCH64,
                            target_mpidr,
                            (uint64_t)&secondary_entry,
                            core);
    return (int)ret;   /* 0 = SUCCESS */
}

uint32_t smp_start_all(void)
{
    uint32_t started = 0;
    for (uint32_t c = 1; c < CFG_NUM_CORES; c++) {
        int r = smp_start_core(c);
        if (r == 0)
            started++;
        else
            klog_write_u("SMP: CPU_ON failed core=", c);
    }
    return started;
}

uint32_t smp_online_count(void)
{
    return g_online;
}

void smp_system_off(void)
{
    /* PSCI SYSTEM_OFF : in QEMU (HVC) cleanly shut down the VM ; on hardware
     * (SMC to ATF) cuts power. Never come back. */
    psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
    for (;;)
        __asm__ volatile("wfi");
}

void smp_system_reset(void)
{
    psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
    for (;;)
        __asm__ volatile("wfi");
}



static inline void irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }

/* Called by each secondary core (from start.S).
 * The MMU for THIS core is already enabled (enabled in ASM
 * inside secondary_entry).
 *
 * The secondary core joins the per-partition scheduler :
 *   1. installs the real EL1 vector table (shared) ;
 *   2. configures ITS GIC CPU interface + local SGI/PPI handling ;
 *   3. enables ITS PMU (WCET/jitter measurements) ;
 *   4. start ITS periodic timer (PPI 30, banked per core) ;
 *   5. waits for the GO signal from core 0 then starts sched_start() for ITS partition.
 *
 * The hard-RT cores (0/1) receive NO I/O IRQs (routed to Core 2 through
 * gic_set_target) — Only their scheduling timer is enabled ("quasi tickless").
 */
void secondary_main(void)
{
    uint32_t id = smp_core_id();

    /* Mark itself as "online" (short critical section, SMP spinlock). */
    spin_lock(&g_online_lock);
    g_online++;
    spin_unlock(&g_online_lock);

    /* Heartbeat through the lock-free log (this core's ring buffer, drained by core 0). */
    klog_write_u("core online, id=", id);

    /* 1. Real EL1 vector table (with irq_handler) on THIS core. */
    __asm__ volatile("msr vbar_el1, %0" :: "r"(vector_table));
    __asm__ volatile("isb");

    /* 2. This core's GIC CPU interface + local SGIs (GICC banked per core). */
    gic_init_cpu();

    /* 3. This core's PMU (cycle counter). */
    pmu_init();

    /* 4. This core's scheduling timer (PPI 30, banked). */
    gic_set_priority(TIMER_IRQ_PPI, 0x80);
    gic_enable_irq(TIMER_IRQ_PPI);
    timer_init_periodic(CFG_TICK_HZ);

    /* 5. Wait for authorization from core 0 (partitions ready). */
    while (g_sched_go == 0)
        __asm__ volatile("wfe");

    klog_write_u("sched start, core=", id);

    irq_enable();
    sched_start();   /* Schedule this core's partition ; never come back */

    for (;;)
        __asm__ volatile("wfe");
}

