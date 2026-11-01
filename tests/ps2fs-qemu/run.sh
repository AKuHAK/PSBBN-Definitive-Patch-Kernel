#!/bin/sh
# Run the ps2fs (PFS) driver test suite.  Reference images are made by
# pfsshell (ps2sdk's libapa/libpfs) and every result is checked with
# ps2sdk's PFS fsck built for the host.  See README.md.
#
# Environment: CONTAINER=docker|podman, CONTAINER_OPTS, PS2FS_TEST_WORK
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${PS2FS_TEST_WORK:-$HERE/work}
CTR=${CONTAINER:-$(command -v docker || command -v podman || echo docker)}
CTR_OPTS=${CONTAINER_OPTS:-}
TOOLCHAIN=docker.io/akuhak/ps2-legacy-woody-slim:latest
TOOLS=localhost/ps2fs-qemu-tools:latest
KERNEL=linux-2.4.17.tar.bz2
KERNEL_SHA256=db191ad39b92e3edb08b007c1b43f78498143a0609f1befad14c268847f891cf
PFSSHELL_URL=https://github.com/AKuHAK/pfsshell.git
PFSSHELL_COMMIT=52b8e8310eb187ed4a55cade52e92cb293468c32

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

echo "== pfsshell and pfsfsck (reference implementation)"
run_tools sh -c "set -e
	if [ ! -d pfsshell-src ]; then
		git clone -q $PFSSHELL_URL pfsshell-src
		cd pfsshell-src && git checkout -q $PFSSHELL_COMMIT
		git submodule update -q --init --recursive --depth 1 && cd ..
	fi
	[ -x pfsshell-src/build/pfsshell ] ||
		(cd pfsshell-src && meson setup build >/dev/null && ninja -C build >/dev/null)
	sh /repo/tests/ps2fs-qemu/pfsfsck/build.sh /work/pfsshell-src /work/pfsfsck >/dev/null
	gcc -O2 -o genpat /repo/tests/ps2fs-qemu/genpat.c"

# pfsshell needs its shared library from the build directory
cat > "$WORK/pfsshell" <<'WRAP'
#!/bin/sh
LD_LIBRARY_PATH=/work/pfsshell-src/build exec /work/pfsshell-src/build/pfsshell "$@"
WRAP
chmod +x "$WORK/pfsshell"

echo "== kernel source"
run_tools sh -c "{ [ -f $KERNEL ] || curl -fsSLO https://cdn.kernel.org/pub/linux/kernel/v2.4/$KERNEL; } &&
	echo '$KERNEL_SHA256  $KERNEL' | sha256sum -c -"
run_tools sh /repo/tests/ps2fs-qemu/prepare-kernel.sh "/work/$KERNEL" /repo /work

echo "== build (gcc 2.95, i386)"
# ARCH must be given to make: the Makefile sets it from uname -m
run_woody sh -c 'cd linux-2.4.17 || exit 1
	M="make ARCH=i386"
	fail() { echo "$1 failed:"; tail -40 "../$2"; exit 1; }
	yes "" | $M oldconfig >../oldconfig.log 2>&1
	grep -q "^CONFIG_PS2_FS=y" .config || fail "ps2fs configuration" oldconfig.log
	$M dep >../dep.log 2>&1 || fail "make dep" dep.log
	$M -j$(grep -c ^processor /proc/cpuinfo) bzImage >../build.log 2>&1 ||
		fail "make bzImage" build.log
	grep -E "^[a-z]+\.c:" ../build.log || true
	cd .. && gcc -static -O2 -Wall -o ps2fstest /repo/tests/ps2fs-qemu/ps2fstest.c'

echo "== reference image"
run_tools sh /repo/tests/ps2fs-qemu/mkimage.sh /work

STATUS=0
run_tools sh /repo/tests/ps2fs-qemu/guest.sh /work || STATUS=1
if [ $STATUS = 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi
exit $STATUS
