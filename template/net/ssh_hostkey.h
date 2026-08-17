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
 * ssh_hostkey.h — Embedded SSH host key (Ed25519), DER (ASN.1) format.
 *   Loaded by wolfSSH_CTX_UsePrivateKey_buffer(..,WOLFSSH_FORMAT_ASN1).
 *
 * This key comes from wolfSSH's TEST keys (keys/server-key-ed25519.der).
 * It is PUBLICLY known: acceptable for an SSH connectivity demonstration,
 * but MUST BE REPLACED by a privately generated and kept-secret key for any
 * real-world use. (Security breach)
 */
#ifndef RTOS_NET_SSH_HOSTKEY_H
#define RTOS_NET_SSH_HOSTKEY_H

#include <stdint.h>

static const uint8_t ssh_hostkey_ed25519_der[] = {
    0x30, 0x50, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70,
    0x04, 0x22, 0x04, 0x20, 0x6A, 0x67, 0xF3, 0x0E, 0x64, 0xEA, 0x52, 0xFE,
    0xF4, 0xAD, 0x65, 0x4D, 0x45, 0x60, 0x61, 0x38, 0x58, 0x11, 0x07, 0x84,
    0xF0, 0x03, 0x94, 0x93, 0x14, 0x7B, 0x7B, 0x33, 0x1A, 0xBA, 0xF6, 0x19,
    0x81, 0x20, 0x0F, 0x56, 0x0C, 0x9F, 0x7D, 0x7A, 0x62, 0x87, 0xF0, 0x26,
    0x16, 0x19, 0x31, 0xE4, 0xB2, 0x1D, 0xE9, 0xBD, 0xEE, 0x4A, 0x7F, 0x55,
    0xAE, 0x26, 0x2D, 0xA1, 0x25, 0xE4, 0xEE, 0x4A, 0x51, 0x00,
};

static const uint32_t ssh_hostkey_ed25519_der_len =
    (uint32_t)sizeof(ssh_hostkey_ed25519_der);

#endif /* RTOS_NET_SSH_HOSTKEY_H */
