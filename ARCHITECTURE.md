# ARCHITECTURE.md — OROS Architecture

> **Hard real-time baremetal AArch64 RTOS with partitioned mixed-criticality execution — Orange Pi R1 Plus LTS / Rockchip RK3328.**
> Architecture document: operating type, layered view, file map, **boot pipeline &
> activation order** (core 0 and secondaries), execution/scheduling model, memory
> model, ASCII diagrams, and appendix critical lessons.
>
> **Consistency:** reflects the code of **Phase 8** (last board-validated phase).
> Complement: `API.md` (function reference).

---

## Table of contents

1. [Operating type](#1-operating-type)
2. [Layered view](#2-layered-view)
3. [File map](#3-file-map)  
3bis. [Dependencies & build environment](#3bis-dependencies--build-environment)
4. [Hardware target & topology](#4-hardware-target--topology)
5. [Boot pipeline & activation order](#5-boot-pipeline--activation-order)
6. [Execution & scheduling model](#6-execution--scheduling-model)
7. [Memory model & cache coherence](#7-memory-model--cache-coherence)
8. [Diagrams](#8-diagrams)
9. [Appendix: critical lessons](#9-appendix-critical-lessons)

---

## 1. Operating type

- **Hard real-time baremetal AArch64 (ARMv8-A)**, preemptive, running in **EL1**.
- **"Zero Linux"** (NFR2): no dependency on glibc, `linux/*.h`, or Linux syscalls. Only
  **custom newlib stubs** (`lib/newlib_stubs.c`) connect the libc to the hardware
  (`_sbrk`, `_write`→UART, etc.).
- **SMP partitioned by criticality**: 4 Cortex-A53 cores, each with a
  dedicated scheduling partition (run-queue + fixed affinity), isolating hard-RT
  workloads from I/O workloads.
- **PLC execution model** on hard-RT cores: fixed-period scan cycle,
  run-to-completion, overrun detection → minimal jitter, bounded WCET.
- **Lock-free inter-core logging**: one ring per core, drained by Core 2.
- **Static task loading**: adding a task = recompiling.
- **Current features**: GPIO, UART (console + RX IRQ), SDMMC + FAT32 (FatFs), USB host
  (xHCI/EHCI/OHCI) + USB-Ethernet RTL8153B + HID keyboard, GMAC + **modified EtherLab-compatible EtherCAT master** (slave in OP, cyclic PDO), **lwIP IP stack** + **SSH (wolfSSH)**,
  **unified shell** UART/telnet/SSH, **WCET results**.
- Hard real-time guarantees apply to statically configured isolated partitions.
  Networking, filesystem, USB and interactive services execute outside the hard
  real-time budget.

---

## 2. Layered view

```
┌──────────────────────────────────────────────────────────────┐
│  Application tasks (statically compiled into the image)      │
├──────────────────────────────────────────────────────────────┤
│  Services: Shell CLI (UART/TCP/SSH) │ SSH server │ FAT32     │
│            │ EtherCAT master │ network apps                  │
├───────────────┬───────────────┬──────────────┬───────────────┤
│  TCP/IP (lwIP)│  FS (FatFs)   │  EtherCAT    │  USB HID      │
├───────────────┴───────────────┴──────────────┴───────────────┤
│  Drivers: GMAC │ SDMMC │ GPIO │ UART │ USB host (xHCI/EHCI/  │
│            OHCI) + RTL8153B                                  │
├──────────────────────────────────────────────────────────────┤
│  RTOS kernel: per-partition scheduler, threads, mailbox,     │
│               spinlocks, mutex(PI), semaphores, PLC, klog    │
├──────────────────────────────────────────────────────────────┤
│  AArch64 HAL: boot (start.S), MMU, caches, GIC-400,          │
│                exceptions/vectors, generic timer, context    │
│                switch, PMU, SMP boot (PSCI)                  │
├──────────────────────────────────────────────────────────────┤
│  Newlib + bare-metal stubs (_sbrk, _write→UART, ...)         │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. File map

| Directory / file | Role |
|----------------------|------|
| `linker/rk3328.ld` | Linker script (regions, sections, stack, heap). |
| `arch/aarch64/start.S` | Core 0 boot + `secondary_entry` (EL2→EL1, stack, BSS, FP/SIMD, MMU ASM). |
| `arch/aarch64/vectors.S` | EL1 vector table; stacks the trap frame → `irq_handler`/`exception_handler`. |
| `arch/aarch64/context.S` | Context save/restore (thread switching). |
| `arch/aarch64/mmu.c` | Translation tables, MMU/cache enable (`mmu_enable`, `mmu_enable_cpu`). |
| `arch/aarch64/gic.c/.h` | GIC-400 (distributor + CPU interface, SGI/IPI, SPI routing). |
| `arch/aarch64/timer.c/.h` | Generic Timer (time base + scheduling tick). |
| `arch/aarch64/pmu.c/.h` | PMU (cycle counter, jitter/WCET). |
| `arch/aarch64/smp.c/.h` | SMP wake-up via PSCI, `secondary_main`. |
| `kernel/config.h` | Compile-time configuration (`CFG_*` macros). |
| `kernel/sched.c` + `thread.h` | Per-partition scheduler, TCB, strict affinity. |
| `kernel/sync.c/.h` | SMP spinlocks. |
| `kernel/mutex.c/.h` | PI mutex + semaphores. |
| `kernel/mailbox.c/.h` | Lock-free inter-core queues (SPSC). |
| `kernel/klog.c/.h` | Lock-free per-core logging. |
| `kernel/plc.c/.h` | Cyclic PLC engine. |
| `kernel/main.c` | `kmain` (core 0 boot) + partition threads + demos. |
| `drivers/uart|gpio|sdmmc|usb|gmac/` | Hardware drivers. |
| `lib/newlib_stubs.c` | Newlib stubs (libc ↔ hardware). |
| `lib/fatfs/` | FatFs (BSD) + `diskio` port → SDMMC. |
| `lib/lwip/` | lwIP 2.2.1 (BSD, NO_SYS=1) + `sys_arch`/`arch/cc.h` port. |
| `lib/wolfssl/` + `lib/wolfssh/` | wolfCrypt + wolfSSH (GPLv3); `user_settings.h`. |
| `lib/ethercat/` | Modified derivative work based on selected Etherlab EtherCAT Master 1.6.8 components (`ecrt_*`, `ec_datagram`, `ec_master`, `ecat_task`). |
| `net/` | RTL8153B netif, shell (net_shell), telnet (tcp_shell), SSH (ssh_server), UART shell, EtherCAT diag, permanent network task. |
| `fs/` | FAT32 demo. |

---

## 3bis. Dependencies & build environment

### Toolchain (mandatory)
- **Arm GNU Toolchain `arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf`**
  (prefix `aarch64-none-elf-`, **bare-metal + newlib** — NOT `aarch64-linux-gnu`).
  - Source: <https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf.tar.xz>
  - Typical installation (see `README.md`): extract into `/opt/aarch64-none-elf`,
    then `export PATH=/opt/aarch64-none-elf/bin:$PATH`.
  - Verify: `aarch64-none-elf-gcc --version`.
  - The prefix is overridable: `make CROSS=<prefix>-` (e.g. `aarch64-elf-`,
    `aarch64-linux-gnu-` — works because of `-nostartfiles` + custom newlib stubs,
    no Linux header/syscall used).
  - Components used: `gcc`, `objcopy`, `objdump`, `size` + **newlib** (libc:
    `printf`, `malloc`, `string`, `math`) and `libgcc`.
  - Key options (`Makefile`): `-mcpu=cortex-a53 -ffreestanding -std=c11 -O2 -g`,
    `-fno-stack-protector -fno-common`, link `-nostartfiles -T linker/rk3328.ld`,
    libs `-lc -lm -lgcc`.

### Host tools
| Tool | Role | Mandatory? |
|-------|------|----------------|
| **GNU Make** | build orchestration | yes |
| **qemu-system-aarch64** | off-hardware tests (`make qemu`, `virt` machine, `-cpu cortex-a53 -smp 4`) | optional (recommended) |
| **u-boot-tools** (`mkimage`) | U-Boot image generation (`make uimage`) | optional |
| **U-Boot** (on the board) | pre-loader: `fatload mmc` + `go` (outside OS image) | yes (board) |
| coreutils (`wget`, `tar`, `git`…) | toolchain / upstream repo retrieval | as needed |

> Indicative Fedora install: `sudo dnf install -y make qemu-system-aarch64 uboot-tools`.

### Upstream software bricks (embedded in `lib/`, linked/ported sources)
| Brick | Version | License | Integration | Role |
|--------|---------|---------|-------------|------|
| **newlib** | 4.4.0 provided by the toolchain | BSD-like | via toolchain + stubs `lib/newlib_stubs.c` | bare-metal libc |
| **FatFs (ChaN)** | **R0.15** | BSD-like | linked sources (`lib/fatfs/`) + `diskio.c` port→SDMMC | FAT32 FS |
| **lwIP** | **2.2.1** | BSD | linked sources (`lib/lwip/`, `NO_SYS=1` mode) + `sys_arch`/`arch/cc.h` port | TCP/IP stack |
| **wolfSSL / wolfCrypt** | **5.9.2** | **GPLv3**/commercial | linked sources (`lib/wolfssl/`, wolfCrypt-only `user_settings.h` profile) | SSH crypto |
| **wolfSSH** | **1.5.0** | **GPLv3**/commercial | linked sources (`lib/wolfssh/`) | SSH server |
| **EtherCAT master** | Modified derivative work based on selected Etherlab EtherCAT Master 1.6.8 components (`ecrt_*` API compatible) | GPLv2 derivative | `lib/ethercat/` | EtherCAT master (ESM/PDO) |

> **Ethercat note:** The implementation is API-compatible with selected EtherLab concepts but is not
> a Linux kernel module and does not depend on the EtherLab kernel architecture.

> **License note:** The current build configuration includes **GPLv3** licenses (wolfSSL/wolfSSH) → Distribution of a proprietary
> product requires a license compatibility review and may require commercial licensing or architectural isolation. lwIP/FatFs 
> are BSD (no copyleft constraint). U-Boot (GPL) stays external (pre-loader) and does not affect the OS license.

> **Reproducibility:** upstream sources are **embedded in the repository** (internal
> `.git` removed) → the build only depends on the toolchain + Make (+ optional
> QEMU/mkimage). No network retrieval at build time.

---

## 4. Hardware target & topology

- **SoC RK3328**: 4× Cortex-A53 (~600 MHz measured/calibrated), GIC-400, ARM Generic
  Timer, PMU, 1 GiB DDR4.
- **Two separate network planes**:
  - **Native GMAC** `gmac2io@0xFF540000` (DWMAC 1000) + PHY **YT8531C** (rgmii-id,
    reset gpio1 PC2) → **raw Layer-2 EtherCAT master traffic** (EtherType 0x88A4), driven by
    **Core 0**, in **polling** (GMAC IRQ disabled).
  - **USB-Ethernet RTL8153B** (USB↔GbE bridge on the xHCI/DWC3 USB3) → **IP/SSH
    stack** (lwIP), driven by **Core 2**.
- **USB host**: xHCI/DWC3 `@0xFF600000` (RTL8153B), EHCI `@0xFF5C0000` / OHCI
  `@0xFF5D0000` (Low/Full-Speed HID keyboard on USB-A), DWC2 OTG `@0xFF580000`
  (USB-C power+data).
- **SDMMC** DesignWare MSHC (micro-SD), **UART** DW8250 (console).
- **Boot**: Rockchip ROM → SPL/DDR → **U-Boot** → OS image (from SD).

---

## 5. Boot pipeline & activation order

### 5.1 Core 0 — `_start` (start.S) → `kmain` (kernel/main.c)

**In assembly (`start.S`)**: entry from U-Boot (expected EL2 on the validated board configuration) →
**clean EL2→EL1 switch** → stack setup (`__stack_top`) → **FP/SIMD enable**
(CPACR) → **BSS clear** → jump to C `kmain`.

**In C (`kmain`)** — **exact** activation order:

```
 1. uart_init()                     → serial console operational
 2. (diag CurrentEL)                → confirms EL1
 3. mmu_enable()                    → MMU + caches (Normal Inner-Shareable)
 4. install_vectors()  (VBAR_EL1)   → EL1 vector table
 5. klog_init() ; mailbox_init()    → lock-free log infra + inter-core queues
 6. gic_init()                      → GICD distributor + core 0 CPU interface
    pmu_init()                      → core 0 PMU (cycle counter)
    gic_set_priority/enable(TIMER)  → timer IRQ (PPI 30) armed
    timer_init_periodic(TICK_HZ)    → core 0 scheduling tick
 7. smp_start_all()  (PSCI CPU_ON)  → wake cores 1..3
    (wait ~300 ms + klog drain)     → secondaries register online
 8. gic_set_target(SPI → Core2)     → route I/O IRQs to Core 2 (isolation)
 9. gpio_init() ; UART RX IRQ armed  → base drivers (IRQ 89 routed to Core 2)
10. sched_init()                    → per-partition scheduler
11. thread_create_on(...) × 4       → one thread per partition:
       * Core0 "ecatM0"  = ecat_task_entry  (permanent EtherCAT master)
       * Core1 "hardRT1" = hard_rt_task     (mutex-PI heartbeat, isolation proof)
       * Core2 "ioSup"   = io_supervisor    (P2/P3/P4 reports then net_task), which performs initialization and then enters the permanent net_task_entry() service loop.
       * Core3 "softRT3" = soft_rt_task     (periodic soft-RT)
12. g_sched_enabled = 1
13. smp_release_schedulers()        → allows secondaries to start
14. irq_enable() ; sched_start()    → schedules Core 0; DOES NOT RETURN
```

### 5.2 Secondary cores — `secondary_entry` (start.S) → `secondary_main` (smp.c)

**In assembly (`secondary_entry`)**: entry via PSCI CPU_ON (in EL2) →
**EL2→EL1 switch** → loads the `g_sec_sp[core]` stack (published at the Point of
Coherency by core 0) → **core MMU enable in ASM** (`mmu_enable_cpu`,
BEFORE any C code) → jump to `secondary_main`.

**In C (`secondary_main`)** — per core:

```
 1. register "online" (spinlock g_online_lock, g_online++)
 2. klog_write_u("core online, id=", id)     → proof of life (this core's ring)
 3. VBAR_EL1 ← vector_table                   → EL1 vectors on this core
 4. gic_init_cpu()                            → GIC CPU interface + local SGI/PPI
 5. pmu_init()                                → this core's PMU
 6. timer_init_periodic(TICK_HZ)              → this core's scheduling tick
 7. while (g_sched_go == 0) wfe               → wait for core 0's GO
 8. irq_enable() ; sched_start()              → schedules its partition; DOES NOT RETURN
```

> **Isolation**: hard-RT cores (0/1) receive NO I/O IRQs (routed to Core 2 via
> `gic_set_target`) — only local kernel interrupts (scheduler timer and IPIs) remain enabled →
> isolated from external I/O interrupt activity.

---

## 6. Execution & scheduling model

### 6.1 Partitions & affinity
- **One run-queue per core**; each thread is pinned to a core (`tcb.core`),
  **no migration** → deterministic cache/TLB. No load balancing is performed between cores. Partition assignment is static.
- **Fixed priorities** (0 = highest), **round-robin** at equal priority.
- **Preemption**: on timer tick (`sched_on_tick` in `irq_handler`) or reschedule
  IPI (`IPI_RESCHED`). `thread_yield()` yields voluntarily.

### 6.2 Interrupts (flow)
```
IRQ → vectors.S (stacks trap frame, x0=SP) → irq_handler(sp)
        │
        ├─ INTID = TIMER_IRQ_PPI (30) : tick++ ; timer_ack_and_reload() ;
        │                               sp' = sched_on_tick(sp)  (if enabled)
        ├─ INTID = IPI_MAILBOX  (1)   : mailbox counter (drained in the thread)
        ├─ INTID = IPI_RESCHED  (0)   : sp' = sched_on_tick(sp)
        └─ INTID = UART_IRQ    (board-specific: 89)   : uart_rx_isr() (Core 2)
        gic_end_of_interrupt(INTID) → return sp' → vectors.S restores context
```

### 6.3 Hard-RT cycle (Core 0: EtherCAT master)
`ecat_task_entry` (infinite thread) clocked by the **Generic Timer** in **absolute
cadence** (`next += period_ticks`), period `CFG_ECAT_CYCLE_US`:
```
  wait for top → build LRW frame → TX (GMAC, DMA) → POLLING RX →
  process image (DI/DO) → publish ecat_diag (+ PMU jitter/WCET) → loop
```
GMAC IRQ **disabled** (pure polling) → deterministic jitter.

### 6.4 I/O loop (Core 2)
`net_task_entry` (infinite thread): `klog_drain_to_uart` + RTL8153B RX polling →
lwIP (`ethernet_input`, `sys_check_timeouts`) + telnet:23 / SSH:22 servers +
UART console, all hooked to the **same interpreter** `net_shell_exec()`.

### 6.5 Inter-core communication
- **Lock-free SPSC mailbox** + notification IPI (`IPI_MAILBOX`).
- **EtherCAT diag**: shared `ecat_diag` snapshot (64-byte aligned), published by
  Core 0 each cycle, read by the shell (Core 2) → real-time `ecat`/`wcet` commands.
- **PI mutex**: avoids unbounded priority inversion.

---

## 7. Memory model & cache coherence

- **MMU** (`mmu.c`): identity mapping VA==PA. RAM in **Normal, Inner-Shareable,
  Write-Back** (essential for LDAXR/STXR exclusives between cores and `dmb ish`
  barriers). MMIO in **Device-nGnRE**.
- **QEMU variant** (`-DMMU_QEMU`): mapping adapted to QEMU `virt`; RK3328 MMIO
  drivers are neutralized (hardware absent). QEMU validates the generic AArch64 kernel path but does not emulate the RK3328 peripheral set.
- **Inter-core coherence**:
  - Lock-free structures (mailbox, klog): Inner-Shareable memory + barriers
    → **no** `dc cvac` needed.
  - `g_sec_sp[core]` (secondary stack): published via **`dc cvac` + `dsb`** before
    CPU_ON (the secondary reads it before its caches are coherent).
  - **DMA buffers** (GMAC/USB/SD): aligned, with explicit `dc cvac` (CPU→DMA) /
    `dc ivac` (DMA→CPU), in dedicated zones outside hard-RT caches.

---

## 8. Diagrams

### 8.1 Boot sequence (summary)
```
 U-Boot ──(go)──► _start (EL2)
                    │ EL2→EL1, stack, FP/SIMD, BSS
                    ▼
                  kmain (Core0, EL1)
                    │ uart→mmu→vbar→klog/mailbox→gic/pmu/timer
                    │ smp_start_all() ──► CPU_ON ──► secondary_entry (Core1..3)
                    │                                   │ EL2→EL1, SP, MMU(asm)
                    │                                   ▼
                    │                                 secondary_main
                    │                                   │ vbar, gic_cpu, pmu, timer
                    │                                   │ wait(g_sched_go)
                    │ route IRQ→Core2, drivers, sched_init, 4 threads
                    │ smp_release_schedulers() ─── SEV ─►(wakes the wfe)
                    ▼                                   ▼
                 sched_start (Core0)                 sched_start (Core1..3)
                    │                                   │
              ecatM0 / … permanent              hardRT1 / ioSup / softRT3
```

### 8.2 IRQ flow (see section 6.2)
### 8.3 Inter-core communication
```
 Core0 (EtherCAT) ──ecat_diag snapshot──► Core2 (shell)  [real-time read]
 Core1/Core3      ──mailbox_send_notify─► Core2           [heartbeats + IPI]
 Core2            ──klog_drain_to_uart──► UART            [logs from all cores]
```

---

## 9. Appendix: critical lessons

### Critical lessons
- **SMP**: secondary MMU enabled in ASM before C; `g_sec_sp` published at the PoC
  (`dc cvac`) before CPU_ON; do not touch `CPUECTLR_EL1` from EL1 (ATF).
- **PMU**: `PMCCNTR_EL0` accessible from EL1 on this board (not trapped) → real jitter.
- **Drivers**: offsets/bits taken FROM the u-boot/Linux headers
  (never deduced) — the SDMMC hang came from offsets shifted by +4.
- **GMAC TX (RGMII 1000)**: 3 fixes required — PHY delays only (rgmii-id);
  external RGMII clock source (mac_con1 b10 + soc_con4 b14); **PHY 125 MHz output
  (SYNCE)** unlocks TX.
- **EtherCAT**: SM config read from the **SyncManager category (0x29) of the
  SII/EEPROM** (not the SM registers); SM length = mapped PDO size.
- **SSH**: wolfSSH requires `HAVE_ED25519_KEY_EXPORT` + `WOLFSSL_ED25519_STREAMING_VERIFY`
  for the ed25519 host key; full RX drain + non-blocking TX ring.
- **Isolation**: over 1000/500/250/100 µs × 5 load scenarios, the
  EtherCAT cycle wake-up jitter (Core 0) stays **100% within [0-1) µs** — Core 2
  load does not disturb Core 0. Minimum sustained period = **100 µs (10 kHz)**.
