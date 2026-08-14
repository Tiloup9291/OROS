/*-----------------------------------------------------------------------*/
/* diskio.c — Couche de portage FatFs <-> driver SDMMC (RTOS RK3328)      */
/*                                                                        */
/* Phase 4 : relie l'interface disque de bas niveau de FatFs (ChaN) au    */
/* pilote SD/MMC DesignWare (drivers/sdmmc). Un seul lecteur physique     */
/* (pdrv = 0) = la carte micro-SD.                                        */
/*                                                                        */
/* En QEMU (MMU_QEMU), le driver SDMMC renvoie SD_ENODEV : disk_status    */
/* signale alors STA_NODISK et FatFs échoue proprement (pas de carte).    */
/*-----------------------------------------------------------------------*/

#include "ff.h"			/* Obtains integer types */
#include "diskio.h"		/* Declarations of disk functions */

#include "../../drivers/sdmmc/sdmmc.h"

/* Un seul lecteur physique. */
#define DEV_SD		0

/* Cache de l'état d'init (évite de ré-initialiser la carte à chaque montage). */
static DSTATUS g_sd_stat = STA_NOINIT;


/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber to identify the drive */
)
{
	if (pdrv != DEV_SD)
		return STA_NOINIT;
	return g_sd_stat;
}


/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive nmuber to identify the drive */
)
{
	if (pdrv != DEV_SD)
		return STA_NOINIT;

	/* Si déjà initialisée avec succès, ne pas refaire toute la séquence. */
	if (!(g_sd_stat & STA_NOINIT) && sdmmc_card_present())
		return g_sd_stat;

	sd_card_t card;
	sd_status_t st = sdmmc_init(&card);
	if (st == SD_OK && sdmmc_card_present()) {
		g_sd_stat = 0;					/* prêt : ni NOINIT ni NODISK */
	} else if (st == SD_ENODEV) {
		g_sd_stat = STA_NOINIT | STA_NODISK;   /* QEMU : pas de contrôleur */
	} else {
		g_sd_stat = STA_NOINIT;			/* échec init */
	}
	return g_sd_stat;
}


/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/
DRESULT disk_read (
	BYTE pdrv,		/* Physical drive nmuber to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
	if (pdrv != DEV_SD)
		return RES_PARERR;
	if (g_sd_stat & STA_NOINIT)
		return RES_NOTRDY;
	if (count == 0)
		return RES_PARERR;

	sd_status_t st = sdmmc_read_blocks((uint32_t)sector, (uint32_t)count, buff);
	return (st == SD_OK) ? RES_OK : RES_ERROR;
}


/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/
#if FF_FS_READONLY == 0

DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
	if (pdrv != DEV_SD)
		return RES_PARERR;
	if (g_sd_stat & STA_NOINIT)
		return RES_NOTRDY;
	if (g_sd_stat & STA_PROTECT)
		return RES_WRPRT;
	if (count == 0)
		return RES_PARERR;

	sd_status_t st = sdmmc_write_blocks((uint32_t)sector, (uint32_t)count, buff);
	return (st == SD_OK) ? RES_OK : RES_ERROR;
}

#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	if (pdrv != DEV_SD)
		return RES_PARERR;
	if (g_sd_stat & STA_NOINIT)
		return RES_NOTRDY;

	switch (cmd) {
	case CTRL_SYNC:
		/* Le driver PIO attend déjà la fin du busy après chaque écriture :
		 * il n'y a pas de cache write-back en attente ici. */
		return RES_OK;

	case GET_SECTOR_COUNT: {
		/* Nombre total de secteurs du lecteur (utile à mkfs, non requis ici
		 * mais renseigné par courtoisie). */
		*(LBA_t *)buff = (LBA_t)0;   /* inconnu sans re-scan CSD : 0 = non fourni */
		return RES_OK;
	}

	case GET_SECTOR_SIZE:
		*(WORD *)buff = SD_SECTOR_SIZE;
		return RES_OK;

	case GET_BLOCK_SIZE:
		/* Taille d'effacement en secteurs (1 = inconnu/pas d'alignement requis). */
		*(DWORD *)buff = 1;
		return RES_OK;

	default:
		return RES_PARERR;
	}
}


/*-----------------------------------------------------------------------*/
/* Horodatage FAT (pas de RTC sur ce board)                              */
/*-----------------------------------------------------------------------*/
/* FatFs appelle get_fattime() pour dater les fichiers créés/modifiés.
 * Faute de RTC, on renvoie une date/heure fixe (2026-01-01 00:00:00).
 * Format FAT : bits 31..25 = année-1980, 24..21 = mois, 20..16 = jour,
 *              15..11 = heure, 10..5 = minute, 4..0 = seconde/2. */
DWORD get_fattime (void)
{
	return  ((DWORD)(2026 - 1980) << 25)   /* année 2026 */
	      | ((DWORD)1 << 21)               /* mois janvier */
	      | ((DWORD)1 << 16)               /* jour 1 */
	      | ((DWORD)0 << 11)               /* heure 0 */
	      | ((DWORD)0 << 5)                /* minute 0 */
	      | ((DWORD)0 >> 1);               /* seconde 0 */
}
