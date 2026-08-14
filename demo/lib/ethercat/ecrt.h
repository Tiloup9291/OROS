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
 * ecrt.h — EtherCAT master application API (MODIFIED DERIVATIVE WORK BASED IgH EtherLab)
 *
 * ─────────────────────────────────────────────────────────────────────────
 * CONTEXT
 * ─────────────────────────────────────────────────────────────────────────
 * Integrate an EtherCAT MASTER on top of the raw L2 GMAC driver
 * (TX+RX proven on the board on the `gmac2io@ff540000`
 * port + YT8531C PHY). Path planned:
 *   - CHOICE: MODIFIED DERIVATIVE WORK BASED
 *       on IgH, restricted scope = ESM
 *       (INIT->PREOP->SAFEOP->OP) + cyclic PDO + SDO/CoE, KEEPING the `ecrt_*`
 *       application API.
 *
 * Reason — consistent with the project's "0 Linux" method (in-house
 * drivers, USB/GMAC: we read the upstream source, we
 * write OUR code). This header FAITHFULLY reproduces the subset of IgH
 * 1.6.8's `ecrt.h` API (types, macros, prototypes) actually used by a
 * typical EtherCAT application — the reference example provided provided
 * through Tiloup9291/YAEMAA (oobThread.c):
 *
 *   ecrt_request_master -> ecrt_master_create_domain -> ecrt_master_slave_config
 *   -> ecrt_slave_config_pdos -> ecrt_domain_reg_pdo_entry_list
 *   -> ecrt_slave_config_dc -> ecrt_master_activate -> ecrt_domain_data
 *   then, per cycle:
 *   ecrt_master_receive -> ecrt_domain_process -> (EC_READ/EC_WRITE at offsets)
 *   -> ecrt_master_application_time -> ecrt_master_sync_reference_clock
 *   -> ecrt_master_sync_slave_clocks -> ecrt_domain_queue -> ecrt_master_send.
 *
 * Types/macros QUOTED from IgH 1.6.8's include/ecrt.h (never deduced):
 *   ec_pdo_entry_info_t {index,subindex,bit_length}       (ecrt.h:531-535)
 *   ec_pdo_info_t {index,n_entries,entries}               (ecrt.h:545-554)
 *   ec_sync_info_t {index,dir,n_pdos,pdos,watchdog_mode}  (ecrt.h:564-573)
 *   ec_pdo_entry_reg_t {alias,position,vendor_id,          (ecrt.h:582-595)
 *       product_code,index,subindex,offset,bit_position}
 *   ec_slave_config_state_t {online:1,operational:1,       (ecrt.h:376-388)
 *       al_state:4}
 *   ec_direction_t / ec_watchdog_mode_t                   (ecrt.h:504-521)
 *   #define EC_END ~0U                                    (ecrt.h:263)
 *   EC_READ_U16 / EC_WRITE_U16 …                          (ecrt.h:2948/3057)
 *
 * LICENSE NOTE: IgH's API is GPLv2/LGPLv2.1; this header REPRODUCES the same
 * interface (source compatibility) but the IMPLEMENTATION is a MODIFIED DERIVATIVE WORK.
 *
 * Neutralized under -DMMU_QEMU (no GMAC on QEMU): the functions return a
 * clean error (ecrt_request_master → NULL).
 */
#ifndef RTOS_ETHERCAT_ECRT_H
#define RTOS_ETHERCAT_ECRT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================== */
/* Global constants (QUOTED from IgH's ecrt.h / globals.h)                */
/* ====================================================================== */

/** End marker of an ec_sync_info_t array (ecrt.h:263). */
#define EC_END              (~0U)

/** Max number of sync managers handled per slave (IgH globals.h: 16). Here
 *  we support 4 (SM0..SM3) = sufficient for a simple PDO I/O slave. */
#define EC_MAX_SYNC_MANAGERS  16u

/* ====================================================================== */
/* Opaque types (handles returned to the application)                     */
/* ====================================================================== */

typedef struct ec_master        ec_master_t;        /**< \see ecrt_request_master */
typedef struct ec_domain        ec_domain_t;        /**< \see ecrt_master_create_domain */
typedef struct ec_slave_config  ec_slave_config_t;  /**< \see ecrt_master_slave_config */

/* ====================================================================== */
/* Enums (QUOTED from ecrt.h)                                             */
/* ====================================================================== */

/** Direction of a sync manager (ecrt.h:504-509). */
typedef enum {
    EC_DIR_INVALID,   /**< Invalid direction (do not use). */
    EC_DIR_OUTPUT,    /**< Written by the master (outputs, 0x7000). */
    EC_DIR_INPUT,     /**< Read by the master (inputs, 0x6000). */
    EC_DIR_COUNT
} ec_direction_t;

/** Watchdog mode of a sync manager (ecrt.h:517-521). */
typedef enum {
    EC_WD_DEFAULT,    /**< SM's default setting. */
    EC_WD_ENABLE,     /**< Watchdog enabled. */
    EC_WD_DISABLE     /**< Watchdog disabled. */
} ec_watchdog_mode_t;

/** Interpretation of a domain's working counter (ecrt.h:481-486). */
typedef enum {
    EC_WC_ZERO = 0,   /**< No process data exchanged. */
    EC_WC_INCOMPLETE, /**< Some data exchanged. */
    EC_WC_COMPLETE    /**< All data exchanged. */
} ec_wc_state_t;

/* ====================================================================== */
/* Configuration structures (QUOTED from ecrt.h)                          */
/* ====================================================================== */

/** Configuration of a PDO entry (ecrt.h:531-535). */
typedef struct {
    uint16_t index;       /**< Index of the PDO entry (0x7000). */
    uint8_t  subindex;    /**< Subindex. */
    uint8_t  bit_length;  /**< Size in bits (16). */
} ec_pdo_entry_info_t;

/** Configuration of a PDO (ecrt.h:545-554). */
typedef struct {
    uint16_t index;                     /**< Index of the PDO (0x1600). */
    unsigned int n_entries;             /**< Number of entries in `entries`. */
    const ec_pdo_entry_info_t *entries; /**< Array of PDO entries to map. */
} ec_pdo_info_t;

/** Configuration of a sync manager (ecrt.h:564-573). */
typedef struct {
    uint8_t index;                  /**< SM index (0..3), or 0xff = end. */
    ec_direction_t dir;             /**< Direction of the SM. */
    unsigned int n_pdos;            /**< Number of PDOs in `pdos`. */
    const ec_pdo_info_t *pdos;      /**< Array of PDOs to assign. */
    ec_watchdog_mode_t watchdog_mode; /**< Watchdog mode. */
} ec_sync_info_t;

/** Record for the bulk registration of PDO entries into a domain
 *  (ecrt.h:582-595). */
typedef struct {
    uint16_t alias;         /**< Slave alias. */
    uint16_t position;      /**< Ring position of the slave. */
    uint32_t vendor_id;     /**< Slave vendor ID. */
    uint32_t product_code;  /**< Slave product code. */
    uint16_t index;         /**< Index of the PDO entry. */
    uint8_t  subindex;      /**< Subindex. */
    unsigned int *offset;   /**< [out] offset (bytes) in the process data. */
    unsigned int *bit_position; /**< [out] bit position (0-7), or NULL. */
} ec_pdo_entry_reg_t;

/** Configuration state of a slave (ecrt.h:376-388). */
typedef struct {
    unsigned int online : 1;       /**< The slave is online. */
    unsigned int operational : 1;  /**< Brought to OP with the given config. */
    unsigned int al_state : 4;     /**< AL state: 1=INIT 2=PREOP 4=SAFEOP 8=OP. */
} ec_slave_config_state_t;

/** State of a domain (ecrt.h:494-498). */
typedef struct {
    unsigned int working_counter;   /**< Last working counter. */
    ec_wc_state_t wc_state;         /**< Interpretation of the WC. */
    unsigned int redundancy_active; /**< Redundant link active (always 0 here). */
} ec_domain_state_t;

/* ====================================================================== */
/* Master API — lifecycle                                                 */
/* ====================================================================== */

/** Reserves the EtherCAT master `index` (0 = our single GMAC).
 *  Initializes the GMAC and the PHY. Returns NULL if unavailable
 *  (QEMU, no link, or already reserved). */
ec_master_t *ecrt_request_master(unsigned int master_index);

/** Releases the master (stops traffic). */
void ecrt_release_master(ec_master_t *master);

/** Creates a process data domain. Returns NULL on failure. */
ec_domain_t *ecrt_master_create_domain(ec_master_t *master);

/** Declares the configuration of a slave at `(alias, position)` with
 *  expected `(vendor_id, product_code)`. Returns a handle or NULL. */
ec_slave_config_t *ecrt_master_slave_config(ec_master_t *master,
        uint16_t alias, uint16_t position,
        uint32_t vendor_id, uint32_t product_code);

/** Configures the sync managers + PDO assignment/mapping of a slave from an
 *  `ec_sync_info_t` array terminated by index 0xff. `n_syncs` = EC_END means
 *  "until the 0xff marker". Returns 0 if OK, <0 otherwise. */
int ecrt_slave_config_pdos(ec_slave_config_t *sc, unsigned int n_syncs,
        const ec_sync_info_t syncs[]);

/** Bulk-registers a list of PDO entries into the domains. The `regs` array
 *  is terminated by an entry with index 0 (product_code/index = 0). Fills
 *  each `*offset`. Returns 0 if OK, <0 otherwise. */
int ecrt_domain_reg_pdo_entry_list(ec_domain_t *domain,
        const ec_pdo_entry_reg_t *regs);

/** Configures a slave's Distributed Clocks (assign_activate, period/offsets
 *  in ns). Minimal implementation: stores the parameters to write the DC
 *  register (0x0980+) at activation. */
void ecrt_slave_config_dc(ec_slave_config_t *sc, uint16_t assign_activate,
        uint32_t sync0_cycle, int32_t sync0_shift,
        uint32_t sync1_cycle, int32_t sync1_shift);

/** Activates the master: bus scan, slave config (SM/FMMU/PDO), transition
 *  INIT->PREOP->SAFEOP->OP, process image allocation. After activation, no more
 *  config is possible. Returns 0 if OK, <0 otherwise. */
int ecrt_master_activate(ec_master_t *master);

/** Deactivates the master (back to PREOP), frees the process image. */
void ecrt_master_deactivate(ec_master_t *master);

/* ====================================================================== */
/* Domain API — process data                                              */
/* ====================================================================== */

/** Returns the pointer to the domain's process image (valid after
 *  ecrt_master_activate). The offsets filled by reg_pdo_entry_list apply to
 *  it. Returns NULL before activation. */
uint8_t *ecrt_domain_data(ec_domain_t *domain);

/** Processes the received datagrams (updates the input process image and
 *  the domain's working counter). To be called after ecrt_master_receive. */
void ecrt_domain_process(ec_domain_t *domain);

/** Queues the domain's datagram for transmission (output process image). To
 *  be called before ecrt_master_send. */
void ecrt_domain_queue(ec_domain_t *domain);

/** Reads the domain's state (working counter, wc_state). */
void ecrt_domain_state(const ec_domain_t *domain, ec_domain_state_t *state);

/* ====================================================================== */
/* Master API — real-time cycle                                           */
/* ====================================================================== */

/** Receive: retrieves (GMAC polling) the returning EtherCAT frame of the
 *  previous cycle and dispatches it to the datagrams/domains. */
void ecrt_master_receive(ec_master_t *master);

/** Send: builds the EtherCAT frame (queued datagrams) and sends it
 *  (gmac_send). */
void ecrt_master_send(ec_master_t *master);

/** Provides the application time (ns) for the Distributed Clocks. */
void ecrt_master_application_time(ec_master_t *master, uint64_t app_time);

/** Sends a reference clock sync datagram (DC). */
void ecrt_master_sync_reference_clock(ec_master_t *master);

/** Sends a slave clock sync datagram (DC). */
void ecrt_master_sync_slave_clocks(ec_master_t *master);

/** Reads a slave's configuration state (online/operational/al_state). */
void ecrt_slave_config_state(const ec_slave_config_t *sc,
        ec_slave_config_state_t *state);

/* ====================================================================== */
/* Process data access macros (QUOTED from ecrt.h:2900-3070)              */
/* Little-endian on the EtherCAT wire; AArch64 is LE -> direct access.    */
/* ====================================================================== */

#define EC_READ_U8(DATA) \
    ((uint8_t) *((volatile uint8_t *)(DATA)))

#define EC_READ_S8(DATA) \
    ((int8_t) *((volatile int8_t *)(DATA)))

#define EC_READ_U16(DATA) \
    ((uint16_t) ( (uint16_t)((const uint8_t *)(DATA))[0] | \
                 ((uint16_t)((const uint8_t *)(DATA))[1] << 8) ))

#define EC_READ_S16(DATA) ((int16_t) EC_READ_U16(DATA))

#define EC_READ_U32(DATA) \
    ((uint32_t) ( (uint32_t)((const uint8_t *)(DATA))[0]        | \
                 ((uint32_t)((const uint8_t *)(DATA))[1] <<  8) | \
                 ((uint32_t)((const uint8_t *)(DATA))[2] << 16) | \
                 ((uint32_t)((const uint8_t *)(DATA))[3] << 24) ))

#define EC_READ_S32(DATA) ((int32_t) EC_READ_U32(DATA))

#define EC_WRITE_U8(DATA, VAL) \
    do { *((volatile uint8_t *)(DATA)) = (uint8_t)(VAL); } while (0)

#define EC_WRITE_S8(DATA, VAL) EC_WRITE_U8(DATA, VAL)

#define EC_WRITE_U16(DATA, VAL) \
    do { \
        ((uint8_t *)(DATA))[0] = (uint8_t)((uint16_t)(VAL) & 0xFF); \
        ((uint8_t *)(DATA))[1] = (uint8_t)(((uint16_t)(VAL) >> 8) & 0xFF); \
    } while (0)

#define EC_WRITE_S16(DATA, VAL) EC_WRITE_U16(DATA, VAL)

#define EC_WRITE_U32(DATA, VAL) \
    do { \
        ((uint8_t *)(DATA))[0] = (uint8_t)((uint32_t)(VAL)        & 0xFF); \
        ((uint8_t *)(DATA))[1] = (uint8_t)(((uint32_t)(VAL) >>  8) & 0xFF); \
        ((uint8_t *)(DATA))[2] = (uint8_t)(((uint32_t)(VAL) >> 16) & 0xFF); \
        ((uint8_t *)(DATA))[3] = (uint8_t)(((uint32_t)(VAL) >> 24) & 0xFF); \
    } while (0)

#define EC_WRITE_S32(DATA, VAL) EC_WRITE_U32(DATA, VAL)

/* ====================================================================== */
/* HOME-GROWN EXTENSION (outside the IgH API)                             */
/* ====================================================================== */
/*
 * Diagnostic probe: DIRECTLY reads (physical FPRD) the input memory of the
 * 1st slave, to ISOLATE a "DI always 0" issue (FMMU/logic bug vs. wrong
 * physical SM address). `probe`!=0 -> also scans candidate addresses.
 * Returns the number of non-zero bytes read at the resolved input SM
 * address.
 * (NOT present in the IgH API — debugging utility)
 */
unsigned ecrt_master_probe_input(ec_master_t *master, int probe);

#ifdef __cplusplus
}
#endif


#endif /* RTOS_ETHERCAT_ECRT_H */
