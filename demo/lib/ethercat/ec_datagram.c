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
 * ec_datagram.c — Assembling/parsing EtherCAT datagrams.
 *
 * Small utility functions manipulating the binary format of EtherCAT
 * datagrams (ec_datagram.h). All little-endian (AArch64 is LE + the
 * EtherCAT wire is LE → byte-by-byte writes to stay portable/aligned).
 */

#include "ec_datagram.h"
#include <string.h>

/* Writes a little-endian u16. */
static inline void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/* Writes a little-endian u32. */
static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Reads a little-endian u16. */
static inline uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/*
 * Common header of a datagram:
 *   [0]   cmd
 *   [1]   index
 *   [2..5] address (ADP:ADO or 32-bit logical)
 *   [6..7] len_flags (bits 0..10 = len; bit15 = 'more'/'last next' per spec)
 *   [8..9] irq
 *   then data[len], then wkc(u16).
 *
 * NB: in the EtherCAT spec, the datagram's "len" field also encodes the
 * "next" bit (bit 15 = "M": another datagram follows). We set this bit if
 * more=1.
 */
static uint32_t build_common(uint8_t *buf, ec_cmd_t cmd, uint8_t idx,
        uint32_t address, const void *data, uint16_t data_len, int more) {
    buf[0] = (uint8_t)cmd;
    buf[1] = idx;
    put_u32(&buf[2], address);
    uint16_t len_flags = (uint16_t)(data_len & 0x07FF);
    if (more) len_flags |= 0x8000u;  /* "next datagram follows" bit */
    put_u16(&buf[6], len_flags);
    put_u16(&buf[8], 0);             /* irq = 0 */

    if (data && data_len)
        memcpy(&buf[EC_DATAGRAM_HEADER_SIZE], data, data_len);
    else if (data_len)
        memset(&buf[EC_DATAGRAM_HEADER_SIZE], 0, data_len);

    /* WKC (2 bytes) initialized to 0: the slaves increment it. */
    put_u16(&buf[EC_DATAGRAM_HEADER_SIZE + data_len], 0);

    return EC_DATAGRAM_HEADER_SIZE + data_len + EC_DATAGRAM_FOOTER_SIZE;
}

uint32_t ec_datagram_build(uint8_t *buf, ec_cmd_t cmd, uint8_t idx,
        uint16_t adp, uint16_t ado, const void *data, uint16_t data_len,
        int more) {
    /* Physical address = ADP (position/station) in the low 16 bits,
     * ADO (register offset) in the high 16 bits. */
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return build_common(buf, cmd, idx, address, data, data_len, more);
}

uint32_t ec_datagram_build_logical(uint8_t *buf, ec_cmd_t cmd, uint8_t idx,
        uint32_t logaddr, const void *data, uint16_t data_len, int more) {
    return build_common(buf, cmd, idx, logaddr, data, data_len, more);
}

uint16_t ec_datagram_wkc(const uint8_t *dg, uint16_t data_len) {
    return get_u16(&dg[EC_DATAGRAM_HEADER_SIZE + data_len]);
}
