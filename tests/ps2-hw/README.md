# Filesystem tests on a real PS2

Tools used for the first hardware test of `fs/exfat` and `fs/ps2fs` on a PS2
running PSBBN (see the commit history around 2026-11-01).  Build the
programs with the PS2 toolchain container
(`mipsEEel-linux-gcc -O2 -o hwfstest hwfstest.c`; same for `pfsdump.c` and
`apalist.c`), copy them to the PS2 and run them as root.

| File | Runs on | Purpose |
|------|---------|---------|
| `hwfstest.c` | PS2 | `hwfstest DIR write\|verify\|clean FLAGS BIGMB`: builds a known tree (files of 0 B..1 MB, a big file, two files grown in turn so their zone lists use SEGI blocks, two writers at once, a 400-entry directory with deletions, renames, symlink, truncate, mmap) and checks it after a remount. FLAGS select what the filesystem supports (`R`ename, `S`ymlink, `T`runcate, `M` chmod/utime, `C`ase-insensitive, `W` mmap write, `P` print CKSUM lines). |
| `apalist.c` | PS2 | Lists the APA partitions; `apalist /dev/hda dump FILE` saves every partition header. |
| `pfsdump.c` | PS2 or PC | `pfsdump /dev/hda PARTITION`: streams the used zones (per the zone bitmaps) of a PFS partition and its sub-partitions.  A few MB for a mostly empty partition, and a complete backup of it: writing the zones back restores it. |
| `pfscheck.py` | PC | Checks a `pfsdump` stream against section 9 of `Documentation/filesystems/ps2fs-format.md`: inode/SEGI checksums and counters, extents, directory tiling, `.`/`..`, zones referenced twice, and an exact bitmap comparison per sub-partition.  Use it for volumes with sub-partitions, where ps2sdk's fsck reports false errors. |
| `pfsundump.py` | PC | Builds a sparse disk image from the APA headers and a dump, for pfsshell (`get`, `ls`) and `tests/ps2fs-qemu/pfsfsck`. |

Before writing to a PSBBN partition, stop the navigator (`/opt0/bn/bn`):
PSBBN accesses PFS through its own user-space server `/opt0/bn/pfsd/pfsd`,
which caches the metadata of the partitions its clients mount.  Killing
`bn` makes pfsd unmount them; restart the navigator afterwards with
`su -l root -s /bin/sh -c "cd /opt0/bn ; ./bn >> /dev/null 2>&1" &`.

Findings of the first run: partitions written by older tools can carry
leaked zones (bitmap bits without a reference, e.g. 109 zones right after
the root directory); compare `pfscheck` output before and after a test
instead of expecting a clean result.
