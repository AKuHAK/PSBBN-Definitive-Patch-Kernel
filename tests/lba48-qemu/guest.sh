#!/bin/sh
# Boot the test kernel in QEMU with a 2.5 TiB sparse disk as hdb: an MBR
# with an empty partition 1 and exFAT as partition 2 at 200 GiB.  Then check
# the sector addresses in the image and the exFAT partition.  Runs in the
# tools container.  Usage: guest.sh <workdir>
set -u
WORK=$1
cd "$WORK" || exit 1
FAILED=0
GIB=2097152			# sectors
EXFAT_START=$((200 * GIB))
EXFAT_SIZE=$GIB

rm -rf rootdir root.img
mkdir -p rootdir/sbin rootdir/dev rootdir/mnt
cp lba48test rootdir/sbin/init
chmod 755 rootdir/sbin/init
mke2fs -q -F -r 0 -d rootdir root.img 4M >/dev/null 2>&1 || exit 1
printf 'cd /dev\nmknod console c 5 1\nmknod null c 1 3\n' |
	debugfs -w -f - root.img >/dev/null 2>&1

rm -f disk.img exfat.img
truncate -s 2560G disk.img
python3 /repo/tests/lba48-qemu/mbr.py disk.img - "7:$EXFAT_START:$EXFAT_SIZE" || exit 1
truncate -s 1G exfat.img
mkfs.exfat -L DATA exfat.img >/dev/null || exit 1
dd if=exfat.img of=disk.img bs=1M seek=$((EXFAT_START / 2048)) conv=notrunc,sparse status=none

LOG=lba48.log
rm -f "$LOG"
timeout 1800 qemu-system-i386 -m 32 -display none -monitor none \
	-serial "file:$LOG" -no-reboot \
	-kernel linux-2.4.17/arch/i386/boot/bzImage \
	-drive file=root.img,format=raw,index=0,media=disk \
	-drive file=disk.img,format=raw,index=1,media=disk \
	-append "root=/dev/hda rw console=ttyS0 expect=4294967295"
echo "== guest (qemu exit $?)"
tr -d '\r' < "$LOG" > lba48.txt
grep -aE '^(FAIL|RESULT)|hdb|Oops|BUG|panic|out of bounds' lba48.txt
grep -aq '^RESULT pass=[0-9]* fail=0$' lba48.txt || FAILED=1
python3 /repo/tests/lba48-qemu/check.py disk.img lba48.txt || FAILED=1
dd if=disk.img of=exfat.img bs=1M skip=$((EXFAT_START / 2048)) count=1024 status=none
if fsck.exfat -n exfat.img >fsck.log 2>&1; then
	echo "PASS fsck.exfat: $(tail -1 fsck.log)"
else
	echo "FAIL fsck.exfat:"; cat fsck.log
	FAILED=1
fi
exit $FAILED
