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
 * sdmmc.c — SD/MMC driver (Synopsys DesignWare MSHC / dw_mmc) for RK3328
 *           : SD card init + sector read (PIO, no DMA).
 *
 * APPROACH (canonical dw_mmc sequence, aligned with u-boot/Linux dw_mmc.c):
 *   1. Full controller RESET (CTRL_RESET|FIFO|DMA) + wait.
 *   2. STABLE clock source: force the SDMMC CRU mux onto the 24 MHz OSC
 *      (instead of the GPLL/6 ~82 MHz left by U-Boot), CRU divider = 1.
 *      -> the block's SOURCE clock = 24 MHz, clean and deterministic.
 *   3. dwmmc_set_clock(freq): the controller's INTERNAL divider to get the
 *      card frequency. freq = 24MHz / (2*div).
 *        - IDENTIFICATION: 400 kHz  (div=30: 24e6/(2*30)=400000).
 *        - TRANSFER      : 24 MHz    (div=0 => bypass, or div=1 => 12 MHz).
 *      The update sequence is RELIABLE: CLKENA=0 → update → CLKDIV → update →
 *      CLKENA=1 → update, each update being VERIFIED (START falls back, HLE retry).
 *   4. Default drive/sample phase (0°): at 24 MHz the margin is large, no
 *      fine calibration needed (unlike 82 MHz).
 *   5. Standard SD sequence, responses validated by CONTENT (RESP0), the
 *      RTO/RCRC flags remaining informational.
 *
 * DesignWare MSHC registers (standard dw_mmc offsets):
 *   0x000 CTRL   0x004 PWREN  0x008 CLKDIV  0x00C CLKSRC  0x010 CLKENA
 *   0x014 TMOUT  0x018 CTYPE  0x01C BLKSIZ  0x020 BYTCNT  0x024 INTMASK
 *   0x028 CMDARG 0x02C CMD    0x030 RESP0.. 0x044 MINTSTS 0x048 RINTSTS
 *   0x04C STATUS 0x050 FIFOTH 0x058 CDETECT 0x080 BMOD    (RK3328 FIFO @0x200)
 */

#include "sdmmc.h"
#include <stdio.h>

#if defined(MMU_QEMU)

/* ---- QEMU: no RK3328 controller ---- */
sd_status_t sdmmc_init(sd_card_t *card) { (void)card; return SD_ENODEV; }
sd_status_t sdmmc_read_blocks(uint32_t lba, uint32_t count, void *buf)
{ (void)lba; (void)count; (void)buf; return SD_ENODEV; }
sd_status_t sdmmc_write_blocks(uint32_t lba, uint32_t count, const void *buf)
{ (void)lba; (void)count; (void)buf; return SD_ENODEV; }
int sdmmc_card_present(void) { return 0; }


#else
/* ---- RK3328 (real hardware) ---- */

#include "../../arch/aarch64/timer.h"

#define SDMMC_BASE   0xFF500000UL

/* ============================ CRU (clock) ============================
 * CRU base = 0xFF440000. clksel_con[n] @ 0x100 + n*4.
 * SDMMC = CLKSEL_CON30 (0xFF440178):
 *   mux bits[9:8]: 0=CPLL, 1=GPLL, 2=OSC 24 MHz  (write-mask [31:16])
 *   div bits[7:0]: source divider (written value = div-1)
 * We force OSC 24 MHz, div=1 => block source = 24 MHz (stable).
 *
 * SOFT-RESET of the SDMMC block: CRU softrst_con[n]. On RK3328, the SDMMC
 * controller reset is bit 10 of softrst_con2 (0xFF440300 + 2*4 = 0x308).
 * (Reference: u-boot rk3328 cru: SRST for sdmmc.) One could also settle for the
 * controller's internal CTRL_RESET, more portable; we keep CTRL_RESET as
 * the primary path and the CRU soft-reset as an option (disabled by default). */
#define CRU_BASE            0xFF440000UL
#define CRU_CLKSEL_CON30    (CRU_BASE + 0x100 + 30 * 4)   /* 0xFF440178 */
#define CRU_SDMMC_PLL_SHIFT 8
#define CRU_SDMMC_PLL_MASK  (0x3u << CRU_SDMMC_PLL_SHIFT)
#define CRU_SDMMC_PLL_CPLL  0u
#define CRU_SDMMC_PLL_GPLL  1u
#define CRU_SDMMC_PLL_24M   2u
#define CRU_SDMMC_DIV_SHIFT 0
#define CRU_SDMMC_DIV_MASK  (0xFFu << CRU_SDMMC_DIV_SHIFT)

#define OSC_HZ    24000000u          /* OSC 24 MHz (if mux switchable) */
#define GPLL_HZ   491520000u         /* RK3328 GPLL */

/* Real SOURCE frequency of the block, deduced from CRU_CON30 at run time.
 * (On this board, the SDMMC CRU mux turned out NON-switchable from EL1:
 * CON30 stays 0x105 = GPLL/6 ≈ 82 MHz. So we compute the internal divider
 * from the REAL source, instead of assuming 24 MHz.) */
static uint32_t g_src_hz = GPLL_HZ / 6u;   /* default: ~82 MHz */

static void detect_src_hz(void)
{
    uint32_t con = *(volatile uint32_t *)CRU_CLKSEL_CON30;
    uint32_t mux = (con >> 8) & 0x3u;
    uint32_t div = (con & 0xFFu) + 1u;      /* field = div-1 */
    uint32_t pll;
    switch (mux) {
        case 2:  pll = OSC_HZ;  break;      /* OSC 24 MHz */
        case 1:  pll = GPLL_HZ; break;      /* GPLL */
        default: pll = GPLL_HZ; break;      /* CPLL ≈ close, approximate GPLL */
    }
    if (div == 0) div = 1;
    g_src_hz = pll / div;
}


/* Clock phase: CRU sdmmc_con[0]=drive @0x380, [1]=sample @0x384.
 * bits[1:0] = degree (0=0°,1=90°,2=180°,3=270°), write-mask [31:16].
 * At 24 MHz we stay at 0° (large margin). */
#define CRU_SDMMC_CON0   (CRU_BASE + 0x380)
#define CRU_SDMMC_CON1   (CRU_BASE + 0x384)
#define MMC_DEGREE_0     0x0u
#define MMC_DEGREE_180   0x2u

static inline void cru_set_phase(unsigned long con_reg, uint32_t degree)
{
    *(volatile uint32_t *)con_reg = (0x07FFu << 16) | (degree & 0x3u);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Forces the SDMMC SOURCE onto the 24 MHz OSC, CRU divider = 1. */
static void cru_sdmmc_src_24m(void)
{
    uint32_t val  = (CRU_SDMMC_PLL_24M << CRU_SDMMC_PLL_SHIFT) |
                    (0u << CRU_SDMMC_DIV_SHIFT);          /* div-1 = 0 => /1 */
    uint32_t mask = CRU_SDMMC_PLL_MASK | CRU_SDMMC_DIV_MASK;
    *(volatile uint32_t *)CRU_CLKSEL_CON30 = (mask << 16) | val;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* HARDWARE SOFT-RESET of the SDMMC block via the CRU.
 * RK3328: softrst_con[2] @ 0xFF440300 + 2*4 = 0xFF440308. The SDMMC reset is
 * bit 10 (write-mask [31:16]). We assert then deassert (the block restarts from
 * a clean state — needed because U-Boot leaves DATA_BUSY frozen in STATUS). */
#define CRU_SOFTRST_CON2   (CRU_BASE + 0x300 + 2 * 4)   /* 0xFF440308 */
#define CRU_SRST_SDMMC_BIT 10
static void cru_sdmmc_soft_reset(void)
{
    uint32_t bit = (1u << CRU_SRST_SDMMC_BIT);
    /* Assert reset. */
    *(volatile uint32_t *)CRU_SOFTRST_CON2 = (bit << 16) | bit;
    __asm__ volatile("dsb sy" ::: "memory");
    { uint64_t d = timer_now_ticks() + timer_us_to_ticks(2000);
      while (timer_now_ticks() < d) {} }
    /* Deassert reset. */
    *(volatile uint32_t *)CRU_SOFTRST_CON2 = (bit << 16) | 0u;
    __asm__ volatile("dsb sy" ::: "memory");
    { uint64_t d = timer_now_ticks() + timer_us_to_ticks(2000);
      while (timer_now_ticks() < d) {} }
}


/* ============================ MSHC registers ============================ */
#define REG_CTRL     0x000
#define REG_PWREN    0x004
#define REG_CLKDIV   0x008
#define REG_CLKSRC   0x00C
#define REG_CLKENA   0x010
#define REG_TMOUT    0x014
#define REG_CTYPE    0x018
#define REG_BLKSIZ   0x01C
#define REG_BYTCNT   0x020
#define REG_INTMASK  0x024
#define REG_CMDARG   0x028
#define REG_CMD      0x02C
#define REG_RESP0    0x030
#define REG_RESP1    0x034
#define REG_RESP2    0x038
#define REG_RESP3    0x03C
/* dw_mmc offsets (confirmed u-boot include/dwmmc.h):
 * MINTSTS=0x040 RINTSTS=0x044 STATUS=0x048 FIFOTH=0x04C CDETECT=0x050.
 */
#define REG_MINTSTS  0x040
#define REG_RINTSTS  0x044
#define REG_STATUS   0x048
#define REG_FIFOTH   0x04C
#define REG_CDETECT  0x050
#define REG_BMOD     0x080
#define REG_FIFO     0x200          /* RK3328: FIFO data at 0x200 (DWMCI_DATA) */


/* CTRL bits */
#define CTRL_RESET         (1u << 0)
#define CTRL_FIFO_RESET    (1u << 1)
#define CTRL_DMA_RESET     (1u << 2)
#define CTRL_INT_ENABLE    (1u << 4)
#define CTRL_DMA_ENABLE    (1u << 5)   /* former external DMA */
#define CTRL_USE_IDMAC     (1u << 25)  /* Internal DMA Controller (DMA mode) */


/* CMD bits (dw_mmc) */
#define CMD_START           (1u << 31)
#define CMD_USE_HOLD_REG    (1u << 29)
#define CMD_UPD_CLK         (1u << 21)  /* "update clock registers only" */
#define CMD_SEND_INIT       (1u << 15)  /* init sequence (80 clk) */
#define CMD_PRV_DAT_WAIT    (1u << 13)  /* wait for end of current data */
#define CMD_DATA_EXPECTED   (1u << 9)
#define CMD_WRITE           (1u << 10)
#define CMD_CHECK_CRC       (1u << 8)
#define CMD_RESP_LONG       (1u << 7)
#define CMD_RESP_EXPECT     (1u << 6)

/* CLKENA bits */
#define CLKEN_ENABLE     (1u << 0)
#define CLKEN_LOW_PWR    (1u << 16)

/* STATUS bits */
#define STATUS_DATA_BUSY        (1u << 9)
#define STATUS_FIFO_EMPTY       (1u << 2)
#define STATUS_FIFO_COUNT_SHIFT 17
#define STATUS_FIFO_COUNT_MASK  0x1FFF

/* RINTSTS bits */
#define INT_CMD_DONE     (1u << 2)
#define INT_DATA_OVER    (1u << 3)
#define INT_TXDR         (1u << 4)
#define INT_RXDR         (1u << 5)
#define INT_RCRC         (1u << 6)
#define INT_DCRC         (1u << 7)
#define INT_RTO          (1u << 8)   /* response timeout */
#define INT_DRTO         (1u << 9)   /* data read timeout */
#define INT_HTO          (1u << 10)
#define INT_FRUN         (1u << 11)
#define INT_HLE          (1u << 12)  /* hardware locked error (update clk) */
#define INT_SBE          (1u << 13)
#define INT_EBE          (1u << 15)
#define INT_DATA_ERR (INT_DCRC | INT_DRTO | INT_SBE | INT_EBE | INT_FRUN)

/* SD commands */
#define MMC_CMD0_GO_IDLE       0
#define MMC_CMD2_ALL_SEND_CID  2
#define MMC_CMD3_SEND_RCA      3
#define SD_CMD8_SEND_IF_COND   8
#define MMC_CMD9_SEND_CSD      9
#define MMC_CMD7_SELECT        7
#define MMC_CMD12_STOP         12
#define MMC_CMD16_SET_BLKLEN   16
#define MMC_CMD17_READ_SINGLE  17
#define MMC_CMD18_READ_MULTI   18
#define MMC_CMD24_WRITE_SINGLE 24
#define MMC_CMD25_WRITE_MULTI  25
#define MMC_CMD13_SEND_STATUS  13
#define MMC_CMD55_APP          55
#define SD_ACMD23_SET_WR_BLK   23
#define SD_ACMD41_OP_COND      41


static int      g_present;
static uint32_t g_rca;
static uint32_t g_sdhc;

static inline void w(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(SDMMC_BASE + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static inline uint32_t r(uint32_t off)
{
    return *(volatile uint32_t *)(SDMMC_BASE + off);
}

static void udelay(uint32_t us)
{
    uint64_t end = timer_now_ticks() + timer_us_to_ticks(us);
    while (timer_now_ticks() < end)
        __asm__ volatile("nop");
}

/* Waits until the CIU (Card Interface Unit) is no longer busy (data_busy). */
static int wait_data_idle(uint32_t us)
{
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(us);
    while (r(REG_STATUS) & STATUS_DATA_BUSY) {
        if (timer_now_ticks() >= deadline)
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Controller RESET (dw_mmc: CTRL_RESET|FIFO|DMA + wait)              */
/* ------------------------------------------------------------------ */
static sd_status_t ctrl_reset_all(void)
{
    w(REG_CTRL, CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(200000);
    while (r(REG_CTRL) & (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)) {
        if (timer_now_ticks() >= deadline)
            return SD_ETIMEOUT;
    }
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* update_clock: "CMD_UPDATE_CLK only" — the critical dw_mmc point.    */
/* We send START|UPD_CLK|PRV_DAT_WAIT without index/arg and wait for   */
/* START to fall back. If HLE is raised, we RETRY (up to 10 times).    */
/* ------------------------------------------------------------------ */
static sd_status_t update_clock(void)
{
    for (int retry = 0; retry < 10; retry++) {
        w(REG_CMDARG, 0);
        w(REG_CMD, CMD_START | CMD_UPD_CLK | CMD_PRV_DAT_WAIT);

        uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(100000);
        int timed_out = 0;
        while (r(REG_CMD) & CMD_START) {
            if (timer_now_ticks() >= deadline) { timed_out = 1; break; }
        }

        uint32_t rint = r(REG_RINTSTS);
        if (rint & INT_HLE) {
            /* Hardware Locked Error: clear and retry. */
            w(REG_RINTSTS, INT_HLE);
            continue;
        }
        if (!timed_out)
            return SD_OK;
    }
    return SD_ETIMEOUT;
}

/* ------------------------------------------------------------------ */
/* set_clock: sets the card frequency via the INTERNAL CLKDIV.         */
/* The block source is fixed to OSC 24 MHz (cru_sdmmc_src_24m).         */
/* freq = 24MHz / (2*div) if div>0; div=0 -> bypass (source = 24 MHz). */
/* Sequence: CLKENA=0 -> upd -> CLKSRC=0,CLKDIV -> upd ->         */
/*                     CLKENA=1 -> upd.                                  */
/* ------------------------------------------------------------------ */
static sd_status_t set_clock(uint32_t freq_hz)
{
    uint32_t div;
    uint32_t src = g_src_hz;          /* REAL source (detected from CON30) */

    if (freq_hz == 0 || freq_hz >= src) {
        div = 0;                      /* bypass: freq = source */
    } else {
        /* div = ceil(source / (2*freq)) */
        div = (src + (2u * freq_hz) - 1u) / (2u * freq_hz);
        if (div > 0xFF) div = 0xFF;
    }

    printf("  [sd] set_clock: src=%luHz freq=%luHz -> CLKDIV=%lu (~%luHz)\n",
           (unsigned long)src, (unsigned long)freq_hz, (unsigned long)div,
           (unsigned long)(div ? src / (2u * div) : src));

    wait_data_idle(200000);


    /* 1) Turn off the card clock. */
    w(REG_CLKENA, 0);
    if (update_clock() != SD_OK) return SD_ETIMEOUT;

    /* 2) Program the internal divider. */
    w(REG_CLKSRC, 0);
    w(REG_CLKDIV, div);
    if (update_clock() != SD_OK) return SD_ETIMEOUT;

    /* 3) Re-enable the card clock (low-power OFF to stay active). */
    w(REG_CLKENA, CLKEN_ENABLE);
    if (update_clock() != SD_OK) return SD_ETIMEOUT;

    /* Small stabilization delay. */
    udelay(1000);
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* send_cmd: sends a command and waits for CMD_DONE.                   */
/* Returns SD_OK if CMD_DONE; err_out receives RINTSTS (RTO/RCRC flags).*/
/* ------------------------------------------------------------------ */
static sd_status_t send_cmd(uint32_t idx, uint32_t arg, uint32_t flags,
                            uint32_t *rint_out)
{
    /* Wait until the controller is ready (no data in progress unless wanted). */
    if (!(flags & CMD_DATA_EXPECTED))
        wait_data_idle(200000);

    w(REG_RINTSTS, 0xFFFFFFFF);          /* clear residual statuses */
    w(REG_CMDARG, arg);

    /* Note: we do NOT use CMD_USE_HOLD_REG. The hold register is only required
     * combined with a non-zero phase delaynum; enabled without an adequate
     * phase, it corrupts the response (simultaneous EBE/RCRC/RTO observed on
     * board). Without hold_reg + phase 0°, the response is read directly. */
    uint32_t cmd = CMD_START | (idx & 0x3F) | flags;
    w(REG_CMD, cmd);


    /* Wait until START is consumed. */
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(500000);
    while (r(REG_CMD) & CMD_START) {
        if (timer_now_ticks() >= deadline) {
            if (rint_out) *rint_out = r(REG_RINTSTS);
            return SD_ETIMEOUT;
        }
    }

    /* Wait for CMD_DONE. */
    deadline = timer_now_ticks() + timer_us_to_ticks(500000);
    for (;;) {
        uint32_t rint = r(REG_RINTSTS);
        if (rint & INT_CMD_DONE) {
            if (rint_out) *rint_out = rint;
            /* At 400 kHz, the card
             * responds PERFECTLY (resp0=0x1AA verified over 15/16 phases), BUT
             * the controller raises RTO+RCRC as FALSE POSITIVES. So we rely ONLY
             * on CMD_DONE and let the caller validate RESP0.
             * (RTO/RCRC informational only.) */
            return SD_OK;
        }
        if (timer_now_ticks() >= deadline) {
            if (rint_out) *rint_out = r(REG_RINTSTS);
            return SD_ETIMEOUT;
        }
    }
}



/* Simple version (no flags needed). */
static sd_status_t cmd(uint32_t idx, uint32_t arg, uint32_t flags)
{
    return send_cmd(idx, arg, flags, 0);
}

/* ------------------------------------------------------------------ */
/* Controller block initialization                                     */
/* ------------------------------------------------------------------ */
/* Clock source choice:
 *   0 = KEEP the U-Boot source (do NOT touch the CRU mux) — recommended, since
 *       switching the mux disturbs the CMD line.
 *   1 = force OSC 24 MHz via the CRU mux (use only if 0 fails).
 * In both cases, we go down to 400 kHz via the controller's INTERNAL CLKDIV,
 * with the robust update_clock() sequence (HLE retry)
 */
#ifndef SDMMC_FORCE_24M
#define SDMMC_FORCE_24M 0
#endif

static sd_status_t controller_setup(void)
{
#if SDMMC_FORCE_24M
    cru_sdmmc_src_24m();
#endif
    /* READ the REAL source (CRU mux + div left by U-Boot or forced):
     * it serves as the basis for computing the internal divider in set_clock(). */
    detect_src_hz();
    printf("  [sd] real source = %lu Hz (CON30=0x%08lx, force24M=%d)\n",
           (unsigned long)g_src_hz,
           (unsigned long)(*(volatile uint32_t *)CRU_CLKSEL_CON30),
           SDMMC_FORCE_24M);

    /* HARDWARE SOFT-RESET of the block via the CRU: the STATUS left by U-Boot is
     * FROZEN (0x207f0080, DATA_BUSY + aberrant FIFO_COUNT) and blocks any data
     * transfer. A CRU reset of the controller zeros it. */
    cru_sdmmc_soft_reset();
    printf("  [sd] after CRU soft-reset : STATUS=0x%08lx\n",
           (unsigned long)r(REG_STATUS));

    if (ctrl_reset_all() != SD_OK)
        return SD_ETIMEOUT;

     /* U-Boot leaves CTRL=0x02000000 = bit25 USE_INTERNAL_DMAC ACTIVE: the
     * controller routes data through its internal DMA (descriptors in RAM),
     * NOT through the FIFO. In PIO we therefore read descriptor addresses
     * (0x00213f20…) instead of the data, identical for every LBA.
     * -> We FORCE FIFO/PIO mode: CTRL without USE_IDMAC, and BMOD=0 (IDMAC off). */
    w(REG_BMOD, 0);                                  /* IDMAC disabled */
    w(REG_CTRL, CTRL_INT_ENABLE);                    /* PIO: no USE_IDMAC */

    /* Base config. */
    w(REG_PWREN, 1);               /* card power */
    w(REG_INTMASK, 0);             /* polling: all IRQs masked on the CPU side */
    w(REG_RINTSTS, 0xFFFFFFFF);    /* clear statuses */
    w(REG_TMOUT, 0xFFFFFFFF);      /* data/resp timeouts at max */
    w(REG_CTYPE, 0);               /* 1-bit bus for identification */

    /* FIFO threshold : RX watermark = 8, burst 8, TX watermark. */
    w(REG_FIFOTH, (0x2u << 28) | (0x7u << 16) | 0x8u);

    printf("  [sd] forced PIO mode : CTRL=0x%08lx BMOD=0x%08lx\n",
           (unsigned long)r(REG_CTRL), (unsigned long)r(REG_BMOD));


    /* IDENTIFICATION clock = 400 kHz. */
    if (set_clock(400000u) != SD_OK)
        return SD_ETIMEOUT;

    /* TESTED: TO APPLY AFTER set_clock + update_clock,
     * exactly like the sweep sequence that returned resp0=0x1AA.
     * At 400 kHz: sample=0° gives corrupted resp0 (0x900); sample=90/180/270°
     * gives resp0=0x1AA. We set drive=0°, sample=90° then re-apply the clock
     * (update_clock) to lock the phase. */
    cru_set_phase(CRU_SDMMC_CON0, MMC_DEGREE_0);   /* drive  0°  */
    cru_set_phase(CRU_SDMMC_CON1, 0x1u);           /* sample 90° */
    update_clock();
    udelay(1000);

    /* 74+ init cycles required by the spec after power-up. */
    udelay(2000);
    return SD_OK;
}


/* ------------------------------------------------------------------ */
/* SD card initialization sequence                                     */
/* ------------------------------------------------------------------ */
sd_status_t sdmmc_init(sd_card_t *card)
{
    g_present = 0;

    printf("  [sd] U-Boot state : CTRL=0x%08lx CLKENA=0x%08lx CLKDIV=0x%08lx\n",
           (unsigned long)r(REG_CTRL), (unsigned long)r(REG_CLKENA),
           (unsigned long)r(REG_CLKDIV));
    printf("  [sd] CRU_CON30=0x%08lx STATUS=0x%08lx\n",
           (unsigned long)(*(volatile uint32_t *)CRU_CLKSEL_CON30),
           (unsigned long)r(REG_STATUS));

    /* --- Reset + stable 24 MHz clock + set_clock(400kHz) --- */
    if (controller_setup() != SD_OK) {
        printf("  [sd] FAILED controller init (reset/clock)\n");
        return SD_ETIMEOUT;
    }
    printf("  [sd] controller ready : ID@~400kHz "
           "(CLKDIV=0x%lx CLKENA=0x%lx)\n",
           (unsigned long)r(REG_CLKDIV), (unsigned long)r(REG_CLKENA));


    sd_status_t st;
    uint32_t rint;

    /* --- CMD0: GO_IDLE (with 80-clk init sequence). --- */
    st = send_cmd(MMC_CMD0_GO_IDLE, 0, CMD_SEND_INIT, &rint);
    printf("  [sd] CMD0 st=%d\n", (int)st);
    udelay(2000);

    /* --- CMD8: SEND_IF_COND (SD v2). arg=0x1AA (VHS=2.7-3.6V + pattern). ---
     * The 1st CMD8 after CMD0 often returns a corrupted
     * resp0 (0x900); the FOLLOWING CMD8 return the correct 0x1AA echo. So we
     * REPEAT CMD8 until the echo is obtained (bits 7:0 = 0xAA), validating by
     * CONTENT (the RTO/RCRC flags are false positives). */
    uint32_t r8 = 0;
    int v2 = 0;
    for (int i = 0; i < 20 && !v2; i++) {
        st = send_cmd(SD_CMD8_SEND_IF_COND, 0x1AA,
                      CMD_RESP_EXPECT | CMD_CHECK_CRC, &rint);
        r8 = r(REG_RESP0);
        v2 = ((r8 & 0xFF) == 0xAA);
        if (!v2) udelay(2000);
    }
    printf("  [sd] CMD8 resp0=0x%08lx rint=0x%08lx -> v2=%d\n",
           (unsigned long)r8, (unsigned long)rint, v2);




    /* --- ACMD41: OCR negotiation (until complete power-up). --- */
    uint32_t ocr = 0;
    int tries = 0;
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(2000000);
    for (;;) {
        st = send_cmd(MMC_CMD55_APP, 0, CMD_RESP_EXPECT | CMD_CHECK_CRC, &rint);
        if (st != SD_OK) {
            printf("  [sd] CMD55 FAILED st=%d rint=0x%08lx (try=%d)\n",
                   (int)st, (unsigned long)rint, tries);
            return SD_ENOCARD;
        }

        uint32_t arg = 0x00FF8000u;          /* voltage window 2.7-3.6V */
        if (v2) arg |= (1u << 30);           /* HCS: accept SDHC/SDXC */
        /* ACMD41: R3 response (OCR), no valid CRC -> do not check CRC. */
        st = send_cmd(SD_ACMD41_OP_COND, arg, CMD_RESP_EXPECT, &rint);
        if (st != SD_OK) {
            printf("  [sd] ACMD41 st=%d rint=0x%08lx (try=%d)\n",
                   (int)st, (unsigned long)rint, tries);
            return SD_ENOCARD;
        }
        ocr = r(REG_RESP0);
        if (ocr & (1u << 31))
            break;                           /* power-up complete */
        tries++;
        if (timer_now_ticks() >= deadline) {
            printf("  [sd] ACMD41 timeout ocr=0x%08lx tries=%d\n",
                   (unsigned long)ocr, tries);
            return SD_ETIMEOUT;
        }
        udelay(10000);                       /* ~10 ms between attempts */
    }
    g_sdhc = (ocr & (1u << 30)) ? 1 : 0;     /* CCS: SDHC/SDXC */
    printf("  [sd] ACMD41 OK ocr=0x%08lx SDHC=%lu tries=%d\n",
           (unsigned long)ocr, (unsigned long)g_sdhc, tries);

    /* --- CMD2: ALL_SEND_CID (long response). --- */
    if (cmd(MMC_CMD2_ALL_SEND_CID, 0,
            CMD_RESP_EXPECT | CMD_RESP_LONG | CMD_CHECK_CRC) != SD_OK) {
        printf("  [sd] CMD2 FAILED\n");
        return SD_EIO;
    }

    /* --- CMD3: the card publishes its RCA. --- */
    if (cmd(MMC_CMD3_SEND_RCA, 0, CMD_RESP_EXPECT | CMD_CHECK_CRC) != SD_OK) {
        printf("  [sd] CMD3 FAILED\n");
        return SD_EIO;
    }
    g_rca = r(REG_RESP0) >> 16;
    printf("  [sd] RCA=0x%04lx\n", (unsigned long)g_rca);

    /* --- CMD9: read the CSD (capacity). ---
     * R2 (long) response: the dw_mmc controller places the 120 CSD bits (the
     * [127:8] field, without the CRC/start bit) into RESP0..3:
     *   RESP3 = CSD[127:96], RESP2 = [95:64], RESP1 = [63:32], RESP0 = [31:0].
     * CSD_STRUCTURE = bits [127:126] = (RESP3 >> 30). */
    uint32_t csd[4] = {0, 0, 0, 0};
    if (cmd(MMC_CMD9_SEND_CSD, g_rca << 16,
            CMD_RESP_EXPECT | CMD_RESP_LONG | CMD_CHECK_CRC) == SD_OK) {
        csd[0] = r(REG_RESP0);
        csd[1] = r(REG_RESP1);
        csd[2] = r(REG_RESP2);
        csd[3] = r(REG_RESP3);
    }
    printf("  [sd] CSD = %08lx %08lx %08lx %08lx\n",
           (unsigned long)csd[3], (unsigned long)csd[2],
           (unsigned long)csd[1], (unsigned long)csd[0]);


    /* --- CMD7: select the card ("transfer" state). --- */
    if (cmd(MMC_CMD7_SELECT, g_rca << 16,
            CMD_RESP_EXPECT | CMD_CHECK_CRC) != SD_OK) {
        printf("  [sd] CMD7 FAILED\n");
        return SD_EIO;
    }

    /* --- Transfer clock: we STAY at the identification speed.
     * Switching to 24 MHz failed (update after CMD7 = card busy) AND the data
     * read was unstable at high frequency. At ~400 kHz the PIO read is slow but
     * RELIABLE — sufficient for the demo.
     * The frequency can be raised later once the DMA flow is in place.
     * So we do NOT touch the clock here. */
    printf("  [sd] transfer clock kept (~400 kHz, reliable)\n");


    /* --- CMD16: 512-byte block size (required on SDSC, harmless on SDHC). --- */
    cmd(MMC_CMD16_SET_BLKLEN, SD_SECTOR_SIZE,
        CMD_RESP_EXPECT | CMD_CHECK_CRC);

    /* --- Capacity from the CSD v2 (SDHC): 22-bit C_SIZE. --- */
    uint64_t cap = 0;
    uint32_t csd_struct = (csd[3] >> 30) & 0x3;
    if (csd_struct == 1) {
        uint32_t c_size = ((csd[1] >> 16) & 0xFFFF) | ((csd[2] & 0x3F) << 16);
        cap = (uint64_t)(c_size + 1) * 512ull * 1024ull;
    }

    g_present = 1;
    if (card) {
        card->rca = g_rca;
        card->is_sdhc = g_sdhc;
        card->capacity_bytes = cap;
        card->sector_count = (uint32_t)(cap / SD_SECTOR_SIZE);
    }
    return SD_OK;
}

int sdmmc_card_present(void)
{
    return g_present;
}

/* ------------------------------------------------------------------ */
/* Sector read (PIO)                                                   */
/* ------------------------------------------------------------------ */
sd_status_t sdmmc_read_blocks(uint32_t lba, uint32_t count, void *buf)
{
    if (!g_present)
        return SD_ENOCARD;
    if (count == 0 || buf == 0)
        return SD_EINVAL;

    uint32_t *out = (uint32_t *)buf;
    uint32_t total_bytes = count * SD_SECTOR_SIZE;

    /* Wait until the card/CIU is free (no data in progress). */
    wait_data_idle(500000);

    /* PURGE any FIFO RESIDUE (U-Boot leaves words inside: STATUS showed
     * FIFO_COUNT≈259 at boot -> otherwise we'd re-read these stale data,
     * giving the SAME content for every LBA). Read and discard first,
     * then do a clean FIFO_RESET. */
    {
        uint32_t drained = 0;
        while (!(r(REG_STATUS) & STATUS_FIFO_EMPTY) && drained < 4096) {
            (void)r(REG_FIFO);
            drained++;
        }
    }
    w(REG_CTRL, r(REG_CTRL) | CTRL_FIFO_RESET);
    { uint64_t d = timer_now_ticks() + timer_us_to_ticks(100000);
      while ((r(REG_CTRL) & CTRL_FIFO_RESET) && timer_now_ticks() < d) {} }

    w(REG_RINTSTS, 0xFFFFFFFF);
    w(REG_BLKSIZ, SD_SECTOR_SIZE);
    w(REG_BYTCNT, total_bytes);


    uint32_t addr = g_sdhc ? lba : (lba * SD_SECTOR_SIZE);
    uint32_t cmd_idx = (count > 1) ? MMC_CMD18_READ_MULTI : MMC_CMD17_READ_SINGLE;

    uint32_t flags = CMD_RESP_EXPECT | CMD_CHECK_CRC | CMD_DATA_EXPECTED |
                     CMD_PRV_DAT_WAIT;   /* READ = no CMD_WRITE */

    uint32_t rint;
    sd_status_t st = send_cmd(cmd_idx, addr, flags, &rint);
    if (st != SD_OK) {
        printf("  [sd] READ CMD%lu st=%d rint=0x%08lx\n",
               (unsigned long)cmd_idx, (int)st, (unsigned long)rint);
        return st;
    }

    /* Read the data from the FIFO in PIO.

     * As for the responses, the DATA error flags (FRUN, RTO,
     * HLE — rint=0x1906) are FALSE POSITIVES at this configuration. So we do
     * NOT rely on them: we drain the FIFO as soon as it holds words, until the
     * sector's 128 words are read OR DATA_OVER, with a timeout. Real validity is
     * checked by the caller (MBR signature 0x55AA). Only a frank DRTO (hardware
     * data read timeout, bit9) + empty FIFO is fatal. */
    uint32_t words_left = total_bytes / 4u;
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(5000000);

    while (words_left > 0) {
        uint32_t fifo_count = (r(REG_STATUS) >> STATUS_FIFO_COUNT_SHIFT)
                              & STATUS_FIFO_COUNT_MASK;
        if (fifo_count == 0) {
            uint32_t sts = r(REG_RINTSTS);
            /* If the transfer is marked done but the FIFO is empty and words
             * remain -> real anomaly: exit (the caller will validate the content). */
            if (sts & INT_DATA_OVER)
                break;
            if (timer_now_ticks() >= deadline) {
                printf("  [sd] READ timeout: %lu words remaining, rint=0x%08lx\n",
                       (unsigned long)words_left, (unsigned long)sts);
                break;
            }
            continue;
        }
        while (fifo_count-- > 0 && words_left > 0) {
            *out++ = r(REG_FIFO);
            words_left--;
        }
    }

    /* Flush any FIFO residue then wait for DATA_OVER (without making it blocking). */
    deadline = timer_now_ticks() + timer_us_to_ticks(500000);
    while (!(r(REG_RINTSTS) & INT_DATA_OVER)) {
        if (timer_now_ticks() >= deadline)
            break;
    }
    w(REG_RINTSTS, 0xFFFFFFFF);


    /* CMD12: stop for multiple reads. */
    if (count > 1)
        cmd(MMC_CMD12_STOP, 0, CMD_RESP_EXPECT | CMD_CHECK_CRC);

    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* Sector write (PIO) — FAT32 R/W                                     */
/* ------------------------------------------------------------------ */
/* dw_mmc write sequence (symmetric to the read):
 *   1. wait for the CIU to be free + FIFO reset;
 *   2. BLKSIZ/BYTCNT, send CMD24 (single) or CMD25 (multiple) with
 *      CMD_DATA_EXPECTED | CMD_WRITE;
 *   3. PUSH words into the FIFO as soon as it has room (TX watermark),
 *      until DATA_OVER;
 *   4. CMD12 (stop) if multiple, then WAIT until the card is no longer busy
 *      (STATUS_DATA_BUSY: the card programs the flash — essential before the
 *      next command, otherwise corruption/error).
 *
 * As for the read, the RTO/RCRC/FRUN flags are FALSE POSITIVES at this
 * config: we rely on CMD_DONE + DATA_OVER + end of busy. Real validity is
 * confirmed by a re-read (write then read-back). */
sd_status_t sdmmc_write_blocks(uint32_t lba, uint32_t count, const void *buf)
{
    if (!g_present)
        return SD_ENOCARD;
    if (count == 0 || buf == 0)
        return SD_EINVAL;

    uint32_t total_bytes = count * SD_SECTOR_SIZE;
    uint32_t addr = g_sdhc ? lba : (lba * SD_SECTOR_SIZE);
    uint32_t cmd_idx = (count > 1) ? MMC_CMD25_WRITE_MULTI
                                   : MMC_CMD24_WRITE_SINGLE;

    /* ⚠️ BOARD BUG Phase 4 — TWO successive causes, resolved:
     *  - The dw_mmc only drains the TX FIFO on request
     *      (TXDR). We fill the FIFO progressively while monitoring FIFO_COUNT
     *      until DATA_OVER (u-boot sequence).
     *  - DCRC (rint 0x94/0x9c): The correct setting depends on the DRIVE
     *      PHASE (distinct from the read's sample phase) + the HOLD_REG hold
     *      register. The right phase is not known a priori on this board:
     *      -> we SWEEP the drive phases {0°,90°,180°,270°} and keep the 1st one
     *         that does NOT trigger DCRC (accepted transfer). We then restore
     *         drive=0° (read phase). The caller validates by re-read.
     *
     * Note: at 400 kHz the CPU fills the FIFO much faster than it drains, no risk of
     * underrun. */
    static const uint32_t drive_phases[4] = { 0u, 1u, 2u, 3u };  /* 0/90/180/270 */

    sd_status_t ret = SD_EIO;

    for (int ph = 0; ph < 4; ph++) {
        /* Apply the candidate drive phase (+ update_clock to lock it). */
        cru_set_phase(CRU_SDMMC_CON0, drive_phases[ph]);
        update_clock();

        /* Wait for the card to be free + clean FIFO. */
        wait_data_idle(1000000);
        w(REG_CTRL, r(REG_CTRL) | CTRL_FIFO_RESET);
        { uint64_t d = timer_now_ticks() + timer_us_to_ticks(100000);
          while ((r(REG_CTRL) & CTRL_FIFO_RESET) && timer_now_ticks() < d) {} }

        w(REG_RINTSTS, 0xFFFFFFFF);
        w(REG_BLKSIZ, SD_SECTOR_SIZE);
        w(REG_BYTCNT, total_bytes);

        const uint32_t *in = (const uint32_t *)buf;
        uint32_t flags = CMD_RESP_EXPECT | CMD_CHECK_CRC | CMD_DATA_EXPECTED |
                         CMD_WRITE | CMD_PRV_DAT_WAIT | CMD_USE_HOLD_REG;

        uint32_t rint;
        sd_status_t st = send_cmd(cmd_idx, addr, flags, &rint);
        if (st != SD_OK) {
            printf("  [sd] WRITE CMD%lu (drive=%lu) st=%d rint=0x%08lx\n",
                   (unsigned long)cmd_idx, (unsigned long)drive_phases[ph],
                   (int)st, (unsigned long)rint);
            continue;   /* try the next phase */
        }

        /* PIO data transfer driven by FIFO_COUNT until DATA_OVER. We REMEMBER
         * whether a DCRC/data error occurs during the transfer. */
        const uint32_t fifo_depth = 256u;
        uint32_t words_left = total_bytes / 4u;
        uint32_t data_err = 0;
        uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(10000000);

        for (;;) {
            uint32_t sts = r(REG_RINTSTS);
            if (sts & INT_DATA_ERR)
                data_err |= (sts & INT_DATA_ERR);

            if (words_left > 0) {
                uint32_t fc = (r(REG_STATUS) >> STATUS_FIFO_COUNT_SHIFT)
                              & STATUS_FIFO_COUNT_MASK;
                while (fc < fifo_depth && words_left > 0) {
                    w(REG_FIFO, *in++);
                    words_left--;
                    fc++;
                }
                if (sts & INT_TXDR)
                    w(REG_RINTSTS, INT_TXDR);
            }

            if ((sts & INT_DATA_OVER) && words_left == 0)
                break;

            if (timer_now_ticks() >= deadline) {
                data_err |= INT_DRTO;   /* mark as failure */
                break;
            }
        }

        /* Wait for the end of busy (flash programming). */
        wait_data_idle(2000000);
        /* CMD12 stop for multi. */
        if (count > 1)
            cmd(MMC_CMD12_STOP, 0, CMD_RESP_EXPECT | CMD_CHECK_CRC);
        w(REG_RINTSTS, 0xFFFFFFFF);

        if (data_err == 0) {
            /* Transfer accepted with no data error at this drive phase. */
            if (ph != 0)
                printf("  [sd] WRITE OK with drive=%lu (auto phase)\n",
                       (unsigned long)drive_phases[ph]);
            ret = SD_OK;
            break;
        }
        /* Otherwise (DCRC/...): retry with the next phase. */
    }

    /* Restore the read drive phase (0°) whatever the result. */
    cru_set_phase(CRU_SDMMC_CON0, MMC_DEGREE_0);
    update_clock();

    if (ret != SD_OK)
        printf("  [sd] WRITE : failed on all drive phases (DCRC)\n");
    return ret;
}


#endif /* MMU_QEMU */

