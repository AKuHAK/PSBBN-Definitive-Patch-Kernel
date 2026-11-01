#!/bin/sh
# Unpack vanilla Linux 2.4.17 and graft this repository's fs/exfat into it,
# configured for an i386 QEMU guest.  Runs in the tools container (GNU sed).
#
# Usage: prepare-kernel.sh <linux-2.4.17.tar.bz2> <repo> <workdir>
set -eu

TARBALL=$1
REPO=$2
WORK=$3
L=$WORK/linux-2.4.17

rm -rf "$L" "$WORK/linux"
tar xjf "$TARBALL" -C "$WORK"
mv "$WORK/linux" "$L"

# QEMU stores its default ramdisk_max at setup offset 0x22c, where a
# boot protocol 2.02 kernel like 2.4.17 still has code; move it away.
sed -i 's/^trampoline:\tcall\tstart_of_setup/qemu_pad:\t.long\t0, 0\t\t# QEMU writes ramdisk_max at 0x22c\n&/' \
	"$L/arch/i386/boot/setup.S"

# The same wiring as in this repository's fs/Config.in, fs/Makefile and
# fs/nls/Config.in
sed -i "/CONFIG_VFAT_FS \$CONFIG_FAT_FS/a tristate 'exFAT fs support' CONFIG_EXFAT_FS" \
	"$L/fs/Config.in"
sed -i '/^subdir-\$(CONFIG_VFAT_FS)/a subdir-$(CONFIG_EXFAT_FS)\t+= exfat' \
	"$L/fs/Makefile"
sed -i 's/-o "\$CONFIG_SMB_NLS" = "y" \]; then/-o "$CONFIG_SMB_NLS" = "y" -o "$CONFIG_EXFAT_FS" != "n" ]; then/' \
	"$L/fs/nls/Config.in"
cp -R "$REPO/fs/exfat" "$L/fs/exfat"

# UP kernel with a serial console, exFAT built in, NLS for iocharset tests
sed -e 's/^CONFIG_SMP=y/# CONFIG_SMP is not set/' \
    -e 's/^# CONFIG_SERIAL_CONSOLE is not set/CONFIG_SERIAL_CONSOLE=y/' \
    -e 's/^# CONFIG_NLS is not set/CONFIG_NLS=y/' \
    "$L/arch/i386/defconfig" > "$L/.config"
cat >> "$L/.config" <<'EOF'
CONFIG_NLS_DEFAULT="iso8859-1"
CONFIG_NLS_ISO8859_1=y
CONFIG_EXFAT_FS=y
EOF

grep -q 'exfat' "$L/fs/Makefile"
grep -q 'CONFIG_EXFAT_FS' "$L/fs/Config.in"
grep -q 'CONFIG_EXFAT_FS' "$L/fs/nls/Config.in"
grep -q 'qemu_pad' "$L/arch/i386/boot/setup.S"
echo "prepared $L"
