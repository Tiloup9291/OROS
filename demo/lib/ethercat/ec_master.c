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
 * ec_master.c — Minimal MODIFIED DERIVATIVE WORK BASED EtherCAT master, wired to the
 *               raw L2 GMAC. Implements the `ecrt_*` application API
 *               (ecrt.h).
 *
 * ─────────────────────────────────────────────────────────────────────────
 * SCOPE
 * ─────────────────────────────────────────────────────────────────────────
 *   ✔ Bus scan (BRD AL_STATUS, slave count via WKC/auto-inc).
 *   ✔ ESM state machine: INIT → PREOP → SAFEOP → OP (AL_CONTROL/AL_STATUS).
 *   ✔ Addressing: assignment of a configured station address (APWR 0x0010).
 *   ✔ Sync Manager config (SM2 outputs, SM3 inputs) via FPWR 0x0800+.
 *   ✔ FMMU config (maps the logical domain ↔ slaves' SM) via FPWR 0x0600+.
 *   ✔ Cyclic process data: LOGICAL LRW datagram (address 0) → reads inputs +
 *     writes outputs in a single "on the fly" datagram.
 *   ✔ Domain working counter (expected WKC = 2×n_output_bits/… simplified:
 *     we report the raw returned WKC).
 *   ~ Distributed Clocks: sync datagrams built but full DC (drift
 *     compensation) is out of scope — stored, writes SYSTIME.
 *   ✘ CoE/SDO mailbox: stubs (to be completed; not required for cyclic PDO).
 *
 * EXECUTION MODEL: this module
 * is driven by the `ecat_master` thread on Core0, in synchronous POLLING
 * within the cycle (GMAC IRQ disabled). `ecrt_master_send` emits the frame
 * (gmac_send), `ecrt_master_receive` reads it back on the fly
 * (gmac_poll_recv). An EtherCAT frame makes a round trip on the slave ring
 * and returns to the master.
 *
 * QEMU NOTE: gmac_init returns GMAC_ENODEV -> ecrt_request_master returns NULL.
 *
 * SOURCES (offsets/values quoted): ec_datagram.h (ETG.1000 spec + IgH), and
 * IgH's ESM/SM/FMMU sequences from master/fsm_slave_config.c / fsm_change.c.
 */

#include "ecrt.h"
#include "ec_datagram.h"
#include "gmac.h"
#include "timer.h"
#include "config.h"
#include <string.h>
#include <stdio.h>

/* ====================================================================== */
/* Internal limits (static, no DMA heap)                                  */
/* ====================================================================== */
#define EC_MAX_SLAVES        8u
#define EC_MAX_DOMAINS       2u
#define EC_MAX_REGS          16u    /* PDO entries registered per domain */
#define EC_PROCESS_IMG_SIZE  256u   /* max size of the process image (bytes) */
#define EC_TX_BUF_SIZE       1536u

/* Timeout of an AL state change (ns -> ticks). */
#define EC_STATE_TIMEOUT_US  100000u  /* 100 ms per transition */

/* ====================================================================== */
/* Internal structures (opaque types from ecrt.h)                         */
/* ====================================================================== */

/* A PDO entry registered in a domain (result of reg_pdo_entry_list). */
typedef struct {
    uint16_t index;
    uint8_t  subindex;
    uint8_t  dir;         /* EC_DIR_OUTPUT / EC_DIR_INPUT */
    uint8_t  bit_length;
    uint32_t byte_offset; /* offset in the process image */
    unsigned int *user_offset; /* where to write the offset (app side) */
} ec_reg_entry_t;

struct ec_domain {
    struct ec_master *master;
    int in_use;
    uint8_t process_image[EC_PROCESS_IMG_SIZE];
    uint32_t image_size;      /* actually used bytes */
    uint32_t out_size;        /* output bytes (start of the image) */
    uint32_t in_size;         /* input bytes (after the outputs) */
    ec_reg_entry_t regs[EC_MAX_REGS];
    unsigned int n_regs;
    unsigned int working_counter;
    ec_wc_state_t wc_state;
};

struct ec_slave_config {
    struct ec_master *master;
    int in_use;
    uint16_t alias;
    uint16_t position;        /* position on the ring */
    uint16_t station_addr;    /* configured address (1000+position) */
    uint32_t vendor_id;
    uint32_t product_code;
    const ec_sync_info_t *syncs; /* SM/PDO config provided by the app */

    /* Stored DC config (ecrt_slave_config_dc). */
    int dc_used;
    uint16_t dc_assign_activate;
    uint32_t dc_sync0_cycle;
    int32_t  dc_sync0_shift;

    /* Current state reported from the bus. */
    uint8_t  al_state;        /* current EC_AL_STATE_* */
    int      online;
    int      operational;

    /* PDO sizes (bytes) computed from the syncs. */
    uint32_t out_bytes;       /* outputs SM (EC_DIR_OUTPUT) */
    uint32_t in_bytes;        /* inputs SM (EC_DIR_INPUT) */
    uint8_t  sm_out_index;    /* outputs SM index (often 2) */
    uint8_t  sm_in_index;     /* inputs SM index (often 3) */
    uint16_t sm_out_phys;     /* physical address of the outputs SM (resolved) */
    uint16_t sm_in_phys;      /* physical address of the inputs SM (resolved) */
    uint16_t sm_out_len;      /* REAL length of the outputs SM (SII) */
    uint16_t sm_in_len;       /* REAL length of the inputs SM (SII) */
    uint8_t  sm_out_ctrl;     /* REAL control byte of the outputs SM (SII) */
    uint8_t  sm_in_ctrl;      /* REAL control byte of the inputs SM (SII) */
};



struct ec_master {
    int in_use;
    int activated;
    gmac_info_t gmac;
    uint8_t mac[6];

    struct ec_slave_config slaves[EC_MAX_SLAVES];
    unsigned int n_configs;

    struct ec_domain domains[EC_MAX_DOMAINS];
    unsigned int n_domains;

    unsigned int slaves_on_bus; /* detected during scan */

    uint64_t app_time;
    uint8_t  dg_index;          /* datagram index counter */

    /* Frame buffers (static). */
    uint8_t txbuf[EC_TX_BUF_SIZE];
    uint8_t rxbuf[EC_TX_BUF_SIZE];
    uint32_t rx_len;
};

/* A single master instance (a single GMAC). */
static struct ec_master g_master;

/* ====================================================================== */
/* Low-level helpers                                                      */
/* ====================================================================== */

static void udelay_us(uint32_t us) {
    uint64_t end = timer_now_ticks() + timer_us_to_ticks(us);
    while (timer_now_ticks() < end) __asm__ volatile("nop");
}

/* Builds the Ethernet + EtherCAT header in txbuf, returns the offset of the
 * 1st datagram. dst = broadcast (the slaves read it on the fly anyway). */
static uint32_t frame_begin(struct ec_master *m) {
    uint8_t *b = m->txbuf;
    memset(&b[0], 0xFF, 6);              /* dst = broadcast */
    memcpy(&b[6], m->mac, 6);            /* src = our MAC */
    b[12] = (uint8_t)(EC_ETHERTYPE >> 8);
    b[13] = (uint8_t)(EC_ETHERTYPE & 0xFF);
    /* EtherCAT header (2 bytes) filled at the end (once we know the len). */
    return EC_ETH_HEADER_SIZE + EC_FRAME_HEADER_SIZE;
}

/* Finalizes the frame: writes the EtherCAT header (len|type), pads to 60 B,
 * and returns the total length to emit. `dg_bytes` = datagram bytes. */
static uint32_t frame_end(struct ec_master *m, uint32_t dg_bytes) {
    uint8_t *hdr = &m->txbuf[EC_ETH_HEADER_SIZE];
    /* len on 11 bits, type=1 (datagrams) in bits 12..15. */
    uint16_t lt = (uint16_t)((dg_bytes & 0x07FF) | (0x1u << 12));
    hdr[0] = (uint8_t)(lt & 0xFF);
    hdr[1] = (uint8_t)((lt >> 8) & 0xFF);

    uint32_t total = EC_ETH_HEADER_SIZE + EC_FRAME_HEADER_SIZE + dg_bytes;
    if (total < EC_MIN_ETH_FRAME) {
        memset(&m->txbuf[total], 0, EC_MIN_ETH_FRAME - total);
        total = EC_MIN_ETH_FRAME;
    }
    return total;
}

/*
 * Sends a frame containing a SINGLE datagram and waits for the response
 * (polling). Returns the WKC, or 0xFFFF on timeout/no response. `resp` (if
 * not NULL) receives a copy of the returned datagram's payload (data_len
 * bytes).
 */
static uint16_t do_single_datagram(struct ec_master *m, ec_cmd_t cmd,
        uint16_t adp, uint16_t ado, const void *data, uint16_t data_len,
        void *resp) {
    uint32_t off = frame_begin(m);
    uint8_t idx = m->dg_index++;
    uint32_t dg = ec_datagram_build(&m->txbuf[off], cmd, idx, adp, ado,
                                    data, data_len, 0);
    uint32_t total = frame_end(m, dg);

    if (gmac_send(m->txbuf, total) != GMAC_OK)
        return 0xFFFF;

    /* Wait for the response (round trip on the ring): polling ~5 ms. */
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(5000);
    while (timer_now_ticks() < deadline) {
        uint32_t rlen = 0;
        if (gmac_poll_recv(m->rxbuf, sizeof(m->rxbuf), &rlen) == GMAC_OK) {
            /* Check EtherCAT EtherType + echoed datagram index. */
            if (rlen >= EC_ETH_HEADER_SIZE + EC_FRAME_HEADER_SIZE +
                        EC_DATAGRAM_HEADER_SIZE + EC_DATAGRAM_FOOTER_SIZE &&
                m->rxbuf[12] == (uint8_t)(EC_ETHERTYPE >> 8) &&
                m->rxbuf[13] == (uint8_t)(EC_ETHERTYPE & 0xFF)) {
                const uint8_t *rdg = &m->rxbuf[off];
                if (rdg[1] == idx) {   /* same index -> our datagram */
                    uint16_t wkc = ec_datagram_wkc(rdg, data_len);
                    if (resp && data_len)
                        memcpy(resp, ec_datagram_data(rdg), data_len);
                    return wkc;
                }
            }
            /* otherwise: non-EtherCAT frame (network noise) -> keep polling */
        }
    }
    return 0xFFFF; /* timeout */
}

/* ====================================================================== */
/* API — lifecycle                                                        */
/* ====================================================================== */

ec_master_t *ecrt_request_master(unsigned int master_index) {
    (void)master_index;
    struct ec_master *m = &g_master;
    if (m->in_use) return NULL;
    memset(m, 0, sizeof(*m));

    /* Board MAC (gmac_demo: C0:74:2B:FC:33:A4). */
    static const uint8_t mac[6] = { 0xC0, 0x74, 0x2B, 0xFC, 0x33, 0xA4 };
    memcpy(m->mac, mac, 6);

    gmac_status_t st = gmac_init(mac, &m->gmac);
    if (st != GMAC_OK) {
        printf("[ecat] gmac_init failed (st=%d) — master not available.\n", (int)st);
        return NULL;
    }
    m->in_use = 1;
    printf("[ecat] master reserved (GMAC link %d Mbit/s %s, MAC "
           "%02X:%02X:%02X:%02X:%02X:%02X).\n",
           m->gmac.speed, m->gmac.duplex ? "full" : "half",
           m->mac[0], m->mac[1], m->mac[2], m->mac[3], m->mac[4], m->mac[5]);
    return m;
}

void ecrt_release_master(ec_master_t *master) {
    if (!master) return;
    master->activated = 0;
    master->in_use = 0;
}

ec_domain_t *ecrt_master_create_domain(ec_master_t *master) {
    if (!master || master->n_domains >= EC_MAX_DOMAINS) return NULL;
    struct ec_domain *d = &master->domains[master->n_domains++];
    memset(d, 0, sizeof(*d));
    d->master = master;
    d->in_use = 1;
    return d;
}

ec_slave_config_t *ecrt_master_slave_config(ec_master_t *master,
        uint16_t alias, uint16_t position,
        uint32_t vendor_id, uint32_t product_code) {
    if (!master || master->n_configs >= EC_MAX_SLAVES) return NULL;
    struct ec_slave_config *sc = &master->slaves[master->n_configs++];
    memset(sc, 0, sizeof(*sc));
    sc->master = master;
    sc->in_use = 1;
    sc->alias = alias;
    sc->position = position;
    sc->station_addr = (uint16_t)(0x1000u + position); /* configured station addr */
    sc->vendor_id = vendor_id;
    sc->product_code = product_code;
    sc->al_state = EC_AL_STATE_INIT;
    sc->sm_out_index = 2;
    sc->sm_in_index = 3;
    return sc;
}

/* Walks the ec_sync_info_t array to compute the size (bytes) of outputs and
 * inputs, and identify the corresponding SM indices. */
int ecrt_slave_config_pdos(ec_slave_config_t *sc, unsigned int n_syncs,
        const ec_sync_info_t syncs[]) {
    (void)n_syncs;
    if (!sc || !syncs) return -1;
    sc->syncs = syncs;
    sc->out_bytes = 0;
    sc->in_bytes = 0;

    for (const ec_sync_info_t *s = syncs; s->index != 0xff; s++) {
        uint32_t bits = 0;
        for (unsigned p = 0; p < s->n_pdos; p++) {
            const ec_pdo_info_t *pdo = &s->pdos[p];
            for (unsigned e = 0; e < pdo->n_entries; e++)
                bits += pdo->entries[e].bit_length;
        }
        uint32_t bytes = (bits + 7u) / 8u;
        if (s->dir == EC_DIR_OUTPUT && bytes) {
            sc->out_bytes += bytes;
            sc->sm_out_index = s->index;
        } else if (s->dir == EC_DIR_INPUT && bytes) {
            sc->in_bytes += bytes;
            sc->sm_in_index = s->index;
        }
    }
    printf("[ecat] slave %u PDO config : outputs=%lu b (SM%u), inputs=%lu b (SM%u)\n",
           sc->position, (unsigned long)sc->out_bytes, sc->sm_out_index,
           (unsigned long)sc->in_bytes, sc->sm_in_index);
    return 0;
}

int ecrt_domain_reg_pdo_entry_list(ec_domain_t *domain,
        const ec_pdo_entry_reg_t *regs) {
    if (!domain || !regs) return -1;
    struct ec_master *m = domain->master;

    /* Process image layout: first all the OUTPUTS, then the INPUTS. We
     * first compute the total outputs of all the configured slaves. */
    uint32_t total_out = 0, total_in = 0;
    for (unsigned i = 0; i < m->n_configs; i++) {
        total_out += m->slaves[i].out_bytes;
        total_in  += m->slaves[i].in_bytes;
    }
    domain->out_size = total_out;
    domain->in_size  = total_in;
    domain->image_size = total_out + total_in;
    if (domain->image_size > EC_PROCESS_IMG_SIZE) {
        printf("[ecat] process image too large (%lu o).\n",
               (unsigned long)domain->image_size);
        return -1;
    }

    /* Assign an offset to each registered PDO entry. We advance a separate
     * cursor for outputs (from 0) and inputs (from out_size). */
    uint32_t cur_out = 0, cur_in = total_out;
    domain->n_regs = 0;

    for (const ec_pdo_entry_reg_t *r = regs;
         r->index != 0 || r->product_code != 0; r++) {
        if (domain->n_regs >= EC_MAX_REGS) break;

        /* Find the slave + determine whether it is an input or an output by
         * inspecting the SM config (index 0x7000 = output, 0x6000 = input
         * by ESI convention; we rely on the mapping's SM direction). */
        ec_reg_entry_t *e = &domain->regs[domain->n_regs];
        e->index = r->index;
        e->subindex = r->subindex;

        /* Determine the direction: look up the index in the slave's syncs
         * matching (vendor, product). */
        uint8_t dir = EC_DIR_INPUT;
        uint8_t bitlen = 16;
        for (unsigned i = 0; i < m->n_configs; i++) {
            struct ec_slave_config *sc = &m->slaves[i];
            if (sc->vendor_id != r->vendor_id ||
                sc->product_code != r->product_code || !sc->syncs)
                continue;
            for (const ec_sync_info_t *s = sc->syncs; s->index != 0xff; s++) {
                for (unsigned p = 0; p < s->n_pdos; p++) {
                    const ec_pdo_info_t *pdo = &s->pdos[p];
                    for (unsigned k = 0; k < pdo->n_entries; k++) {
                        if (pdo->entries[k].index == r->index &&
                            pdo->entries[k].subindex == r->subindex) {
                            dir = (s->dir == EC_DIR_OUTPUT) ? EC_DIR_OUTPUT
                                                            : EC_DIR_INPUT;
                            bitlen = pdo->entries[k].bit_length;
                        }
                    }
                }
            }
        }
        e->dir = dir;
        e->bit_length = bitlen;
        uint32_t bytes = (bitlen + 7u) / 8u;
        if (dir == EC_DIR_OUTPUT) {
            e->byte_offset = cur_out;
            cur_out += bytes;
        } else {
            e->byte_offset = cur_in;
            cur_in += bytes;
        }
        e->user_offset = r->offset;
        if (r->offset) *r->offset = e->byte_offset;
        if (r->bit_position) *r->bit_position = 0;

        printf("[ecat] domain reg : idx=0x%04X sub=%u dir=%s off=%lu\n",
               r->index, r->subindex,
               (dir == EC_DIR_OUTPUT) ? "OUT" : "IN",
               (unsigned long)e->byte_offset);
        domain->n_regs++;
    }
    return 0;
}

void ecrt_slave_config_dc(ec_slave_config_t *sc, uint16_t assign_activate,
        uint32_t sync0_cycle, int32_t sync0_shift,
        uint32_t sync1_cycle, int32_t sync1_shift) {
    (void)sync1_cycle; (void)sync1_shift;
    if (!sc) return;
    sc->dc_used = 1;
    sc->dc_assign_activate = assign_activate;
    sc->dc_sync0_cycle = sync0_cycle;
    sc->dc_sync0_shift = sync0_shift;
    printf("[ecat] slave %u DC : assign=0x%04X cycle=%lu ns shift=%ld ns\n",
           sc->position, assign_activate,
           (unsigned long)sync0_cycle, (long)sync0_shift);
}

/* ====================================================================== */
/* ESM — application state machine (INIT→PREOP→SAFEOP→OP)                 */
/* ====================================================================== */

/* Requests an AL state from a slave (by station address, FPWR AL_CONTROL)
 * then waits for AL_STATUS to reflect the state (or reports an error).
 * Returns 0 if OK. */
static int request_state(struct ec_master *m, uint16_t station,
        uint8_t state, const char *name) {
    uint16_t ctrl = state; /* no error ack on the 1st attempt */
    uint8_t buf[2] = { (uint8_t)(ctrl & 0xFF), (uint8_t)(ctrl >> 8) };
    uint16_t wkc = do_single_datagram(m, EC_CMD_FPWR, station,
                                      EC_REG_AL_CONTROL, buf, 2, NULL);
    if (wkc == 0 || wkc == 0xFFFF) {
        printf("[ecat] -> %s : AL_CONTROL signal no response (wkc=%u)\n",
               name, wkc);
        return -1;
    }

    /* Poll AL_STATUS until the requested state or timeout. */
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(EC_STATE_TIMEOUT_US);
    while (timer_now_ticks() < deadline) {
        uint8_t st[2] = { 0, 0 };
        uint16_t rwkc = do_single_datagram(m, EC_CMD_FPRD, station,
                                           EC_REG_AL_STATUS, st, 2, st);
        if (rwkc != 0 && rwkc != 0xFFFF) {
            uint8_t cur = (uint8_t)(st[0] & EC_AL_STATE_MASK);
            if (cur == (state & EC_AL_STATE_MASK)) {
                printf("[ecat] -> %s : OK (AL_STATUS=0x%02X)\n", name, cur);
                return 0;
            }
            if (st[0] & 0x10) { /* AL error bit */
                uint8_t code[2] = { 0, 0 };
                do_single_datagram(m, EC_CMD_FPRD, station,
                                   EC_REG_AL_STATUS_CODE, code, 2, code);
                printf("[ecat] -> %s : AL ERROR (status=0x%02X code=0x%02X%02X)\n",
                       name, st[0], code[1], code[0]);
                return -1;
            }
        }
        udelay_us(1000);
    }
    printf("[ecat] -> %s : TIMEOUT (slave not responding / no slave)\n", name);
    return -1;
}

/*
 * Reads the 8 config bytes of a Sync Manager (FPRD SM(index)) and decodes
 * its fields. Returns the datagram's WKC (0 = the slave did NOT respond ->
 * values NOT reliable). The *_out may be NULL.
 *
 * IMPORTANT: the content is valid ONLY if the WKC is > 0. An FPRD whose
 * WKC stays 0 returns the zeros we emitted (-> misleading phys=0/len=0).
 * That is the likely cause of a "SM phys=0 len=0".
 */
__attribute__((unused))
static uint16_t read_sm(struct ec_master *m, uint16_t station, uint8_t sm_index,
        uint16_t *phys_out, uint16_t *len_out, uint8_t *ctrl_out,
        uint8_t *status_out, uint8_t *act_out) {

    uint8_t sm[8] = {0};
    uint16_t wkc = do_single_datagram(m, EC_CMD_FPRD, station,
                                      EC_REG_SM(sm_index), sm, 8, sm);
    if (phys_out)   *phys_out   = (uint16_t)(sm[0] | (sm[1] << 8));
    if (len_out)    *len_out    = (uint16_t)(sm[2] | (sm[3] << 8));
    if (ctrl_out)   *ctrl_out   = sm[4];
    if (status_out) *status_out = sm[5];
    if (act_out)    *act_out    = sm[6];
    return wkc;
}

/*
 * Reads ONE word (16 bits) from the slave's SII/EEPROM via the ESC's EEPROM
 * interface (registers 0x0502+). Sequence QUOTED from IgH's `fsm_sii.c`:
 *   1) FPWR 0x0502 (4 B) = {0x80, 0x01, word_offset:u16} -> read request;
 *   2) poll FPRD 0x0502 (10 B): byte[1] & 0x81 = busy -> retry;
 *      byte[1] & 0x20 = error; otherwise the 2 data words are at offset +6.
 * Returns 0 if OK (val filled), -1 otherwise.
 *
 * THIS IS HOW IgH obtains the real physical addresses of the Sync
 * Managers: it READS the SyncManager category (0x29) of the SII, NOT the SM
 * registers (0x0800+) which stay at 0 until the master has written them.
 */
static int sii_read_word(struct ec_master *m, uint16_t station,
        uint16_t word_offset, uint16_t *val) {
    uint8_t req[4];
    req[0] = 0x80;                          /* 2 address bytes */
    req[1] = 0x01;                          /* request read operation */
    req[2] = (uint8_t)(word_offset & 0xFF);
    req[3] = (uint8_t)(word_offset >> 8);
    uint16_t wkc = do_single_datagram(m, EC_CMD_FPWR, station, EC_REG_SII_CTRL,
                                      req, 4, NULL);
    if (wkc == 0 || wkc == 0xFFFF) return -1;

    /* Poll the read status (busy bit) + fetch the 10 bytes. */
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(10000);
    while (timer_now_ticks() < deadline) {
        uint8_t st[10] = {0};
        uint16_t rwkc = do_single_datagram(m, EC_CMD_FPRD, station,
                                           EC_REG_SII_CTRL, st, 10, st);
        if (rwkc == 0 || rwkc == 0xFFFF) { udelay_us(200); continue; }
        if (st[1] & EC_SII_STAT_ERROR) return -1;
        if (st[1] & EC_SII_STAT_BUSY) { udelay_us(200); continue; }
        /* SII data: 4 bytes at offset +6 (we take the 1st word). */
        *val = (uint16_t)(st[6] | (st[7] << 8));
        return 0;
    }
    return -1;
}

/*
 * Reads the SyncManager category (0x29) of the SII to retrieve the
 * physical address / length / control / enable of each SM (like IgH
 * `slave.c` ec_slave_fetch_sii_syncs). Fills sm_phys[]/sm_len[]/sm_ctrl[]/
 * sm_en[] for indices 0..max_sm-1. Returns the number of SMs found in the
 * SII (0 = failure/silent SII -> the caller falls back to the default).
 *
 * Category traversal (IgH fsm_slave_scan.c): starting at WORD 0x40, each
 * category = {type:u16 (bits0..14), size:u16 (in WORDS)} followed by `size`
 * words; end = type 0xFFFF. Category 0x0029 contains N×4 words (per SM):
 *   word0 = physical_start_address, word1 = length, word2 = control|status,
 *   word3 = enable.
 */
static unsigned read_syncs_from_sii(struct ec_master *m, uint16_t station,
        uint16_t sm_phys[], uint16_t sm_len[], uint8_t sm_ctrl[],
        uint8_t sm_en[], unsigned max_sm) {
    uint16_t word = EC_SII_FIRST_CAT_WORD;
    unsigned found = 0;
    /* Safety: bound the number of categories traversed. */
    for (unsigned guard = 0; guard < 64; guard++) {
        uint16_t type = 0, size = 0;
        if (sii_read_word(m, station, word, &type) != 0) return found;
        if (type == 0xFFFF) break;               /* end of categories */
        uint16_t cat_type = (uint16_t)(type & 0x7FFF);
        if (sii_read_word(m, station, (uint16_t)(word + 1), &size) != 0)
            return found;
        uint16_t data_word = (uint16_t)(word + 2); /* start of the data (words) */

        if (cat_type == EC_SII_CAT_SYNC) {
            unsigned n = size / 4;               /* 4 words per SM */
            for (unsigned i = 0; i < n && i < max_sm; i++) {
                uint16_t w0=0, w1=0, w2=0, w3=0;
                uint16_t base = (uint16_t)(data_word + i * 4);
                if (sii_read_word(m, station, base,     &w0) ||
                    sii_read_word(m, station, base + 1, &w1) ||
                    sii_read_word(m, station, base + 2, &w2) ||
                    sii_read_word(m, station, base + 3, &w3))
                    return found;
                sm_phys[i] = w0;
                sm_len[i]  = w1;
                sm_ctrl[i] = (uint8_t)(w2 & 0xFF);
                sm_en[i]   = (uint8_t)(w3 & 0xFF);
                if (i + 1 > found) found = i + 1;
            }
        }
        word = (uint16_t)(data_word + size);     /* next category */
    }
    return found;
}

/* Configures a slave's Sync Manager (8 bytes at SM(index)). */
static void config_sm(struct ec_master *m, uint16_t station, uint8_t sm_index,
        uint16_t phys_addr, uint16_t length, uint8_t control) {


    uint8_t sm[8];
    sm[0] = (uint8_t)(phys_addr & 0xFF);   /* Physical Start Address */
    sm[1] = (uint8_t)(phys_addr >> 8);
    sm[2] = (uint8_t)(length & 0xFF);      /* Length */
    sm[3] = (uint8_t)(length >> 8);
    sm[4] = control;                       /* Control byte */
    sm[5] = 0x00;                          /* Status (RO on the ECAT side) */
    sm[6] = EC_SM_ACT_ENABLE;              /* Activate: SM enable */
    sm[7] = 0x00;                          /* PDI control */
    do_single_datagram(m, EC_CMD_FPWR, station, EC_REG_SM(sm_index), sm, 8, NULL);
}

/* Configures a slave's FMMU (16 bytes at FMMU(index)): maps a logical range
 * [logaddr, logaddr+length) to a physical SM of the slave. */
static void config_fmmu(struct ec_master *m, uint16_t station, uint8_t fmmu_index,
        uint32_t logaddr, uint16_t length, uint16_t phys_addr, uint8_t type) {
    uint8_t f[16];
    memset(f, 0, sizeof(f));
    f[0] = (uint8_t)(logaddr & 0xFF);         /* Logical Start Address */
    f[1] = (uint8_t)((logaddr >> 8) & 0xFF);
    f[2] = (uint8_t)((logaddr >> 16) & 0xFF);
    f[3] = (uint8_t)((logaddr >> 24) & 0xFF);
    f[4] = (uint8_t)(length & 0xFF);          /* Length (bytes) */
    f[5] = (uint8_t)(length >> 8);
    f[6] = 0x00;                              /* Logical Start Bit */
    f[7] = 0x07;                              /* Logical Stop Bit */
    f[8] = (uint8_t)(phys_addr & 0xFF);       /* Physical Start Address */
    f[9] = (uint8_t)(phys_addr >> 8);
    f[10] = 0x00;                             /* Physical Start Bit */
    f[11] = type;                             /* 1=read(inputs) 2=write(outputs) */
    f[12] = EC_FMMU_ENABLE;                   /* Activate */
    do_single_datagram(m, EC_CMD_FPWR, station, EC_REG_FMMU(fmmu_index), f, 16, NULL);
}

int ecrt_master_activate(ec_master_t *master) {
    if (!master) return -1;
    struct ec_master *m = master;

    printf("[ecat] === activation : scan + config + ESM INIT->OP ===\n");

    /* --- 1. Scan: count the slaves (BRD AL_STATUS, WKC = slave count) --- */
    uint8_t rd[2] = { 0, 0 };
    uint16_t wkc = do_single_datagram(m, EC_CMD_BRD, 0x0000, EC_REG_AL_STATUS,
                                      rd, 2, rd);
    if (wkc == 0xFFFF) {
        printf("[ecat] scan : NO response (no slave on the bus ?).\n");
        m->slaves_on_bus = 0;
    } else {
        m->slaves_on_bus = wkc;
        printf("[ecat] scan : %u slave(s) detected (BRD wkc=%u, AL=0x%02X).\n",
               m->slaves_on_bus, wkc, rd[0]);
    }
    if (m->slaves_on_bus == 0) {
        printf("[ecat] activation abort : 0 slave. (Connect an EtherCAT slave)\n");
        return -1;
    }

    /* --- 2. Addressing: assign a station address to each slave (by
     *        auto-incremented position). APWR 0x0010 = station address. --- */
    for (unsigned i = 0; i < m->n_configs; i++) {
        struct ec_slave_config *sc = &m->slaves[i];
        uint8_t sa[2] = { (uint8_t)(sc->station_addr & 0xFF),
                          (uint8_t)(sc->station_addr >> 8) };
        /* APWR: address = -(position) on 16 bits (auto-increment). */
        uint16_t adp = (uint16_t)(0 - sc->position);
        do_single_datagram(m, EC_CMD_APWR, adp, EC_REG_STATION_ADDR, sa, 2, NULL);
        printf("[ecat] slave %u : station address = 0x%04X\n",
               sc->position, sc->station_addr);
    }

    /* --- 3. ESM: INIT -> PREOP (by station address) --- */
    for (unsigned i = 0; i < m->n_configs; i++) {
        struct ec_slave_config *sc = &m->slaves[i];
        if (request_state(m, sc->station_addr, EC_AL_STATE_INIT, "INIT") != 0) {
            /* some slaves already start in INIT — we continue */
        }
        if (request_state(m, sc->station_addr, EC_AL_STATE_PREOP, "PREOP") != 0)
            return -1;
        sc->al_state = EC_AL_STATE_PREOP;
        sc->online = 1;
    }

    /* --- 4. SM config (outputs/inputs) + FMMU (logical mapping) --- */
    uint32_t log_out = 0;                    /* outputs start at 0 */
    uint32_t log_in = 0;
    for (unsigned i = 0; i < m->n_configs; i++)
        log_in += m->slaves[i].out_bytes;    /* inputs after the outputs */

    uint32_t cur_out = 0, cur_in = log_in;
    for (unsigned i = 0; i < m->n_configs; i++) {
        struct ec_slave_config *sc = &m->slaves[i];

        /* IgH does NOT READ
         * the SM registers (0x0800+) — these stay at 0 until the master
         * has written them. IgH READS the slave's SyncManager category
         * (0x29) from the SII/EEPROM (fsm_slave_scan.c ->
         * ec_slave_fetch_sii_syncs), which gives the REAL
         * `physical_start_address` of each SM (fixed by the manufacturer).
         * We reproduce this exactly via `read_syncs_from_sii()`. The PDO
         * mapping (SM2 outputs, SM3 inputs) comes from the static config
         * (Tiloup9291/YAEMAA). */
        uint16_t sm2_phys = 0x1000; /* fallback if the SII is unreadable */
        uint16_t sm3_phys = 0x1100;
        uint16_t sm2_len = (uint16_t)sc->out_bytes; /* fallback = PDO size */
        uint16_t sm3_len = (uint16_t)sc->in_bytes;
        uint8_t  sm2_ctrl = EC_SM_CTRL_BUFFERED | EC_SM_CTRL_DIR_WRITE;
        uint8_t  sm3_ctrl = EC_SM_CTRL_BUFFERED;
        int found_out = 0, found_in = 0;
        {
            uint16_t sm_phys[8] = {0}, sm_len[8] = {0};
            uint8_t  sm_ctrl[8] = {0}, sm_en[8] = {0};
            unsigned n = read_syncs_from_sii(m, sc->station_addr,
                                             sm_phys, sm_len, sm_ctrl, sm_en, 8);
            printf("[ecat] slave %u : %u SM read from SII/EEPROM :\n",
                   sc->position, n);
            for (unsigned si = 0; si < n; si++) {
                printf("[ecat]   SM%u : phys=0x%04X len=%u ctrl=0x%02X en=%u\n",
                       si, sm_phys[si], sm_len[si], sm_ctrl[si], sm_en[si]);
            }
            /* We use the REAL ADDRESS, LENGTH and CONTROL read from the
             * SII for each process-data SM (like IgH `ec_sync_page`). The
             * real SM length (here SM2=6 B, SM3=14 B) is OFTEN LARGER than
             * our mapped PDO (2 B): a module has several objects in the
             * same SM. Configuring the SM with a TOO SHORT length (2)
             * invalidates the SM -> the slave puts the outputs in SAFE
             * state (0) / watchdog. The full SII length is therefore
             * REQUIRED. The SII control also carries the right
             * mode/watchdog. */
            if (sc->sm_out_index < n && sm_phys[sc->sm_out_index]) {
                sm2_phys = sm_phys[sc->sm_out_index];
                sm2_len  = sm_len[sc->sm_out_index];
                sm2_ctrl = sm_ctrl[sc->sm_out_index];
                found_out = 1;
            }
            if (sc->sm_in_index < n && sm_phys[sc->sm_in_index]) {
                sm3_phys = sm_phys[sc->sm_in_index];
                sm3_len  = sm_len[sc->sm_in_index];
                sm3_ctrl = sm_ctrl[sc->sm_in_index];
                found_in = 1;
            }
            printf("[ecat] slave %u : outputs SM -> SM%u phys=0x%04X len=%u ctrl=0x%02X (%s) ;\n"
                   "[ecat]            inputs  SM -> SM%u phys=0x%04X len=%u ctrl=0x%02X (%s)\n",
                   sc->position, sc->sm_out_index, sm2_phys, sm2_len, sm2_ctrl,
                   found_out ? "SII" : "hard RETREAT",
                   sc->sm_in_index, sm3_phys, sm3_len, sm3_ctrl,
                   found_in ? "SII" : "hard RETREAT");
        }

        sc->sm_out_phys = sm2_phys;
        sc->sm_in_phys  = sm3_phys;
        sc->sm_out_len  = sm2_len;
        sc->sm_in_len   = sm3_len;
        sc->sm_out_ctrl = sm2_ctrl;
        sc->sm_in_ctrl  = sm3_ctrl;

        (void)sm2_len; (void)sm3_len;   /* default_length SII = NOT used (see below) */

        /* The SM LENGTH to write is NOT the SII's
         * `default_length` (SM2=6, SM3=14) but the SIZE OF THE PDOs
         * ACTUALLY MAPPED (2 B). This is what IgH's
         * `ec_fsm_slave_config_enter_pdo_sync` does: `size =
         * ec_pdo_list_total_size(&sync_config->pdos)`, NOT
         * `sync->default_length`. Configuring SM3 with len=14 BREAKS it
         * (FPRD wkc=0, DI=0). We therefore keep:
         *   - the PHYSICAL ADDRESS from the SII (SM2=0x0F00, SM3=0x1000)
         *   - the LENGTH = size of the mapped PDO (out_bytes/in_bytes = 2 B);
         *   - the CONTROL from the SII (buffered/mailbox mode + watchdog),
         *     just forcing the direction bit (bit2: write for outputs SM,
         *     read for inputs SM) like `ec_sync_page`. */
        if (sc->out_bytes) {
            config_sm(m, sc->station_addr, sc->sm_out_index, sm2_phys,
                      (uint16_t)sc->out_bytes,
                      (uint8_t)(sm2_ctrl | EC_SM_CTRL_DIR_WRITE));
            /* FMMU0 = outputs (logical write -> SM2), PDO size. */
            config_fmmu(m, sc->station_addr, 0, log_out + cur_out,
                        (uint16_t)sc->out_bytes, sm2_phys, EC_FMMU_WRITE);
            cur_out += sc->out_bytes;
        }
        if (sc->in_bytes) {
            config_sm(m, sc->station_addr, sc->sm_in_index, sm3_phys,
                      (uint16_t)sc->in_bytes,
                      (uint8_t)(sm3_ctrl & ~EC_SM_CTRL_DIR_WRITE));
            /* FMMU1 = inputs (logical read <- SM3), PDO size. */
            config_fmmu(m, sc->station_addr, 1, cur_in,
                        (uint16_t)sc->in_bytes, sm3_phys, EC_FMMU_READ);
            cur_in += sc->in_bytes;
        }




        /* DC (optional): enable SYNC0 if configured. */
        if (sc->dc_used && sc->dc_sync0_cycle) {
            uint8_t act[1] = { 0x03 }; /* SYNC0 + cyclic gen enable */
            do_single_datagram(m, EC_CMD_FPWR, sc->station_addr,
                               EC_REG_DC_SYNC_ACT, act, 1, NULL);
        }

        printf("[ecat] slave %u : SM+FMMU configured.\n", sc->position);
    }

    /* --- 5. ESM: PREOP -> SAFEOP -> OP --- */


    for (unsigned i = 0; i < m->n_configs; i++) {
        struct ec_slave_config *sc = &m->slaves[i];
        if (request_state(m, sc->station_addr, EC_AL_STATE_SAFEOP, "SAFEOP") != 0)
            return -1;
        sc->al_state = EC_AL_STATE_SAFEOP;
    }
    /* A process data cycle is required BEFORE OP (the slaves require valid
     * data on SM3). We emit a blank LRW frame. */
    for (unsigned d = 0; d < m->n_domains; d++) {
        ecrt_domain_queue(&m->domains[d]);
    }
    ecrt_master_send(m);
    udelay_us(2000);
    ecrt_master_receive(m);

    for (unsigned i = 0; i < m->n_configs; i++) {
        struct ec_slave_config *sc = &m->slaves[i];
        if (request_state(m, sc->station_addr, EC_AL_STATE_OP, "OP") != 0)
            return -1;
        sc->al_state = EC_AL_STATE_OP;
        sc->operational = 1;
    }

    m->activated = 1;
    printf("[ecat] === ENABLED : %u slave(s) in OP. ===\n", m->n_configs);
    return 0;
}

void ecrt_master_deactivate(ec_master_t *master) {
    if (!master) return;
    for (unsigned i = 0; i < master->n_configs; i++)
        request_state(master, master->slaves[i].station_addr,
                      EC_AL_STATE_PREOP, "PREOP");
    master->activated = 0;
}

/* ====================================================================== */
/* Domain API + real-time cycle                                           */
/* ====================================================================== */

uint8_t *ecrt_domain_data(ec_domain_t *domain) {
    if (!domain) return NULL;
    return domain->process_image;
}

/* The reception/emission of a process data cycle goes through a LOGICAL
 * LRW datagram (logical address 0), which writes the outputs AND reads the
 * inputs in a single pass on the ring. The frame is stored in txbuf at
 * ecrt_domain_queue and read back at ecrt_master_receive.
 *
 * To stay simple and deterministic, ecrt_master_send sends the LRW frame
 * built by the last ecrt_domain_queue, and ecrt_master_receive reads the
 * response and updates the INPUTS part of the process image + the WKC. */

/* Offset (in txbuf) of the LRW datagram + data length, stored between
 * queue() and send()/receive(). */
static uint32_t g_lrw_off;
static uint16_t g_lrw_len;
static uint8_t  g_lrw_idx;
static uint32_t g_lrw_total;

void ecrt_domain_queue(ec_domain_t *domain) {
    if (!domain) return;
    struct ec_master *m = domain->master;

    uint32_t off = frame_begin(m);
    g_lrw_off = off;
    g_lrw_idx = m->dg_index++;

    /* LRW data = the whole process image (outputs first, then inputs). The
     * outputs are written by the CPU; the inputs will be overwritten by
     * the slaves on the fly. We copy the current image as the payload. */
    uint16_t dlen = (uint16_t)domain->image_size;
    if (dlen == 0) dlen = 1; /* at least 1 byte for a valid LRW */
    g_lrw_len = dlen;

    uint32_t dg = ec_datagram_build_logical(&m->txbuf[off], EC_CMD_LRW,
            g_lrw_idx, 0x00000000u, domain->process_image, dlen, 0);
    g_lrw_total = frame_end(m, dg);
}

void ecrt_master_send(ec_master_t *master) {
    if (!master) return;
    if (g_lrw_total)
        gmac_send(master->txbuf, g_lrw_total);
}

void ecrt_master_receive(ec_master_t *master) {
    if (!master) return;
    struct ec_master *m = master;

    /* Poll for the LRW response (round trip on the ring). The actual round
     * trip on a small EtherCAT ring takes a few µs; we bound to 300 µs so
     * as NOT to overrun the 1 ms cycle (otherwise overruns). Beyond that =
     * lost frame → WKC=0. */
    uint64_t deadline = timer_now_ticks() + timer_us_to_ticks(300);
    while (timer_now_ticks() < deadline) {
        uint32_t rlen = 0;
        if (gmac_poll_recv(m->rxbuf, sizeof(m->rxbuf), &rlen) == GMAC_OK) {
            if (rlen >= EC_ETH_HEADER_SIZE + EC_FRAME_HEADER_SIZE +
                        EC_DATAGRAM_HEADER_SIZE + EC_DATAGRAM_FOOTER_SIZE &&
                m->rxbuf[12] == (uint8_t)(EC_ETHERTYPE >> 8) &&
                m->rxbuf[13] == (uint8_t)(EC_ETHERTYPE & 0xFF)) {
                const uint8_t *rdg = &m->rxbuf[g_lrw_off];
                if (rdg[1] == g_lrw_idx) {
                    m->rx_len = rlen;
                    /* Copy the payload (process image updated by the
                     * slaves) into the domain; the WKC into the domain. */
                    for (unsigned d = 0; d < m->n_domains; d++) {
                        struct ec_domain *dom = &m->domains[d];
                        if (dom->image_size) {
                            memcpy(dom->process_image, ec_datagram_data(rdg),
                                   dom->image_size);
                            dom->working_counter = ec_datagram_wkc(rdg, g_lrw_len);
                            dom->wc_state = (dom->working_counter == 0)
                                ? EC_WC_ZERO : EC_WC_COMPLETE;
                        }
                    }
                    return;
                }
            }
        }
    }
    /* No response: WKC = 0 (the slaves did not respond). */
    for (unsigned d = 0; d < m->n_domains; d++) {
        m->domains[d].working_counter = 0;
        m->domains[d].wc_state = EC_WC_ZERO;
    }
}

void ecrt_domain_process(ec_domain_t *domain) {
    /* The inputs have already been copied into process_image by
     * ecrt_master_receive. Nothing more to do here (the app reads via
     * EC_READ_* at the offsets). */
    (void)domain;
}

void ecrt_domain_state(const ec_domain_t *domain, ec_domain_state_t *state) {
    if (!domain || !state) return;
    state->working_counter = domain->working_counter;
    state->wc_state = domain->wc_state;
    state->redundancy_active = 0;
}

void ecrt_master_application_time(ec_master_t *master, uint64_t app_time) {
    if (master) master->app_time = app_time;
}

void ecrt_master_sync_reference_clock(ec_master_t *master) {
    if (!master || master->n_configs == 0) return;
    /* Writes the reference SYSTIME (FRMW/ARMW in IgH; here FPWR SYSTIME on
     * the 1st slave = reference clock). Simplified: sends the app_time. */
    struct ec_slave_config *ref = &master->slaves[0];
    uint8_t t[8];
    uint64_t v = master->app_time;
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    do_single_datagram(master, EC_CMD_FPWR, ref->station_addr,
                       EC_REG_DC_SYSTIME, t, 8, NULL);
}

void ecrt_master_sync_slave_clocks(ec_master_t *master) {
    /* Full DC drift compensation = not yet implemented. No-op (the reference
     * sync above is enough for simple cyclic PDO operation). */
    (void)master;
}

void ecrt_slave_config_state(const ec_slave_config_t *sc,
        ec_slave_config_state_t *state) {
    if (!sc || !state) return;
    state->online = sc->online;
    state->operational = sc->operational;
    state->al_state = sc->al_state;
}

/* ====================================================================== */
/* DIAGNOSTIC — direct probe of the input memory (physical FPRD)          */
/* ====================================================================== */
/*
 * DIRECTLY reads the 1st slave's input bytes via an FPRD at the PHYSICAL
 * address of its inputs SM (resolved at activation), instead of going
 * through the logical LRW + FMMU. Goal: ISOLATE the cause of "DI always 0":
 *
 *   - If this direct FPRD reads the real value (e.g. 0x0002) while the
 *     logical LRW reads 0 -> the bug is in the FMMU/logical address mapping
 *     (not in the SM's physical address). Lead: FMMU1 Logical Start
 *     Address / input offset.
 *   - If this direct FPRD ALSO reads 0 -> the inputs SM's physical address
 *     is WRONG (or the DI are not yet mapped on the slave side). Lead:
 *     scan other candidate addresses (see `probe`).
 *
 * `probe` != 0: instead of reading the resolved physical address, we SCAN a
 * list of common candidate addresses and display the one that returns
 * non-zero. Returns the number of non-zero bytes read at the resolved
 * address (0 if all zero).
 */
unsigned ecrt_master_probe_input(ec_master_t *master, int probe) {
    if (!master || master->n_configs == 0) return 0;
    struct ec_master *m = master;
    struct ec_slave_config *sc = &m->slaves[0];
    if (!sc->in_bytes) {
        printf("[ecat][probe] slave 0 without inputs.\n");
        return 0;
    }
    uint16_t len = (uint16_t)sc->in_bytes;
    uint8_t buf[32];
    if (len > sizeof(buf)) len = sizeof(buf);

    /* 1. Direct read at the resolved physical address of the inputs SM. */
    memset(buf, 0, sizeof(buf));
    uint16_t wkc = do_single_datagram(m, EC_CMD_FPRD, sc->station_addr,
                                      sc->sm_in_phys, buf, len, buf);
    unsigned nonzero = 0;
    for (uint16_t i = 0; i < len; i++) if (buf[i]) nonzero++;
    printf("[ecat][probe] direct FPRD SM inputs phys=0x%04X len=%u wkc=%u : ",
           sc->sm_in_phys, len, wkc);
    for (uint16_t i = 0; i < len; i++) printf("%02X ", buf[i]);
    printf("(%s)\n", nonzero ? "NOT NULL -> bug FMMU/logic" :
                               "ALL NULL -> SM address or slave mapping");

    /* 2. Scan of candidate physical addresses (if requested). Helps find
     *    the real input memory when the read above is zero. */
    if (probe) {
        static const uint16_t cand[] = {
            0x1000, 0x1002, 0x1080, 0x1100, 0x1100 + 2, 0x1200,
            0x1400, 0x1800, 0x1C00, 0x2000
        };
        printf("[ecat][probe] scanning of candidate physical input addresses:\n");
        for (unsigned k = 0; k < sizeof(cand) / sizeof(cand[0]); k++) {
            memset(buf, 0, sizeof(buf));
            uint16_t w = do_single_datagram(m, EC_CMD_FPRD, sc->station_addr,
                                            cand[k], buf, len, buf);
            unsigned nz = 0;
            for (uint16_t i = 0; i < len; i++) if (buf[i]) nz++;
            printf("[ecat][probe]   phys=0x%04X wkc=%u : ", cand[k], w);
            for (uint16_t i = 0; i < len; i++) printf("%02X ", buf[i]);
            printf("%s\n", nz ? "  <-- NOT NULL !" : "");
        }
    }
    return nonzero;
}
