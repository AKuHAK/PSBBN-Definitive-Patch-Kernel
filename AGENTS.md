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
- `sysconf.c`: OSD settings from the MECHACON NVM (`/dev/ps2sysconf`, `/proc/ps2sysconf`).
  - The boot loader passes Japanese in `ps2_sysconf` on every console, so at init the driver takes `language` from the OSD block. Bits 5-7 of block 1 byte 0 are zero only on the first Japanese consoles (Japanese/English bit 4 only); later ones hold the language in byte 1 bits 0-4, including 8-11 (Russian, Korean, Traditional and Simplified Chinese) on models sold with them. Layout reference: `OSDConfigStore_t` in ps2homebrew's OSD-Initialization-Libraries.
- Memory card (`mc.c`, `mcdir.c`, `mcfile.c`, `mcfs/`): the kernel talks to the card through the PS2 Linux SBIOS calls `SBR_MC_OPEN`..`SBR_MC_GETENTSPACE` (145-159).  Under PSBBN these fail with `-SIF_RPCE_GETP` as soon as the IOP has data to return (any non-empty directory, opening a file); only GetInfo and lookups of missing names work.  PSBBN's own `libmc` (statically linked into `/opt0/bn/bn`) uses `SBR_MC_CALL` (ioctl `PS2MC_CALL`): a pass-through RPC to the IOP memory card server with `{u32 1, fno, send, ssize, recv, rsize}` and KSEG0 addresses; GetDir is function 13 with a 1044-byte request `{port, slot, mode, maxent, table, name[1024]}` and a 4-byte result.  A read-only probe through `SBR_MC_CALL` lists the card correctly.  Passing physical addresses makes the SBIOS oops with the card lock held.  Never write to a card without the owner's consent: it may be the boot card.

- IOP environment under PSBBN: Sony's boot loader (`__system:/osdboot.elf` = `p2lboot/BB.ELF`) reboots the IOP with its packed image "234" before starting the kernel. Linux cannot load IOP modules later (no LOADFILE function 6; `mc0:` paths fail with -201), so modules Linux needs must be added to that image: `tools/osdboot-mmce/` (format, layout, builder, emulator-based check).

### C. Hard Disk & ATA Subsystem (`drivers/ide/`)
- `ps2ide.c`: PS2 IDE host controller registration and DEV9 interface setup.
  - Contains DEV9 reset logic required to initialize GameStar SATA network/HDD adapter clones.
- `ps2dma.c`: PS2 IDE DMA engine.
  - Implements GameStar clone detection and DEV9 transfer register configuration (`DEV9M_BASE + 0x32`, `0x64`).
  - Driver-private data is stored in `hwif->hwif_data` (`struct ps2ide_dma_priv`).
  - Configures UDMA modes for disk transfers.

### C2. CD/DVD (`drivers/ps2/cdvd.c`)
- DVD-Video sectors need SBIOS call 196, which only the kernelloader TGE SBIOS has. It is used only with `ps2cdvd_dvdvideo=1` (boot or module parameter); PSBBN's Sony SBIOS lacks it.

### D. Partition Table Format (`fs/partitions/ps2.c`)
- Reads the PlayStation 2 APA (Authentic Partition Architecture) partition table format used on PS2 HDDs.

### E. Additional Filesystems
- `fs/ps2fs/`: APA/PFS driver, read-write since the journal/segment rewrite. Mounts are **read-only unless `-o unsafe_write`** is given. The format and Sony's modification rules are in `Documentation/filesystems/ps2fs-format.md`; keep the driver to them (journaled metadata, exact bitmap, dentry type bits, CLOSED attr). ps2sdk is AFL-2.0: use it as a test oracle only, never copy its code.
- `fs/exfat/`: exFAT driver built on FatFs R0.16 by ChaN plus Linux 2.4 VFS glue. Local FatFs changes are listed in `fs/exfat/README`.
- `fs/mmcefs/`: Files on the SD card of an MMCE (SD2PSX, MemCard PRO2) through the IOP: a fileXio RPC client for `mmce0:`/`mmce1:`.
  - On first mount it loads `iomanX.irx`, `fileXio.irx` and `mmceman.irx` (shipped in `fs/mmcefs/irx/`) with `arch/mips/ps2/loadfile.c` (IOP LOADFILE RPC).
  - Buffers the IOP reaches by DMA must come from the page allocator, since module data lives in vmalloc space.
  - Not yet tested on real hardware; status, test plan and pitfalls: `fs/mmcefs/README`.

---

## 5. Guidelines for Future AI Agents

0. **Testing**:
   - `tests/exfat-qemu/run.sh` builds a vanilla 2.4.17 i386 kernel with `fs/exfat` and runs a test suite in QEMU, with `fsck.exfat` after each phase. Run it after any change to `fs/exfat`; CI runs it too.
   - `tests/ps2-hw/` has the tools for testing on a real PS2 (PSBBN), including a PFS checker that handles sub-partitions.
   - `tests/ps2fs-qemu/run.sh` does the same for `fs/ps2fs`, with pfsshell and ps2sdk's PFS fsck as the oracle. Run it after any change to `fs/ps2fs`.
   - Code that talks to the IOP cannot be emulated; say so when it is untested on hardware.
   - SIF RPC calls must retry on both `-SIF_RPCE_SENDP` (Sony SBIOS, used by PSBBN) and `-E_SIF_PKT_ALLOC` (kernelloader TGE).

1. **PSBBN Compatibility**:
   - The primary goal of this repository is rock-solid stability with the latest PlayStation Broadband Navigator release.
   - Never alter APA partition structures or MCFS ioctl interfaces in ways that break compatibility with Sony's userland libraries.

2. **Case-Insensitive Filesystems**:
   - macOS (APFS) and Windows (NTFS) are case-insensitive by default.
   - Netfilter files were renamed (`ipt_MARK.c` -> `ipt_MARK_target.c`) to prevent collision with match files (`ipt_mark.c`). Do not introduce files differing only in letter case.

3. **Generated / Ignore Files**:
   - `.hhl_cross_compile` and `.hhl_target_cpu` are created dynamically by `./setup-ps2` from `.hhl_*-mips-r5900` templates. They must remain in `.gitignore`.
   - Never commit `.depend` files, object files, or build binaries.
