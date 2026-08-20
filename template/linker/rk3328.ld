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
 * rk3328.ld — Linker script for OROS baremetal AArch64 (RK3328 / Orange Pi R1 Plus LTS)
 * bring-up UART.
 *
 * The RK3328 has 1 GB of DDR starting at 0x0000_0000. U-Boot loads our image
 * at a load address (LOAD_ADDR). We choose 0x0020_0000 (2 MiB),
 * a comfortable free RAM area above the first few megabytes reserved for boot.
 *
 * QEMU test address (machine 'virt'): RAM starts at 0x4000_0000.
 * We provide two values through the Makefile (LOAD_ADDR) to target either platform.
 */

ENTRY(_start)

/* LOAD_ADDR is provided by Makefile ( -Wl,--defsym=... ) ; default value below */
PROVIDE(LOAD_ADDR = 0x00200000);

SECTIONS
{
    . = LOAD_ADDR;

    .text : ALIGN(0x1000)
    {
        KEEP(*(.text.boot))     /* _start must be at the very beginning of the image */
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    }

    .rodata : ALIGN(0x1000)
    {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    }

    .data : ALIGN(0x1000)
    {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
    }

    /* BSS : erased by start.S before entering in C */
    .bss (NOLOAD) : ALIGN(0x1000)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(0x1000);

    /* Heap : used by _sbrk (newlib). Fixed size.
     * 8 MiB: the SFTP service (wolfSSH) allocates its per-request state,
     * transfer buffers and WS_SFTPNAME directory-listing nodes from the heap.
     * The RK3328 has 1 GiB of DDR and the image is loaded at 2 MiB, so this
     * larger heap stays well within the mapped Normal Memory range. */
    __heap_start = .;
    . = . + 0x00800000;     /* 8 MiB of heap */
    __heap_end = .;

    . = ALIGN(0x1000);

    /* Initial stack (boot mode / current EL). 64 KiB. */
    . = . + 0x00010000;
    __stack_top = .;

    __image_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.interp)
    }
}
