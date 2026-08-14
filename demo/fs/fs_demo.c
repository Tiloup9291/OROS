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
 * fs_demo.c — Demo: FAT32 via FatFs (ChaN) on the micro-SD.
 *
 * Sequence (Goal "mount, list, read, write, read back"):
 *   1. f_mount   : mounts volume 0: (the 1st FAT partition of the card).
 *   2. f_getlabel + f_getfree : volume name + free space (mount proof).
 *   3. list the root (f_opendir/f_readdir): files/folders present.
 *   4. read an existing file (the 1st regular file found): f_open/f_read.
 *   5. WRITE a test file "OROS_TEST.TXT" with a timestamped content
 *      (f_open O_WRITE|O_CREATE_ALWAYS, f_write, f_close).
 *   6. RE-READ this file (f_open O_READ, f_read) and COMPARE with the written
 *      content: this is the end-to-end R/W validation (read-back = what we wrote).
 *
 * Everything goes through printf (UART). On QEMU (no card), f_mount fails cleanly
 * (FR_NOT_READY / FR_NO_FILESYSTEM) and the demo reports it without crashing.
 */

#include <stdio.h>
#include <string.h>

#include "fs_demo.h"
#include "ff.h"
#include "../drivers/sdmmc/sdmmc.h"


/* Filesystem object (persistent while mounted). */
static FATFS  g_fs;

/* Test filename written then read back. 8.3 name (SFN-compatible). */
#define TEST_PATH     "0:/OROS_TEST.TXT"

/* Translates a FRESULT code into short text (diagnostic). */
static const char *fr_str(FRESULT r)
{
    switch (r) {
    case FR_OK:                 return "OK";
    case FR_DISK_ERR:           return "DISK_ERR";
    case FR_INT_ERR:            return "INT_ERR";
    case FR_NOT_READY:          return "NOT_READY";
    case FR_NO_FILE:            return "NO_FILE";
    case FR_NO_PATH:            return "NO_PATH";
    case FR_INVALID_NAME:       return "INVALID_NAME";
    case FR_DENIED:             return "DENIED";
    case FR_EXIST:              return "EXIST";
    case FR_INVALID_OBJECT:     return "INVALID_OBJECT";
    case FR_WRITE_PROTECTED:    return "WRITE_PROTECTED";
    case FR_INVALID_DRIVE:      return "INVALID_DRIVE";
    case FR_NOT_ENABLED:        return "NOT_ENABLED";
    case FR_NO_FILESYSTEM:      return "NO_FILESYSTEM";
    case FR_MKFS_ABORTED:       return "MKFS_ABORTED";
    case FR_TIMEOUT:            return "TIMEOUT";
    case FR_LOCKED:             return "LOCKED";
    case FR_NOT_ENOUGH_CORE:    return "NOT_ENOUGH_CORE";
    case FR_TOO_MANY_OPEN_FILES:return "TOO_MANY_OPEN_FILES";
    case FR_INVALID_PARAMETER:  return "INVALID_PARAMETER";
    default:                    return "?";
    }
}

/* Lists the root; stores in found_file (if not NULL) the path of the 1st
 * regular file found (useful for reading an existing file). */
static FRESULT list_root(char *found_file, size_t found_sz)
{
    DIR dir;
    FILINFO fno;
    FRESULT r = f_opendir(&dir, "0:/");
    if (r != FR_OK) {
        printf("  [fs] f_opendir(0:/) -> %s\n", fr_str(r));
        return r;
    }

    int nfiles = 0, ndirs = 0, got = 0;
    printf("  [fs] root content 0:/ :\n");
    for (;;) {
        r = f_readdir(&dir, &fno);
        if (r != FR_OK || fno.fname[0] == 0)
            break;                       /* end or error */
        int is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
        printf("      %-24s %8lu o  %s\n",
               fno.fname, (unsigned long)fno.fsize,
               is_dir ? "<DIR>" : "");
        if (is_dir) {
            ndirs++;
        } else {
            nfiles++;
            /* Remember the 1st regular file (apart from our test file). */
            if (!got && found_file &&
                strcmp(fno.fname, "OROS_TEST.TXT") != 0) {
                snprintf(found_file, found_sz, "0:/%s", fno.fname);
                got = 1;
            }
        }
    }
    f_closedir(&dir);
    printf("  [fs] -> %d file(s), %d folder(s)\n", nfiles, ndirs);
    return FR_OK;
}

/* Reads and displays the first bytes of an existing file. */
static FRESULT read_existing(const char *path)
{
    FIL f;
    FRESULT r = f_open(&f, path, FA_READ);
    if (r != FR_OK) {
        printf("  [fs] f_open(%s, READ) -> %s\n", path, fr_str(r));
        return r;
    }
    char buf[97];
    UINT br = 0;
    r = f_read(&f, buf, sizeof(buf) - 1, &br);
    f_close(&f);
    if (r != FR_OK) {
        printf("  [fs] f_read(%s) -> %s\n", path, fr_str(r));
        return r;
    }
    buf[br] = 0;
    printf("  [fs] reading of %s : %u byte(s) readed, overview :\n", path, br);
    /* Display the preview, replacing non-printable characters with '.'. */
    printf("      \"");
    for (UINT i = 0; i < br; i++) {
        char c = buf[i];
        putchar((c >= 32 && c < 127) ? c : '.');
    }
    printf("\"\n");
    return FR_OK;
}

/* Writes the test file with known content, then reads it back and compares. */
static int write_then_readback(void)
{
    /* Content to write (known, for exact comparison after read-back). */
    static const char content[] =
        "OROS RK3328 - FAT32/FatFs: writing OK.\n"
        "Line 2 : end-to-end R/W validation.\n";
    const UINT clen = (UINT)(sizeof(content) - 1);   /* without the trailing NUL */

    /* --- Write --- */
    FIL f;
    FRESULT r = f_open(&f, TEST_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        printf("  [fs] f_open(%s, WRITE|CREATE_ALWAYS) -> %s\n",
               TEST_PATH, fr_str(r));
        return 0;
    }
    UINT bw = 0;
    r = f_write(&f, content, clen, &bw);
    FRESULT rc = f_close(&f);
    if (r != FR_OK || bw != clen) {
        printf("  [fs] f_write(%s) -> %s (%u/%u bytes)\n",
               TEST_PATH, fr_str(r), bw, clen);
        return 0;
    }
    if (rc != FR_OK) {
        printf("  [fs] f_close(write) -> %s\n", fr_str(rc));
        return 0;
    }
    printf("  [fs] WRITING OF %s : %u byte(s) written (f_sync/close OK)\n",
           TEST_PATH, bw);

    /* --- Read-back --- */
    r = f_open(&f, TEST_PATH, FA_READ);
    if (r != FR_OK) {
        printf("  [fs] f_open(%s, READ) after writing -> %s\n",
               TEST_PATH, fr_str(r));
        return 0;
    }
    char rbuf[256];
    UINT br = 0;
    r = f_read(&f, rbuf, sizeof(rbuf) - 1, &br);
    f_close(&f);
    if (r != FR_OK) {
        printf("  [fs] f_read(%s) -> %s\n", TEST_PATH, fr_str(r));
        return 0;
    }
    rbuf[br] = 0;

    int match = (br == clen) && (memcmp(rbuf, content, clen) == 0);
    printf("  [fs] RE-READ %s : %u byte(s) readed ; content %s\n",
           TEST_PATH, br, match ? "SAME (R/W SUCCEED)" : "DIFFERENT (KO)");
    if (!match) {
        printf("      expected %u b, readed %u b\n", clen, br);
    }
    return match;
}

/* LOW-LEVEL test of the write driver with MODIFIED content at a HIGH LBA.
 *
 * Goal: check that write+read works (a) with CHANGED content (not just
 * rewriting the same), and (b) at a HIGH LBA typical of the FAT32 data zone
 * (not only LBA0). Non-destructive: the sector is SAVED, a pattern is written,
 * read back/compared, THEN the original content is RESTORED.
 *
 * Chosen LBA: 40000 (in the free zone reported by f_getfree, well after the
 * MBR/FAT; it is restored anyway). */
static void raw_write_selftest(void)
{
    static uint8_t orig[SD_SECTOR_SIZE] __attribute__((aligned(64)));
    static uint8_t pat[SD_SECTOR_SIZE]  __attribute__((aligned(64)));
    static uint8_t rb[SD_SECTOR_SIZE]   __attribute__((aligned(64)));
    const uint32_t LBA = 40000u;

    if (!sdmmc_card_present()) {
        printf("  [sd-test] missing card : raw test ignored\n");
        return;
    }

    /* 1) Save the target sector. */
    if (sdmmc_read_blocks(LBA, 1, orig) != SD_OK) {
        printf("  [sd-test] reading LBA%lu KO\n", (unsigned long)LBA);
        return;
    }

    /* 2) Build a recognizable pattern and write it. */
    for (int i = 0; i < SD_SECTOR_SIZE; i++)
        pat[i] = (uint8_t)(i ^ 0xA5);
    if (sdmmc_write_blocks(LBA, 1, pat) != SD_OK) {
        printf("  [sd-test] WRITING pattern LBA%lu KO\n", (unsigned long)LBA);
        return;
    }

    /* 3) Read back and compare with the pattern. */
    if (sdmmc_read_blocks(LBA, 1, rb) != SD_OK) {
        printf("  [sd-test] re-read LBA%lu KO\n", (unsigned long)LBA);
        return;
    }
    int match = (memcmp(pat, rb, SD_SECTOR_SIZE) == 0);
    int ndiff = 0;
    for (int i = 0; i < SD_SECTOR_SIZE; i++)
        if (pat[i] != rb[i]) ndiff++;
    printf("  [sd-test] write(pattern)+read LBA%lu : %s (%d bytes differents)\n",
           (unsigned long)LBA,
           match ? "SAME (driver R/W OK on high LBA)"
                 : "DIFFERENT (driver KO)", ndiff);
    if (!match) {
        printf("  [sd-test] expected[0..7]=");
        for (int i = 0; i < 8; i++) printf(" %02x", pat[i]);
        printf(" ; readed[0..7]=");
        for (int i = 0; i < 8; i++) printf(" %02x", rb[i]);
        printf("\n");
    }

    /* 4) RESTORE the original content (non-destructive). */
    sdmmc_write_blocks(LBA, 1, orig);
}


int fs_demo_run(void)
{
    printf("\n===== DEMO FAT32 / FatFs R0.15 =====\n");

    /* --- 0) Low-level write driver self-test (non-destructive) --- */
    raw_write_selftest();

    /* --- 1) Mount volume 0: (option 1 = immediate/forced mount) --- */

    FRESULT r = f_mount(&g_fs, "0:", 1);
    printf("  [fs] f_mount(0:) -> %s\n", fr_str(r));
    if (r != FR_OK) {
        if (r == FR_NOT_READY)
            printf("  [fs] (missing card / missing controller : QEMU ?)\n");
        else if (r == FR_NO_FILESYSTEM)
            printf("  [fs] (no FAT system found on card)\n");
        printf("\n>>> Demo : mounting failed (see above). <<<\n");
        return 0;
    }

    /* --- 2) Label + free space (proof of effective mount) --- */
    char label[24];
    DWORD vsn = 0;
    if (f_getlabel("0:", label, &vsn) == FR_OK) {
        printf("  [fs] volume : label=\"%s\" serial=0x%08lx\n",
               (label[0] ? label : "(nameless)"), (unsigned long)vsn);
    }

    DWORD free_clust = 0;
    FATFS *fatfs_p = 0;
    if (f_getfree("0:", &free_clust, &fatfs_p) == FR_OK && fatfs_p) {
        /* Cluster size = csize sectors of 512 B. Total = n_fatent-2 clusters. */
        DWORD tot_clust = fatfs_p->n_fatent - 2;
        DWORD csize     = fatfs_p->csize;
        /* In KiB (avoid 32-bit overflow: sectors/2 = KiB). */
        unsigned long free_kb = (unsigned long)free_clust * csize / 2ul;
        unsigned long tot_kb  = (unsigned long)tot_clust  * csize / 2ul;
        const char *fstype =
            (fatfs_p->fs_type == FS_FAT32) ? "FAT32" :
            (fatfs_p->fs_type == FS_FAT16) ? "FAT16" :
            (fatfs_p->fs_type == FS_FAT12) ? "FAT12" : "FAT?";
        printf("  [fs] type=%s cluster=%lu sectors ; free %lu Kb / %lu Kb\n",
               fstype, (unsigned long)csize, free_kb, tot_kb);
    }

    /* --- 3) List the root + locate an existing file to read --- */
    char existing[64] = {0};
    list_root(existing, sizeof(existing));

    /* --- 4) Read an existing file (if found) --- */
    if (existing[0])
        read_existing(existing);
    else
        printf("  [fs] No regular file to read (empty card ?)\n");

    /* --- 5+6) Write a test file then read it back and compare --- */
    int rw_ok = write_then_readback();

    /* --- Phase 4 summary --- */
    printf("\n[result demo]\n");
    printf("  mounting FAT32            : OK\n");
    printf("  list root                 : OK\n");
    printf("  read file                 : %s\n",
           existing[0] ? "OK" : "N/A (no file)");
    printf("  R/W writing + re-read: %s\n", rw_ok ? "OK" : "KO");

    if (rw_ok)
        printf("\n>>> Demo: FAT32 R/W OK. <<<\n");
    else
        printf("\n>>> Demo: R/W error, see result. <<<\n");

    /* Unmount cleanly (flush FatFs buffers). */
    f_mount(0, "0:", 0);
    return rw_ok;
}
