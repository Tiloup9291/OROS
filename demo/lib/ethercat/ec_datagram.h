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
 * ec_datagram.h — Low-level EtherCAT protocol (frame + datagrams)
 *
 * Defines the format of an EtherCAT frame (EtherType 0x88A4) and its
 * datagrams, as well as the commands and registers of the ESCs (EtherCAT
 * Slave Controller). The master (ec_master.c) assembles a frame containing
 * one or more datagrams, emits it via the GMAC, and reads the
 * response on the fly (each slave increments the working counter and
 * possibly writes its response "on the fly").
 *
 * ─────────────────────────────────────────────────────────────────────────
 * REFERENCE SOURCES (offsets/values QUOTED — never deduced):
 *   - Spec ETG.1000.3/.4 (EtherCAT frame + PDU + ESC register map).
 *   - IgH EtherLab 1.6.8: master/datagram.h (ec_command_type_t: APRD/APWR/
 *     BRD/BWR/FPRD/FPWR/LRD/LWR/LRW…), master/globals.h (ESC registers:
 *     0x0120 AL_CONTROL, 0x0130 AL_STATUS, 0x0134 AL_STATUS_CODE, 0x0800+ SM,
 *     0x0600+ FMMU, 0x0980+ DC), master/master.c (frame assembly:
 *     EtherType 0x88A4, 2-byte len|type header, chained datagrams).
 *
 * ─────────────────────────────────────────────────────────────────────────
 * FORMAT (little-endian on the wire):
 *
 *   [Ethernet]  dst(6) src(6) ethertype(2)=0x88A4
 *   [EC hdr ]   u16: bits 0..10 = length (datagram bytes),
 *                     bits 12..15 = type (1 = frames with datagrams)
 *   [Datagram]* each:
 *       u8  cmd            (ec_cmd_t)
 *       u8  index          (datagram id, echoed by the slave)
 *       u32 address        (ADP:ADO position/broadcast or logical)
 *       u16 len_flags      bits 0..10 len, bit 14 = circulating, bit 15 = more
 *       u16 irq            (IRQ mask, 0)
 *       u8  data[len]
 *       u16 wkc            (working counter, incremented by the slaves)
 *
 * Independent of Linux: no kernel include.
 */
#ifndef RTOS_ETHERCAT_EC_DATAGRAM_H
#define RTOS_ETHERCAT_EC_DATAGRAM_H

#include <stdint.h>

/* EtherCAT EtherType (ETG.1000, IgH globals.h). */
#define EC_ETHERTYPE          0x88A4u

/* Header sizes. */
#define EC_ETH_HEADER_SIZE    14u   /* dst+src+ethertype */
#define EC_FRAME_HEADER_SIZE  2u    /* EtherCAT len|type */
#define EC_DATAGRAM_HEADER_SIZE 10u /* cmd+idx+addr+len_flags+irq */
#define EC_DATAGRAM_FOOTER_SIZE 2u  /* wkc */
#define EC_MAX_DATA_SIZE      1486u /* max EtherCAT payload in an Eth frame */

/* Minimum length of an Ethernet frame (padding to 60 B excluding FCS). */
#define EC_MIN_ETH_FRAME      60u

/* ─── Datagram commands (ec_command_type_t, IgH master/datagram.h) ─── */
typedef enum {
    EC_CMD_NOP  = 0x00, /**< No operation. */
    EC_CMD_APRD = 0x01, /**< Auto-increment physical read. */
    EC_CMD_APWR = 0x02, /**< Auto-increment physical write. */
    EC_CMD_APRW = 0x03, /**< Auto-increment physical read-write. */
    EC_CMD_FPRD = 0x04, /**< Configured-address physical read. */
    EC_CMD_FPWR = 0x05, /**< Configured-address physical write. */
    EC_CMD_FPRW = 0x06, /**< Configured-address physical read-write. */
    EC_CMD_BRD  = 0x07, /**< Broadcast read. */
    EC_CMD_BWR  = 0x08, /**< Broadcast write. */
    EC_CMD_BRW  = 0x09, /**< Broadcast read-write. */
    EC_CMD_LRD  = 0x0A, /**< Logical read. */
    EC_CMD_LWR  = 0x0B, /**< Logical write. */
    EC_CMD_LRW  = 0x0C, /**< Logical read-write. */
    EC_CMD_ARMW = 0x0D, /**< Auto-increment physical read multiple write. */
    EC_CMD_FRMW = 0x0E  /**< Configured-address physical read multiple write. */
} ec_cmd_t;

/* ─── ESC registers (ETG.1000.4 / IgH master/globals.h) ─── */
#define EC_REG_TYPE           0x0000u /* Type (u8) */
#define EC_REG_DL_STATUS      0x0110u /* DL status (u16): bit4/8/12/16 link port */
#define EC_REG_AL_CONTROL     0x0120u /* AL control (u16): requested state */
#define EC_REG_AL_STATUS      0x0130u /* AL status (u16): current state */
#define EC_REG_AL_STATUS_CODE 0x0134u /* AL status code (u16): error cause */
#define EC_REG_STATION_ADDR   0x0010u /* Configured station address (u16) */
#define EC_REG_STATION_ALIAS  0x0012u /* Configured station alias (u16) */
#define EC_REG_SM0            0x0800u /* Sync Manager 0 (8 bytes/SM) */
#define EC_REG_SM(n)          (EC_REG_SM0 + (n) * 8u)
#define EC_REG_FMMU0          0x0600u /* FMMU 0 (16 bytes/FMMU) */
#define EC_REG_FMMU(n)        (EC_REG_FMMU0 + (n) * 16u)
#define EC_REG_DC_SYSTIME     0x0910u /* System time (u64) */
#define EC_REG_DC_SYNC_ACT    0x0981u /* DC sync activation (u8) */
#define EC_REG_DC_START0      0x0990u /* DC SYNC0 start time (u64) */
#define EC_REG_DC_CYCLE0      0x09A0u /* DC SYNC0 cycle time (u32) */

/* ─── ESC EEPROM/SII interface (ETG.1000.4 / IgH fsm_sii.c) ─── */
/* SII access is done via FPWR/FPRD starting at 0x0502 (IgH indeed cites
 * 0x0502, NOT 0x0500): control/status(u16)@0x0502, address(u16)@0x0504,
 * data(4B)@0x0508.
 * Read sequence (fsm_sii.c): FPWR 0x0502 = {0x80,0x01,word_offset:u16} then
 * FPRD 0x0502 (10 B) → byte[1] busy(0x81)/error(0x20) bits, data at offset +6. */
#define EC_REG_SII_CTRL       0x0502u /* SII control/status (u16) */
#define EC_REG_SII_ADDR       0x0504u /* SII address (word offset, u16) */
#define EC_REG_SII_DATA       0x0508u /* SII data (4 bytes = 2 words) */
#define EC_SII_READ_REQUEST   0x0080u /* byte0=0x80 (2 addr bytes), byte1=0x01 */
#define EC_SII_STAT_BUSY      0x81u   /* byte[1]: busy bit OR read-op busy */
#define EC_SII_STAT_ERROR     0x20u   /* byte[1]: error on last command */
/* The SII categories area starts at WORD 0x40 (IgH EC_FIRST_SII_CATEGORY_
 * OFFSET). SyncManager category = type 0x0029 (each SM = 4 words:
 * phys_start_addr, length, control, enable). */
#define EC_SII_FIRST_CAT_WORD 0x0040u
#define EC_SII_CAT_SYNC       0x0029u


/* ─── AL states (AL_CONTROL / AL_STATUS bits 0..3, ETG.1000.6) ─── */
#define EC_AL_STATE_INIT      0x01u
#define EC_AL_STATE_PREOP     0x02u
#define EC_AL_STATE_BOOT      0x03u
#define EC_AL_STATE_SAFEOP    0x04u
#define EC_AL_STATE_OP        0x08u
#define EC_AL_STATE_MASK      0x0Fu
#define EC_AL_CTRL_ACK        0x10u  /* error acknowledge bit */

/* ─── Sync Manager: control (SM byte 4), ETG.1000.4 ─── */
#define EC_SM_CTRL_BUFFERED   0x00u  /* buffered mode (3 buffers) */
#define EC_SM_CTRL_MAILBOX    0x02u  /* mailbox mode (1 buffer) */
#define EC_SM_CTRL_DIR_WRITE  0x04u  /* direction: written by the master (ECAT→PDI) */
#define EC_SM_CTRL_PDI_IRQ    0x00u
/* SM byte 6 = activate: bit0 = SM enable. */
#define EC_SM_ACT_ENABLE      0x01u

/* ─── FMMU: access type (ETG.1000.4) ─── */
#define EC_FMMU_READ          0x01u  /* maps inputs (logical read) */
#define EC_FMMU_WRITE         0x02u  /* maps outputs (logical write) */
#define EC_FMMU_ENABLE        0x01u  /* activate byte */

/*
 * Assembles a datagram header into `buf` (at least 10 bytes) and copies
 * `data`/`data_len` right after (zeros if data==NULL). Writes the WKC (0) at
 * the end. Returns the TOTAL size of the datagram (header+data+wkc).
 *   cmd     : ec_cmd_t
 *   idx     : index (echoed by the slave)
 *   adp     : address position (auto-inc) or station address
 *   ado     : address offset (ESC register) — for physical cmds
 *   more    : 1 if another datagram follows in the frame
 */
uint32_t ec_datagram_build(uint8_t *buf, ec_cmd_t cmd, uint8_t idx,
        uint16_t adp, uint16_t ado, const void *data, uint16_t data_len,
        int more);

/* Same for a LOGICAL datagram (LRD/LWR/LRW): `logaddr` = 32-bit logical
 * address (the EtherCAT domain). */
uint32_t ec_datagram_build_logical(uint8_t *buf, ec_cmd_t cmd, uint8_t idx,
        uint32_t logaddr, const void *data, uint16_t data_len, int more);

/* Extracts the working counter of a received datagram located at `dg`
 * (pointer to the start of the datagram in the RX frame), `data_len` =
 * length of its payload. */
uint16_t ec_datagram_wkc(const uint8_t *dg, uint16_t data_len);

/* Pointer to the payload (data[]) of a datagram at `dg`. */
static inline const uint8_t *ec_datagram_data(const uint8_t *dg) {
    return dg + EC_DATAGRAM_HEADER_SIZE;
}

#endif /* RTOS_ETHERCAT_EC_DATAGRAM_H */
