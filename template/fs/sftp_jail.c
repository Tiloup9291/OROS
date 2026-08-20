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
 * sftp_jail.c — FatFs "chroot" layer for the SFTP service (wolfSSH).
 *
 * Bridges the POSIX namespace wolfSSH speaks ("/srv/...") to the FatFs one
 * ("0:/srv/..."), and enforces — as a second, independent barrier on top of
 * wolfSSH's own confinement — that nothing ever escapes /srv.
 *
 * Path handling is 100% lexical, which is sound here: FAT has no symbolic
 * links, so a normalized path cannot be re-pointed elsewhere after the check
 * (no TOCTOU window of the kind wolfSSH warns about for POSIX).
 *
 * See fs/sftp_jail.h for the rationale and the full API contract.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sftp_jail.h"
#include "fs_mount.h"

/* Jail root existence, probed once by oros_sftp_jail_init(). */
static int g_jail_ready;

/* ------------------------------------------------------------------ */
/* Path normalization + confinement                                   */
/* ------------------------------------------------------------------ */

/* Both separators are accepted on input: SFTP clients normally send '/', but
 * wolfSSH's own helpers tolerate '\' and we must not let it slip through
 * unnormalized (it would otherwise defeat the ".." handling below). */
static int jail_is_delim(char c)
{
    return (c == '/' || c == '\\');
}

/*
 * Normalizes `in` into `out` as an absolute FatFs path under the jail.
 *
 * Algorithm: split on separators, then maintain an explicit stack of segment
 * start offsets inside `out`. "." is dropped, ".." pops one segment, and a
 * ".." that would pop past the jail root is REJECTED (FR_DENIED) rather than
 * clamped — clamping would silently turn an escape attempt into a valid
 * request on the root, which is exactly what we do not want.
 */
FRESULT oros_sftp_jail_resolve(const char *in, char *out, unsigned out_sz)
{
    /* Offsets in `out` where each accepted path segment starts. Depth is
     * bounded by the path length, but a fixed cap keeps this stack-friendly
     * (this runs on Core2 with a 32 KiB thread stack). */
    unsigned seg_start[32];
    unsigned depth = 0;
    unsigned len;
    unsigned root_len;

    if (in == NULL || out == NULL)
        return FR_INVALID_PARAMETER;

    /* The physical jail root ("0:/srv") is the immutable prefix of the
     * result; segments are appended after it and can never pop below it. */
    root_len = (unsigned)strlen(OROS_SFTP_JAIL_PHYS);
    if (out_sz <= root_len + 1u)
        return FR_INVALID_NAME;
    memcpy(out, OROS_SFTP_JAIL_PHYS, root_len);
    out[root_len] = '\0';
    len = root_len;

    /* Strip a drive prefix a hostile client may have smuggled in ("0:/etc",
     * "C:\x"): the jail drive is imposed by us, never taken from the peer. */
    if (in[0] != '\0' && in[1] == ':')
        in += 2;

    /* Strip the logical jail root when wolfSSH hands us a path already
     * expressed in the client namespace ("/srv/cfg" -> "cfg"). Only a real
     * segment match counts, so "/srvxyz" is NOT treated as being in the jail
     * and will simply be resolved as a segment named "srvxyz". */
    {
        unsigned jr_len = (unsigned)strlen(OROS_SFTP_JAIL_ROOT);
        if (strncmp(in, OROS_SFTP_JAIL_ROOT, jr_len) == 0 &&
                (in[jr_len] == '\0' || jail_is_delim(in[jr_len])))
            in += jr_len;
    }

    while (*in != '\0') {
        const char *seg;
        unsigned seg_len;

        /* Skip run of separators (also collapses "//"). */
        while (jail_is_delim(*in))
            in++;
        if (*in == '\0')
            break;

        seg = in;
        while (*in != '\0' && !jail_is_delim(*in))
            in++;
        seg_len = (unsigned)(in - seg);

        /* "." : current directory, nothing to do. */
        if (seg_len == 1u && seg[0] == '.')
            continue;

        /* ".." : pop one segment, or refuse to climb out of the jail. */
        if (seg_len == 2u && seg[0] == '.' && seg[1] == '.') {
            if (depth == 0u)
                return FR_DENIED;       /* escape attempt above /srv */
            depth--;
            len = seg_start[depth] - 1u;  /* drop the leading '/' too */
            out[len] = '\0';
            continue;
        }

        if (depth >= (sizeof(seg_start) / sizeof(seg_start[0])))
            return FR_INVALID_NAME;     /* path nested too deeply */

        /* Append "/<segment>", keeping room for the NUL terminator. */
        if (len + 1u + seg_len + 1u > out_sz)
            return FR_INVALID_NAME;
        out[len++] = '/';
        seg_start[depth++] = len;
        memcpy(out + len, seg, seg_len);
        len += seg_len;
        out[len] = '\0';
    }

    /* Final safety net: whatever the input did, the result must still start
     * with the physical jail root. Cheap, and catches any future edit of the
     * loop above that would break the invariant. */
    if (strncmp(out, OROS_SFTP_JAIL_PHYS, root_len) != 0)
        return FR_DENIED;

    return FR_OK;
}

/* ------------------------------------------------------------------ */
/* Jail bring-up                                                      */
/* ------------------------------------------------------------------ */
int oros_sftp_jail_init(void)
{
    FILINFO fno;
    FRESULT r;

    g_jail_ready = 0;

    /* The jail lives on the micro-SD FAT volume: without it, no SFTP. */
    if (!fs_mount_ready()) {
        printf("[sftp] volume 0: not mounted : %s unavailable, SFTP disabled.\n",
               OROS_SFTP_JAIL_ROOT);
        return -1;
    }

    r = f_stat(OROS_SFTP_JAIL_PHYS, &fno);
    if (r == FR_NO_FILE || r == FR_NO_PATH) {
        /* First boot on a fresh card: create the jail root. */
        r = f_mkdir(OROS_SFTP_JAIL_PHYS);
        if (r != FR_OK) {
            printf("[sftp] ERROR: mkdir(%s) -> %d : SFTP disabled.\n",
                   OROS_SFTP_JAIL_PHYS, (int)r);
            return -1;
        }
        printf("[sftp] created %s on volume 0:.\n", OROS_SFTP_JAIL_PHYS);
    }
    else if (r != FR_OK) {
        printf("[sftp] ERROR: stat(%s) -> %d : SFTP disabled.\n",
               OROS_SFTP_JAIL_PHYS, (int)r);
        return -1;
    }
    else if (!(fno.fattrib & AM_DIR)) {
        printf("[sftp] ERROR: %s is a file, not a directory : SFTP disabled.\n",
               OROS_SFTP_JAIL_PHYS);
        return -1;
    }

    g_jail_ready = 1;
    printf("[sftp] jail root %s -> %s (clients are confined to %s).\n",
           OROS_SFTP_JAIL_ROOT, OROS_SFTP_JAIL_PHYS, OROS_SFTP_JAIL_ROOT);
    return 0;
}

int oros_sftp_jail_ready(void)
{
    return g_jail_ready;
}

/* ------------------------------------------------------------------ */
/* Path-based FatFs wrappers                                          */
/* ------------------------------------------------------------------ */
FRESULT oros_ff_stat(const char *path, FILINFO *fno)
{
    char p[OROS_SFTP_JAIL_PATH_MAX];
    FRESULT r = oros_sftp_jail_resolve(path, p, sizeof(p));

    if (r != FR_OK)
        return r;

    /* FatFs rejects a bare volume root ("0:/") with FR_INVALID_NAME, so the
     * jail root itself is reported synthetically as a directory. */
    if (strcmp(p, OROS_SFTP_JAIL_PHYS) == 0) {
        if (fno == NULL)
            return FR_INVALID_PARAMETER;
        memset(fno, 0, sizeof(*fno));
        fno->fattrib = AM_DIR;
        fno->fname[0] = '/';
        fno->fname[1] = '\0';
        return FR_OK;
    }
    return f_stat(p, fno);
}

FRESULT oros_ff_unlink(const char *path)
{
    char p[OROS_SFTP_JAIL_PATH_MAX];
    FRESULT r = oros_sftp_jail_resolve(path, p, sizeof(p));

    if (r != FR_OK)
        return r;
    /* Never let a client delete the jail root itself. */
    if (strcmp(p, OROS_SFTP_JAIL_PHYS) == 0)
        return FR_DENIED;
    return f_unlink(p);
}

FRESULT oros_ff_rename(const char *path_old, const char *path_new)
{
    char po[OROS_SFTP_JAIL_PATH_MAX];
    char pn[OROS_SFTP_JAIL_PATH_MAX];
    FRESULT r;

    /* BOTH ends must be inside the jail: renaming is a move, so an
     * unchecked destination would be a write primitive outside /srv. */
    r = oros_sftp_jail_resolve(path_old, po, sizeof(po));
    if (r != FR_OK)
        return r;
    r = oros_sftp_jail_resolve(path_new, pn, sizeof(pn));
    if (r != FR_OK)
        return r;

    if (strcmp(po, OROS_SFTP_JAIL_PHYS) == 0 ||
            strcmp(pn, OROS_SFTP_JAIL_PHYS) == 0)
        return FR_DENIED;
    return f_rename(po, pn);
}

FRESULT oros_ff_mkdir(const char *path)
{
    char p[OROS_SFTP_JAIL_PATH_MAX];
    FRESULT r = oros_sftp_jail_resolve(path, p, sizeof(p));

    if (r != FR_OK)
        return r;
    if (strcmp(p, OROS_SFTP_JAIL_PHYS) == 0)
        return FR_EXIST;
    return f_mkdir(p);
}

FRESULT oros_ff_opendir(DIR *dp, const char *path)
{
    char p[OROS_SFTP_JAIL_PATH_MAX];
    FRESULT r = oros_sftp_jail_resolve(path, p, sizeof(p));

    if (r != FR_OK)
        return r;
    return f_opendir(dp, p);
}

char *oros_ff_getcwd(char *r, int r_sz)
{
    unsigned need = (unsigned)strlen(OROS_SFTP_JAIL_ROOT) + 1u;

    if (r == NULL || r_sz <= 0 || (unsigned)r_sz < need)
        return NULL;
    memcpy(r, OROS_SFTP_JAIL_ROOT, need);
    return r;
}

/* ------------------------------------------------------------------ */
/* Stream open (WFOPEN)                                               */
/* ------------------------------------------------------------------ */
int oros_ff_fopen(FIL **f, const char *filename, const char *mode)
{
    char p[OROS_SFTP_JAIL_PATH_MAX];
    FRESULT r;
    BYTE m = 0;

    if (f == NULL || mode == NULL)
        return -1;

    r = oros_sftp_jail_resolve(filename, p, sizeof(p));
    if (r != FR_OK)
        return (int)r;

    /* Map the "r"/"w" mode string onto FatFs access flags. Unlike upstream
     * ff_fopen(), FA_CREATE_NEW is NOT forced on read-only opens: that would
     * make every plain read of an existing file fail with FR_EXIST. */
    if (strchr(mode, 'r') != NULL && strchr(mode, 'w') != NULL)
        m = FA_READ | FA_WRITE | FA_OPEN_ALWAYS;
    else if (strchr(mode, 'w') != NULL)
        m = FA_WRITE | FA_CREATE_ALWAYS;
    else
        m = FA_READ;

    if (strchr(mode, 'a') != NULL)
        m = FA_WRITE | FA_OPEN_APPEND;

    if (*f == NULL) {
        *f = (FIL *)malloc(sizeof(FIL));
        if (*f == NULL)
            return -1;
    }

    r = f_open(*f, p, m);
    return (r == FR_OK) ? 0 : (int)r;
}

/* ------------------------------------------------------------------ */
/* Descriptor pool (WOPEN / WCLOSE / WPREAD / WPWRITE)                */
/* ------------------------------------------------------------------ */
/*
 * SFTP is an offset-based protocol: READ/WRITE carry an explicit file offset
 * and may arrive out of order, so each access seeks before transferring
 * instead of relying on the FatFs file pointer.
 *
 * The pool is static and deliberately small (one SFTP session at a time, and
 * a FIL embeds a 512-byte sector window).
 */
#ifndef OROS_SFTP_JAIL_MAX_FILES
#define OROS_SFTP_JAIL_MAX_FILES 4
#endif

static struct {
    FIL f;
    int used;
} g_fd_pool[OROS_SFTP_JAIL_MAX_FILES];

int oros_ff_open(const char *fname, int flag, int perm)
{
    char p[OROS_SFTP_JAIL_PATH_MAX];
    FRESULT r;
    int i;

    (void)perm;   /* FAT has no POSIX permission bits */

    r = oros_sftp_jail_resolve(fname, p, sizeof(p));
    if (r != FR_OK)
        return -1;

    for (i = 0; i < OROS_SFTP_JAIL_MAX_FILES; i++) {
        if (g_fd_pool[i].used)
            continue;
        if (f_open(&g_fd_pool[i].f, p, (BYTE)flag) != FR_OK)
            return -1;
        g_fd_pool[i].used = 1;
        return i;
    }
    return -1;    /* pool exhausted */
}

int oros_ff_close(int fd)
{
    if (fd < 0 || fd >= OROS_SFTP_JAIL_MAX_FILES)
        return -1;
    if (!g_fd_pool[fd].used)
        return -1;
    f_close(&g_fd_pool[fd].f);
    g_fd_pool[fd].used = 0;
    return 0;
}

int oros_ff_pread(int fd, unsigned char *buffer, int sz, const unsigned int *ofst)
{
    UINT done = 0;

    if (fd < 0 || fd >= OROS_SFTP_JAIL_MAX_FILES || sz < 0)
        return -1;
    if (!g_fd_pool[fd].used)
        return -1;

    /* wolfSSH passes the offset as a 2-word32 little-endian pair; only the
     * low word is meaningful here (FF_LBA64=0, files are < 4 GiB). */
    if (ofst != NULL && f_lseek(&g_fd_pool[fd].f, (FSIZE_t)ofst[0]) != FR_OK)
        return -1;

    if (f_read(&g_fd_pool[fd].f, buffer, (UINT)sz, &done) != FR_OK)
        return -1;
    return (int)done;
}

int oros_ff_pwrite(int fd, const unsigned char *buffer, int sz,
                   const unsigned int *ofst)
{
    UINT done = 0;

    if (fd < 0 || fd >= OROS_SFTP_JAIL_MAX_FILES || sz < 0)
        return -1;
    if (!g_fd_pool[fd].used)
        return -1;

    if (ofst != NULL && f_lseek(&g_fd_pool[fd].f, (FSIZE_t)ofst[0]) != FR_OK)
        return -1;

    if (f_write(&g_fd_pool[fd].f, buffer, (UINT)sz, &done) != FR_OK)
        return -1;

    /* Flush metadata as we go: a session cut mid-transfer must not leave the
     * FAT inconsistent (there is no unmount path in production). */
    f_sync(&g_fd_pool[fd].f);
    return (int)done;
}
