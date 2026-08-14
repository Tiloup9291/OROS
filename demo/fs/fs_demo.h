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
 * fs_demo.h — Demo: FAT32 filesystem (FatFs) on micro-SD.
 *
 * Goal: mount the card, list the root, read a file,
 * write a test file then read it back (R/W validation). Runs on Core 2
 * (IO_SOFT).
 */
#ifndef RTOS_FS_DEMO_H
#define RTOS_FS_DEMO_H

/* Runs the FAT32 demonstration. Prints a summary on the UART.
 * Returns 1 if the succeed (mount + list + read + write + read-back) is validated,
 * 0 otherwise (e.g. no card / QEMU). */
int fs_demo_run(void);

#endif /* RTOS_FS_DEMO_H */
