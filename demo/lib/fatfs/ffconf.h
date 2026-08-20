/*---------------------------------------------------------------------------/
/  FatFs R0.15 Configuration for OROS (FAT32)
/---------------------------------------------------------------------------/
/  Adapted from the ffconf.h template provided by ChaN (FatFs R0.15).
/  Choices made for this bare-metal project:
/    - Read AND write (FF_FS_READONLY = 0).
/    - FAT12/16/32 (FAT32 required). exFAT disabled (paid/64-bit unnecessary here).
/    - LFN (long file names) enabled, buffer on the stack, OEM/ANSI (SFN 437).
/    - 1 logical volume, one partition (SFD/AUTO mode).
/    - No mkfs/fdisk (the card is already FAT32).
/    - No FatFs internal thread-safety: access the FS from
/      ONE core (Core 2 / IO_SOFT), so FF_FS_REENTRANT = 0. (Enable with
/      a mutex if multiple threads ever share the same volume.)
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80286	/* Revision ID (must == FF_DEFINED in ff.h, R0.15) */


/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
/* 0: Read/Write or 1: Read only */

#define FF_FS_MINIMIZE	0
/* 0 to 3: function reduction level. 0 = all functions. */

#define FF_USE_FIND		1
/* f_findfirst/f_findnext (useful for listing). */

#define FF_USE_MKFS		0
/* f_mkfs() disabled (the card is not formatted). */

#define FF_USE_FASTSEEK	0

#define FF_USE_EXPAND	0

#define FF_USE_CHMOD	0

#define FF_USE_LABEL	1
/* f_getlabel/f_setlabel — useful for displaying the volume name. */

#define FF_USE_FORWARD	0

#define FF_USE_STRFUNC	1
#define FF_PRINT_LLI	1
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	3
/* f_gets/f_putc/f_puts/f_printf enabled (convenient for writing text). */

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437
/* OEM code page (437 = US). */

#define FF_USE_LFN		2
#define FF_MAX_LFN		255
/* 0: SFN only, 1: LFN static buffer, 2: LFN on the stack, 3: LFN on the heap.
 * 2 = LFN buffer allocated on the function stack (no heap required). */

#define FF_LFN_UNICODE	0
/* 0: ANSI/OEM API (BYTE char). */

#define FF_LFN_BUF		255
#define FF_SFN_BUF		12

#define FF_FS_RPATH		0
/* 0: no relative paths (absolute paths such as "0:/..." are used). */

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES		1
/* One logical volume. */

#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"

#define FF_MULTI_PARTITION	0
/* 0: each volume = one physical drive (the first FAT partition is mounted
 * automatically by find_volume). */

#define FF_MIN_SS		512
#define FF_MAX_SS		512
/* Logical sector size fixed at 512 bytes (standard SD card). */

#define FF_LBA64		0
/* 32-bit LBA addressing (sufficient: a 32 GB card has ~61 M sectors < 2^32). */

#define FF_MIN_GPT		0x10000000

#define FF_USE_TRIM		0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY		0
/* 0: one sector buffer per file object (faster). */

#define FF_FS_EXFAT		0
/* exFAT disabled. */

#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
/* No RTC: get_fattime() returns a fixed date. */

#define FF_FS_NOFSINFO	0

#define FF_FS_LOCK		0
/* No file locking (single-threaded access in the demo). */

#define FF_FS_REENTRANT	0
/* FatFs is not reentrant (single core for the FS). */

#define FF_FS_TIMEOUT	1000
#define FF_SYNC_t		HANDLE

/*--- End of configuration options ---*/
