# IOP modules for Linux in PSBBN's boot loader

PSBBN boots Linux with Sony's `osdboot.elf` (`hdd0:__system:pfs:/osdboot.elf`,
copied as `p2lboot/BB.ELF`). The IOP environment Linux finds is fixed by that
loader: under it the IOP cannot load modules from EE memory (LOADFILE has no
function 6), and its module loader cannot open `mc0:`/`mc1:` paths. Modules
that Linux needs on the IOP, such as iomanX, fileXio and mmceman for
`fs/mmcefs`, therefore have to be loaded by the boot loader.
`osdboot_mmce.py` adds them.

## How osdboot sets up the IOP

- `osdboot.elf` has one program segment (VA `0x1000000`, size `0x10a6ff`)
  and `.bss` up to `0x12c4a78`, where `_start` starts the heap (`SetupHeap`,
  and `sbrk()`'s break at `0x11027dc`).
- The image table at `0x10465a0` has 24-byte entries `{id, name, irx start,
  irx end, sbios start, sbios end}`. Entry 0, `"rt"`, is the IOP image the
  loader itself runs on (HDD access, reading the kernel). Entry 1, `"234"`,
  is the image for Linux. The `--module` option of the loader's command line
  selects it by name; `234` is the default.
- Before starting the kernel, the loader reboots the IOP, loads
  `rom0:SYSCLIB`, and loads the selected IRX from memory. That IRX holds a
  packed IOPRP image, unpacks it, merges it with the ROM (`!include
  IOPBTCONF`) and reboots the IOP with it.
- The packed format is decoded from the IRX's unpacker (function `0xb20`):
  - a header `{u32 total, u32 outsize, u32 keylen, u8, 3 bytes}`, then the
    key, then an LZSS stream;
  - every stream byte is XORed with the key, cycling through it;
  - every 30 items a big-endian 32-bit control word is read. Its low 2 bits
    pick the split of a 16-bit match (offset mask `0x3fff >> s`, length
    `(v >> (14 - s)) + 3`), and its top 30 bits mark matches (1) or
    literals (0).
- IRX `234` refers to its data end in two `lui`/`addiu` pairs (at `0xe50` and
  `0xfdc`): it allocates IOP memory from there.

Image `234` loads, after the ROM modules: BBSYS102, BBSYS103, SIO2MAN, MCMAN,
MCSERV, PADMAN, LIBSD, MODMIDI, MODHSYN, OSDSDRDRV, IOPINTR, DMARELAY, RMMAN,
RMMAN2, EESYNC2.

## What the script changes

1. It adds `IOMANX`, `FILEXIO`, `MMCEMAN`, and optionally `POWEROFF` to image `234`
   and loads them before `EESYNC2`. Other files of the image are unchanged.
2. It packs the image again. Its compressor is simpler than Sony's, so the
   image grows, and it rebuilds the IRX: data section, `.iopmod` sizes and
   `gp`, the two data-end constants, and section offsets.
3. It places the new IRX in a second program segment at `0x12c5000`, right
   after `.bss`. It points table entry `234` there and moves the heap start
   (`SetupHeap` in `_start` and `sbrk()`'s initial break) behind it.
4. With `--no-crc-check`, it also lets the loader boot kernels without the
   trailing cksum. That is the branch at `0x1009f88`, as in the widely used
   CRC patch. Kernels built by this repository's CI are signed anyway.

It accepts only the known `osdboot.elf` (md5
`1649fba52ce8eb81dd9e5172921c14ed`). Run it on the file from `__system`, not
on an already patched copy. A copy patched at `0xf59c` is broken: that
location is inside the loader's unpacking code, and with that patch the
loader cannot set up the IOP.

```sh
python3 osdboot_mmce.py osdboot.elf osdboot_mmce.elf \
    ../../fs/mmcefs/irx/iomanX.irx ../../fs/mmcefs/irx/fileXio.irx \
    ../../fs/mmcefs/irx/mmceman.irx \
    --poweroff ../../fs/mmcefs/irx/poweroff.irx \
    --no-crc-check
python3 verify.py osdboot.elf osdboot_mmce.elf   # needs: pip install unicorn
```

`verify.py` runs the IRX's own unpacker in an emulator. It checks that the
unpacker reads the new data back to the new image, and that only `ROMDIR` and
`IOPBTCONF` of the original files changed.

With the modules resident, mount mmcefs with `-o noload` (it also finds
fileXio by itself).

## IOP Memory Budget and Limits for Adding Modules

Retail PS2 consoles (Fat and Slim) have exactly **2 MB (2,048 KB)** of IOP RAM. When adding extra IRX modules (such as `iLinkman` + `sbp2man` for i.Link/FireWire storage, or custom filesystem daemons), memory is constrained in two distinct phases:

### 1. Decompression phase (peak memory usage)
Before rebooting the IOP into Linux mode, the loader executes the unpacker function in IRX `234`. The unpacker allocates a contiguous block from IOP sysmem (`AllocSysMemory(mode=2, size=unpacked_size + 1056)`):
- Base minimal IOP kernel (LOADCORE, SYSMEM, SYSCLIB): ~150 KB
- IRX `234` itself (code + compressed IOPRP payload): ~350 KB
- Temporary buffer for unpacked IOPRP: ~747 KB
- **Peak RAM used during unpack:** ~1,247 KB out of 2,048 KB (~61%).
- **Headroom during decompression:** **~800 KB**.

### 2. Resident phase (after IOP reboot)
Once unpacked, the IOP reboots with the unpacked IOPRP, clearing the unpacker and temporary buffers.
- Total resident memory (`.text` + `.data` + `.bss`) across all 34 modules currently in image `234` is **937 KB** (~0.92 MB).
- **Free IOP RAM remaining:** **~1,000 KB (~1 MB)** for heap, thread stacks, and DMA buffers.

### 3. Guidelines for adding extra drivers
- **Safe margin:** Up to **~400–450 KB** of additional unpacked IRX modules can be added safely to image `234` without risking `AllocSysMemory` failure during decompression.
- **EE memory:** The new ELF segment in `osdboot.elf` is placed at `0x12c5000` with heap following at `0x131ab00`. With 32 MB of EE RAM, over 12 MB of headroom remains.
- **Address patching:** `osdboot_mmce.py` dynamically computes and patches the `IRX_END` and `IRX_ENDM4` `lui`/`addiu` instruction pairs in the unpacker, so compressed payloads of arbitrary sizes are handled automatically.
- **Hardware conflicts:** Avoid loading network drivers (`smap.irx` / `ps2ip.irx`) on the IOP if the Linux kernel on the EE operates its own `smap.c` driver, as concurrent register access and interrupt handling will conflict. i.Link storage (`iLinkman.irx` + `sbp2man.irx`, ~55 KB total) has no EE driver conflict and fits easily.
