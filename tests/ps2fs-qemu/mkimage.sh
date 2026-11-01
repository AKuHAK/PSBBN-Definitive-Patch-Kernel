#!/bin/sh
# Build the reference disk image with pfsshell (libapa/libpfs from ps2sdk):
# a 4 GB APA disk with the standard PSBBN partitions plus PP.TEST holding
# pattern files of many sizes, a 300-entry directory, nesting and a long name.
# Usage: mkimage.sh <workdir with pfsshell, genpat>
set -eu
cd "$1"
rm -rf files && mkdir files && cd files
SIZES="0 1 511 512 513 4095 4096 4097 8191 8192 8193 65537 1048579 5242880"
CMDS="device ../hdd.img
initialize yes
mkpart PP.TEST 256M PFS
mount PP.TEST
mkdir sizes
cd sizes"
i=0
for s in $SIZES; do
	../genpat $s $i f_$s
	CMDS="$CMDS
put f_$s"
	i=$((i + 1))
done
CMDS="$CMDS
cd ..
mkdir many
cd many"
for n in $(seq 1 300); do
	f=entry_$(printf %04d $n)_with_a_rather_long_name_to_fill_directory_sectors
	../genpat $n $n $f
	CMDS="$CMDS
put $f"
done
../genpat 12345 77 deep.bin
L=$(printf 'L%.0s' $(seq 1 200))
../genpat 33 5 $L
CMDS="$CMDS
cd ..
mkdir a
cd a
mkdir b
cd b
mkdir c
cd c
put deep.bin
cd /
put $L
umount
exit"
rm -f ../hdd.img
truncate -s 4G ../hdd.img
echo "$CMDS" | ../pfsshell > ../mkimage.log 2>&1
if grep -E '\(!\)' ../mkimage.log | grep -v 'Unable to open hdd0'; then
	echo "pfsshell reported errors (see mkimage.log)"; exit 1
fi
cd .. && ./pfsfsck hdd.img PP.TEST > mkimage-fsck.log 2>&1 || { tail -5 mkimage-fsck.log; exit 1; }
echo "image: $(tail -1 mkimage-fsck.log)"
