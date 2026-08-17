/*
 * user_settings.h — Configuration wolfSSL/wolfCrypt BARE-METAL pour le RTOS
 *                   RK3328 (AArch64, newlib, GCC). Phase 7.3 (serveur SSH).
 *
 * Compilé avec -DWOLFSSL_USER_SETTINGS : ce header est inclus automatiquement
 * par wolfssl/wolfcrypt/settings.h AVANT toute autre option. Il définit un
 * profil MINIMAL "wolfCrypt-only" (PAS de TLS) suffisant pour wolfSSH :
 *   - SINGLE_THREADED (mono-thread, tourne sur Core2 IO_SOFT en polling) ;
 *   - WOLFSSL_USER_IO (aucun socket BSD : I/O via nos callbacks lwIP raw) ;
 *   - NO_FILESYSTEM / NO_WOLFSSL_DIR (pas de FS pour la crypto) ;
 *   - base de temps + RNG câblés sur notre matériel (Generic Timer + PMU) ;
 *   - algos SSH modernes et LÉGERS : SHA-256/512, HMAC, AES-CTR/GCM, ChaCha20-
 *     Poly1305, Curve25519 (KEX) + Ed25519 (host key) ; RSA/ECDSA désactivés.
 *
 * Réf. : wolfSSL manual ch.2 (building) + exemples IDE user_settings.
 */
#ifndef RTOS_WOLFSSL_USER_SETTINGS_H
#define RTOS_WOLFSSL_USER_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Plateforme / modèle d'exécution                                     */
/* ------------------------------------------------------------------ */
#define SINGLE_THREADED            /* pas de threads internes wolfSSL   */
#define WOLFSSL_USER_IO            /* pas de sockets BSD : I/O via cb    */
#define WOLFSSL_NO_SOCK            /* pas de couche socket               */
#define NO_WRITEV                  /* pas de writev()                    */
#define NO_FILESYSTEM              /* pas de fopen/fread (crypto en RAM) */
#define NO_WOLFSSL_DIR             /* pas de parcours de répertoire      */
#define WOLFSSL_IGNORE_FILE_WARN   /* silence sur fichiers non compilés  */
#define NO_MAIN_DRIVER

/* Pas de couche TLS : wolfSSH n'utilise QUE wolfCrypt. */
#define WOLFCRYPT_ONLY

/* Active les primitives crypto specifiques requises par wolfSSH. */
#define WOLFSSL_WOLFSSH
#define WOLFSSL_PUBLIC_MP
/* I/O SSH via nos callbacks (TCP raw lwIP) : pas de sockets BSD. */
#define WOLFSSH_USER_IO
/* Modules wolfSSH non utilises (footprint). */
#define WOLFSSH_NO_SFTP
#define WOLFSSH_NO_SCP
#define WOLFSSH_NO_AGENT
#define WOLFSSH_NO_CERTS
#define WOLFSSH_NO_PORT_FORWARD
#define NO_TERMIOS
#define WOLFSSH_NO_FILESYSTEM

/* Petites piles (bare-metal) : privilégier le heap aux gros buffers de pile. */
#define WOLFSSL_SMALL_STACK

/* newlib fournit malloc/free (via _sbrk). On garde l'alloc dynamique C. */
/* (ne PAS définir WOLFSSL_STATIC_MEMORY / XMALLOC custom pour l'instant)  */

/* ------------------------------------------------------------------ */
/* Endianness / mots                                                   */
/* ------------------------------------------------------------------ */
/* AArch64 = little-endian. (wolfSSL détecte, mais on est explicite.)   */
#ifndef LITTLE_ENDIAN_ORDER
#define LITTLE_ENDIAN_ORDER
#endif

/* ------------------------------------------------------------------ */
/* Base de temps (pas de RTC) : wolfSSH horodate ses logs / KEX.        */
/* On fournit XTIME via un hook maison (net/wolf_port.c) → sys temps.   */
/* Pas de vérification de certificats X.509 en SSH → NO_ASN_TIME OK.    */
/* ------------------------------------------------------------------ */
#define NO_ASN_TIME                /* pas de validation de dates de cert */
/* Hook de temps fourni en externe : XTIME = wc_rtos_time (net/wolf_port.c),
 * au lieu de la time() de newlib (dependante d'un stub _gettimeofday). */
#define USER_TIME
extern long wc_rtos_time(long *t);
#define XTIME(t1)  wc_rtos_time((t1))

/* ------------------------------------------------------------------ */
/* RNG — pas de /dev/urandom : seed matériel (PMU + Generic Timer).     */
/* wc_GenerateSeed() est fournie dans net/wolf_port.c (CUSTOM seed).    */
/* ------------------------------------------------------------------ */
#define WC_NO_HARDEN               /* pas de contre-mesures timing (POC) */
#define HAVE_HASHDRBG              /* DRBG basé hash (recommandé)        */
#define CUSTOM_RAND_GENERATE_SEED  wc_rtos_GenerateSeed

/* ------------------------------------------------------------------ */
/* Maths — Single Precision (SP) optimisé, avec repli générique.        */
/* SP couvre ecc/curve25519/ed25519 ; on active l'accélération ARM64.   */
/* ------------------------------------------------------------------ */
#define WOLFSSL_SP_MATH_ALL        /* moteur SP multi-précision complet  */
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_ARM64           /* asm/optim AArch64 (Cortex-A53)     */
#define SP_WORD_SIZE 64
/* GCC AArch64 supporte __int128 : requis par sp_c64.c (nistp256 en 64b). */
#define HAVE___UINT128_T
#define WOLFSSL_SP_NO_MALLOC

/* ------------------------------------------------------------------ */
/* Hachage                                                             */
/* ------------------------------------------------------------------ */
#define WOLFSSL_SHA512             /* SHA-384/512 (KEX curve25519-sha256 */
#define WOLFSSL_SHA384            /*  n'en a pas besoin, mais ed25519    */
                                   /*  utilise SHA-512 en interne)        */
#define WOLFSSL_SHA256             /* SHA-256 (KEX / MAC)                */
#define HAVE_HKDF
/* SHA-1 requis par le protocole SSH (ex. certains KEX/ext) → garder.   */
/* (NO_SHA non défini → SHA-1 disponible.)                              */
#define NO_MD4
#define NO_MD5_DECOMPRESS

/* ------------------------------------------------------------------ */
/* Chiffrement symétrique                                              */
/* ------------------------------------------------------------------ */
#define HAVE_AES_CBC
#define WOLFSSL_AES_COUNTER        /* AES-CTR (aes128/256-ctr SSH)       */
#define HAVE_AESGCM                /* AES-GCM (aes256-gcm)               */
#define WOLFSSL_AES_DIRECT
#define HAVE_CHACHA                /* ChaCha20                           */
#define HAVE_POLY1305              /* Poly1305 (chacha20-poly1305 SSH)   */
#define HAVE_ONE_TIME_AUTH

/* Désactiver les algos non utilisés (footprint). */
#define NO_DES3
#define NO_RC4
#define NO_RABBIT
#define NO_HC128
#define NO_PSK
#define NO_PWDBASED_needed_only  /* PBKDF utilisé par wolfSSH keygen → garder PBKDF */

/* ------------------------------------------------------------------ */
/* Clé publique — Curve25519 (KEX) + Ed25519 (host key).               */
/* RSA/ECDSA/DSA désactivés (choix utilisateur : ed25519/curve25519).  */
/* ------------------------------------------------------------------ */
#define HAVE_CURVE25519
#define HAVE_ED25519
/* Deriver la cle publique ed25519 depuis la privee (host key priv-only) +
 * sign/verify (KEX signature). Sans MAKE_KEY, wolfSSH rejette la cle. */
#define HAVE_ED25519_MAKE_KEY
#define HAVE_ED25519_SIGN
#define HAVE_ED25519_VERIFY
#define WOLFSSL_SHA512             /* requis par ed25519                 */
#define HAVE_ED25519_KEY_IMPORT
/* ⭐ CAUSE RACINE test board #1 (leçon P7.3#6) : wolfSSH DESACTIVE Ed25519
 * (WOLFSSH_NO_ED25519, wolfssh/internal.h l.134-140) si l'UN de ces 4 flags
 * manque : HAVE_ED25519, WOLFSSL_ED25519_STREAMING_VERIFY, HAVE_ED25519_KEY_IMPORT,
 * HAVE_ED25519_KEY_EXPORT. Il manquait KEY_EXPORT et STREAMING_VERIFY → tout le
 * code Ed25519 de IdentifyAsn1Key etait compile OUT → la cle hote etait REJETEE
 * (ID_UNKNOWN → WS_UNIMPLEMENTED_E, code -3). On ajoute les 2 flags manquants. */
#define HAVE_ED25519_KEY_EXPORT
#define WOLFSSL_ED25519_STREAMING_VERIFY
#define HAVE_CURVE25519_SHARED_SECRET


/* ECC générique : requis par certains chemins wolfSSH ; on le garde léger. */
#define HAVE_ECC
#define ECC_USER_CURVES
#define HAVE_ECC256                /* nistp256 (au cas où le client l'exige) */
#define ECC_TIMING_RESISTANT

#define NO_DSA
/* RSA : désactivé (host key = ed25519). Si un client exige ssh-rsa il
 * sera refusé côté négociation → acceptable (clients modernes OK). */
#define NO_RSA

/* ------------------------------------------------------------------ */
/* Divers                                                              */
/* ------------------------------------------------------------------ */
#define WOLFSSL_KEY_GEN            /* génération/import de clés (host key) */
#define WOLFSSL_BASE64_ENCODE
#define NO_OLD_TLS
#define NO_DEV_RANDOM
#define WOLFSSL_NO_CURRDIR

/* Alloc dynamique via newlib (malloc/free) : ne PAS definir XMALLOC_USER
 * (sinon wolfSSL attend des XMALLOC/XFREE fournis par l'utilisateur). */

#ifdef __cplusplus
}
#endif

#endif /* RTOS_WOLFSSL_USER_SETTINGS_H */
