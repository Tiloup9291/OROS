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
 * sdmmc.h — SD/MMC driver (Synopsys DesignWare MSHC / dw_mmc) for RK3328
 *           : SD card init + sector read (PIO).
 *
 * The RK3328 exposes the SDMMC controller (micro-SD slot) at 0xFF500000. It is
 * a DesignWare Mobile Storage Host Controller (the same IP as Linux "dw_mmc").
 *
 * We target for the demo "SD sector read OK": SPI-less init in native
 * SD mode, block-by-block PIO read (FIFO), without DMA/IDMAC (avoids the
 * descriptor and cache-coherence complexity).
 *
 * On QEMU 'virt' there is no RK3328 SDMMC controller: the functions return an
 * error (SD_ENODEV) so the demo remains harmless.
 */
#ifndef RTOS_DRIVERS_SDMMC_H
#define RTOS_DRIVERS_SDMMC_H

#include <stdint.h>

/* Size of a logical SD sector (fixed at 512 bytes). */
#define SD_SECTOR_SIZE   512u

/* GIC INTID (SPI+32) of the RK3328 SDMMC controller. */
#define SDMMC_IRQ        44u

/* Return codes. */
typedef enum {
    SD_OK       = 0,
    SD_ENODEV   = -1,   /* no controller (QEMU) */
    SD_ENOCARD  = -2,   /* no card detected */
    SD_ETIMEOUT = -3,   /* command / data timeout */
    SD_EIO      = -4,   /* CRC / transfer error */
    SD_EINVAL   = -5,   /* invalid parameter */
} sd_status_t;

/* Information about the detected card. */
typedef struct {
    uint32_t rca;           /* Relative Card Address */
    uint32_t is_sdhc;       /* 1 = block addressing (SDHC/SDXC) */
    uint64_t capacity_bytes;/* total capacity */
    uint32_t sector_count;  /* number of 512-B sectors */
} sd_card_t;

/* Initializes the controller and the SD card. Returns SD_OK if a card is
 * ready to be read. Fills *card if not NULL. */
sd_status_t sdmmc_init(sd_card_t *card);

/* Reads 'count' 512-B sectors starting at logical sector 'lba' into 'buf'.
 * PIO mode (FIFO read). Returns SD_OK or an error code. */
sd_status_t sdmmc_read_blocks(uint32_t lba, uint32_t count, void *buf);

/* Writes 'count' 512-B sectors from 'buf' to logical sector 'lba'.
 * PIO mode (FIFO write). Required for FAT32 R/W.
 * Returns SD_OK or an error code. */
sd_status_t sdmmc_write_blocks(uint32_t lba, uint32_t count, const void *buf);


/* Returns 1 if a card was initialized successfully. */
int sdmmc_card_present(void);

#endif /* RTOS_DRIVERS_SDMMC_H */
