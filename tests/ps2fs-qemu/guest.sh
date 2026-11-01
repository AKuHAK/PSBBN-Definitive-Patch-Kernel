#!/bin/sh
# Boot the test kernel for each phase of ps2fstest and validate the disk
# with the reference implementation after every phase:
#  - pfsfsck (ps2sdk's PFS checker) must report the partition clean;
#  - files the driver reported (CKSUM lines) must read back identically
#    through pfsshell;
#  - directories the driver listed (LS lines) must list the same entries in
#    the same order through pfsshell.
# Between phases 3 and 4 pfsshell itself changes what Linux wrote.
# Usage: guest.sh <workdir>.  Exit status is non-zero on any failure.
set -u
cd "$1" || exit 1
FAILED=0

# pfsshell session on test.img: commands on stdin, output without the
# driver banners (prompts go to stderr)
pfs() {
	{ printf 'device ../test.img\nmount PP.TEST\n'; cat; printf 'umount\nexit\n'; } |
		(cd got && ../pfsshell 2>/dev/null) |
		grep -avE '^(hdd|pfs|ps2fs)[: ]|^Device|^hdd0:'
}

rm -rf rootdir root.img
mkdir -p rootdir/sbin rootdir/dev rootdir/mnt
cp ps2fstest rootdir/sbin/init
chmod 755 rootdir/sbin/init
mke2fs -q -F -r 0 -d rootdir root.img 4M >/dev/null 2>&1 || exit 1
# glibc 2.2's static startup needs /dev/null; init needs /dev/console
printf 'cd /dev\nmknod console c 5 1\nmknod null c 1 3\n' |
	debugfs -w -f - root.img >/dev/null 2>&1
cp --sparse=always hdd.img test.img

for PH in 1 2 3 4; do
	LOG=phase$PH.log
	rm -f "$LOG"
	rm -rf got && mkdir got
	if [ $PH = 4 ]; then
		# pfsshell changes the Linux-written tree; phase 4 checks it.  (Not
		# rm of a file with SEGIs: ps2sdk's pfs then leaks the zones of
		# its indirect segments, which fsck reports as a bitmap mismatch.)
		./genpat 70000 99 got/pfs_put.bin
		printf 'cd /lx\nput pfs_put.bin\nmkdir pdir\nrm victim\nrename fragB /lx/fragB_r\n' |
			pfs > pfsshell-edit.log
		if grep -q '(!)' pfsshell-edit.log; then
			echo "== pfsshell edit failed:"; cat pfsshell-edit.log; FAILED=1
		fi
		if ./pfsfsck test.img PP.TEST > fsck-edit.log 2>&1; then
			echo "   pfsfsck after pfsshell edit: $(tail -1 fsck-edit.log)"
		else
			echo "   pfsfsck after pfsshell edit FAILED: $(tail -1 fsck-edit.log)"
			FAILED=1
		fi
		rm -f got/pfs_put.bin
	fi
	timeout 600 qemu-system-i386 -m 32 -display none -monitor none \
		-serial "file:$LOG" -no-reboot \
		-kernel linux-2.4.17/arch/i386/boot/bzImage \
		-drive file=root.img,format=raw,index=0,media=disk \
		-drive file=test.img,format=raw,index=1,media=disk \
		-append "root=/dev/hda rw console=ttyS0 panic=1 phase=$PH"
	echo "== phase $PH"
	tr -d '\r' < "$LOG" | grep -aE '^(FAIL|RESULT)|Oops|BUG|[Pp]anic|ps2fs'
	tr -d '\r' < "$LOG" | grep -aq '^RESULT pass=[0-9]* fail=0$' || FAILED=1

	if ./pfsfsck test.img PP.TEST > fsck$PH.log 2>&1; then
		echo "   pfsfsck: $(tail -1 fsck$PH.log)"
	else
		echo "   pfsfsck FAILED: $(tail -1 fsck$PH.log)"
		grep -v '^hdd:' fsck$PH.log | grep -vE 'done\.|^Check|^Compare|^\t' | head -20
		FAILED=1
	fi

	# Files the driver reported must read back identically through pfsshell
	tr -d '\r' < "$LOG" | grep -a '^CKSUM' > cksum$PH.txt
	if [ -s cksum$PH.txt ]; then
		while read x path crc len; do
			printf 'cd /%s\nget %s\n' "$(dirname "$path")" "$(basename "$path")"
		done < cksum$PH.txt | pfs > readback$PH.log
		n=0
		while read x path crc len; do
			set -- $(cksum < "got/$(basename "$path")" 2>/dev/null)
			if [ "${1:-}" = "$crc" ] && [ "${2:-}" = "$len" ]; then
				n=$((n + 1))
			else
				echo "   MISMATCH $path: driver $crc/$len, pfsshell ${1:-none}/${2:-none}"
				FAILED=1
			fi
		done < cksum$PH.txt
		echo "   pfsshell reads $n files identically"
	fi

	# Directory listings must match pfsshell's, entry for entry
	tr -d '\r' < "$LOG" | grep -a '^LS ' > ls$PH.txt
	for d in $(cut -d' ' -f2 ls$PH.txt | sort -u); do
		grep "^LS $d " ls$PH.txt | cut -d' ' -f3- > ls-driver.txt
		printf 'cd %s\nls\n' "$d" | pfs > ls-pfsshell.txt
		if cmp -s ls-driver.txt ls-pfsshell.txt; then
			echo "   pfsshell lists $d/ identically ($(wc -l < ls-driver.txt) entries)"
		else
			echo "   LISTING MISMATCH in $d/:"
			diff ls-driver.txt ls-pfsshell.txt | head -10
			FAILED=1
		fi
	done
done
exit $FAILED
