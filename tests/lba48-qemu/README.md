# 48-bit LBA test (QEMU)

The 2.4.17 IDE driver addressed only 2^28 sectors (128 GiB).
`drivers/ide/ide-disk.c` now reads the 48-bit capacity (IDENTIFY words
100-103) and uses READ/WRITE (MULTIPLE/DMA) EXT for requests that end above
the 28-bit limit; everything below keeps the 28-bit commands.  The 2.4 block
layer counts sectors in 32 bits, so a larger disk is used up to 2^32 - 1
sectors (2 TiB).  DMA is used for 48-bit requests only on interfaces that
set `hwif->lba48_dma`: the PS2 one (`ps2dma.c`; DMARELAY on the IOP writes
`req->command` to the ATA command register as given) and, in this test,
PIIX, whose dmaproc hands reads and writes to `ide_dmaproc`.

The test builds a vanilla 2.4.17 i386 kernel with this repository's
`ide-disk.c`, `ide-dma.c`, `hdreg.h`, the new `ide.h` fields and `fs/exfat`,
and boots it in QEMU with a 2.5 TiB sparse disk as `hdb`.  The guest
(`lba48test.c`, run as init) checks that the capacity is 2^32 - 1 sectors,
then in three passes (PIO, DMA, PIO with multiple mode 16) writes and reads
back sectors below, across and above 2^28, across 2^31 and at the end of the
disk (up to its last whole KiB: the 2.4 block layer sizes devices in KiB, so
the odd last sector of 2^32 - 1 is not reachable), each stamped with its LBA.  `check.py` verifies on the host that every
stamped sector is at its own address in the image.  Partition 2 of the disk,
at 200 GiB, holds exFAT: the guest writes and reads an 8 MiB file there and
the host runs `fsck.exfat` on it afterwards.

    tests/lba48-qemu/run.sh       # docker or podman

What runs on the PS2 only (`ps2dma.c`, DMARELAY, the DEV9 ATA interface)
cannot be emulated; test it on the console.
