# ps2fs (PFS) driver tests (QEMU)

Like `../exfat-qemu`: a **vanilla Linux 2.4.17 i386 kernel** with this
repository's `fs/ps2fs` is built with the gcc 2.95 container and booted in
QEMU, with `ps2fstest` as `/sbin/init`.  The oracle is ps2sdk's own PFS
code, through [pfsshell](https://github.com/AKuHAK/pfsshell) (a pinned
commit is built from source):

- `mkimage.sh` makes the reference disk with pfsshell: a 4 GB APA disk
  with a 256 MB PFS partition `PP.TEST` holding files of many sizes, a
  300-entry directory, nested directories and a 200-character name;
- `pfsfsck/` builds ps2sdk's PFS fsck for the host; it must report the
  partition clean after every phase;
- files the driver prints as `CKSUM` lines must read back identically
  through pfsshell, and directories it prints as `LS` lines must list the
  same entries in the same (on-disk) order.

## Running

```sh
tests/ps2fs-qemu/run.sh
```

Same requirements and variables as the exFAT suite (`CONTAINER`,
`CONTAINER_OPTS`), with `PS2FS_TEST_WORK` for the work directory (default
`tests/ps2fs-qemu/work`).  It prints `ALL TESTS PASSED` or `TESTS FAILED`;
guest logs are `work/phase<N>.log`, fsck output `work/fsck<N>.log`.

## Phases

1. Read-only mount: contents of every file, the large directory,
   telldir/seekdir, mmap, statfs; writes are refused without
   `unsafe_write`.
2. `unsafe_write`: overwrite, append and truncate pfsshell's files; delete
   half of the 300 entries and create 100 into the freed slots; mkdir,
   create, 255-byte names, a 400-file directory spanning several zones,
   two files grown in turn (one-zone extents, so the zone list continues
   in SEGI blocks), truncation that drops SEGIs and regrowth, a 3 MB file,
   sparse writes, symlinks (903-byte limit), rename in all variants,
   rmdir/unlink rules, unlink of an open file, chmod, utime, chown refusal.
3. Read-only after a reboot: everything from phase 2 persisted.
4. pfsshell first changes the Linux-written tree (put, mkdir, rm, rename);
   then the driver reads that, fills the partition to ENOSPC, frees it,
   and removes everything it created.

Do not let pfsshell `rm` a file whose zone list has SEGI blocks: ps2sdk's
pfs then leaks the zones of those blocks and fsck reports a bitmap
mismatch (a known bug of the reference, see
`Documentation/filesystems/ps2fs-format.md`, section 3.9).
