/*
 * user_settings.h — wolfSSL/wolfCrypt BARE-METAL configuration for
 *                   OROS (AArch64, newlib, GCC). SSH server.
 *
 * Compiled with -DWOLFSSL_USER_SETTINGS: this header is automatically included
 * by wolfssl/wolfcrypt/settings.h BEFORE any other option. It defines a
 * MINIMAL "wolfCrypt-only" profile (NO TLS) sufficient for wolfSSH:
 *   - SINGLE_THREADED (single-threaded, runs on Core2 IO_SOFT in polling mode);
 *   - WOLFSSL_USER_IO (no BSD sockets: I/O through our lwIP raw callbacks);
 *   - NO_FILESYSTEM / NO_WOLFSSL_DIR (no filesystem for crypto);
 *   - time base + RNG wired to our hardware (Generic Timer + PMU);
 *   - modern and LIGHTWEIGHT SSH algorithms: SHA-256/512, HMAC, AES-CTR/GCM,
 *     ChaCha20-Poly1305, Curve25519 (KEX) + Ed25519 (host key);
 *     RSA/ECDSA disabled.
 *
 * Ref.: wolfSSL manual ch.2 (building) + IDE user_settings examples.
 */
#ifndef RTOS_WOLFSSL_USER_SETTINGS_H
#define RTOS_WOLFSSL_USER_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Platform / execution model                                         */
/* ------------------------------------------------------------------ */
#define SINGLE_THREADED            /* no internal wolfSSL threads       */
#define WOLFSSL_USER_IO            /* no BSD sockets: I/O via callbacks */
#define WOLFSSL_NO_SOCK            /* no socket layer                   */
#define NO_WRITEV                  /* no writev()                       */
#define NO_FILESYSTEM              /* no fopen/fread (crypto in RAM)    */
#define NO_WOLFSSL_DIR             /* no directory traversal            */
#define WOLFSSL_IGNORE_FILE_WARN   /* suppress warnings about uncompiled files */
#define NO_MAIN_DRIVER

/* No TLS layer: wolfSSH uses ONLY wolfCrypt. */
#define WOLFCRYPT_ONLY

/* Enable the specific crypto primitives required by wolfSSH. */
#define WOLFSSL_WOLFSSH
#define WOLFSSL_PUBLIC_MP
/* SSH I/O via our callbacks (lwIP raw TCP): no BSD sockets. */
#define WOLFSSH_USER_IO
/* Unused wolfSSH modules (reduce footprint). */
#define WOLFSSH_NO_SCP
#define WOLFSSH_NO_AGENT
#define WOLFSSH_NO_CERTS
#define WOLFSSH_NO_PORT_FORWARD
#define NO_TERMIOS

/* ------------------------------------------------------------------ */
/* SFTP subsystem (shares TCP port 22 with the interactive shell)      */
/* ------------------------------------------------------------------ */
/* SFTP is served on the SAME listener/handshake/auth as the SSH shell:
 * wolfSSH_accept() returns WS_SFTP_COMPLETE when the peer requests the
 * "sftp" subsystem, and net/ssh_server.c then drives wolfSSH_SFTP_read().
 * No second TCP port, no second host key. */
#define WOLFSSH_SFTP

/* Back the SFTP filesystem with OUR FatFs (lib/fatfs), not a wolfSSH copy:
 * the WOLFSSH_FATFS block of wolfssh/port.h does #include <ff.h>, which
 * resolves to lib/fatfs/ff.h thanks to the include order in the Makefile.
 * WOLFSSH_FATFS also takes precedence over the NO_FILESYSTEM defined above
 * for wolfCrypt (see wolfssh/port.h, "NO_FILESYSTEM && !WOLFSSH_FATFS"). */
#define WOLFSSH_FATFS

/* Confine SFTP to /srv (physically 0:/srv). Enables the OROS chroot layer
 * (fs/sftp_jail.c) wired into the WOLFSSH_FATFS macros of wolfssh/port.h.
 * This is the second barrier; the first one is the wolfSSH default-path
 * confinement set up with wolfSSH_SFTP_SetDefaultPath("/srv"). */
#define OROS_SFTP_JAIL

/* wolfsftp.c logs raw FatFs opens through PRINTF(); we have no such macro
 * (and want no console noise per transfer) -> compile it out. */
#define PRINTF(...)   do { } while (0)

/* Static pool of FIL objects held by the SFTP server. 4 instead of the
 * upstream 32: one session at a time, and a FIL embeds a 512-byte sector
 * window (FF_MAX_SS) -> ~2.4 KiB of .bss instead of ~19 KiB. */
#define WOLFSSH_FATFS_MAX_FILES  4

/* Bare-metal sizing: bound the SFTP buffers. The upstream defaults (32 KiB
 * of payload, 1 MiB per NAME listing) are sized for hosted systems and do
 * not fit our lwIP memory pools (TCP_SND_BUF = 8 * MSS) nor our heap. */
#define WOLFSSH_MAX_SFTP_RW      4096
#define WOLFSSH_MAX_SFTP_RECV    4096
#define WOLFSSH_MAX_SFTP_NAME    (32 * 1024)
#define DEFAULT_WINDOW_SZ        (16 * 1024)
#define DEFAULT_MAX_PACKET_SZ    4096

/* Small stacks (bare-metal): prefer the heap for large stack buffers. */
#define WOLFSSL_SMALL_STACK

/* newlib provides malloc/free (via _sbrk). Keep standard C dynamic allocation. */
/* (do NOT define WOLFSSL_STATIC_MEMORY / custom XMALLOC for now) */

/* ------------------------------------------------------------------ */
/* Endianness / word size                                             */
/* ------------------------------------------------------------------ */
/* AArch64 = little-endian. (wolfSSL detects this, but we make it explicit.) */
#ifndef LITTLE_ENDIAN_ORDER
#define LITTLE_ENDIAN_ORDER
#endif

/* ------------------------------------------------------------------ */
/* Time base (no RTC): wolfSSH timestamps its logs / KEX.              */
/* We provide XTIME through a custom hook (net/wolf_port.c) -> system time. */
/* No X.509 certificate date validation in SSH -> NO_ASN_TIME is OK.    */
/* ------------------------------------------------------------------ */
#define NO_ASN_TIME                /* no certificate date validation */
/* External time hook: XTIME = wc_rtos_time (net/wolf_port.c),
 * instead of newlib's time(), which depends on a _gettimeofday stub. */
#define USER_TIME
extern long wc_rtos_time(long *t);
#define XTIME(t1)  wc_rtos_time((t1))

/* USER_TIME stops wolfSSL from pulling <time.h> in, but wolfSSH's SFTP long
 * name formatting (SFTP_CreateLongName / SetAttrTime) needs struct tm, time_t
 * and mktime(). newlib provides them, so include the header explicitly. */
#include <time.h>

/* ------------------------------------------------------------------ */
/* RNG — no /dev/urandom: hardware seed (PMU + Generic Timer).         */
/* wc_GenerateSeed() is provided in net/wolf_port.c (CUSTOM seed).     */
/* ------------------------------------------------------------------ */
#define WC_NO_HARDEN               /* no timing-attack countermeasures (POC) */
#define HAVE_HASHDRBG              /* hash-based DRBG (recommended) */
#define CUSTOM_RAND_GENERATE_SEED  wc_rtos_GenerateSeed

/* ------------------------------------------------------------------ */
/* Math — Single Precision (SP) optimized, with generic fallback.     */
/* SP covers ecc/curve25519/ed25519; ARM64 acceleration is enabled.    */
/* ------------------------------------------------------------------ */
#define WOLFSSL_SP_MATH_ALL        /* complete multi-precision SP engine */
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_ARM64           /* AArch64 assembly/optimization (Cortex-A53) */
#define SP_WORD_SIZE 64
/* GCC AArch64 supports __int128: required by sp_c64.c (nistp256 in 64-bit). */
#define HAVE___UINT128_T
#define WOLFSSL_SP_NO_MALLOC

/* ------------------------------------------------------------------ */
/* Hashing                                                            */
/* ------------------------------------------------------------------ */
#define WOLFSSL_SHA512             /* SHA-384/512 (KEX curve25519-sha256 */
#define WOLFSSL_SHA384             /*  does not need it, but Ed25519      */
                                   /*  uses SHA-512 internally)            */
#define WOLFSSL_SHA256             /* SHA-256 (KEX / MAC)                 */
#define HAVE_HKDF
/* SHA-1 is required by the SSH protocol (e.g. some KEX/extensions) -> keep it. */
/* (NO_SHA is not defined -> SHA-1 remains available.) */
#define NO_MD4
#define NO_MD5_DECOMPRESS

/* ------------------------------------------------------------------ */
/* Symmetric encryption                                               */
/* ------------------------------------------------------------------ */
#define HAVE_AES_CBC
#define WOLFSSL_AES_COUNTER        /* AES-CTR (aes128/256-ctr SSH)       */
#define HAVE_AESGCM                /* AES-GCM (aes256-gcm)               */
#define WOLFSSL_AES_DIRECT
#define HAVE_CHACHA                /* ChaCha20                           */
#define HAVE_POLY1305              /* Poly1305 (chacha20-poly1305 SSH)   */
#define HAVE_ONE_TIME_AUTH

/* Disable unused algorithms (footprint reduction). */
#define NO_DES3
#define NO_RC4
#define NO_RABBIT
#define NO_HC128
#define NO_PSK
#define NO_PWDBASED_needed_only    /* PBKDF used by wolfSSH keygen → keep PBKDF */

/* ------------------------------------------------------------------ */
/* Public key — Curve25519 (KEX) + Ed25519 (host key).                 */
/* RSA/ECDSA/DSA disabled (ed25519/curve25519).                       */
/* ------------------------------------------------------------------ */
#define HAVE_CURVE25519
#define HAVE_ED25519
/* Derive the Ed25519 public key from the private key (private-only host key) +
 * sign/verify (KEX signature). Without MAKE_KEY, wolfSSH rejects the key. */
#define HAVE_ED25519_MAKE_KEY
#define HAVE_ED25519_SIGN
#define HAVE_ED25519_VERIFY
#define WOLFSSL_SHA512             /* required by Ed25519                 */
#define HAVE_ED25519_KEY_IMPORT
/* wolfSSH DISABLES Ed25519
 * (WOLFSSH_NO_ED25519, wolfssh/internal.h lines 134-140) if ANY ONE of these
 * 4 flags is missing: HAVE_ED25519, WOLFSSL_ED25519_STREAMING_VERIFY,
 * HAVE_ED25519_KEY_IMPORT, HAVE_ED25519_KEY_EXPORT. KEY_EXPORT and
 * STREAMING_VERIFY were missing -> all Ed25519 code in IdentifyAsn1Key was
 * compiled OUT -> the host key was REJECTED (ID_UNKNOWN → WS_UNIMPLEMENTED_E,
 * code -3). Add the 2 missing flags. */
#define HAVE_ED25519_KEY_EXPORT
#define WOLFSSL_ED25519_STREAMING_VERIFY
#define HAVE_CURVE25519_SHARED_SECRET


/* Generic ECC: required by some wolfSSH paths; keep it lightweight. */
#define HAVE_ECC
#define ECC_USER_CURVES
#define HAVE_ECC256                /* nistp256 (in case the client requires it) */
#define ECC_TIMING_RESISTANT

#define NO_DSA
/* RSA: disabled (host key = Ed25519). If a client requires ssh-rsa, it
 * will be rejected during negotiation -> acceptable (modern clients OK). */
#define NO_RSA

/* ------------------------------------------------------------------ */
/* Miscellaneous                                                      */
/* ------------------------------------------------------------------ */
#define WOLFSSL_KEY_GEN            /* key generation/import (host key) */
#define WOLFSSL_BASE64_ENCODE
#define NO_OLD_TLS
#define NO_DEV_RANDOM
#define WOLFSSL_NO_CURRDIR

/* Dynamic allocation via newlib (malloc/free): do NOT define XMALLOC_USER
 * (otherwise wolfSSL expects XMALLOC/XFREE implementations from the user). */

#ifdef __cplusplus
}
#endif

#endif /* RTOS_WOLFSSL_USER_SETTINGS_H */
