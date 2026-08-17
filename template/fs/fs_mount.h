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
 * fs_mount.h — PERMANENT FAT mount of the micro-SD card (volume "0:").
 *
 * Production behaviour (unlike the demo): the volume is mounted ONCE at boot
 * and STAYS MOUNTED for the whole life of the system. The application (Core2,
 * IO_SOFT partition) can therefore use the FatFs API (f_open / f_read /
 * f_write / f_opendir ...) at any moment, with no re-mount.
 *
 * RULE: FatFs is ONLY usable from Core2 (blocking SDMMC PIO accesses). The
 * hard-RT cores must go through the mailbox/klog if they need persistence.
 */
#ifndef OROS_FS_MOUNT_H
#define OROS_FS_MOUNT_H

/*
 * fs_mount_init — initializes the SD card (sdmmc_init) then mounts volume
 * "0:" and KEEPS it mounted. Emits ONE status line (or an error line).
 * Returns 0 if the volume is mounted, non-zero otherwise (no card, no
 * controller, no FAT filesystem). A failure is NOT fatal: the rest of the
 * system keeps running.
 */
int fs_mount_init(void);

/* 1 if the volume "0:" is currently mounted and usable. */
int fs_mount_ready(void);

/* Free / total space of the volume in KiB (0 if not mounted). Diagnostics. */
void fs_mount_space_kb(unsigned long *free_kb, unsigned long *total_kb);

#endif /* OROS_FS_MOUNT_H */
