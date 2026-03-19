# Developer & AI Agent Guide for PSBBN Linux Kernel

This document provides essential context, architectural reference, and guidelines for AI coding agents and human developers maintaining and improving this kernel.

---

## 1. Project Background & Heritage

- **Base Kernel**: Linux 2.4.17, originally ported to PlayStation 2 by MontaVista Software (`linux-2.4.17_mvl21`) and Sony Computer Entertainment Inc. (SCEI).
- **Target Platform**: Sony PlayStation 2 (Emotion Engine R5900, 32-bit MIPS III / IV hybrid architecture with 128-bit SIMD multimedia instructions).
- **Primary Use Case**: PlayStation Broadband Navigator (PSBBN), operating with Sony Network Adapter + Internal HDD, or third-party GameStar SATA adapter clones.
- **Git History Heritage**: Reconstructed from original MontaVista 2.4.17 sources and official SCEI RPMs (`kernel-source-2.4.17_ps2-26.mipsel.rpm`), with CosmicScale's modern additions and AKuHAK's improvements isolated as clean commits.

---

## 2. Toolchain & Environment Specifications

- **Toolchain Container**: `akuhak/ps2-legacy-woody-slim:latest` (Debian 3.0 Woody environment).
- **Target Triple**: `mipsEEel-linux-` (MIPS Emotion Engine Little-Endian Linux).
- **Compiler**: `gcc version 2.95.2 19991024 (release)`.
- **Target CPU Flags**: `-mcpu=r5900 -mips2 -pipe -G 0 -mno-abicalls -fno-pic`.
- **CRITICAL C Syntax Constraint**: GCC 2.95 strictly requires **C89 / GNU89** syntax.
  - Variables **MUST** be declared at the start of a block `{ ... }` before any executable statements.
  - No C99 declarations in `for` loops (e.g. `for (int i = 0; ...)` is invalid).
  - No C++ style `//` comments in files passed through traditional assembly or strict C compilers unless supported.

---

## 3. Kernel Build Workflow

Inside the `akuhak/ps2-legacy-woody-slim:latest` container:

```bash
# 1. Initialize configuration and cross-compiler flags
./setup-ps2

# 2. Confirm configuration against Kconfig
yes "" | make oldconfig || true

# 3. Build dependencies
make dep

# 4. Build kernel binary
# Note: Woody container lacks `nproc`; use /proc/cpuinfo
JOBS=$(grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 2)
make -j"$JOBS" vmlinux

# 5. Build and install modules
make -j"$JOBS" modules || true
make modules_install INSTALL_MOD_PATH=rootfs

# 6. Copy boot artifacts
mkdir -p rootfs/boot
cp vmlinux rootfs/boot/vmlinux-2.4.17_mvl21
cp System.map rootfs/boot/System.map-2.4.17_mvl21
```

---

## 4. Key PS2 Subsystems & Architecture Reference

### A. Core Architecture (`arch/mips/ps2/`)
- `prom.c`: Reads PS2 BIOS arguments and boot parameters passed by the loader/kloader.
- `gs.c`: Controls the PS2 Graphics Synthesizer (video modes, CRTC, framebuffers).
- `dma.c`: EE DMA controller initialization and channel management (VIF, GIF, SIF, IPU).
- `sbcall.c`: SIF RPC protocol for communicating with the I/O Processor (IOP).
- `rtc.c` & `time.c`: PS2 Real-Time Clock and timer synchronization.

### B. Device Drivers (`drivers/ps2/`)
- `mcfs/`: PlayStation 2 Memory Card File System.
  - `MCFS_MAX_DIRENT` is increased to 65536 to support modern high-capacity memory cards.
  - The legacy check enforcing `/BWLINUX` as the root directory has been disabled to allow PSBBN full access to all memory card saves.
- `smap.c`: Sony SpeedMachine 10/100 Mbit Ethernet driver.
  - Refactored by Andrew Church for high-throughput DMA transfers.
- `pad.c`: DualShock 2 controller driver.
  - Automatically swaps Cross (X) and Circle (O) buttons on Western consoles while preserving native behavior on Japanese consoles (`region == 'J'`).
- `rm.c`: PS2 DVD Remote Control driver.

### C. Hard Disk & ATA Subsystem (`drivers/ide/`)
- `ps2ide.c`: PS2 IDE host controller registration and DEV9 interface setup.
  - Contains DEV9 reset logic required to initialize GameStar SATA network/HDD adapter clones.
- `ps2dma.c`: PS2 IDE DMA engine.
  - Implements GameStar clone detection and DEV9 transfer register configuration (`DEV9M_BASE + 0x32`, `0x64`).
  - Driver-private data is stored in `hwif->hwif_data` (`struct ps2ide_dma_priv`).
  - Configures UDMA modes for disk transfers.

### D. Partition Table Format (`fs/partitions/ps2.c`)
- Reads the PlayStation 2 APA (Authentic Partition Architecture) partition table format used on PS2 HDDs.

---

## 5. Guidelines for Future AI Agents

1. **PSBBN Compatibility**:
   - The primary goal of this repository is rock-solid stability with the latest PlayStation Broadband Navigator release.
   - Never alter APA partition structures or MCFS ioctl interfaces in ways that break compatibility with Sony's userland libraries.

2. **Case-Insensitive Filesystems**:
   - macOS (APFS) and Windows (NTFS) are case-insensitive by default.
   - Netfilter files were renamed (`ipt_MARK.c` -> `ipt_MARK_target.c`) to prevent collision with match files (`ipt_mark.c`). Do not introduce files differing only in letter case.

3. **Generated / Ignore Files**:
   - `.hhl_cross_compile` and `.hhl_target_cpu` are created dynamically by `./setup-ps2` from `.hhl_*-mips-r5900` templates. They must remain in `.gitignore`.
   - Never commit `.depend` files, object files, or build binaries.
