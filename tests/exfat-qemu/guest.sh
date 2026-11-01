#!/bin/sh
# Boot the test kernel in QEMU for each test phase and check the exFAT image
# with fsck.exfat after every phase.  Runs in the tools container.
#
# Usage: guest.sh <workdir> <cluster size, e.g. 4K> [ram MB]
# Exit status is non-zero if any check failed.
set -u

WORK=$1
CL=$2
MEM=${3:-24}
cd "$WORK" || exit 1
FAILED=0

# Root filesystem: just the test program as /sbin/init and two device nodes
rm -rf rootdir root.img
mkdir -p rootdir/sbin rootdir/dev rootdir/mnt
cp exfattest rootdir/sbin/init
chmod 755 rootdir/sbin/init
mke2fs -q -F -r 0 -d rootdir root.img 4M >/dev/null 2>&1 || exit 1
printf 'cd /dev\nmknod console c 5 1\nmknod null c 1 3\n' |
	debugfs -w -f - root.img >/dev/null 2>&1

rm -f exfat.img
truncate -s 64M exfat.img
mkfs.exfat -c "$CL" -L TEST exfat.img >/dev/null || exit 1

for PH in 1 2 3; do
	LOG=phase$PH-$CL.log
	rm -f "$LOG"
	timeout 1500 qemu-system-i386 -m "$MEM" -display none -monitor none \
		-serial "file:$LOG" -no-reboot \
		-kernel linux-2.4.17/arch/i386/boot/bzImage \
		-drive file=root.img,format=raw,index=0,media=disk \
		-drive file=exfat.img,format=raw,index=1,media=disk \
		-append "root=/dev/hda rw console=ttyS0 phase=$PH"
	echo "== cluster $CL, phase $PH (qemu exit $?)"
	grep -aE '^(FAIL|RESULT)|Oops|BUG|panic' "$LOG"
	# the serial log has CRLF line ends
	tr -d '\r' < "$LOG" | grep -aq '^RESULT pass=[0-9]* fail=0$' || FAILED=1
	if fsck.exfat -n exfat.img >fsck.log 2>&1; then
		echo "   fsck.exfat: $(tail -1 fsck.log)"
	else
		echo "   fsck.exfat FAILED:"; cat fsck.log
		FAILED=1
	fi
done
exit $FAILED
