# API.md — OROS Programming Reference

> ***Hard real-time baremetal AArch64 RTOS with partitioned mixed-criticality execution — Orange Pi R1 Plus LTS / Rockchip RK3328.**
> Reference manual for the kernel and HAL **public modules**. For each module:
> public functions (signature, role, parameters, return value, calling
> constraints, side effects), short examples, and — at the end of the document — the full
> table of **configuration macros** (`kernel/config.h`) and **build defines**, plus the
> "add a task" procedure.
>
> **Consistency:** this document reflects the **Phase 8** code (last board-validated
> phase). See `ARCHITECTURE.md` for the overall operation.

---

## Table of Contents

1. [General Conventions](#1-general-conventions)
2. [Threads & per-partition scheduler — `kernel/thread.h`](#2-threads--per-partition-scheduler--kernelthreadh)
3. [Lock-free inter-core mailbox — `kernel/mailbox.h`](#3-lock-free-inter-core-mailbox--kernelmailboxh)
4. [Priority inheritance mutex & semaphores — `kernel/mutex.h`](#4-priority-inheritance-mutex--semaphores--kernelmutexh)
5. [SMP spinlock — `kernel/sync.h`](#5-smp-spinlock--kernelsynch)
6. [Lock-free logging (klog) — `kernel/klog.h`](#6-lock-free-logging-klog--kernelklogh)
7. [Cyclic PLC engine — `kernel/plc.h`](#7-cyclic-plc-engine--kernelplch)
8. [Generic Timer — `arch/aarch64/timer.h`](#8-generic-timer--archaarch64timerh)
9. [GIC-400 — `arch/aarch64/gic.h`](#9-gic-400--archaarch64gich)
10. [PMU (cycle counter) — `arch/aarch64/pmu.h`](#10-pmu-cycle-counter--archaarch64pmuh)
11. [SMP (core boot) — `arch/aarch64/smp.h`](#11-smp-core-boot--archaarch64smph)
12. [UART (console + interrupt-driven RX) — `drivers/uart/uart.h`](#12-uart-console--interrupt-driven-rx--driversuartuarth)
13. [Configuration macros table — `kernel/config.h`](#13-configuration-macros-kernelconfigh)
14. [Build defines](#14-build-defines)
15. [Adding a task](#15-adding-a-task)
16. [Real-Time Safety Rules](#16-real-time-safety-rules)
17. [Timing Guarantees](#17-timing-guarantees)

---

## 1. General Conventions

- **Language / ABI:** C11, `aarch64-none-elf-gcc`, freestanding, newlib. No Linux
  dependency.
- **Exception level:** the entire kernel runs at **EL1** (U-Boot/ATF passes
  control at EL2 → EL2→EL1 switch in `start.S`).
- **Core / partition:** the system is **partitioned SMP**. Each core has
  a run-queue and a fixed criticality class:
  | Core | Partition | Role |
  |------|-----------|------|
  | Core 0 | `RT_HARD` | permanent EtherCAT master (`CFG_CORE_ECAT_HARD`) |
  | Core 1 | `RT_HARD` | critical PLC tasks (`CFG_CORE_RT_HARD_1`) |
  | Core 2 | `IO_SOFT` | USB/USB-Eth/lwIP/SSH/shell/logs (`CFG_CORE_IO_SOFT`) |
  | Core 3 | `RT_SOFT` | soft-RT tasks (`CFG_CORE_RT_SOFT`) |
- **Calling constraints — notation used below:**
  - *after MMU*: the function requires an active MMU (Normal Inner-Shareable
    memory for atomics/barriers);
  - *per core*: to be called individually by EACH concerned core;
  - *IRQ-context*: may/must be called from an interrupt handler;
  - *non-blocking*: never suspends the caller;
  - *hard-RT safe*: O(1), lock-free, no allocation → usable on Core 0/1.

---

## 2. Threads & per-partition scheduler — `kernel/thread.h`

**Preemptive fixed-priority** scheduler (0 = highest), **round-robin** at equal
priority, **one run-queue per core**, **strict affinity without migration**.

### Types

```c
typedef void (*thread_entry_t)(void *arg);   /* thread entry point */

typedef enum { THREAD_UNUSED, THREAD_READY, THREAD_RUNNING, THREAD_BLOCKED }
        thread_state_t;
```

### `void sched_init(void)`
Initializes the threads/scheduler subsystem (all partitions). To be called
**exactly once** by core 0, before any thread creation.
*Constraints:* after MMU. *Effects:* zeroes the TCBs and run-queues.

### `int thread_create_on(const char *name, thread_entry_t entry, void *arg, uint32_t priority, uint32_t core)`
Creates a thread **pinned to core `core`** (strict affinity).
- `name`: label (diagnostic, not copied — keep it valid).
- `entry`/`arg`: routine and its argument.
- `priority`: 0..`CFG_NUM_PRIORITIES-1` (0 = highest).
- `core`: 0..`CFG_NUM_CORES-1`.
- **Return value:** the `id` (≥ 0) of the created thread, or **-1** on failure
  (no free TCB).
*Constraints:* call before the target core's `sched_start()`. *Effects:* allocates a
`CFG_THREAD_STACK_SIZE` stack, prepares an initial trap frame.

### `int thread_create(const char *name, thread_entry_t entry, void *arg, uint32_t priority)`
Phase 1 compatibility: equivalent to `thread_create_on(..., core = 0)`.

### `void sched_start(void)` `[noreturn]`
Starts scheduling on the **current core**: switches to the highest-priority
ready thread of ITS partition. **Never returns.** Each core calls
`sched_start()` after registering its threads.
*Constraints:* timer IRQ already armed; per core.

### `void thread_yield(void)`
Voluntarily yields the CPU (triggers a reschedule). *Constraints:* thread
context (not IRQ).

### `uint32_t thread_self(void)`
Returns the `id` of the current thread on the current core.

### `uint64_t sched_on_tick(uint64_t sp_current)`
**Called by the IRQ handler** (timer tick or IPI reschedule): decides on a
context switch on the current core. Receives the saved SP (pointer to the
trap frame), returns the SP to restore (identical if no switch).
*Constraints:* IRQ-context only.

### `uint32_t sched_partition_count(uint32_t core)`
Number of ready/running threads in core `core`'s partition (diagnostic).

### PI mutex support
```c
uint32_t thread_get_priority(uint32_t id);              /* effective priority */
void     thread_set_effective_priority(uint32_t id, uint32_t prio); /* PI boost */
void     thread_restore_priority(uint32_t id);          /* back to base */
uint32_t thread_core_of(uint32_t id);                   /* affinity core */
```
These functions are used **internally by `mutex.c`** (priority inheritance);
rarely called directly.

### Example — creating a task on a partition
```c
static void my_task(void *arg) {
    for (;;) {
        /* ... periodic work ... */
        thread_yield();
    }
}

/* in kmain(), after sched_init() and before sched_start(): */
thread_create_on("myTask", my_task, NULL, /*prio*/ 5, /*core*/ CFG_CORE_RT_SOFT);
```

---

## 3. Lock-free inter-core mailbox — `kernel/mailbox.h`

**SPSC** (single-producer single-consumer) queue per (source → dest) pair, one
message = **64 bits**. O(1), non-blocking operations → **hard-RT safe**. Coherence
is ensured by `dmb ish` barriers (Normal Inner-Shareable memory).

```c
#define MBX_ENTRIES 64u   /* depth per queue (power of 2) */
```

### `void mailbox_init(void)`
Initializes the full mailbox matrix. Once, by core 0, after MMU.

### `int mailbox_send(uint32_t dst_core, uint64_t msg)`
Sends `msg` from the **current core** to `dst_core`. **Return value:** 1 if accepted,
0 if the queue is full (message counted as lost). Non-blocking, hard-RT safe.

### `int mailbox_send_notify(uint32_t dst_core, uint64_t msg)`
Like `mailbox_send`, **plus** an IPI (`SGI IPI_MAILBOX`) to wake up the destination
core.

### `int mailbox_recv(uint32_t src_core, uint64_t *out)`
Removes a message intended for the current core coming from `src_core`. **Return value:**
1 if a message was extracted (`*out` filled), 0 if the queue is empty.

### `int mailbox_recv_any(uint32_t *src_core_out, uint64_t *out)`
Removes a message from any sender (scan of sources). **Return value:** 1 if
extracted, 0 otherwise.

### `uint32_t mailbox_dropped(void)`
Total number of lost messages (overflow), across all queues.

### Example — Core 1 → Core 2 heartbeat
```c
/* producer (Core 1, hard-RT): light, non-blocking */
uint64_t msg = ((uint64_t)core << 32) | (iter & 0xFFFFFFFF);
mailbox_send_notify(CFG_CORE_IO_SOFT, msg);

/* consumer (Core 2): drains all sources */
uint32_t src; uint64_t m;
while (mailbox_recv_any(&src, &m)) { /* process m */ }
```

---

## 4. Priority inheritance mutex & semaphores — `kernel/mutex.h`

### `mutex_t` — priority inheritance (PI) mutex
Avoids unbounded priority inversion: if a high-priority thread waits
for a mutex held by a low-priority thread, the latter is temporarily
**boosted** to the waiting thread's level.

```c
#define MUTEX_NO_OWNER 0xFFFFFFFFu
#define MUTEX_INIT     { SPINLOCK_INIT, MUTEX_NO_OWNER, 0, 0 }
```

| Function | Role |
|----------|------|
| `void mutex_init(mutex_t *m)` | Initializes a mutex. |
| `void mutex_lock(mutex_t *m)` | Takes the mutex (blocking: short active wait + yield). Applies the PI boost. |
| `int  mutex_trylock(mutex_t *m)` | Tries without blocking. **1** if taken, **0** otherwise. |
| `void mutex_unlock(mutex_t *m)` | Releases (restores the owner's base priority). |

*Constraints:* after MMU (internal spinlock). Thread context (not IRQ). Not
recursive.

### `sem_t` — counting semaphore
```c
#define SEM_INIT(n) { SPINLOCK_INIT, (n) }
```

| Function | Role |
|----------|------|
| `void sem_init(sem_t *s, int32_t initial)` | Initializes with `initial` tokens. |
| `void sem_wait(sem_t *s)` | P(): decrements; blocks (active wait + yield) if `count <= 0`. |
| `int  sem_trywait(sem_t *s)` | Tries without blocking. **1** if acquired, **0** otherwise. |
| `void sem_post(sem_t *s)` | V(): increments. |
| `int32_t sem_value(sem_t *s)` | Current value (diagnostic). |

### Example — protecting a shared resource
```c
static mutex_t g_res = MUTEX_INIT;
static volatile uint64_t g_shared;

mutex_lock(&g_res);
g_shared++;
mutex_unlock(&g_res);
```

---

## 5. SMP spinlock — `kernel/sync.h`

ARMv8 spinlock (LDAXR/STXR + acquire/release barriers), safe between cores, **not
recursive**. Requires an active MMU (Normal Inner-Shareable memory).

```c
typedef struct { volatile uint32_t lock; } spinlock_t;
#define SPINLOCK_INIT { 0 }

static inline void spin_init(spinlock_t *s);   /* inline */
void spin_lock(spinlock_t *s);                 /* active wait */
int  spin_trylock(spinlock_t *s);              /* 1 if taken, 0 otherwise */
void spin_unlock(spinlock_t *s);
```

*Note:* the spinlock **does not mask** IRQs; the caller decides. Reserve it for
**very short** sections (determinism).

---

## 6. Lock-free logging (klog) — `kernel/klog.h`

**One ring buffer per core**. Each core writes to ITS OWN ring (O(1), no
lock → hard-RT safe); **Core 2 drains** to the UART. Text formatting happens on
Core 2.

| Function | Role |
|----------|------|
| `void klog_init(void)` | Initializes the rings (all cores). Once, core 0. |
| `void klog_write(const char *msg)` | Writes a short message into the current core's ring. Non-blocking; full ring → `dropped++`. |
| `void klog_write_u(const char *msg, uint64_t value)` | Same + a hexadecimal integer appended (useful in RT without printf). |
| `uint32_t klog_drain_to_uart(void)` | Drains all rings to the UART (called by Core 2). **Return value:** number of messages emitted. |
| `uint32_t klog_dropped(void)` | Total number of lost messages (all cores). |

Config: `CFG_LOG_RING_ENTRIES` (entries/ring, power of 2), `CFG_LOG_MSG_MAXLEN`.

### Example
```c
/* on a hard-RT core (no printf): */
klog_write_u("core online, id=", smp_core_id());
/* on Core 2, periodically: */
klog_drain_to_uart();
```

---

## 7. Cyclic PLC engine — `kernel/plc.h`

Scan cycle at a **fixed period** (automation model, D9):
`READ INPUTS → EXECUTE LOGIC → WRITE OUTPUTS → WAIT FOR top`. Run-to-completion
(no preemption between tasks of a cycle) → determinism, overrun detection.

### Types
```c
typedef void (*plc_io_fn)(void);         /* read/write hooks (may be NULL) */
typedef void (*plc_task_fn)(void *arg);  /* cyclic task routine */

typedef struct {
    uint64_t cycles_done;        /* executed cycles */
    uint64_t overruns;           /* T_cycle overruns */
    uint64_t last_exec_ticks;    /* last cycle duration (ticks) */
    uint64_t max_exec_ticks;     /* worst observed duration */
    uint64_t last_jitter_ticks;  /* deviation from theoretical top */
    uint64_t max_jitter_ticks;   /* worst observed jitter */
} plc_stats_t;
```

### `void plc_init(uint32_t period_us, plc_io_fn read_inputs, plc_io_fn write_outputs)`
Initializes the engine (period in µs + optional I/O hooks).

### `int plc_register_task(plc_task_fn fn, void *arg, uint32_t every_cycles, const char *name)`
Registers a cyclic task executed every `every_cycles` cycles.
**Return value:** 0 on success (limit: `CFG_MAX_CYCLIC_TASKS`).

### `void plc_run(plc_stats_t *stats)` `[noreturn]`
Infinite scan loop. `stats` (if non-NULL) updated on every cycle.

### `void plc_run_bounded(uint64_t n_cycles, plc_stats_t *stats)`
Bounded variant (test/bench): executes `n_cycles` cycles then returns.

### `void plc_get_stats(plc_stats_t *out)`
Copies the latest statistics.

> **Architecture note:** in v1, Core 0 runs its permanent EtherCAT cycle via
> `ecat_task_entry` (direct Generic Timer cadence, `CFG_ECAT_CYCLE_US`); the
> `plc.c` engine serves as a generic cyclic engine (Core 1 / bench).

---

## 8. Generic Timer — `arch/aarch64/timer.h`

Time base (CNTP EL1) + scheduling tick. **One comparator per core** (PPI 30).

| Function | Role |
|----------|------|
| `uint64_t timer_frequency(void)` | Counter frequency (`CNTFRQ_EL0`), in Hz. |
| `uint64_t timer_now_ticks(void)` | Current physical counter (`CNTPCT_EL0`) — high-resolution timestamp. |
| `uint64_t timer_ticks_to_us(uint64_t ticks)` | Ticks → µs conversion. |
| `uint64_t timer_us_to_ticks(uint64_t us)` | µs → ticks conversion. |
| `void timer_set_oneshot_us(uint32_t us)` | Programs an IRQ in `us` µs then arms it. |
| `void timer_init_periodic(uint32_t hz)` | Initializes periodic tick at `hz` IRQ/s. *Per core.* The IRQ (PPI 30) must be enabled in the GIC. |
| `void timer_ack_and_reload(void)` | Rearms the next top (to be called in the timer IRQ handler). |

```c
#define TIMER_IRQ_PPI 30u   /* CNTP physical EL1 timer = INTID 30 */
```

### Example — precise busy-wait
```c
uint64_t end = timer_now_ticks() + timer_us_to_ticks(5000000ull); /* 5 s */
while (timer_now_ticks() < end) __asm__ volatile("nop");
```

---

## 9. GIC-400 — `arch/aarch64/gic.h`

Interrupt controller (GICv2). **GICD** shared, **GICC** banked per core.

| Function | Role |
|----------|------|
| `void gic_init(void)` | Initializes distributor (GICD) + CPU interface of core 0. Once. |
| `void gic_init_cpu(void)` | Initializes CPU interface (GICC) + local SGIs/PPIs of the **current core**. *Per secondary core.* |
| `void gic_enable_irq(uint32_t intid)` / `gic_disable_irq(...)` | Enables / disables an IRQ. |
| `void gic_set_priority(uint32_t intid, uint8_t prio)` | Priority (0 = highest). |
| `uint32_t gic_acknowledge(void)` | Reads IAR → returns the current INTID. *IRQ-context.* |
| `void gic_end_of_interrupt(uint32_t intid)` | Signals the end (EOIR). *IRQ-context.* |
| `void gic_send_sgi(uint32_t sgi_id, uint32_t target_core)` | Sends an IPI/SGI to a target core. |
| `void gic_set_target(uint32_t intid, uint32_t target_core)` | Routes an SPI (≥ 32) to a single core (I/O IRQ isolation on Core 2). |

```c
#define IPI_RESCHED   0u    /* SGI: reschedule request */
#define IPI_MAILBOX   1u    /* SGI: "mailbox not empty" */
#define GIC_SPURIOUS  1023u /* no real IRQ */
```

### Example — isolating the UART IRQ on Core 2
```c
gic_set_priority(UART_IRQ, 0x80);
gic_set_target(UART_IRQ, CFG_CORE_IO_SOFT);
gic_enable_irq(UART_IRQ);
```

---

## 10. PMU — `arch/aarch64/pmu.h`

Cycle counter (`PMCCNTR_EL0`) for measuring jitter/WCET. **Each
core has its own PMU** → `pmu_init()` per core.

| Function | Role |
|----------|------|
| `void pmu_init(void)` | Enables the PMU + starts the current core's cycle counter. *Per core.* |
| `uint64_t pmu_cycles(void)` `[inline]` | Current cycle counter (resolution = 1 CPU cycle). |
| `void pmu_reset_cycles(void)` | Resets the counter to 0. |

### Example — measuring jitter
```c
uint64_t t0 = pmu_cycles();
/* ... measured section ... */
uint64_t cycles = pmu_cycles() - t0;  /* convert to ns via the CPU frequency */
```

---

## 11. SMP — `arch/aarch64/smp.h`

Secondary core wakeup via **PSCI CPU_ON** (SMC to the ATF on board, HVC in
QEMU).

| Function | Role |
|----------|------|
| `int smp_start_core(uint32_t core)` | Wakes core 1..3. **Return value:** 0 if PSCI OK, PSCI code (< 0) otherwise. |
| `uint32_t smp_start_all(void)` | Wakes all secondary cores. **Return value:** number of started cores. |
| `uint32_t smp_core_id(void)` `[inline]` | Current core ID (`MPIDR_EL1.Aff0`). |
| `uint32_t smp_online_count(void)` | Number of online cores. |
| `void secondary_main(void)` | C entry point of secondary cores (called from `start.S`). |
| `void smp_release_schedulers(void)` | Allows the secondary cores to launch `sched_start()`. To be called by core 0 after creating the threads. |
| `void smp_system_off(void)` `[noreturn]` | Powers off via PSCI SYSTEM_OFF (QEMU/CI). **Do not call on the board** if the system must stay active. |

> **Critical SMP lessons (STATE.md):** each secondary's MMU is enabled in ASM
> before the first C code; `g_sec_sp[core]` published at the Point of Coherency
> (`dc cvac`) before CPU_ON; do not touch `CPUECTLR_EL1` from EL1 (trapped by
> the ATF).

---

## 12. UART — `drivers/uart/uart.h`

Serial console (DesignWare 8250 on board, PL011 in QEMU). TX (Phase 0) +
interrupt-driven buffered RX (Phase 3, lock-free SPSC ring).

| Function | Role |
|----------|------|
| `void uart_init(void)` | Minimal idempotent init (U-Boot has already set the baud rate). |
| `void uart_putc(char c)` | Emits one byte (blocking if TX FIFO full). |
| `void uart_puts(const char *s)` | Emits a string (`\n` → `\r\n`). |
| `size_t uart_write(const char *buf, size_t len)` | Emits `len` raw bytes (used by the newlib `_write` stub → `printf`). |
| `void uart_rx_init_irq(void)` | Enables interrupt-driven reception (after `gic_enable_irq(UART_IRQ)`). |
| `void uart_rx_isr(void)` | RX ISR: drains the hardware FIFO into the software ring. *IRQ-context.* |
| `uint32_t uart_rx_available(void)` | Bytes available in the RX ring. |
| `int uart_getc(char *c)` | Reads one byte from the ring. **1** + `*c` if available, **0** otherwise. Non-blocking. |
| `uint32_t uart_rx_dropped(void)` | RX bytes lost (full ring) since boot. |

```c
#define UART_IRQ 89u   /* GIC INTID (SPI+32) of the RK3328 UART2 (debug console) */
```

> newlib's `printf`/`puts` go through the `_write` stub → `uart_write` (see
> `lib/newlib_stubs.c`).

---

## 13. Configuration macros (`kernel/config.h`)

| Macro | Default | Range / values | Impact |
|-------|---------|----------------|--------|
| `CFG_CYCLE_US` | `1000u` | > 0 (µs) | **PLC engine** period (Core 1). 1000 = 1 kHz. **Do not** use it for the EtherCAT WCET campaign. |
| `CFG_TICK_HZ` | `1000u` | 100..several kHz | Preemptive scheduling tick frequency (1000 = 1 ms). |
| `CFG_MAX_THREADS` | `32u` | ≥ number of created threads | Max number of static TCBs. |
| `CFG_NUM_PRIORITIES` | `32u` | > max used priority | Number of levels (0 = highest). |
| `CFG_THREAD_STACK_SIZE` | `16 KiB` | multiple of 16, ≥ real need | Stack per thread. |
| `CFG_IDLE_STACK_SIZE` | `4 KiB` | ≥ minimal | Idle task stack. |
| `CFG_MAX_CYCLIC_TASKS` | `16u` | ≥ registered PLC tasks | Cyclic task table. |
| `CFG_LOG_RING_ENTRIES` | `256u` | **power of 2** | Entries per log ring/core. |
| `CFG_LOG_MSG_MAXLEN` | `96u` | > message length | Max length of a text message. |
| `CFG_NUM_CORES` | `4u` | = number of A53 cores | Number of SMP cores. |
| `CFG_CORE_ECAT_HARD` | `0u` | 0..3 | RT_HARD core dedicated to the EtherCAT master. |
| `CFG_CORE_RT_HARD_1` | `1u` | 0..3 | RT_HARD core (critical PLC). |
| `CFG_CORE_IO_SOFT` | `2u` | 0..3 | IO_SOFT core (USB/network/SSH/shell/logs). |
| `CFG_CORE_RT_SOFT` | `3u` | 0..3 | RT_SOFT core (periodic). |
| `CFG_CORE_RT_HARD_0` | = `CFG_CORE_ECAT_HARD` | — | Compatibility alias (Phase ≤ 3 demos). |
| `CFG_ECAT_CYCLE_US` | `1000u` | 100 / 250 / 500 / 1000 | **EtherCAT master cycle period** (Core 0). The only parameter to change for the WCET campaign (`1000u`/`500u`/`250u`/`100u`). |

---

## 14. Build defines

> **Required toolchain:** **Arm GNU Toolchain `arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf`**
> (prefix `aarch64-none-elf-`, bare-metal + newlib). Overridable via
> `make CROSS=<prefix>-`. Full dependency list (toolchain, QEMU/mkimage/U-Boot
> host tools, upstream components FatFs R0.15 / lwIP 2.2.1 / wolfSSL 5.9.2 /
> wolfSSH 1.5.0 with versions & licenses): see `ARCHITECTURE.md` §4.

Passed by the Makefile depending on the target. The `make` target (board)
defines nothing special; `make qemu` defines the QEMU variants.

| Define (Makefile variable) | Effect | Affected files |
|----------------------------|--------|----------------|
| `-DUART_PL011` (`UART_DEFS`) | Selects the PL011 UART (QEMU) instead of the DW8250 (board). | `uart.c` |
| `-DGIC_QEMU` (`GIC_DEFS`) | QEMU `virt` GIC addresses instead of the RK3328. | `gic.c` |
| `-DMMU_QEMU` (`MMU_DEFS`) | QEMU MMU mapping + **disables the RK3328 MMIO drivers** (GPIO/SDMMC/USB/GMAC/EtherCAT/network) that do not exist in QEMU. | `mmu.c`, `start.S`, drivers, `net/*` |
| `-DPSCI_HVC` (`PSCI_DEFS`) | PSCI calls via **HVC** (QEMU) instead of **SMC** (ATF board); enables `SYSTEM_OFF` shutdown at the end of the demo. | `smp.c`, `main.c` |
| `-DWOLFSSL_USER_SETTINGS -DWOLFSSH_USER_SETTINGS` (`WOLF_CFLAGS`) | Loads `lib/wolfssl/user_settings.h` (wolfCrypt-only bare-metal profile). | wolfSSL/wolfSSH, `wolf_port.c`, `ssh_server.c`, `net_task.c` |

**Build commands:**
```
make            # board build (DW8250 UART, RK3328 GIC, PSCI SMC/ATF)
make qemu       # build + launch QEMU virt (PL011, QEMU GIC, PSCI HVC)
make uimage     # U-Boot image (uImage)
make clean      # cleans build/
```

**Board flash / boot (standalone):** copy `build/kernel.bin` to the SD card, then
under U-Boot: `fatload mmc 1:1 0x00200000 kernel.bin` then `go 0x00200000`.

---

## 15. Adding a task

Task loading is **static**: adding a task = writing its routine then
**recompiling** the OS (no dynamic loading).

1. **Write the routine** (`thread_entry_t` signature):
   ```c
   static void my_task(void *arg) {
       for (;;) {
           /* work */
           thread_yield();   /* or wait for an event/mailbox */
       }
   }
   ```
2. **Register it** in `kmain()` (`kernel/main.c`), **between** `sched_init()` and
   `sched_start()`, on the desired partition:
   ```c
   thread_create_on("myTask", my_task, NULL, /*prio*/ 8, /*core*/ CFG_CORE_RT_SOFT);
   ```
   Choose the core according to criticality: `CFG_CORE_RT_HARD_1` (hard-RT),
   `CFG_CORE_RT_SOFT` (periodic soft-RT), `CFG_CORE_IO_SOFT` (I/O/network).
   ⚠️ Avoid Core 0 (`CFG_CORE_ECAT_HARD`), reserved for the EtherCAT master.
3. **Cyclic PLC task** (deterministic variant, run-to-completion): instead of
   `thread_create_on`, use `plc_register_task()` then `plc_run()` on a hard-RT
   core.
4. **Adding a shell command** (UART/telnet/SSH): edit the table in
   `net/net_shell.c` (`net_shell_exec`), then `make`. The command is immediately
   available on all **3 transports**.
5. `make` (or `make qemu`), reflash, test.

> If the task uses a new compilation unit, add it to the `OBJ` list in the
> `Makefile` with its compilation rule.

## 16. Real-Time Safety Rules

|     API     |  RT_HARD  |  RT_SOFT  | IO |
|-------------|-----------|-----------|----|
|   mailbox   |     OK    |     OK    | OK |
|    mutex    | forbidden |     OK    | OK |
|    malloc   | forbidden | forbidden | OK |
|    printf   | forbidden | forbidden | OK |
|     klog    |     OK    |     OK    | OK |
| Direct UART | forbidden | forbidden | OK |

## 17. Timing Guarantees

Scheduler tick:  
1000 Hz

Maximum interrupt latency:  
TBD cycles

Context switch:  
TBD cycles

Mailbox send:  
TBD cycles

klog_write:  
TBD cycles