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
 * netif_r8152.h — lwIP network interface on top of the RTL8153B USB-Ethernet
 *                 driver (r8152).
 *
 * Bridges the lwIP stack (NO_SYS=1, main loop on Core2) and the r8152 class
 * driver (board-validated). The netif:
 *   - emits frames via r8152_send()   (netif->linkoutput)
 *   - receives frames via r8152_recv() (polling, called by netif_r8152_poll)
 *
 * Usage (on Core2 / IO_SOFT):
 *   lwip_init();
 *   netif_r8152_add(&netif, rt, &ip, &mask, &gw);   // rt = r8152_dev_t already probe+link
 *   netif_set_up(&netif);
 *   // loop:
 *   for (;;) { netif_r8152_poll(&netif); sys_check_timeouts(); }
 */
#ifndef RTOS_NET_NETIF_R8152_H
#define RTOS_NET_NETIF_R8152_H

#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "../drivers/usb/class_r8152.h"

/*
 * netif_r8152_add — registers a lwIP netif attached to the r8152 device 'rt'.
 * 'rt' must have been probe()'d (MAC read) AND ideally link_wait()'d (link up).
 * Static IP address provided. Returns ERR_OK / lwIP err.
 */
err_t netif_r8152_add(struct netif *netif, r8152_dev_t *rt,
                      const ip4_addr_t *ip, const ip4_addr_t *mask,
                      const ip4_addr_t *gw);

/*
 * netif_r8152_poll — polls the RTL8153B for a received frame (non-blocking)
 * and injects it into lwIP (netif->input). To be called in a loop on Core2.
 * Returns the number of frames processed during this call (0 if none).
 */
int netif_r8152_poll(struct netif *netif);

#endif /* RTOS_NET_NETIF_R8152_H */
