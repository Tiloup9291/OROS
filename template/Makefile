#
# OROS - OROS' Real-Time Operating System
# Copyright (C) 2026  Tiloup9291 <-> John Doe
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# =============================================================================
# Makefile — OROS baremetal AArch64 (RK3328 / Orange Pi R1 Plus LTS)
# kernel (MMU, GIC-400, timer, preemptive scheduler, PLC, logging)
# SMP wake-up (PSCI CPU_ON)
#
# Targets:
#   make            -> real hardware build (UART DW8250, GIC RK3328)
#   make qemu       -> build + QEMU virt (UART PL011, GIC QEMU)
#   make uimage     -> U-Boot image
#   make clean      -> cleans build/
# =============================================================================

CROSS      ?= aarch64-none-elf-
CC          = $(CROSS)gcc
OBJCOPY     = $(CROSS)objcopy
OBJDUMP     = $(CROSS)objdump
SIZE        = $(CROSS)size

BUILD       = build
LDSCRIPT    = linker/rk3328.ld

LOAD_ADDR      ?= 0x00200000
QEMU_LOAD_ADDR ?= 0x40080000

INCLUDES = -Iapp -Idrivers -Idrivers/uart -Idrivers/gpio -Idrivers/sdmmc -Idrivers/usb -Idrivers/gmac -Ikernel -Iarch/aarch64 -Ilib -Ilib/fatfs -Ilib/ethercat -Ifs \
           -Inet -Ilib/lwip/src/include -Ilib/lwip/port -Ilib/lwip/port/arch \
           -Ilib/wolfssl -Ilib/wolfssh




# -mcpu=cortex-a53: FP/SIMD enabled at boot (start.S); MMU enabled (mmu.c)
ARCHFLAGS = -mcpu=cortex-a53
CFLAGS    = $(ARCHFLAGS) -ffreestanding -std=c11 -O2 -g \
            -Wall -Wextra -Wno-unused-parameter \
            -fno-stack-protector -fno-common $(INCLUDES)
LDFLAGS   = $(ARCHFLAGS) -nostartfiles -T $(LDSCRIPT) \
            -Wl,--defsym=LOAD_ADDR=$(LOAD_ADDR) -Wl,-Map=$(BUILD)/kernel.map

LIBS      = -lc -lm -lgcc

# Objects
OBJ  = $(BUILD)/start.o \
       $(BUILD)/vectors.o \
       $(BUILD)/context.o \
       $(BUILD)/mmu.o \
       $(BUILD)/smp.o \
       $(BUILD)/timer.o \
       $(BUILD)/gic.o \
       $(BUILD)/pmu.o \
       $(BUILD)/main.o \
       $(BUILD)/sched.o \
       $(BUILD)/sync.o \
       $(BUILD)/klog.o \
       $(BUILD)/mailbox.o \
       $(BUILD)/mutex.o \
       $(BUILD)/plc.o \
       $(BUILD)/app_core0.o \
       $(BUILD)/app_core1.o \
       $(BUILD)/app_core2.o \
       $(BUILD)/app_core3.o \
       $(BUILD)/drivers_init.o \
       $(BUILD)/uart.o \
       $(BUILD)/gpio.o \
       $(BUILD)/sdmmc.o \
       $(BUILD)/usb_core.o \
       $(BUILD)/hcd_xhci.o \
       $(BUILD)/hcd_ehci.o \
       $(BUILD)/hcd_ohci.o \
       $(BUILD)/class_r8152.o \
       $(BUILD)/class_hid.o \
       $(BUILD)/kbd_service.o \
       $(BUILD)/gmac.o \
       $(BUILD)/ec_datagram.o \
       $(BUILD)/ec_master.o \
       $(BUILD)/ecat_task.o \
       $(BUILD)/ff.o \
       $(BUILD)/ffunicode.o \
       $(BUILD)/ffsystem.o \
       $(BUILD)/diskio.o \
       $(BUILD)/fs_mount.o \
       $(LWIP_OBJ) \
       $(BUILD)/lwip_sys_arch.o \
       $(BUILD)/netif_r8152.o \
       $(BUILD)/ecat_diag.o \
       $(BUILD)/net_shell.o \
       $(BUILD)/uart_shell.o \
       $(BUILD)/tcp_shell.o \
       $(WOLFCRYPT_OBJ) \
       $(WOLFSSH_OBJ) \
       $(BUILD)/wolf_port.o \
       $(BUILD)/ssh_server.o \
       $(BUILD)/net_task.o \
       $(BUILD)/newlib_stubs.o




TARGET_ELF = $(BUILD)/kernel.elf
TARGET_BIN = $(BUILD)/kernel.bin

.PHONY: all
all: $(TARGET_BIN)
	@$(SIZE) $(TARGET_ELF)
	@echo ""
	@echo "  Build OK -> $(TARGET_ELF) / $(TARGET_BIN)"
	@echo "  LOAD_ADDR = $(LOAD_ADDR)"

$(BUILD):
	@mkdir -p $(BUILD)

# ---- Assembler (arch) ----
# start.S contains the hardcoded SMP debug UART (RK3328 @0xFF130000); we pass
# it MMU_DEFS so it can neutralize it under QEMU (-DMMU_QEMU).
$(BUILD)/start.o: arch/aarch64/start.S | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
$(BUILD)/vectors.o: arch/aarch64/vectors.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/context.o: arch/aarch64/context.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- C (arch) ----
$(BUILD)/mmu.o: arch/aarch64/mmu.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
$(BUILD)/smp.o: arch/aarch64/smp.c | $(BUILD)
	$(CC) $(CFLAGS) $(PSCI_DEFS) -c $< -o $@
$(BUILD)/timer.o: arch/aarch64/timer.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/gic.o: arch/aarch64/gic.c | $(BUILD)
	$(CC) $(CFLAGS) $(GIC_DEFS) -c $< -o $@
$(BUILD)/pmu.o: arch/aarch64/pmu.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


# ---- C (kernel) ----
# main.c uses PSCI_HVC (QEMU shutdown by SYSTEM_OFF): pass it PSCI_DEFS.
$(BUILD)/main.o: kernel/main.c | $(BUILD)
	$(CC) $(CFLAGS) $(PSCI_DEFS) -c $< -o $@

$(BUILD)/sched.o: kernel/sched.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sync.o: kernel/sync.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/klog.o: kernel/klog.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/mailbox.o: kernel/mailbox.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/mutex.o: kernel/mutex.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/plc.o: kernel/plc.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- C (application layer: the ONLY files to be coded) ----
# app_core0.c : EtherCAT hook  (RT_HARD, called by ecat_task.c)
# app_core1.c : PLC engine     (RT_HARD, plc_run loop ready)
# app_core2.c : supervisor     (IO_SOFT, drivers_init + permanent services)
# app_core3.c : soft periodic  (RT_SOFT, absolute-cadence loop ready)
$(BUILD)/app_core0.o: app/app_core0.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/app_core1.o: app/app_core1.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
$(BUILD)/app_core2.o: app/app_core2.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
$(BUILD)/app_core3.o: app/app_core3.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Permanent driver bring-up (Core2) ----
# drivers_init.c: SD card + PERMANENT FAT mount, USB2 controllers + USB-A
# keyboard hot-plug service. Includes sdmmc.h/usb -> $(MMU_DEFS).
$(BUILD)/drivers_init.o: drivers/drivers_init.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@


# ---- C (drivers / lib) ----
# gpio.c and sdmmc.c use MMU_QEMU to neutralize themselves under QEMU
# (RK3328 hardware missing): we therefore pass them $(MMU_DEFS).
$(BUILD)/uart.o: drivers/uart/uart.c | $(BUILD)
	$(CC) $(CFLAGS) $(UART_DEFS) -c $< -o $@
$(BUILD)/gpio.o: drivers/gpio/gpio.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
$(BUILD)/sdmmc.o: drivers/sdmmc/sdmmc.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@

# ---- USB: xHCI/DWC3 host + core + demo ----
# hcd_xhci.c neutralizes itself under MMU_QEMU (RK3328 MMIO missing): $(MMU_DEFS).
$(BUILD)/usb_core.o: drivers/usb/usb_core.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
$(BUILD)/hcd_xhci.o: drivers/usb/hcd_xhci.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
# hcd_ehci.c / hcd_ohci.c: USB2 HCD (companion) — neutralize themselves under MMU_QEMU.
$(BUILD)/hcd_ehci.o: drivers/usb/hcd_ehci.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
$(BUILD)/hcd_ohci.o: drivers/usb/hcd_ohci.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
# class_hid.c: HID keyboard class driver (independent of the HCD).
$(BUILD)/class_hid.o: drivers/usb/class_hid.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
# kbd_service.c: PERMANENT USB-A keyboard service (hot-plug state machine),
# polled by Core2. Works with or without a keyboard plugged in at boot.
$(BUILD)/kbd_service.o: drivers/usb/kbd_service.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@

# class_r8152.c: RTL8153B class driver (independent of the controller). Some
# register offsets are defined for future use -> -Wno-unused.
$(BUILD)/class_r8152.o: drivers/usb/class_r8152.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -Wno-unused-const-variable -Wno-unused-function -c $< -o $@

# ---- GMAC: DWMAC 1000 raw L2 + YT8531C PHY + demo ----
# gmac.c/gmac_demo.c neutralize themselves under MMU_QEMU (RK3328 MMIO missing).
$(BUILD)/gmac.o: drivers/gmac/gmac.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@

# ---- EtherCAT: HOME-GROWN master (ecrt_*) on raw GMAC ----
# Does not depend on direct RK3328 MMIO: goes through the GMAC API. On
# QEMU, gmac_init -> ENODEV so ecrt_request_master -> NULL (demo self-ignores).
# We still pass $(MMU_DEFS) for consistency (gmac.h includes board defines).
$(BUILD)/ec_datagram.o: lib/ethercat/ec_datagram.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
$(BUILD)/ec_master.o: lib/ethercat/ec_master.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@
# ecat_task.c: PERMANENT EtherCAT master on Core0 (infinite thread).
$(BUILD)/ecat_task.o: lib/ethercat/ecat_task.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@


$(BUILD)/newlib_stubs.o: lib/newlib_stubs.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


# ---- FatFs (ChaN, R0.15 — upstream BSD sources) + diskio port ----
# Upstream sources compiled as-is (upstream warnings neutralized).
# diskio.c includes sdmmc.h: pass it $(MMU_DEFS) to stay QEMU-consistent.
FATFS_CFLAGS = $(CFLAGS) -Wno-unused-function -Wno-sign-compare
$(BUILD)/ff.o: lib/fatfs/ff.c | $(BUILD)
	$(CC) $(FATFS_CFLAGS) -c $< -o $@
$(BUILD)/ffunicode.o: lib/fatfs/ffunicode.c | $(BUILD)
	$(CC) $(FATFS_CFLAGS) -c $< -o $@
$(BUILD)/ffsystem.o: lib/fatfs/ffsystem.c | $(BUILD)
	$(CC) $(FATFS_CFLAGS) -c $< -o $@
$(BUILD)/diskio.o: lib/fatfs/diskio.c | $(BUILD)
	$(CC) $(FATFS_CFLAGS) $(MMU_DEFS) -c $< -o $@

# ---- FS: PERMANENT FAT mount (volume 0: stays mounted) ----
$(BUILD)/fs_mount.o: fs/fs_mount.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@


# ---- lwIP (upstream BSD sources, NO_SYS=1 mode) ----------------
# Core subset sufficient for IPv4 + ARP + ICMP + UDP + TCP + RAW in
# "main loop" mode (no netconn/sockets API, no tcpip.c). The config is in
# lib/lwip/port/lwipopts.h (found via -Ilib/lwip/port). Upstream warnings
# neutralized (third-party code). NO_SYS=1 => minimal sys_arch (sys_now).
LWIP_SRC = lib/lwip/src
LWIP_CFLAGS = $(CFLAGS) -Wno-unused-parameter -Wno-sign-compare -Wno-address

# core
LWIP_OBJ = \
       $(BUILD)/lwip_init.o \
       $(BUILD)/lwip_def.o \
       $(BUILD)/lwip_inet_chksum.o \
       $(BUILD)/lwip_ip.o \
       $(BUILD)/lwip_mem.o \
       $(BUILD)/lwip_memp.o \
       $(BUILD)/lwip_netif.o \
       $(BUILD)/lwip_pbuf.o \
       $(BUILD)/lwip_raw.o \
       $(BUILD)/lwip_stats.o \
       $(BUILD)/lwip_sys.o \
       $(BUILD)/lwip_tcp.o \
       $(BUILD)/lwip_tcp_in.o \
       $(BUILD)/lwip_tcp_out.o \
       $(BUILD)/lwip_timeouts.o \
       $(BUILD)/lwip_udp.o \
       $(BUILD)/lwip_etharp.o \
       $(BUILD)/lwip_icmp.o \
       $(BUILD)/lwip_ip4.o \
       $(BUILD)/lwip_ip4_addr.o \
       $(BUILD)/lwip_ip4_frag.o \
       $(BUILD)/lwip_ethernet.o

$(BUILD)/lwip_init.o: $(LWIP_SRC)/core/init.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_def.o: $(LWIP_SRC)/core/def.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_inet_chksum.o: $(LWIP_SRC)/core/inet_chksum.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_ip.o: $(LWIP_SRC)/core/ip.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_mem.o: $(LWIP_SRC)/core/mem.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_memp.o: $(LWIP_SRC)/core/memp.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_netif.o: $(LWIP_SRC)/core/netif.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_pbuf.o: $(LWIP_SRC)/core/pbuf.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_raw.o: $(LWIP_SRC)/core/raw.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_stats.o: $(LWIP_SRC)/core/stats.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_sys.o: $(LWIP_SRC)/core/sys.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_tcp.o: $(LWIP_SRC)/core/tcp.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_tcp_in.o: $(LWIP_SRC)/core/tcp_in.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_tcp_out.o: $(LWIP_SRC)/core/tcp_out.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_timeouts.o: $(LWIP_SRC)/core/timeouts.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_udp.o: $(LWIP_SRC)/core/udp.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_etharp.o: $(LWIP_SRC)/core/ipv4/etharp.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_icmp.o: $(LWIP_SRC)/core/ipv4/icmp.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_ip4.o: $(LWIP_SRC)/core/ipv4/ip4.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_ip4_addr.o: $(LWIP_SRC)/core/ipv4/ip4_addr.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_ip4_frag.o: $(LWIP_SRC)/core/ipv4/ip4_frag.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/lwip_ethernet.o: $(LWIP_SRC)/netif/ethernet.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# lwIP port (NO_SYS sys_arch)
$(BUILD)/lwip_sys_arch.o: lib/lwip/port/sys_arch.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# ---- Network: r8152 netif + IP demo -------------------------
# netif_r8152.c bridges lwIP <-> the r8152 driver. net_demo.c enumerates
# the RTL8153B, brings up lwIP and answers ping. Self-neutralize on QEMU by
# the USB driver (xhci_init -> ENODEV): we pass $(MMU_DEFS) for consistency.
$(BUILD)/netif_r8152.o: net/netif_r8152.c | $(BUILD)
	$(CC) $(CFLAGS) $(MMU_DEFS) -c $< -o $@

# ---- Remote shell: shared EtherCAT diag + shell + TCP server -
# ecat_diag.c: shared snapshot (published by the EtherCAT partition, read by
# the shell). net_shell.c: command interpreter (transport-agnostic).
# tcp_shell.c: telnet-like TCP server (lwIP raw API, NO_SYS=1).
$(BUILD)/ecat_diag.o: net/ecat_diag.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/net_shell.o: net/net_shell.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
$(BUILD)/tcp_shell.o: net/tcp_shell.c | $(BUILD)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@
# uart_shell.c: unified UART console on net_shell (serial
# transport). Only depends on net_shell.h + uart.h -> simple CFLAGS.
$(BUILD)/uart_shell.o: net/uart_shell.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@


# ---- SSH (wolfSSL/wolfCrypt + wolfSSH, GPLv2, linked sources) ----
# MINIMAL "wolfCrypt-only" subset + wolfSSH, configured by
# lib/wolfssl/user_settings.h (-DWOLFSSL_USER_SETTINGS). No TLS, no BSD
# sockets (WOLFSSH_USER_IO: I/O via our lwIP raw callbacks). SSH algos:
# curve25519 (KEX) + ed25519 (host key) + AES/ChaCha + SHA2/HMAC.
# Upstream warnings neutralized (third-party code). On QEMU: ssh_server
# starts but no session (xhci ENODEV -> no netif) -> harmless.
WOLF_CFLAGS = $(CFLAGS) -DWOLFSSL_USER_SETTINGS -DWOLFSSH_USER_SETTINGS \
              -Wno-unused -Wno-unused-function -Wno-unused-variable \
              -Wno-unused-but-set-variable -Wno-implicit-function-declaration \
              -Wno-int-conversion -Wno-sign-compare -Wno-type-limits

# wolfCrypt: subset required by SSH (curve/ed25519, aes, chacha, sha2,
# hmac, kdf, rng, asn, coding, math SP, port, memory).
WOLFCRYPT_SRCDIR = lib/wolfssl/wolfcrypt/src
WOLFCRYPT_NAMES = random sha sha256 sha512 md5 hmac aes ecc ed25519 curve25519 \
                  ge_operations fe_operations hash asn coding kdf chacha \
                  poly1305 chacha20_poly1305 wc_encrypt signature memory \
                  wolfmath sp_int sp_c64 logging error wc_port pwdbased dh \
                  dsa
WOLFCRYPT_OBJ = $(patsubst %,$(BUILD)/wc_%.o,$(sort $(WOLFCRYPT_NAMES)))

$(BUILD)/wc_%.o: $(WOLFCRYPT_SRCDIR)/%.c | $(BUILD)
	$(CC) $(WOLF_CFLAGS) -c $< -o $@

# wolfSSH: core of the SSH server (ssh/internal/io/log/misc/port/keygen).
WOLFSSH_SRCDIR = lib/wolfssh/src
WOLFSSH_NAMES = ssh internal io log misc port keygen ossh
WOLFSSH_OBJ = $(patsubst %,$(BUILD)/ws_%.o,$(WOLFSSH_NAMES))

$(BUILD)/ws_%.o: $(WOLFSSH_SRCDIR)/%.c | $(BUILD)
	$(CC) $(WOLF_CFLAGS) -c $< -o $@

# wolfCrypt hardware port (PMU/timer RNG seed, XTIME) + SSH server (lwIP raw).
$(BUILD)/wolf_port.o: net/wolf_port.c | $(BUILD)
	$(CC) $(WOLF_CFLAGS) -c $< -o $@
$(BUILD)/ssh_server.o: net/ssh_server.c | $(BUILD)
	$(CC) $(WOLF_CFLAGS) $(LWIP_CFLAGS) -c $< -o $@

# ---- PERMANENT network: net_task.c ----
# "Infinite thread" version of net_demo (Core2). Includes ssh_server.h (wolfSSH) +
# lwIP + the USB driver -> same flags as ssh_server.o + $(MMU_DEFS) (xhci ENODEV
# on QEMU). Neutralized on QEMU (goes idle).
$(BUILD)/net_task.o: net/net_task.c | $(BUILD)
	$(CC) $(WOLF_CFLAGS) $(LWIP_CFLAGS) $(MMU_DEFS) -c $< -o $@


# ---- Link ----
$(TARGET_ELF): $(OBJ) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJ) $(LIBS) -o $@

$(TARGET_BIN): $(TARGET_ELF)
	$(OBJCOPY) -O binary $< $@

.PHONY: dis
dis: $(TARGET_ELF)
	$(OBJDUMP) -d $< | less

# ---- QEMU (UART PL011 + GIC QEMU) ----
.PHONY: qemu
qemu: clean
	$(MAKE) all LOAD_ADDR=$(QEMU_LOAD_ADDR) \
		UART_DEFS=-DUART_PL011 GIC_DEFS=-DGIC_QEMU MMU_DEFS=-DMMU_QEMU \
		PSCI_DEFS=-DPSCI_HVC
	@echo ""
	@echo "  Launching QEMU (Ctrl-A then X to quit)..."
	qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 -m 1024 \
		-nographic -kernel $(TARGET_ELF)

# ---- U-Boot Image ----
.PHONY: uimage
uimage: $(TARGET_BIN)
	mkimage -A arm64 -O linux -T kernel -C none \
		-a $(LOAD_ADDR) -e $(LOAD_ADDR) \
		-n "OROS" \
		-d $(TARGET_BIN) $(BUILD)/uImage
	@echo "  -> $(BUILD)/uImage (load at $(LOAD_ADDR), launch with 'bootm')"

.PHONY: clean
clean:
	rm -rf $(BUILD)
