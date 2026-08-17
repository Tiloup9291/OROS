/*---------------------------------------------------------------------------/
/  Configuration de FatFs R0.15 pour le RTOS RK3328 (Phase 4 — FAT32)
/---------------------------------------------------------------------------/
/  Adapté du template ffconf.h fourni par ChaN (FatFs R0.15, licence BSD).
/  Choix retenus pour ce projet baremetal :
/    - Lecture ET écriture (FF_FS_READONLY = 0) : DoD Phase 4.
/    - FAT12/16/32 (FAT32 requis).  exFAT désactivé (payant/64 bits inutile ici).
/    - LFN (noms longs) activé, buffer sur la pile (option 2), OEM/ANSI (SFN 437).
/    - 1 seul volume logique, une seule partition (mode SFD/AUTO).
/    - Pas de mkfs/fdisk (on ne formate pas : la carte est déjà FAT32).
/    - Pas de thread-safety FatFs interne : la démo Phase 4 accède au FS depuis
/      UN SEUL cœur (Core 2 / IO_SOFT), donc FF_FS_REENTRANT = 0. (À activer avec
/      un mutex si un jour plusieurs threads partagent le même volume.)
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80286	/* Revision ID (doit == FF_DEFINED dans ff.h, R0.15) */


/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
/* 0:Read/Write or 1:Read only */

#define FF_FS_MINIMIZE	0
/* 0 to 3 : niveau de réduction des fonctions. 0 = toutes les fonctions. */

#define FF_USE_FIND		1
/* f_findfirst/f_findnext (utile pour lister). */

#define FF_USE_MKFS		0
/* f_mkfs() désactivé (on ne formate pas la carte). */

#define FF_USE_FASTSEEK	0

#define FF_USE_EXPAND	0

#define FF_USE_CHMOD	0

#define FF_USE_LABEL	1
/* f_getlabel/f_setlabel — utile pour afficher le nom de volume. */

#define FF_USE_FORWARD	0

#define FF_USE_STRFUNC	1
#define FF_PRINT_LLI	1
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	3
/* f_gets/f_putc/f_puts/f_printf activés (pratique pour écrire du texte). */

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437
/* Code page OEM (437 = US). */

#define FF_USE_LFN		2
#define FF_MAX_LFN		255
/* 0:SFN seul, 1:LFN buffer statique, 2:LFN sur la pile, 3:LFN sur le heap.
 * 2 = buffer LFN alloué sur la pile de la fonction (pas de heap requis). */

#define FF_LFN_UNICODE	0
/* 0:ANSI/OEM en API (BYTE char). */

#define FF_LFN_BUF		255
#define FF_SFN_BUF		12

#define FF_FS_RPATH		0
/* 0 : pas de chemins relatifs (on utilise des chemins absolus "0:/..."). */

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES		1
/* Un seul volume logique. */

#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"

#define FF_MULTI_PARTITION	0
/* 0 : chaque volume = un lecteur physique (la 1re partition FAT est montée
 * automatiquement par find_volume). */

#define FF_MIN_SS		512
#define FF_MAX_SS		512
/* Secteur logique fixé à 512 o (carte SD standard). */

#define FF_LBA64		0
/* Adressage LBA 32 bits (suffisant : carte 32 Go = ~61 M secteurs < 2^32). */

#define FF_MIN_GPT		0x10000000

#define FF_USE_TRIM		0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY		0
/* 0 : un buffer de secteur par objet fichier (plus rapide). */

#define FF_FS_EXFAT		0
/* exFAT désactivé. */

#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
/* Pas de RTC : get_fattime() renvoie une date fixe. */

#define FF_FS_NOFSINFO	0

#define FF_FS_LOCK		0
/* Pas de verrou de fichier (accès mono-thread dans la démo). */

#define FF_FS_REENTRANT	0
/* Pas de réentrance FatFs (mono-cœur pour le FS). */

#define FF_FS_TIMEOUT	1000
#define FF_SYNC_t		HANDLE

/*--- End of configuration options ---*/
