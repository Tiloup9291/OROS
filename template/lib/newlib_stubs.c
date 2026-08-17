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
 * newlib_stubs.c — Bare-metal syscall stubs for newlib
 *
 * Newlib relies on a handful of low-level "syscalls" to implement printf,
 * malloc, etc. Here, NO Linux kernel calls are made: these are OUR bare-metal
 * implementations.
 *
 *   _write  -> routes stdout/stderr to the UART
 *   _sbrk   -> allocates from the heap defined by the linker script
 *   _read/_close/_fstat/_isatty/_lseek/_kill/_getpid/_exit -> minimal stubs
 *
 * Indepedent of Linux: no Linux kernel header nor Linux sys/syscall.h is included.
 * Only the headers provided by newlib itself are used.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>

#include "../drivers/uart/uart.h"

/* newlib errno */
#undef errno
extern int errno;

/* Symbols provided by the linker script (rk3328.ld) */
extern char __heap_start;
extern char __heap_end;

/* Standard descriptors */
#define STDIN_FD   0
#define STDOUT_FD  1
#define STDERR_FD  2

/* -------------------------------------------------------------------------
 * _write : character output. stdout and stderr are routed to the UART.
 * ------------------------------------------------------------------------- */
ssize_t _write(int fd, const void *buf, size_t count)
{
    if (fd == STDOUT_FD || fd == STDERR_FD) {
        return (ssize_t)uart_write((const char *)buf, count);
    }
    errno = EBADF;
    return -1;
}

/* -------------------------------------------------------------------------
 * _read : input.
 * Returns 0 = EOF to avoid blocking.
 * ------------------------------------------------------------------------- */
ssize_t _read(int fd, void *buf, size_t count)
{
    (void)fd; (void)buf; (void)count;
    return 0;
}

/* -------------------------------------------------------------------------
 * _sbrk : incremental heap allocator used by malloc.
 * Allocates linearly within [__heap_start, __heap_end).
 * ------------------------------------------------------------------------- */
void *_sbrk(ptrdiff_t incr)
{
    static char *heap_ptr = NULL;
    char *prev;

    if (heap_ptr == NULL)
        heap_ptr = &__heap_start;

    /* Heap overflow -> clean failure */
    if (heap_ptr + incr > &__heap_end) {
        errno = ENOMEM;
        return (void *)-1;
    }

    prev = heap_ptr;
    heap_ptr += incr;
    return (void *)prev;
}

/* -------------------------------------------------------------------------
 * Remaining stubs: minimal, consistent behaviour for a bare-metal
 * environment without a filesystem or processes.
 * ------------------------------------------------------------------------- */
int _close(int fd)
{
    (void)fd;
    return -1;
}

int _fstat(int fd, struct stat *st)
{
    (void)fd;
    /* Everything is declared as a character device (console). */
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd)
{
    (void)fd;
    return 1;   /* interactive console */
}

off_t _lseek(int fd, off_t offset, int whence)
{
    (void)fd; (void)offset; (void)whence;
    return 0;
}

int _kill(int pid, int sig)
{
    (void)pid; (void)sig;
    errno = EINVAL;
    return -1;
}

int _getpid(void)
{
    return 1;
}

void _exit(int status)
{
    (void)status;
    /* No host OS: loop forever. */
    for (;;) {
        __asm__ volatile("wfe");
    }
}
