#!/bin/sh
# The exFAT test kernel (see ../exfat-qemu) with this repository's IDE disk
# driver: drivers/ide/ide-disk.c and ide-dma.c, include/linux/hdreg.h, and
# the new fields of include/linux/ide.h.  Runs in the tools container.
# Usage: prepare-kernel.sh <linux-2.4.17.tar.bz2> <repo> <workdir>
set -eu
sh "$2/tests/exfat-qemu/prepare-kernel.sh" "$1" "$2" "$3" >/dev/null
L=$3/linux-2.4.17
R=$2

cp "$R/drivers/ide/ide-disk.c" "$R/drivers/ide/ide-dma.c" "$L/drivers/ide/"
cp "$R/include/linux/hdreg.h" "$L/include/linux/"

# ide.h fields that the copied files use, taken from this repository
grep -E '^	unsigned (addressing|rq_lba48)	' "$R/include/linux/ide.h" > "$3/f-drive"
grep -E '^	unsigned	lba48_dma  ' "$R/include/linux/ide.h" > "$3/f-hwif"
grep -E '^	byte		part_usage\[' "$R/include/linux/ide.h" > "$3/f-usage"
sed -i -e '/^	unsigned ata_flash	: 1;/r '"$3/f-drive" \
       -e '/^	unsigned	udma_four  : 1;/r '"$3/f-hwif" \
       -e '/^	unsigned int	failures;/r '"$3/f-usage" "$L/include/linux/ide.h"

# PIIX hands reads and writes to ide_dmaproc, which issues 48-bit commands
sed -i 's/^	hwif->dmaproc = &piix_dmaproc;$/&\n	hwif->lba48_dma = 1;/' "$L/drivers/ide/piix.c"

[ "$(grep -c -E 'unsigned[ 	]+(addressing|rq_lba48|lba48_dma)[ 	]|part_usage\[' "$L/include/linux/ide.h")" = 4 ]
grep -q 'lba48_dma = 1' "$L/drivers/ide/piix.c"
grep -q 'WIN_READ_EXT' "$L/drivers/ide/ide-disk.c"
echo "prepared $L"
