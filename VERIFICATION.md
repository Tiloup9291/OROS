# Verification — OROS

> **Purpose.** Document the methodology, scenarios, and results of the **WCET
> (Worst-Case Execution Time)** verification for the **EtherCAT**
> hard-RT cycle running permanently on **Core0**, IN PARALLEL with the
> network/USB/SSH load carried by **Core2**.

---

## 0. Procedure

1. **Flash** `build/kernel.bin` to the SD, connect the EtherCAT slave (GMAC)
   and the RTL8153B cable, boot (`fatload mmc 1:1 0x00200000 kernel.bin` + `go`).
2. **Idle run ≥ 60 s** while applying ONE load scenario (see §4).
3. **Open a shell** (choose: UART console, `telnet 192.168.1.50 23`, or
   `ssh oros@192.168.1.50`) and type **`wcet`**. Copy the 6 numbers from the
   scenario's row into the §5 table.
4. **Repeat** for each scenario S0 → S4.
5. **Iterate on the period**: change `CFG_ECAT_CYCLE_US` in
   `kernel/config.h` (e.g. 250), `make`, reflash, redo S0→S4 (§7).

> ⭐ **Measurements are CUMULATIVE since boot** (min/max/histogram
> accumulate).

### Interpreting the TWO numbers that matter

| Reading | Meaning | Good result |
|---|---|---|
| **proc MAX (ns)** + **cycle load (%)** | The CPU time of the EtherCAT processing, worst case. | ≪ period (e.g. < 5 % of 1000 µs). |
| **wakeup jitter MAX (µs)** + **histogram** | The regularity of the cycle wakeup = **the isolation**. | Stays small AND **does not grow** from S0 (idle) to S4 (max load). |

---

## 1. Measurement objective

Design choices imposes a **bounded and proven determinism** for the
EtherCAT cycle. The central question of the verification is:

> **Does the Core2 load (USB, lwIP, telnet, encrypted SSH) disturb the
> determinism of the Core0 EtherCAT cycle?**

The partitioning (strict affinity, no migration, I/O IRQs routed only to
Core2, Core0 nearly tickless, GMAC in polling with RX IRQ disabled) is designed
so that the answer is **NO**. This verification **measures** and **documents** it.

## 2. Two measured indicators (PMU + Generic Timer)

At each EtherCAT cycle, `lib/ethercat/ecat_task.c` measures and publishes into
the shared snapshot `ecat_diag` (read by the shell on Core2):

| Indicator | Definition | Source | What it proves |
|---|---|---|---|
| **(a) Processing time** | Duration of `receive → domain_process → EC_READ/WRITE → domain_queue → send`, between two `PMCCNTR_EL0` reads. | PMU (CPU cycles) | The **actual CPU load** per cycle. Must fit **very comfortably** under the period (WCET margin). |
| **(b) Wakeup jitter** | Deviation between the **theoretical cycle top** (absolute cadence `next += period`) and the **actual instant** of exiting the wait loop. | Generic Timer (CNTPCT, ticks → µs/ns) | **Isolation**: if Core2 disturbed Core0 (bus, cache, IRQ), this jitter would rise. A bounded and stable jitter = effective isolation. |

Statistics kept: **min / average / max** for (a), **average / max + histogram** for (b). An **overrun counter** (delay > 1 full period) completes the picture.

### Warm-up

The **first 1000 cycles** (`WCET_WARMUP`) are **excluded** from the statistics:
the EtherCAT bus establishment (first exchanges, cache filling) produces
extremes not representative of steady state.

### Cycles → nanoseconds conversion

The PMU counts **CPU cycles** (Cortex-A53). On this board **the measured CPU frequency is 600 MHz** (board calibration, see §8). To convert PMU cycles to ns, `ecat_task.c` **calibrates the CPU frequency at startup**: it counts the PMU
cycles elapsed over a **20 ms** window measured by the Generic Timer (whose
frequency is exact), then:

```
cpu_hz = cycles_PMU * 1e6 / window_us
```

`cpu_hz` is published in `ecat_diag` and used by the shell command `wcet` to
display times in ns (with a 0.3–3 GHz guard).

## 3. Reading the results

The verification is **observable in real time remotely**, without interrupting the
EtherCAT cycle:

- **UART (periodic log, ~every 15 s)**:
  ```
  [ecat] t=N s : DI=... DO=... WKC=.. | cycles=... WKC>0=.. overruns=.. | proc max=... ns | wakeup max=.. us
  ```
- **Shell (telnet:23 / SSH:22 / UART console), command `wcet`**: full report
  (processing time min/avg/max in cyc+ns, wakeup jitter avg/max,
  **histogram**, cycle load in %, verdict).

```
oros> wcet
===== WCET VERIFICATION - EtherCAT hard-RT cycle (Core0) =====
cycle period    : 1000 us   (warm-up ignored : 1000 cycles)
samples         : NNNNN measured cycles   |   overruns : 0
CPU frequency   : 600 MHz (PMU calibrated)

-- cycle processing time (CPU load) --
  min :     NNNN cyc  (    NNNN ns)
  avg :     NNNN cyc  (    NNNN ns)
  MAX :     NNNN cyc  (    NNNN ns)  <= processing WCET
  cycle load : N.N % of the period (NNNN ns / 1000000 ns)

-- wakeup jitter (isolation vs Core2 load) --
  avg : NNN ns   |   MAX : NNNN ns (N us)
  histogram (wakeup jitter) :
    [0-1) us     : NNNNN
    [1-2) us     : NN
    ...
    >=100 us     : 0

-- verdict --
  overruns=0 : no period overrun. Processing WCET < period : OK.
```

## 4. Load scenarios (board protocol)

Each scenario is run on the board **in autonomous boot**
(`fatload mmc 1:1 0x00200000 kernel.bin` + `go`), with:
- a **real EtherCAT slave** (16DI/16DO) on the **GMAC** port (RJ45);
- an **RJ45 cable** on the 2nd port (USB-Ethernet **RTL8153B**), PC in the
  same /24 (default `192.168.1.50/24`).

For each scenario: idle run ≥ 60 s, then read `wcet`.

| # | Load scenario on Core2 | How to generate it |
|---|---|---|
| **S0** | **Idle** (reference) — no network traffic. | Cable connected, no open session, no ping. |
| **S1** | **Sustained ping** (ICMP flood). | From the PC: `ping -f 192.168.1.50` (or `ping -i 0.002`). |
| **S2** | **Active telnet shell** — repeated commands. | `telnet 192.168.1.50 23`, loop `ecat` / `wcet` / `stats`. |
| **S3** | **Active encrypted SSH shell** — the heaviest crypto load. | `ssh rtos@192.168.1.50`, loop `ecat` / `wcet`. |
| **S4** | **Maximum combined load** — ping flood + SSH + telnet + traffic. | S1 + S2 + S3 simultaneously; possibly `iperf`/scp if available. |

> **Note.** Raw throughput is capped by USB 2.0 (~300–400 Mbps), enough to
> saturate Core2 in interrupts/packet processing — the goal is not throughput
> but to **stress Core2** to verify the non-disturbance of Core0.

## 5. Results table (filled during board testing)

Calibrated CPU frequency: **600 MHz** (board measured) — Cycle period: **1000 µs**
(`CFG_ECAT_CYCLE_US` default).

| Scenario | Proc avg (ns) | **Proc MAX (ns)** | Cycle load (%) | Wakeup jitter avg (ns) | **Wakeup jitter MAX (µs)** | Overruns |
|---|---|---|---|---|---|---|
| S0 idle           | 25955 | **27773** | 2.7 % | 125* | **< 1** (excluding log artifact) | 0** |
| S1 ping flood     | 25826 | **27773** | 2.7 % | 125* | **< 1** (excluding log artifact) | 0** |
| S2 telnet         | 25821 | **27773** | 2.7 % | 125* | **< 1** (excluding log artifact) | 0** |
| S3 encrypted SSH  | 25828 | **32646** | 3.2 % | 125* | **< 1** (excluding log artifact) | 0** |
| S4 combined max   | 25825 | **32646** | 3.2 % | 125* | **< 1** (excluding log artifact) | 0** |

**Wakeup jitter histogram (scenario S4, worst case)** — copied from the
`wcet` command:

| Bucket | [0-1) | [1-2) | [2-5) | [5-10) | [10-20) | [20-50) | [50-100) | ≥100 |
|---|---|---|---|---|---|---|---|---|
| Occurrences (µs) | 1383982 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

Calibrated CPU frequency: **600 MHz** (board measured) — Cycle period: **500 µs**
(`CFG_ECAT_CYCLE_US` default).

| Scenario | Proc avg (ns) | **Proc MAX (ns)** | Cycle load (%) | Wakeup jitter avg (ns) | **Wakeup jitter MAX (µs)** | Overruns |
|---|---|---|---|---|---|---|
| S0 idle           | 25916 | **32311** | 6.4 % | 125* | **< 1** (excluding log artifact) | 0** |
| S1 ping flood     | 25833 | **32311** | 6.4 % | 125* | **< 1** (excluding log artifact) | 0** |
| S2 telnet         | 25818 | **32311** | 6.4 % | 125* | **< 1** (excluding log artifact) | 0** |
| S3 encrypted SSH  | 25820 | **32311** | 6.4 % | 125* | **< 1** (excluding log artifact) | 0** |
| S4 combined max   | 25833 | **32311** | 6.4 % | 125* | **< 1** (excluding log artifact) | 0** |

**Wakeup jitter histogram (scenario S4, worst case)** — copied from the
`wcet` command:

| Bucket | [0-1) | [1-2) | [2-5) | [5-10) | [10-20) | [20-50) | [50-100) | ≥100 |
|---|---|---|---|---|---|---|---|---|
| Occurrences (µs) | 1852362 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

Calibrated CPU frequency: **600 MHz** (board measured) — Cycle period: **250 µs**
(`CFG_ECAT_CYCLE_US` default).

| Scenario | Proc avg (ns) | **Proc MAX (ns)** | Cycle load (%) | Wakeup jitter avg (ns) | **Wakeup jitter MAX (µs)** | Overruns |
|---|---|---|---|---|---|---|
| S0 idle           | 26033 | **29860** | 11.9 % | 125* | **< 1** (excluding log artifact) | 0** |
| S1 ping flood     | 25908 | **29860** | 11.9 % | 125* | **< 1** (excluding log artifact) | 0** |
| S2 telnet         | 25886 | **29860** | 11.9 % | 125* | **< 1** (excluding log artifact) | 0** |
| S3 encrypted SSH  | 25880 | **33231** | 13.2 % | 125* | **< 1** (excluding log artifact) | 0** |
| S4 combined max   | 25875 | **33231** | 13.2 % | 125* | **< 1** (excluding log artifact) | 0** |

**Wakeup jitter histogram (scenario S4, worst case)** — copied from the
`wcet` command:

| Bucket | [0-1) | [1-2) | [2-5) | [5-10) | [10-20) | [20-50) | [50-100) | ≥100 |
|---|---|---|---|---|---|---|---|---|
| Occurrences (µs) | 3881519 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

Calibrated CPU frequency: **600 MHz** (board measured) — Cycle period: **100 µs**
(`CFG_ECAT_CYCLE_US` default).

| Scenario | Proc avg (ns) | **Proc MAX (ns)** | Cycle load (%) | Wakeup jitter avg (ns) | **Wakeup jitter MAX (µs)** | Overruns |
|---|---|---|---|---|---|---|
| S0 idle           | 26050 | **29884** | 29.8 % | 125* | **< 1** (excluding log artifact) | 1** |
| S1 ping flood     | 25884 | **29884** | 29.8 % | 125* | **< 1** (excluding log artifact) | 1** |
| S2 telnet         | 25865 | **29884** | 29.8 % | 125* | **< 1** (excluding log artifact) | 1** |
| S3 encrypted SSH  | 25854 | **29884** | 29.8 % | 125* | **< 1** (excluding log artifact) | 1** |
| S4 combined max   | 25859 | **29884** | 29.8 % | 125* | **< 1** (excluding log artifact) | 1** |

**Wakeup jitter histogram (scenario S4, worst case)** — copied from the
`wcet` command:

| Bucket | [0-1) | [1-2) | [2-5) | [5-10) | [10-20) | [20-50) | [50-100) | ≥100 |
|---|---|---|---|---|---|---|---|---|
| Occurrences (µs) | 9096813 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

## 6. Acceptance criteria

1. **Overruns = 0** (no cycle overflows the period — excluding log artifact);
2. **Processing WCET ≪ period** — on this board, ~29 µs for a period of
   1000 µs = **2.8 %** (huge margin, already achieved in S0);
3. **Wakeup jitter bounded and STABLE from one scenario to another** — no
   significant increase between S0 (idle) and S4 (max load): this is the
   **proof of Core0/Core2 isolation**;
4. Results **recorded** in the §5 table (measured + documented indicator).

## 7. Lowering the period

1. Edit **`kernel/config.h`**:
   ```c
   #define CFG_ECAT_CYCLE_US       250u   /* instead of CFG_CYCLE_US (=1000) */
   ```
   (Current line: `#define CFG_ECAT_CYCLE_US CFG_CYCLE_US`. Replace the value
   with `250u`, `500u`, etc.)
2. `make` → reflash `build/kernel.bin`.
3. Redo the S0→S4 verification. The `wcet` command automatically displays the
   **new period** and recomputes the **cycle load in %**.

**What we're looking for:** the lowest period that still holds **overruns = 0**
with a comfortable margin. Estimate at 600 MHz: proc max ≈ 29 µs, so:
- at **250 µs** → load ≈ **11.6 %** (large margin, should pass);
- at **100 µs** → load ≈ **29 %** (still OK if the jitter stays bounded);
- below that, watch the overruns and the wakeup jitter.

> ⚠️ When lowering the period, the **processing** WCET (~29 µs) does not
> change, but it occupies a **larger fraction** of the period → the margin
> decreases. The wakeup jitter, on the other hand, must stay in the same order
> of magnitude (it depends on isolation, not on the period).