# OROS
## OROS' Real-time Operating System (OROS is Real-time Operating System)

**A hard real-time, preemptive, bare-metal AArch64 open-source operating system for ARMv8-A / Rockchip RK3328.**
**Initial design done for Orange Pi R1 Plus LTS**

OROS is a from-scratch, hard real-time operating system targeting the ARMv8-A (AArch64) architecture. It runs entirely in **EL1**, is **completely independent of Linux** (no glibc, no `linux/*.h`, no Linux syscalls), and is built with a bare-metal GCC toolchain and newlib. The system implements a **critically-partitioned SMP** model across the four Cortex-A53 cores, a **PLC-style cyclic execution engine** for hard real-time tasks, and a full industrial feature set including an **EtherCAT master**, **TCP/IP + SSH**, **FAT32**, and **USB host** support.

---

## Table of Contents

- [Overview](#overview)
- [Key Features](#key-features)
- [Architecture](#architecture)
- [Hardware Target](#hardware-target)
- [Getting Started](#getting-started)
- [Building](#building)
- [Running on QEMU](#running-on-qemu)
- [Running on Hardware](#running-on-hardware)
- [Documentation](#documentation)
- [Project Status roadmap](#project-status-roadmap)
- [License](#license)

---

## Overview

OROS is a hard real-time operating system designed for deterministic, industrial-grade control applications. It is written in C (C11), compiled with GCC, and runs bare-metal on the Rockchip RK3328 SoC. The design philosophy is **"zero Linux"**: the entire kernel, drivers, and services are implemented from scratch, with only newlib providing the standard C library (linked through custom bare-metal stubs).

The system is organized around a **critically-partitioned SMP** architecture: each of the four Cortex-A53 cores is statically assigned a scheduling partition with a fixed criticality class, strict affinity, and no migration. This isolates hard real-time workloads from I/O and best-effort traffic, guaranteeing bounded worst-case execution times (WCET).

The actual state feature set includes a **home-grown EtherCAT master** (API-compatible with EtherLab IgH 1.6.8), a **TCP/IP stack (lwIP)** with **SSH (wolfSSH/wolfSSL)**, a **unified shell** available over UART, telnet, and SSH, **FAT32** file system support, and a full **USB host** stack (xHCI/EHCI/OHCI) with USB-Ethernet and HID keyboard drivers.

---

## Key Features

- **Hard real-time, preemptive, bare-metal AArch64 (ARMv8-A)** running in EL1.
- **"Zero Linux"** — no glibc, no `linux/*.h`, no Linux syscalls; only custom newlib stubs.
- **Critically-partitioned SMP**: 4 cores, each with a dedicated scheduling partition and strict affinity:
  - **Core 0** — `RT_HARD`: dedicated **EtherCAT master** (permanent, polling, quasi-tickless).
  - **Core 1** — `RT_HARD`: critical PLC tasks (WCET guaranteed).
  - **Core 2** — `IO_SOFT`: USB, USB-Ethernet, lwIP, SSH, shell, logs.
  - **Core 3** — `RT_SOFT`: soft real-time periodic tasks.
- **PLC-style cyclic execution engine**: fixed-period scan cycle, run-to-completion, overrun detection.
- **Lock-free inter-core logging**: per-core ring buffers drained by Core 2.
- **EtherCAT master** (home-grown, `ecrt_*` API compatible with IgH 1.6.8): ESM INIT→PREOP→SAFEOP→OP, cyclic PDO (LRW), SII/EEPROM SyncManager configuration.
- **Two separate network planes**:
  - **Native GMAC** (DWMAC 1000 + Motorcomm YT8531C PHY) → dedicated **EtherCAT L2** (raw, EtherType 0x88A4).
  - **USB-Ethernet RTL8153B** (USB 2.0) → **TCP/IP + SSH** (lwIP + wolfSSH).
- **Unified shell**: same command interpreter over **UART console**, **telnet:23**, and **SSH:22**.
- **USB host stack**: xHCI/DWC3 (USB3), EHCI/OHCI (USB2), RTL8153B USB-Ethernet, HID keyboard.
- **FAT32** file system (FatFs R0.15) on micro-SD (DesignWare MSHC).
- **GPIO**, **UART** (interrupt-driven RX), **SDMMC** drivers.
- **PMU-based WCET instrumentation**.
- **Static task loading**: adding a task = write the routine + recompile.

---

## Architecture

The software is organized in layers:

```
┌──────────────────────────────────────────────────────────────┐
│  Application tasks (statically compiled into the image)      │
├──────────────────────────────────────────────────────────────┤
│  Services: Shell CLI (UART/TCP/SSH) │ SSH server │ FAT32     │
│            │ EtherCAT master │ network apps                  │
├───────────────┬───────────────┬──────────────┬───────────────┤
│  TCP/IP (lwIP)│  FS (FatFs)   │  EtherCAT     │  USB HID     │
├───────────────┴───────────────┴──────────────┴───────────────┤
│  Drivers: GMAC │ SDMMC │ GPIO │ UART │ USB host (xHCI/EHCI/  │
│            OHCI) + RTL8153B                                  │
├──────────────────────────────────────────────────────────────┤
│  RTOS kernel: per-partition scheduler, threads, mailbox,     │
│               spinlocks, mutex (PI), semaphores, PLC, klog   │
├──────────────────────────────────────────────────────────────┤
│  AArch64 HAL: boot (start.S), MMU, caches, GIC-400,          │
│                exceptions/vectors, generic timer, context    │
│                switch, PMU, SMP boot (PSCI)                  │
├──────────────────────────────────────────────────────────────┤
│  Newlib + bare-metal stubs (_sbrk, _write→UART, ...)         │
└──────────────────────────────────────────────────────────────┘
```

### Execution Model

- **Per-core run queues** with fixed priorities (0 = highest) and round-robin at equal priority.
- **Preemption** on timer tick or IPI reschedule; `thread_yield()` for voluntary yield.
- **Hard-RT cores (0/1)** follow a **PLC scan cycle** at a fixed period: read inputs → execute logic → write outputs → wait for next tick. Run-to-completion within a cycle, with overrun detection.
- **Core 0** runs the permanent EtherCAT master cycle (Generic Timer, absolute cadence, GMAC polling with RX IRQ disabled) — the most deterministic configuration.
- **Core 2** runs the I/O loop: lwIP polling, telnet:23, SSH:22, UART console, and log draining.
- **Inter-core communication**: lock-free SPSC mailboxes + IPI notification, shared `ecat_diag` snapshot, priority-inheritance mutexes.

### Memory Model

- **MMU**: identity mapping (VA==PA). RAM in Normal, Inner-Shareable, Write-Back; MMIO in Device-nGnRE.
- **Cache coherence**: lock-free structures use Inner-Shareable memory + barriers; DMA buffers use explicit `dc cvac`/`dc ivac`; secondary-core stacks published at the Point of Coherency before PSCI CPU_ON.

---

## Hardware Target

| Component | Detail |
|-----------|--------|
| **SoC** | Rockchip RK3328 |
| **CPU** | 4× ARM Cortex-A53 (ARMv8-A, AArch64), ~600 MHz (measured/calibrated) |
| **Interrupt controller** | GIC-400 |
| **Timer** | ARM Generic Timer (CNTP) |
| **PMU** | Performance Monitoring Unit (cycle counter) |
| **RAM** | 1 GiB DDR4 |
| **Board** | Orange Pi R1 Plus LTS |
| **Network port 1** | Native GMAC (Synopsys DWMAC 1000) + Motorcomm YT8531C PHY (rgmii-id) → **EtherCAT master** |
| **Network port 2** | USB-Ethernet RTL8153B (USB 2.0) → **TCP/IP + SSH** |
| **USB** | xHCI/DWC3 USB3 (RTL8153B), EHCI/OHCI USB2 (HID keyboard) |
| **Storage** | micro-SD (DesignWare MSHC) + FAT32 |
| **Console** | UART (DesignWare 8250) |
| **Boot** | Rockchip ROM → SPL/DDR → U-Boot → OS image from SD |

---

## Getting Started

### Prerequisites

- **Arm GNU Toolchain** `arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf` (bare-metal + newlib, prefix `aarch64-none-elf-`). **Not** `aarch64-linux-gnu`.
  - Download: <https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf.tar.xz>
  - Install (e.g. Fedora):
    ```bash
    cd ~/Downloads
    tar -xf arm-gnu-toolchain-*-aarch64-none-elf.tar.xz
    sudo mv arm-gnu-toolchain-*-aarch64-none-elf /opt/aarch64-none-elf
    echo 'export PATH=/opt/aarch64-none-elf/bin:$PATH' >> ~/.bashrc
    source ~/.bashrc
    aarch64-none-elf-gcc --version
    ```
- **GNU Make** (build orchestration).
- **Optional**: `qemu-system-aarch64` (QEMU testing), `u-boot-tools` (`mkimage` for U-Boot image).

### Upstream Components

The following upstream components are **vendored in the repository** (`.git` removed) — no network access is needed at build time:

| Component | Version | License | Role |
|-----------|---------|---------|------|
| newlib | (from toolchain) | BSD-like | Bare-metal libc |
| FatFs (ChaN) | R0.15 | BSD-like | FAT32 file system |
| lwIP | 2.2.1 | BSD | TCP/IP stack (NO_SYS=1) |
| wolfSSL / wolfCrypt | 5.9.2 | GPLv2 / commercial | SSH crypto |
| wolfSSH | 1.5.0 | GPLv2 / commercial | SSH server |
| EtherCAT master | home-grown (`ecrt_*` API, IgH 1.6.8 compatible) | GPLv3 | EtherCAT master (ESM/PDO) |

> **License note**: the linked image embeds GPLv2 sources (wolfSSL/wolfSSH). For a proprietary product, a commercial license from wolfSSL (and IgH) is required, or these components must be isolated. lwIP and FatFs are BSD (no copyleft constraint). U-Boot (GPL) remains an external bootloader and does not affect the OS license.

---

## Building

```bash
make            # build for the board (UART DW8250, GIC RK3328, PSCI SMC/ATF)
make qemu       # build + run in QEMU virt (PL011, GIC QEMU, PSCI HVC)
make uimage     # build a U-Boot image (uImage)
make clean      # clean build/
```

If your toolchain has a different prefix:

```bash
make CROSS=aarch64-elf-            # or
make CROSS=aarch64-linux-gnu-
```

Outputs: `build/kernel.elf`, `build/kernel.bin`, `build/kernel.map`.

---

## Running on QEMU

```bash
make qemu
```

Expected console output (summary):

```
 OROS - Phase 1 : kernel + RT
[boot] EL2, MMU on, vecteurs on
[timer] frequency = ... Hz
[testA] OK : 5 ticks received
[testB] cycles=2000 overruns=0
[testC] starting preemptive scheduler...
[ts c0] thread A count=...
[ts c0] thread B count=...
```

> QEMU uses a PL011 UART (`-DUART_PL011`) and RAM at `0x40000000`. The MMIO drivers for RK3328 hardware (GPIO/SDMMC/USB/GMAC/EtherCAT/network) are neutralized under `-DMMU_QEMU` since the hardware is absent. QEMU boots in EL1 and the demo auto-shuts down via PSCI SYSTEM_OFF.

---

## Running on Hardware

### 1. Connect the serial console

- USB-UART 3.3 V adapter on the board's serial header (UART2).
- On the PC:
  ```bash
  sudo dnf install -y tio       # or 'picocom' / 'screen'
  tio /dev/ttyUSB0 -b 112500   # RK3328 U-Boot: often 1500000 baud by default, change to 112500 if adapter cannot manage high baud
  ```

### 2. Copy the binary to the SD card

The SD card must already have a working **U-Boot** (standard Armbian/official image/or follow my repo: u-boot-aarch64-none-elf). Copy `build/kernel.bin` to the boot partition (usually FAT):

```bash
cp build/kernel.bin /run/media/$USER/<boot-partition>/
sync
```

### 3. Load and run from U-Boot

Interrupt autoboot (any key), then at the `=>` prompt:

```
fatload mmc 1:1 0x00200000 kernel.bin
go 0x00200000
```

> `go` jumps directly to the raw binary. If `go` fails (CPU state), use the U-Boot image variant below.

### 4. U-Boot image variant (`bootm`)

```bash
make uimage      # -> build/uImage
```

Copy `build/uImage` to the SD, then in U-Boot:

```
fatload mmc 1:1 0x00200000 uImage
bootm 0x00200000
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Architecture document: layered view, boot pipeline, execution/scheduling model, memory model, diagrams. |
| [`API.md`](API.md) | Programming reference: public functions, signatures, call constraints, config macros, "add a task" procedure. |

---

## Project Status roadmap

All phases **0 → 8** are **validated on hardware** (board). The actual initial build is complete.
Order of development phases done and tested.

| Phase | Description                                                   | Status                       |
|-------|---------------------------------------------------------------|----------------------------- |
|   0   | Environment & bring-up (UART, newlib)                         | ✅ Validated (QEMU + board) |
|   1   | Single-core kernel (MMU, GIC, timer, scheduler, PLC, logging) | ✅ Validated (QEMU + board) |
|   2   | SMP 4-core partitioned + RT primitives                        | ✅ Validated (QEMU + board) |
|   3   | GPIO, UART RX, SDMMC                                          | ✅ Validated (board)        |
|   4   | FAT32 (FatFs)                                                 | ✅ Validated (board)        |
|   5   | USB host (xHCI/EHCI/OHCI) + RTL8153B + HID keyboard           | ✅ Validated (board)        |
|   6   | GMAC L2 + EtherCAT master                                     | ✅ Validated (board)        |
|   7   | lwIP + TCP shell + SSH                                        | ✅ Validated (board)        |
|   8   | Unified shell + WCET campaign                                 | ✅ Validated (board)        |

### WCET Highlights

The WCET campaign measured the EtherCAT cycle on **Core 0** under various load scenarios on **Core 2** (idle, ping flood, telnet, SSH, combined). CPU calibrated at **600 MHz**.

| EtherCAT period | Max processing (cycle load)    | Overruns    | Wake-up jitter        |
|-----------------|--------------------------------|-------------|-----------------------|
| 1000 µs         | 27.8 → 32.6 µs (2.7–3.2 %)     |    **0**    | **100 % in [0-1) µs** |
| 500 µs          | 32.3 µs (6.4 %)                |    **0**    | **100 % in [0-1) µs** |
| 250 µs          | 29.9 → 33.2 µs (11.9–13.2 %)   |    **0**    | **100 % in [0-1) µs** |
| 100 µs          | 29.9 µs (29.8 %)               | 1 (warm-up) | **100 % in [0-1) µs** |

**Isolation proven**: across all periods and all load scenarios, the EtherCAT cycle wake-up jitter on Core 0 remains **100 % within [0-1) µs** — the network/USB/SSH load on Core 2 does **not** perturb the determinism of Core 0. The minimum sustained tested period is **100 µs (10 kHz)**.

---

## License

The OROS kernel, HAL, drivers, and services are original work. The linked image embeds **GPLv2** components (wolfSSL/wolfSSH, and the EtherCAT master is a home-grown reimplementation inspired by EtherLab IgH, also GPLv2). For a proprietary product, a commercial license from wolfSSL and IgH is required, or these components must be isolated. lwIP and FatFs are BSD-licensed (no copyleft constraint). U-Boot (GPL) remains an external bootloader and does not affect the OS license.
