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
 * fs_mount.c — PERMANENT FAT mount of the micro-SD card (volume "0:").
 *
 * Called once by drivers_init() at boot. The FATFS object is STATIC and
 * never unmounted: FatFs stays usable for the whole life of the system
 * (production behaviour). No periodic message: one init line, or one error
 * line.
 */

#include <stdio.h>

#include "fs_mount.h"
#include "ff.h"
#include "../drivers/sdmmc/sdmmc.h"

/* Filesystem object: STATIC and PERMANENT (must outlive the mount). */
static FATFS g_fs;
static int   g_mounted;

/* Short text for a FatFs code (error reporting). */
static const char *fr_str(FRESULT r)
{
    switch (r) {
    case FR_OK:              return "OK";
    case FR_DISK_ERR:        return "DISK_ERR";
    case FR_NOT_READY:       return "NOT_READY";
    case FR_NO_FILESYSTEM:   return "NO_FILESYSTEM";
    case FR_WRITE_PROTECTED: return "WRITE_PROTECTED";
    case FR_INVALID_DRIVE:   return "INVALID_DRIVE";
    case FR_TIMEOUT:         return "TIMEOUT";
    default:                 return "ERR";
    }
}

int fs_mount_init(void)
{
    g_mounted = 0;

    /* --- 1) SD card (controller + card init) --- */
    sd_card_t card;
    sd_status_t sst = sdmmc_init(&card);
    if (sst != SD_OK) {
        if (sst == SD_ENODEV)
            printf("[fs] no SDMMC controller : volume 0: not mounted.\n");
        else if (sst == SD_ENOCARD)
            printf("[fs] no micro-SD card : volume 0: not mounted.\n");
        else
            printf("[fs] ERROR: SD card init failed (code %d).\n", (int)sst);
        return -1;
    }
    printf("[sdmmc] card OK : %lu sectors (~%lu MiB), SDHC=%lu\n",
           (unsigned long)card.sector_count,
           (unsigned long)(card.capacity_bytes / (1024ull * 1024ull)),
           (unsigned long)card.is_sdhc);

    /* --- 2) PERMANENT mount of volume "0:" (option 1 = mount now) --- */
    FRESULT r = f_mount(&g_fs, "0:", 1);
    if (r != FR_OK) {
        printf("[fs] ERROR: f_mount(0:) -> %s : volume not mounted.\n",
               fr_str(r));
        return -1;
    }
    g_mounted = 1;

    /* --- 3) ONE status line (label + free space) --- */
    char label[24];
    DWORD vsn = 0;
    label[0] = '\0';
    (void)f_getlabel("0:", label, &vsn);

    unsigned long free_kb = 0, tot_kb = 0;
    fs_mount_space_kb(&free_kb, &tot_kb);

    printf("[fs] volume 0: MOUNTED (FAT, label=\"%s\") free=%lu KiB / %lu KiB\n",
           (label[0] ? label : "(unnamed)"), free_kb, tot_kb);
    return 0;
}

int fs_mount_ready(void)
{
    return g_mounted;
}

void fs_mount_space_kb(unsigned long *free_kb, unsigned long *total_kb)
{
    if (free_kb)  *free_kb = 0;
    if (total_kb) *total_kb = 0;
    if (!g_mounted)
        return;

    DWORD free_clust = 0;
    FATFS *fsp = 0;
    if (f_getfree("0:", &free_clust, &fsp) != FR_OK || !fsp)
        return;

    /* Cluster = csize sectors of 512 B -> sectors/2 = KiB. */
    DWORD tot_clust = fsp->n_fatent - 2;
    DWORD csize     = fsp->csize;
    if (free_kb)  *free_kb  = (unsigned long)free_clust * csize / 2ul;
    if (total_kb) *total_kb = (unsigned long)tot_clust  * csize / 2ul;
}
