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
 * ecat_demo.h — Demo: EtherCAT master (ecrt_*) on raw L2 GMAC.
 *
 * Reproduces the flow of a typical EtherCAT application (the reference
 * example through Tiloup9291/YAEMAA/oobThread.c) with a
 * 16-bit digital I/O slave: 1 output PDO (0x7000, DO) + 1 input PDO (0x6000,
 * DI), vendor 0x0000_0B95, product 0x0000_1500.
 *
 * Sequence (identical to Tiloup9291/YEAMAA's oobThread.c):
 *   ecrt_request_master -> create_domain -> slave_config -> slave_config_pdos
 *   -> domain_reg_pdo_entry_list -> slave_config_dc -> master_activate
 *   -> domain_data, then cycle loop:
 *   receive -> domain_process -> (EC_READ/EC_WRITE) -> application_time
 *   -> sync_reference_clock -> sync_slave_clocks -> domain_queue -> send.
 *
 * Succes on: ≥1 slave reaches OP, stable cyclic PDO (non-zero WKC),
 * cycle overrun = 0. Without a slave (or on QEMU), the demo self-ignores
 * cleanly (the master reports it) — useful to validate the build/logic chain.
 */
#ifndef RTOS_ETHERCAT_ECAT_DEMO_H
#define RTOS_ETHERCAT_ECAT_DEMO_H

/* Runs the EtherCAT master demo (blocking for ~a few seconds of cycles). */
void ecat_demo_run(void);

#endif /* RTOS_ETHERCAT_ECAT_DEMO_H */
