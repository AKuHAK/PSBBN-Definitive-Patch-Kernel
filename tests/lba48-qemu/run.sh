#!/bin/sh
# Run the 48-bit LBA test: build a vanilla Linux 2.4.17 i386 kernel with
# this repository's IDE disk driver and fs/exfat, boot it in QEMU with a
# 2.5 TiB disk and check reads and writes at every boundary.  See README.md.
#
# Environment: CONTAINER=docker|podman, CONTAINER_OPTS, LBA48_TEST_WORK
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${LBA48_TEST_WORK:-$HERE/work}
CTR=${CONTAINER:-$(command -v docker || command -v podman || echo docker)}
CTR_OPTS=${CONTAINER_OPTS:-}
TOOLCHAIN=docker.io/akuhak/ps2-legacy-woody-slim:latest
TOOLS=localhost/lba48-qemu-tools:latest
KERNEL=linux-2.4.17.tar.bz2
KERNEL_SHA256=db191ad39b92e3edb08b007c1b43f78498143a0609f1befad14c268847f891cf

mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

run_tools() {
	$CTR $CTR_OPTS run --rm -v "$REPO:/repo:ro" -v "$WORK:/work" -w /work "$TOOLS" "$@"
}
run_woody() {
	$CTR $CTR_OPTS run --rm --platform linux/amd64 \
		-v "$REPO:/repo:ro" -v "$WORK:/work" -w /work "$TOOLCHAIN" "$@"
}

echo "== tools image"
$CTR $CTR_OPTS build -q -t "$TOOLS" -f "$HERE/Containerfile" "$HERE" >/dev/null

echo "== kernel source"
run_tools sh -c "{ [ -f $KERNEL ] || curl -fsSLO https://cdn.kernel.org/pub/linux/kernel/v2.4/$KERNEL; } &&
	echo '$KERNEL_SHA256  $KERNEL' | sha256sum -c -"
run_tools sh /repo/tests/lba48-qemu/prepare-kernel.sh "/work/$KERNEL" /repo /work

echo "== build (gcc 2.95, i386)"
# ARCH must be given to make: the Makefile sets it from uname -m
run_woody sh -c 'cd linux-2.4.17 || exit 1
	M="make ARCH=i386"
	fail() { echo "$1 failed:"; tail -40 "../$2"; exit 1; }
	yes "" | $M oldconfig >../oldconfig.log 2>&1
	grep -q "^CONFIG_BLK_DEV_PIIX=y" .config || fail "PIIX configuration" oldconfig.log
	$M dep >../dep.log 2>&1 || fail "make dep" dep.log
	$M -j$(grep -c ^processor /proc/cpuinfo) bzImage >../build.log 2>&1 ||
		fail "make bzImage" build.log
	grep -E "^ide-(disk|dma)\.c:" ../build.log || true
	cd .. && gcc -static -O2 -Wall -o lba48test /repo/tests/lba48-qemu/lba48test.c'

STATUS=0
run_tools sh /repo/tests/lba48-qemu/guest.sh /work || STATUS=1
if [ $STATUS = 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi
exit $STATUS
