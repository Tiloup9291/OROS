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
 * gic.c — GIC-400 (GICv2) — distributor + CPU interface
 *
 * MMIO addresses :
 *   - RK3328 (real hardware) : GICD = 0xFF811000, GICC = 0xFF812000
 *   - QEMU 'virt' (GICv2)    : GICD = 0x08000000, GICC = 0x08010000
 *
 * Select through -DGIC_QEMU (see Makefile target qemu).
 *
 * GICv2 : The distributor route interrupts to the CPU interface ;
 * CPU interface delivers IRQs to the core, handles acknowledge (IAR) and end
 * of interrupt (EOIR).
 *
 * Independent of Linux : MMIO registers accesses only.
 */

#include "gic.h"

#if defined(GIC_QEMU)
#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL
#else
/* RK3328 */
#define GICD_BASE   0xFF811000UL
#define GICC_BASE   0xFF812000UL
#endif

/* ---- Distributor registers (GICD) ---- */
#define GICD_CTLR         (GICD_BASE + 0x000)
#define GICD_TYPER        (GICD_BASE + 0x004)
#define GICD_IGROUPR      (GICD_BASE + 0x080)  /* +4*n */
#define GICD_ISENABLER    (GICD_BASE + 0x100)  /* +4*n */
#define GICD_ICENABLER    (GICD_BASE + 0x180)  /* +4*n */
#define GICD_IPRIORITYR   (GICD_BASE + 0x400)  /* +1*n (bytes) */
#define GICD_ITARGETSR    (GICD_BASE + 0x800)  /* +1*n (bytes) */
#define GICD_ICFGR        (GICD_BASE + 0xC00)  /* +4*n */
#define GICD_SGIR         (GICD_BASE + 0xF00)

/* ---- CPU interface registers (GICC) ---- */
#define GICC_CTLR         (GICC_BASE + 0x000)
#define GICC_PMR          (GICC_BASE + 0x004)  /* Priority Mask */
#define GICC_BPR          (GICC_BASE + 0x008)
#define GICC_IAR          (GICC_BASE + 0x00C)  /* Interrupt Acknowledge */
#define GICC_EOIR         (GICC_BASE + 0x010)  /* End Of Interrupt */

static inline void mmio_w(unsigned long a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}
static inline uint32_t mmio_r(unsigned long a)
{
    return *(volatile uint32_t *)a;
}

void gic_init(void)
{
    /* --- Distributor --- */
    /* Disable for configuring. */
    mmio_w(GICD_CTLR, 0);

    /* Number of INTID supported (ITLinesNumber). */
    uint32_t typer = mmio_r(GICD_TYPER);
    uint32_t nlines = ((typer & 0x1F) + 1) * 32;

    /* Disable all, default priority, target = CPU0. */
    for (uint32_t i = 32; i < nlines; i += 32)
        mmio_w(GICD_ICENABLER + (i / 32) * 4, 0xFFFFFFFF);

    /* Priority (bytes) : default median value for SPI. */
    for (uint32_t i = 32; i < nlines; i++)
        *(volatile uint8_t *)(GICD_IPRIORITYR + i) = 0xA0;

    /* Targets of SPI = CPU0 (bit 0). */
    for (uint32_t i = 32; i < nlines; i++)
        *(volatile uint8_t *)(GICD_ITARGETSR + i) = 0x01;

    /* Enable distributor (Group 0 + Group 1). */
    mmio_w(GICD_CTLR, 0x1);

    /* --- CPU interface (core 0) --- */
    gic_init_cpu();
}

/* GICC CPU interface : BANKED registers PER CORE. Each core (including
 * secondary) must configure its own CPU interface + local SGI/PPI. */
void gic_init_cpu(void)
{
    /* SGI (0..15) and PPI (16..31) are banked per core : enable and
     * set default priority for THIS core. */
    mmio_w(GICD_ISENABLER + 0, 0x0000FFFF);   /* SGI 0..15 enabled */
    for (uint32_t i = 0; i < 32; i++)
        *(volatile uint8_t *)(GICD_IPRIORITYR + i) = 0x80;

    /* Priority mask : accept all priorities (0xFF = most permissive). */
    mmio_w(GICC_PMR, 0xFF);
    mmio_w(GICC_BPR, 0x0);
    /* Enable IRQ signaling to the core. */
    mmio_w(GICC_CTLR, 0x1);
}


void gic_enable_irq(uint32_t intid)
{
    mmio_w(GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

void gic_disable_irq(uint32_t intid)
{
    mmio_w(GICD_ICENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

void gic_set_priority(uint32_t intid, uint8_t prio)
{
    *(volatile uint8_t *)(GICD_IPRIORITYR + intid) = prio;
}

uint32_t gic_acknowledge(void)
{
    return mmio_r(GICC_IAR) & 0x3FF;   /* INTID on 10 bits */
}

void gic_end_of_interrupt(uint32_t intid)
{
    mmio_w(GICC_EOIR, intid);
}

void gic_send_sgi(uint32_t sgi_id, uint32_t target_core)
{
    /* GICD_SGIR : TargetListFilter=00 (list), CPUTargetList = 1<<core, SGIINTID */
    uint32_t val = ((1u << target_core) << 16) | (sgi_id & 0xF);
    mmio_w(GICD_SGIR, val);
}

void gic_set_target(uint32_t intid, uint32_t target_core)
{
    /* ITARGETSR : 1 byte per INTID, CPU mask (bit c => CPU c).
     * Only SPI (>=32) are route ; SGI/PPI are banked per core. */
    if (intid < 32)
        return;
    *(volatile uint8_t *)(GICD_ITARGETSR + intid) = (uint8_t)(1u << target_core);
}

