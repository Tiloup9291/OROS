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
 * gpio.h — GPIO + pinctrl (IOMUX) driver for RK3328
 *
 * The RK3328 has 4 GPIO banks (gpio0..gpio3), each with 32 pins
 * (A0..A7, B0..B7, C0..C7, D0..D7). Each bank is a Synopsys DesignWare APB
 * GPIO controller (DR/DDR/EXT_PORTA registers + interrupt block).
 *
 * The IOMUX (selection of a pin's function: GPIO vs alternate function)
 * is driven by the GRF (General Register Files, 0xFF100000).
 *
 * On QEMU 'virt' there is no RK3328 GPIO: the functions are then no-ops
 * (compiled with -DGPIO_STUB via MMU_QEMU) so the demo can run.
 *
 * Independent of Linux: MMIO register access only.
 */
#ifndef RTOS_DRIVERS_GPIO_H
#define RTOS_DRIVERS_GPIO_H

#include <stdint.h>

/* GPIO bank number. */
#define GPIO_BANK0   0u
#define GPIO_BANK1   1u
#define GPIO_BANK2   2u
#define GPIO_BANK3   3u
#define GPIO_NUM_BANKS 4u

/* Pins per group: A=0, B=1, C=2, D=3; index 0..7 within the group. */
#define GPIO_PIN(group, idx)   ((uint32_t)((group) * 8u + (idx)))
#define GPIO_GROUP_A  0u
#define GPIO_GROUP_B  1u
#define GPIO_GROUP_C  2u
#define GPIO_GROUP_D  3u

/* Pin direction. */
typedef enum {
    GPIO_IN  = 0,
    GPIO_OUT = 1,
} gpio_dir_t;

/* Logic level. */
#define GPIO_LOW   0u
#define GPIO_HIGH  1u

/* IOMUX function selection (0 = GPIO, 1..3 = alternate functions). */
#define GPIO_FUNC_GPIO  0u

/* Internal pull resistor (for digital INPUTS). */
typedef enum {
    GPIO_PULL_NONE = 0,   /* no pull (floating input) */
    GPIO_PULL_UP,         /* pull to + (rest = 1, contact to GND = 0) */
    GPIO_PULL_DOWN,       /* pull to - (rest = 0, contact to +3V3 = 1) */
} gpio_pull_t;


/* GPIO interrupt trigger type. */
typedef enum {
    GPIO_IRQ_LEVEL_HIGH = 0,
    GPIO_IRQ_LEVEL_LOW,
    GPIO_IRQ_EDGE_RISING,
    GPIO_IRQ_EDGE_FALLING,
} gpio_irq_trig_t;

/* Initializes the GPIO subsystem (no destructive hardware effect). */
void gpio_init(void);

/* Programs the IOMUX function of a pin via the GRF (0 = GPIO). */
void gpio_set_iomux(uint32_t bank, uint32_t pin, uint32_t func);

/* Sets the direction of a pin (GPIO mode). */
void gpio_set_direction(uint32_t bank, uint32_t pin, gpio_dir_t dir);

/* Writes a level on an output pin. */
void gpio_set_value(uint32_t bank, uint32_t pin, uint32_t value);

/* Reads the level present on a pin (input or output). */
uint32_t gpio_get_value(uint32_t bank, uint32_t pin);

/* Programs the internal pull resistor of a pin (via the GRF). */
void gpio_set_pull(uint32_t bank, uint32_t pin, gpio_pull_t pull);

/* ---- General-purpose digital I/O helpers ---- */
/* Configures a pin as digital OUTPUT (IOMUX=GPIO, direction=OUT) and
 * applies an initial level (GPIO_LOW/GPIO_HIGH). To drive a relay, external
 * LED, transistor, etc. */
void gpio_output_setup(uint32_t bank, uint32_t pin, uint32_t initial_value);

/* Configures a pin as digital INPUT (IOMUX=GPIO, direction=IN) with a pull
 * resistor. To read a button, dry contact, digital sensor. */
void gpio_input_setup(uint32_t bank, uint32_t pin, gpio_pull_t pull);


/* Configures and enables the interrupt of a pin (input mode). */
void gpio_irq_enable(uint32_t bank, uint32_t pin, gpio_irq_trig_t trig);
void gpio_irq_disable(uint32_t bank, uint32_t pin);

/* Reads a bank's interrupt status register (one bit per pin). */
uint32_t gpio_irq_status(uint32_t bank);

/* Acknowledges (clears) the bank's interrupts per the pin mask. */
void gpio_irq_clear(uint32_t bank, uint32_t pin_mask);

/* GIC INTID (SPI+32) of the RK3328 GPIO banks. */
#define GPIO0_IRQ   83u
#define GPIO1_IRQ   84u
#define GPIO2_IRQ   85u
#define GPIO3_IRQ   86u

/* ------------------------------------------------------------------ */
/* Board wiring: Orange Pi R1 Plus LTS (3 drivable LEDs)               */
/* ------------------------------------------------------------------ */
/* Source: arch/arm64/boot/dts/rockchip/rk3328-orangepi-r1-plus.dtsi
 * The 3 LEDs are active-HIGH (GPIO_ACTIVE_HIGH). */

/* "LAN" LED (green, Ethernet port): gpio2 RK_PB7 = group B, pin 7 = index 15. */
#define BOARD_LED_LAN_BANK    GPIO_BANK2
#define BOARD_LED_LAN_PIN     GPIO_PIN(GPIO_GROUP_B, 7)

/* "STATUS" LED (system, heartbeat trigger under Linux): gpio3 RK_PC5 = index 21. */
#define BOARD_LED_STATUS_BANK GPIO_BANK3
#define BOARD_LED_STATUS_PIN  GPIO_PIN(GPIO_GROUP_C, 5)

/* "WAN" LED (Ethernet port): gpio2 RK_PC2 = group C, pin 2 = index 18. */
#define BOARD_LED_WAN_BANK    GPIO_BANK2
#define BOARD_LED_WAN_PIN     GPIO_PIN(GPIO_GROUP_C, 2)

/* On level (active high). */
#define BOARD_LED_ON          GPIO_HIGH
#define BOARD_LED_OFF         GPIO_LOW

/* ------------------------------------------------------------------ */
/* 13-pin expansion connector — general-purpose GPIO (digital I/O)     */
/* ------------------------------------------------------------------ */
/* Orange Pi R1 Plus LTS (13-pin) pinout. These pins operate at 3.3 V and
 * serve as digital I/O (drive external relay/LED, read button/contact/sensor).
 * Pins 1=VCC5V, 2=GND, 7/8=LineOut, 9=TVout.
 *
 * | Pin | Signal    | GPIO      | bank / group / idx        |
 * |-----|-----------|-----------|----------------------------|
 * |  3  | TWI0-SDA  | GPIO2_D1  | gpio2 D1  (if I2C0 unused) |
 * |  4  | TWI0-SCK  | GPIO2_D0  | gpio2 D0                    |
 * |  5  | UART1-TX  | GPIO3_A4  | gpio3 A4  (if UART1 unused) |
 * |  6  | UART1-RX  | GPIO3_A6  | gpio3 A6                    |
 * | 10  | GPIO3_C0  | GPIO3_C0  | gpio3 C0  (most free pin)   |
 * | 11  | UART1-CTS | GPIO3_A7  | gpio3 A7                    |
 * | 12  | UART1-RTS | GPIO3_A5  | gpio3 A5                    |
 * | 13  | IR-RX     | GPIO2_A2  | gpio2 A2  (if IR unused)    |
 */
#define HDR_PIN10_BANK   GPIO_BANK3               /* GPIO3_C0: recommended digital output */
#define HDR_PIN10_PIN    GPIO_PIN(GPIO_GROUP_C, 0)
#define HDR_PIN13_BANK   GPIO_BANK2               /* GPIO2_A2 (IR-RX): digital input    */
#define HDR_PIN13_PIN    GPIO_PIN(GPIO_GROUP_A, 2)
#define HDR_PIN3_BANK    GPIO_BANK2               /* GPIO2_D1 (I2C SDA)               */
#define HDR_PIN3_PIN     GPIO_PIN(GPIO_GROUP_D, 1)
#define HDR_PIN4_BANK    GPIO_BANK2               /* GPIO2_D0 (I2C SCK)               */
#define HDR_PIN4_PIN     GPIO_PIN(GPIO_GROUP_D, 0)

#endif /* RTOS_DRIVERS_GPIO_H */


