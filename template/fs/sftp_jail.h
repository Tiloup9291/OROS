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
 * sftp_jail.h — FatFs "chroot" layer for the SFTP service (wolfSSH).
 *
 * PURPOSE. The SFTP client must only ever see the subtree /srv, which is
 * physically stored as "0:/srv" on the micro-SD FAT volume. wolfSSH hands its
 * filesystem port ABSOLUTE, already canonicalized POSIX-style paths such as
 * "/srv/cfg/plc.cfg"; FatFs on the other hand expects a drive-prefixed path
 * such as "0:/srv/cfg/plc.cfg" (FF_VOLUMES=1, FF_STR_VOLUME_ID=0). This module
 * is the single place where the two namespaces are bridged:
 *
 *     wolfSSH  "/srv/cfg/plc.cfg"  --oros_ff_*-->  FatFs  "0:/srv/cfg/plc.cfg"
 *
 * CONFINEMENT (second barrier). wolfSSH already refuses requests resolving
 * outside its default path (wolfSSH_SFTP_SetDefaultPath("/srv") + the
 * GetAndCleanPath prefix check). This layer is a deliberate SECOND, independent
 * barrier: every path is re-normalized here (removal of ".", "..", duplicate
 * separators and any accidental drive prefix) and re-checked against the jail
 * root AFTER normalization. Anything that does not resolve strictly inside
 * /srv is rejected with FR_DENIED and never reaches FatFs. A FAT filesystem
 * has no symbolic links, so a purely lexical check is sound here (this is why
 * wolfSSH does not define WOLFSSH_HAVE_SYMLINK for the FatFs port).
 *
 * These oros_ff_* wrappers are wired into wolfSSH through the WOLFSSH_FATFS
 * macro block of lib/wolfssh/wolfssh/port.h, guarded by OROS_SFTP_JAIL.
 *
 * RULE: as for the rest of FatFs in OROS, these functions are only usable from
 * Core2 (IO_SOFT), which owns the blocking SDMMC PIO accesses.
 */
#ifndef OROS_FS_SFTP_JAIL_H
#define OROS_FS_SFTP_JAIL_H

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Jail root as seen by the SFTP client (POSIX-style, no drive prefix). */
#ifndef OROS_SFTP_JAIL_ROOT
#define OROS_SFTP_JAIL_ROOT     "/srv"
#endif

/* FatFs drive holding the jail (the permanently mounted micro-SD volume). */
#ifndef OROS_SFTP_JAIL_DRIVE
#define OROS_SFTP_JAIL_DRIVE    "0:"
#endif

/* Physical FatFs path of the jail root: "0:/srv". */
#define OROS_SFTP_JAIL_PHYS     OROS_SFTP_JAIL_DRIVE OROS_SFTP_JAIL_ROOT

/* Upper bound on a translated FatFs path. Must stay >= the longest path
 * wolfSSH can hand us (WOLFSSH_MAX_FILENAME) plus the "0:" drive prefix. */
#ifndef OROS_SFTP_JAIL_PATH_MAX
#define OROS_SFTP_JAIL_PATH_MAX 320
#endif

/*
 * oros_sftp_jail_init — creates the jail root "0:/srv" if it does not exist
 * yet. To be called once at boot, AFTER fs_mount_init() (the volume must be
 * mounted). Emits one status line, or one error line.
 *
 * Returns 0 when the jail root exists and is a directory, non-zero otherwise.
 * A failure is NOT fatal: SFTP simply refuses sessions (the rest of the system
 * keeps running), exactly like the FAT volume being absent.
 */
int oros_sftp_jail_init(void);

/* 1 if the jail root is present and usable (SFTP can accept a session). */
int oros_sftp_jail_ready(void);

/*
 * oros_sftp_jail_resolve — normalizes `in` (a POSIX path coming from wolfSSH)
 * and translates it into an absolute FatFs path written to `out`.
 *
 * Handles "", "/", ".", "..", duplicated '/' and '\' separators, and strips any
 * drive prefix a client might try to smuggle in. Refuses to escape the jail:
 * a ".." that would climb above /srv is rejected, NOT silently clamped.
 *
 * Returns FR_OK on success, FR_DENIED when the path escapes the jail, or
 * FR_INVALID_NAME when the result would not fit in `out`.
 */
FRESULT oros_sftp_jail_resolve(const char *in, char *out, unsigned out_sz);

/* ------------------------------------------------------------------ */
/* FatFs wrappers used by the wolfSSH FATFS port (see port.h).         */
/* Each one resolves + confines its path argument, then forwards to    */
/* the real FatFs call. Signatures mirror the FatFs ones so the port.h */
/* macros stay a straight substitution.                                */
/* ------------------------------------------------------------------ */
FRESULT oros_ff_stat(const char *path, FILINFO *fno);
FRESULT oros_ff_unlink(const char *path);
FRESULT oros_ff_rename(const char *path_old, const char *path_new);
FRESULT oros_ff_mkdir(const char *path);
FRESULT oros_ff_opendir(DIR *dp, const char *path);

/*
 * oros_ff_fopen — jailed variant of wolfSSH's ff_fopen(): opens (allocating the
 * FIL when *f is NULL) using an "r"/"w" style mode string. Returns 0 on
 * success, non-zero otherwise (wfopen convention).
 */
int oros_ff_fopen(FIL **f, const char *filename, const char *mode);

/*
 * Descriptor-based API backing wolfSSH's WOPEN/WCLOSE/WPREAD/WPWRITE. Mirrors
 * the ff_open/ff_close/ff_pread/ff_pwrite helpers of wolfsftp.c, but with the
 * jail translation applied and with an explicit offset (the SFTP protocol is
 * offset-based, so a stateful file pointer is not enough).
 *
 * oros_ff_open returns a small non-negative descriptor, or -1 on error.
 */
int oros_ff_open(const char *fname, int flag, int perm);
int oros_ff_close(int fd);
int oros_ff_pread(int fd, unsigned char *buffer, int sz, const unsigned int *ofst);
int oros_ff_pwrite(int fd, const unsigned char *buffer, int sz,
                   const unsigned int *ofst);

/*
 * oros_ff_getcwd — always reports the jail root ("/srv").
 *
 * Two reasons: (1) inside a chroot the current directory IS the jail root, and
 * (2) our FatFs build has FF_FS_RPATH=0, so f_getcwd() is NOT compiled in at
 * all — wolfSSH's WGETCWD would otherwise be an unresolved symbol at link time.
 * Returns `r` on success, NULL if the buffer is too small.
 */
char *oros_ff_getcwd(char *r, int r_sz);

#ifdef __cplusplus
}
#endif

#endif /* OROS_FS_SFTP_JAIL_H */
