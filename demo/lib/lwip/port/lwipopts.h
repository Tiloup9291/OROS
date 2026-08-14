/*
 * lwipopts.h — Configuration lwIP pour le RTOS RK3328 (Phase 7.1)
 *
 * Mode BARE-METAL "main loop" : NO_SYS=1 (pas d'OS interne à lwIP, pas de
 * threads/sémaphores/mbox lwIP). La pile tourne dans la boucle du thread
 * io_supervisor (Core2, IO_SOFT) : on injecte les trames reçues du RTL8153B
 * via netif->input() et on appelle sys_check_timeouts() régulièrement.
 *
 * ⚠️ En NO_SYS=1, on N'UTILISE PAS l'API netconn/sockets (api) ni tcpip.c :
 * uniquement l'API "raw" (callbacks) — raw/udp/tcp. Voir net/net_demo.c.
 *
 * IPv4 seul, IP statique (pas de DHCP en P7.1). ICMP + ARP pour le ping.
 */
#ifndef RTOS_LWIPOPTS_H
#define RTOS_LWIPOPTS_H

/* ------------------------------------------------------------------ */
/* Modèle d'exécution                                                  */
/* ------------------------------------------------------------------ */
#define NO_SYS                      1   /* pas d'OS interne lwIP (main loop) */
#define LWIP_TIMERS                 1   /* timers cycliques (sys_check_timeouts) */
#define SYS_LIGHTWEIGHT_PROT        0   /* mono-thread lwIP (Core2 uniquement) */

/* Pas d'API séquentielle (netconn/sockets) en NO_SYS. */
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

/* ------------------------------------------------------------------ */
/* Mémoire — allocateurs internes lwIP (pas de malloc système)         */
/* ------------------------------------------------------------------ */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (32 * 1024)   /* heap lwIP (pbuf RAM, etc.) */

/* Nombre de structures pré-allouées (pools memp). */
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          8
#define MEMP_NUM_SYS_TIMEOUT        8

/* Pool de pbuf pour la réception (PBUF_POOL). */
#define PBUF_POOL_SIZE              32
#define PBUF_POOL_BUFSIZE           1536   /* >= MTU 1500 + en-têtes */

/* ------------------------------------------------------------------ */
/* Protocoles                                                          */
/* ------------------------------------------------------------------ */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1        /* ping (echo reply) */
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   0        /* IP statique en P7.1 */
#define LWIP_DNS                    0
#define LWIP_IGMP                   0
#define LWIP_AUTOIP                 0

/* TCP : paramètres raisonnables pour un lien USB2 (~300 Mbps). */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_QUEUE_OOSEQ             1
#define LWIP_TCP_KEEPALIVE          1

/* ------------------------------------------------------------------ */
/* netif                                                               */
/* ------------------------------------------------------------------ */
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_TX_SINGLE_PBUF   1   /* r8152_send prend un buffer contigu */
#define LWIP_SINGLE_NETIF           1   /* un seul netif (RTL8153B) */

/* ------------------------------------------------------------------ */
/* Checksums — calculés en logiciel (le RTL8153B ne les offloade pas ici) */
/* ------------------------------------------------------------------ */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_ICMP         1

/* ------------------------------------------------------------------ */
/* Statistiques / debug                                                */
/* ------------------------------------------------------------------ */
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#define LWIP_DEBUG                  0    /* mettre à 1 pour tracer (verbeux) */

/* Assertions : router vers notre printf (via cc.h LWIP_PLATFORM_ASSERT). */

/* Providers pour htons/htonl : lwIP fournit ses propres versions. */
#define LWIP_PROVIDE_ERRNO          1

#endif /* RTOS_LWIPOPTS_H */
