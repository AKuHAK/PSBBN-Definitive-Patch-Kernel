#!/bin/sh
# Run the exFAT driver test suite: build a vanilla Linux 2.4.17 i386 kernel
# with this repository's fs/exfat, boot it in QEMU and exercise the driver,
# checking the image with fsck.exfat after every phase.  See README.md.
#
# Usage: tests/exfat-qemu/run.sh [cluster sizes]     (default: 4K 128K)
# Environment: CONTAINER=docker|podman, CONTAINER_OPTS (e.g. "-c machine"),
#              EXFAT_TEST_WORK=<work directory>
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${EXFAT_TEST_WORK:-$HERE/work}
CTR=${CONTAINER:-$(command -v docker || command -v podman || echo docker)}
CTR_OPTS=${CONTAINER_OPTS:-}
TOOLCHAIN=docker.io/akuhak/ps2-legacy-woody-slim:latest
TOOLS=localhost/exfat-qemu-tools:latest
KERNEL=linux-2.4.17.tar.bz2
KERNEL_SHA256=db191ad39b92e3edb08b007c1b43f78498143a0609f1befad14c268847f891cf
CLUSTERS=${*:-4K 128K}

mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

run_tools() {
	$CTR $CTR_OPTS run --rm -v "$REPO:/repo:ro" -v "$WORK:/work" "$TOOLS" "$@"
}
run_woody() {
	$CTR $CTR_OPTS run --rm --platform linux/amd64 \
		-v "$REPO:/repo:ro" -v "$WORK:/work" -w /work "$TOOLCHAIN" "$@"
}

echo "== tools image"
$CTR $CTR_OPTS build -q -t "$TOOLS" -f "$HERE/Containerfile" "$HERE" >/dev/null

echo "== kernel source"
run_tools sh -c "cd /work && { [ -f $KERNEL ] || curl -fsSLO https://cdn.kernel.org/pub/linux/kernel/v2.4/$KERNEL; } &&
	echo '$KERNEL_SHA256  $KERNEL' | sha256sum -c -"
run_tools sh /repo/tests/exfat-qemu/prepare-kernel.sh "/work/$KERNEL" /repo /work

echo "== build (gcc 2.95, i386)"
run_woody sh -c 'cd linux-2.4.17 || exit 1
	fail() { echo "$1 failed:"; tail -40 "../$2"; exit 1; }
	yes "" | make oldconfig >../oldconfig.log 2>&1
	grep -q "^CONFIG_EXFAT_FS=y" .config || fail "exFAT configuration" oldconfig.log
	make dep >../dep.log 2>&1 || fail "make dep" dep.log
	make -j$(grep -c ^processor /proc/cpuinfo) bzImage >../build.log 2>&1 ||
		fail "make bzImage" build.log
	grep -E "^(super|namei|file|misc)\.c:" ../build.log || true
	cd .. && gcc -static -O2 -Wall -o exfattest /repo/tests/exfat-qemu/exfattest.c'

STATUS=0
for CL in $CLUSTERS; do
	run_tools sh /repo/tests/exfat-qemu/guest.sh /work "$CL" || STATUS=1
done
if [ $STATUS = 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi
exit $STATUS
