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
 * gpio.c — GPIO + pinctrl (IOMUX) driver for RK3328
 *
 * Each GPIO bank is a Synopsys DesignWare APB GPIO controller:
 *   0x00 SWPORTA_DR     data register (outputs)
 *   0x04 SWPORTA_DDR    direction (1 = output, 0 = input)
 *   0x30 INTEN          per-pin IRQ enable
 *   0x34 INTMASK        IRQ mask
 *   0x38 INTTYPE_LEVEL  1 = edge, 0 = level
 *   0x3C INT_POLARITY   1 = active high / rising edge
 *   0x40 INT_STATUS     IRQ status (after masking)
 *   0x4C PORTA_EOI      edge-IRQ acknowledgment
 *   0x50 EXT_PORTA      input read
 *
 * The IOMUX (function of a pin) is in the GRF (0xFF100000). On RK3328 each
 * pin is coded on 2 bits; the GRF IOMUX registers use a "write-enable mask":
 * bits [31:16] = mask of the bits [15:0] to modify.
 *
 * On QEMU (-DMMU_QEMU) the hardware does not exist: all functions are
 * neutralized (no-op) so the demo runs without an MMIO fault.
 */

#include "gpio.h"

/* ------------------------------------------------------------------ */
/* QEMU case: no RK3328 GPIO -> no-op                                  */
/* ------------------------------------------------------------------ */
#if defined(MMU_QEMU)

void gpio_init(void) {}
void gpio_set_iomux(uint32_t bank, uint32_t pin, uint32_t func) {}
void gpio_set_direction(uint32_t bank, uint32_t pin, gpio_dir_t dir) {}
void gpio_set_value(uint32_t bank, uint32_t pin, uint32_t value) {}
uint32_t gpio_get_value(uint32_t bank, uint32_t pin) { return 0; }
void gpio_set_pull(uint32_t bank, uint32_t pin, gpio_pull_t pull) {}
void gpio_output_setup(uint32_t bank, uint32_t pin, uint32_t initial_value) {}
void gpio_input_setup(uint32_t bank, uint32_t pin, gpio_pull_t pull) {}
void gpio_irq_enable(uint32_t bank, uint32_t pin, gpio_irq_trig_t trig) {}
void gpio_irq_disable(uint32_t bank, uint32_t pin) {}
uint32_t gpio_irq_status(uint32_t bank) { return 0; }
void gpio_irq_clear(uint32_t bank, uint32_t pin_mask) {}


#else
/* ------------------------------------------------------------------ */
/* RK3328 case (real hardware)                                         */
/* ------------------------------------------------------------------ */

#define GRF_BASE        0xFF100000UL

/* Bases of the 4 GPIO banks. */
static const unsigned long GPIO_BASE[GPIO_NUM_BANKS] = {
    0xFF210000UL,   /* GPIO0 */
    0xFF220000UL,   /* GPIO1 */
    0xFF230000UL,   /* GPIO2 */
    0xFF240000UL,   /* GPIO3 */
};

/* DW APB GPIO register offsets. */
#define GPIO_SWPORTA_DR     0x00
#define GPIO_SWPORTA_DDR    0x04
#define GPIO_INTEN          0x30
#define GPIO_INTMASK        0x34
#define GPIO_INTTYPE_LEVEL  0x38
#define GPIO_INT_POLARITY   0x3C
#define GPIO_INT_STATUS     0x40
#define GPIO_PORTA_EOI      0x4C
#define GPIO_EXT_PORTA      0x50

/* ------------------------------------------------------------------ *
 * RK3328 IOMUX: the GRF does NOT have a regular layout! Some groups
 * use 4 bits/pin split over 2 L/H registers (gpio2b, gpio2c, gpio3a,
 * gpio3b), the others 2 bits/pin on 1 register. So for each (bank,group)
 * we encode: the offset of the "low" register, and the number of bits/pin.
 * (Reference: u-boot arch-rockchip/grf_rk3328.h struct rk3328_grf_regs.)
 *
 * Offsets (bytes, from GRF 0xFF100000):
 *  gpio0a=0x00 b=0x04 c=0x08 d=0x0C
 *  gpio1a=0x10 b=0x14 c=0x18 d=0x1C
 *  gpio2a=0x20 bl=0x24 bh=0x28 cl=0x2C ch=0x30 d=0x34
 *  gpio3al=0x38 ah=0x3C bl=0x40 bh=0x44 c=0x48 d=0x4C
 */
typedef struct { uint16_t off; uint8_t bits; } iomux_desc_t;
/* index = bank*4 + group (group A=0..D=3). off = L register, bits = 2 or 4. */
static const iomux_desc_t IOMUX[GPIO_NUM_BANKS * 4] = {
    /* bank0 */ {0x00,2},{0x04,2},{0x08,2},{0x0C,2},
    /* bank1 */ {0x10,2},{0x14,2},{0x18,2},{0x1C,2},
    /* bank2 */ {0x20,2},{0x24,4},{0x2C,4},{0x34,2},   /* A=2b, B=4b(bl/bh), C=4b(cl/ch), D=2b */
    /* bank3 */ {0x38,4},{0x40,4},{0x48,2},{0x4C,2},   /* A=4b(al/ah), B=4b(bl/bh), C=2b, D=2b */
};

/* Pull resistor registers, regular: 2 bits/pin, one register per group,
 * starting at 0x100 (bank*4 + group) * 4.
 * RK3328 encoding: 00 = none, 01 = pull-up, 02 = pull-down. */
#define GRF_GPIO0A_P        0x0100


static inline void mmio_w(unsigned long a, uint32_t v)

{
    *(volatile uint32_t *)a = v;
}
static inline uint32_t mmio_r(unsigned long a)
{
    return *(volatile uint32_t *)a;
}

static inline void reg_setbit(unsigned long a, uint32_t bit, int set)
{
    uint32_t v = mmio_r(a);
    if (set) v |= (1u << bit);
    else     v &= ~(1u << bit);
    mmio_w(a, v);
}

void gpio_init(void)
{
    /* Nothing to force: U-Boot leaves the banks in a usable state.
     * Simply mask all GPIO IRQs to start clean. */
    for (uint32_t b = 0; b < GPIO_NUM_BANKS; b++) {
        mmio_w(GPIO_BASE[b] + GPIO_INTEN, 0);
        mmio_w(GPIO_BASE[b] + GPIO_INTMASK, 0xFFFFFFFF);
    }
}

void gpio_set_iomux(uint32_t bank, uint32_t pin, uint32_t func)
{
    if (bank >= GPIO_NUM_BANKS || pin >= 32u)
        return;
    uint32_t group = pin / 8u;          /* A/B/C/D */
    uint32_t idx   = pin % 8u;          /* 0..7 within the group */
    const iomux_desc_t d = IOMUX[bank * 4u + group];

    unsigned long reg;
    uint32_t shift, width, fmask;

    if (d.bits == 4u) {
        /* 4 bits/pin, split L (pins 0..3) / H (pins 4..7). */
        width = 4u;
        fmask = 0xFu;
        if (idx < 4u) {
            reg = GRF_BASE + d.off;          /* L register */
            shift = idx * 4u;
        } else {
            reg = GRF_BASE + d.off + 4u;     /* H register */
            shift = (idx - 4u) * 4u;
        }
    } else {
        /* 2 bits/pin, 1 register per group. */
        width = 2u;
        fmask = 0x3u;
        reg = GRF_BASE + d.off;
        shift = idx * 2u;
    }

    (void)width;
    /* GRF write-enable mask: [31:16] mask, [15:0] value. */
    uint32_t val = ((fmask << shift) << 16) | ((func & fmask) << shift);
    mmio_w(reg, val);
}


void gpio_set_direction(uint32_t bank, uint32_t pin, gpio_dir_t dir)
{
    if (bank >= GPIO_NUM_BANKS || pin >= 32u)
        return;
    reg_setbit(GPIO_BASE[bank] + GPIO_SWPORTA_DDR, pin, dir == GPIO_OUT);
}

void gpio_set_value(uint32_t bank, uint32_t pin, uint32_t value)
{
    if (bank >= GPIO_NUM_BANKS || pin >= 32u)
        return;
    reg_setbit(GPIO_BASE[bank] + GPIO_SWPORTA_DR, pin, value != 0);
}

uint32_t gpio_get_value(uint32_t bank, uint32_t pin)
{
    if (bank >= GPIO_NUM_BANKS || pin >= 32u)
        return 0;
    return (mmio_r(GPIO_BASE[bank] + GPIO_EXT_PORTA) >> pin) & 1u;
}

void gpio_set_pull(uint32_t bank, uint32_t pin, gpio_pull_t pull)
{
    if (bank >= GPIO_NUM_BANKS || pin >= 32u)
        return;
    uint32_t group = pin / 8u;
    uint32_t idx   = pin % 8u;
    unsigned long reg = GRF_BASE + GRF_GPIO0A_P + bank * 0x10 + group * 4u;
    uint32_t shift = idx * 2u;
    uint32_t code;
    switch (pull) {
        case GPIO_PULL_UP:   code = 0x1; break;
        case GPIO_PULL_DOWN: code = 0x2; break;
        default:             code = 0x0; break;   /* none */
    }
    /* Write-enable mask GRF : [31:16] mask, [15:0] value. */
    uint32_t val = ((0x3u << shift) << 16) | (code << shift);
    mmio_w(reg, val);
}

void gpio_output_setup(uint32_t bank, uint32_t pin, uint32_t initial_value)
{
    /* Pin to GPIO, output, initial level applied BEFORE switching to output
     * to avoid a glitch (order: DR then DDR). */
    gpio_set_iomux(bank, pin, GPIO_FUNC_GPIO);
    gpio_set_value(bank, pin, initial_value);
    gpio_set_direction(bank, pin, GPIO_OUT);
}

void gpio_input_setup(uint32_t bank, uint32_t pin, gpio_pull_t pull)
{
    /* Pin to GPIO, input, with the chosen internal pull. */
    gpio_set_iomux(bank, pin, GPIO_FUNC_GPIO);
    gpio_set_direction(bank, pin, GPIO_IN);
    gpio_set_pull(bank, pin, pull);
}

void gpio_irq_enable(uint32_t bank, uint32_t pin, gpio_irq_trig_t trig)

{
    if (bank >= GPIO_NUM_BANKS || pin >= 32u)
        return;
    unsigned long base = GPIO_BASE[bank];

    /* Pin as input. */
    reg_setbit(base + GPIO_SWPORTA_DDR, pin, 0);

    /* Type: level (0) or edge (1). */
    int edge = (trig == GPIO_IRQ_EDGE_RISING || trig == GPIO_IRQ_EDGE_FALLING);
    reg_setbit(base + GPIO_INTTYPE_LEVEL, pin, edge);

    /* Polarity: 1 = active high / rising edge. */
    int high = (trig == GPIO_IRQ_LEVEL_HIGH || trig == GPIO_IRQ_EDGE_RISING);
    reg_setbit(base + GPIO_INT_POLARITY, pin, high);

    /* Unmask + enable. */
    reg_setbit(base + GPIO_INTMASK, pin, 0);
    reg_setbit(base + GPIO_INTEN, pin, 1);
}

void gpio_irq_disable(uint32_t bank, uint32_t pin)
{
    if (bank >= GPIO_NUM_BANKS || pin >= 32u)
        return;
    reg_setbit(GPIO_BASE[bank] + GPIO_INTEN, pin, 0);
    reg_setbit(GPIO_BASE[bank] + GPIO_INTMASK, pin, 1);
}

uint32_t gpio_irq_status(uint32_t bank)
{
    if (bank >= GPIO_NUM_BANKS)
        return 0;
    return mmio_r(GPIO_BASE[bank] + GPIO_INT_STATUS);
}

void gpio_irq_clear(uint32_t bank, uint32_t pin_mask)
{
    if (bank >= GPIO_NUM_BANKS)
        return;
    /* PORTA_EOI only clears edge-type IRQs; no effect for level. */
    mmio_w(GPIO_BASE[bank] + GPIO_PORTA_EOI, pin_mask);
}

#endif /* MMU_QEMU */
