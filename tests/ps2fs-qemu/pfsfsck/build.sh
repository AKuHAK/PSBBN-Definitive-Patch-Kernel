#!/bin/sh
# build.sh <pfsshell source tree> <output binary>
set -eu
P=$1
H=$P/external/ps2sdk/iop/hdd
B=$(mktemp -d)
CF="-O1 -w -D_FILE_OFFSET_BITS=64 -include fcntl.h -I$(dirname "$0")/shim -DAPA_POSIX_VER -DAPA_USE_ATAD -DPFS_POSIX_VER
 -I$P/subprojects/fakeps2sdk/include -I$P/subprojects/iomanX -I$P/subprojects/iomanX/include
 -I$P/subprojects/apa -I$P/subprojects/apa/include -I$P/subprojects/apa/irx
 -I$P/subprojects/pfs/include -I$H/fsck/src"
SRC="$P/subprojects/fakeps2sdk/atad.c $P/subprojects/fakeps2sdk/thsemap.c
 $P/subprojects/iomanX/src/ioman_sbv.c $P/subprojects/iomanX/src/iomanX.c
 $P/subprojects/apa/_init.c $P/subprojects/apa/irx/hdd_fio.c $P/subprojects/apa/irx/hdd_blkio.c $P/subprojects/apa/irx/hdd.c
 $P/subprojects/apa/src/apa.c $P/subprojects/apa/src/cache.c $P/subprojects/apa/src/free.c
 $P/subprojects/apa/src/journal.c $P/subprojects/apa/src/misc.c $P/subprojects/apa/src/password.c
 $H/libpfs/src/bitmap.c $H/libpfs/src/block.c $H/libpfs/src/blockWrite.c $H/libpfs/src/cache.c
 $H/libpfs/src/dir.c $H/libpfs/src/inode.c $H/libpfs/src/journal.c $H/libpfs/src/misc.c
 $H/libpfs/src/super.c $H/libpfs/src/superWrite.c
 $H/fsck/src/bitmap_fsck.c $(dirname "$0")/pfsfsck.c"
gcc $CF -o "$2" $SRC -lpthread
echo "built $2"
