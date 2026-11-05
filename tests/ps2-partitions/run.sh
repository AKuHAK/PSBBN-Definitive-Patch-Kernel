#!/bin/sh
# Test the MBR partitions next to APA in fs/partitions/ps2.c: the parser
# runs in user space (parttest.c) on APA disks made by pfsshell, with
# different MBRs in sector 0.  See README.md.
#
# Environment: CONTAINER=docker|podman, CONTAINER_OPTS, PS2PART_TEST_WORK
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${PS2PART_TEST_WORK:-$HERE/work}
CTR=${CONTAINER:-$(command -v docker || command -v podman || echo docker)}
CTR_OPTS=${CONTAINER_OPTS:-}
TOOLS=localhost/ps2-partitions-tools:latest
PFSSHELL_URL=https://github.com/AKuHAK/pfsshell.git
PFSSHELL_COMMIT=52b8e8310eb187ed4a55cade52e92cb293468c32

mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

echo "== tools image"
$CTR $CTR_OPTS build -q -t "$TOOLS" -f "$HERE/Containerfile" "$HERE" >/dev/null

# pfsshell needs its shared library from the build directory
cat > "$WORK/pfsshell" <<'WRAP'
#!/bin/sh
LD_LIBRARY_PATH=/work/pfsshell-src/build exec /work/pfsshell-src/build/pfsshell "$@"
WRAP
chmod +x "$WORK/pfsshell"

$CTR $CTR_OPTS run --rm -v "$REPO:/repo:ro" -v "$WORK:/work" -w /work "$TOOLS" sh -c "set -e
	if [ ! -d pfsshell-src ]; then
		git clone -q $PFSSHELL_URL pfsshell-src
		cd pfsshell-src && git checkout -q $PFSSHELL_COMMIT
		git submodule update -q --init --recursive --depth 1 && cd ..
	fi
	[ -x pfsshell-src/build/pfsshell ] ||
		(cd pfsshell-src && meson setup build >/dev/null && ninja -C build >/dev/null)
	cd /repo/tests/ps2-partitions
	gcc -std=gnu89 -O2 -Wall -Werror -I stub -o /work/parttest parttest.c
	python3 cases.py /work"
