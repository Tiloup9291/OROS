/*
 * cc.h — Couche compilateur (compiler/platform abstraction) lwIP pour le
 *        RTOS RK3328 (bare-metal AArch64, newlib, GCC). Phase 7.1.
 *
 * lwIP attend ce header (via -Ilib/lwip/port) pour connaître : les types
 * entiers (via <stdint.h>/<inttypes.h>), l'ordre des octets, l'alignement des
 * structures paquet, et les macros de diagnostic (assert/printf).
 *
 * AArch64 est little-endian (comme le protocole exige BYTE_ORDER défini) et
 * dispose de printf via newlib.
 */
#ifndef RTOS_LWIP_ARCH_CC_H
#define RTOS_LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/types.h>   /* ssize_t (newlib) : évite la redéfinition lwIP */

/* newlib fournit déjà ssize_t (via sys/types.h). On renseigne SSIZE_MAX pour
 * que lwIP (arch.h) NE redéfinisse PAS ssize_t (typedef int) — sinon conflit
 * avec le ssize_t de newlib (long int). */
#ifndef SSIZE_MAX
#define SSIZE_MAX  LONG_MAX
#endif
/* On n'inclut pas unistd.h dans lwIP (freestanding) : ssize_t vient de
 * sys/types.h ci-dessus. */
#define LWIP_NO_UNISTD_H  1


/* AArch64 tourne en little-endian pour ce RTOS. */
#ifndef BYTE_ORDER
#define BYTE_ORDER  LITTLE_ENDIAN
#endif

/* Attributs d'empaquetage des structures "paquet réseau" (GCC). */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT  __attribute__((packed))
#define PACK_STRUCT_FIELD(x)  x

/* Diagnostic : router vers printf (newlib) + boucle sur assert échouée. */
#include <stdio.h>

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)

#define LWIP_PLATFORM_ASSERT(x) \
    do { \
        printf("[lwip] ASSERT: %s @ %s:%d\n", (x), __FILE__, __LINE__); \
        for (;;) { } \
    } while (0)

/* Formats d'affichage : lwIP utilise ces macros (X8_F, U16_F, ...). */
#define U16_F   "u"
#define S16_F   "d"
#define X16_F   "x"
#define U32_F   "u"
#define S32_F   "d"
#define X32_F   "x"
#define SZT_F   "zu"

#endif /* RTOS_LWIP_ARCH_CC_H */
