#!/bin/sh
# The exFAT test kernel (see ../exfat-qemu) plus this repository's fs/ps2fs.
# Usage: prepare-kernel.sh <linux-2.4.17.tar.bz2> <repo> <workdir>
set -eu
sh "$2/tests/exfat-qemu/prepare-kernel.sh" "$1" "$2" "$3" >/dev/null
L=$3/linux-2.4.17
sed -i "/^tristate 'exFAT fs support'/a tristate 'PlayStation 2 HDD filesystem (PFS) support' CONFIG_PS2_FS" \
	"$L/fs/Config.in"
sed -i '/^subdir-\$(CONFIG_EXFAT_FS)/a subdir-$(CONFIG_PS2_FS)\t\t+= ps2fs' "$L/fs/Makefile"
cp -R "$2/fs/ps2fs" "$L/fs/ps2fs"
echo "CONFIG_PS2_FS=y" >> "$L/.config"
grep -q CONFIG_PS2_FS "$L/fs/Config.in"
grep -q ps2fs "$L/fs/Makefile"
echo "prepared $L"
