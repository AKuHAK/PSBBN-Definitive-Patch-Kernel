# exFAT driver tests (QEMU)

The PS2 kernel cannot be booted in an emulator, but `fs/exfat` only uses
the generic Linux 2.4 VFS.  This suite builds a **vanilla Linux 2.4.17 i386
kernel** with this repository's `fs/exfat`, using the same gcc 2.95 toolchain
container, boots it in QEMU and runs a test program as `/sbin/init` against
an exFAT disk image.  After every phase the image is checked with
`fsck.exfat` (exfatprogs).

## Running

```sh
tests/exfat-qemu/run.sh            # 4 KB and 128 KB clusters
tests/exfat-qemu/run.sh 32K        # other cluster sizes
```

Needs `docker` or `podman` (amd64 images; on Apple Silicon they run under
emulation, which is slower).  Environment variables:

| Variable          | Meaning                                                  |
|-------------------|----------------------------------------------------------|
| `CONTAINER`       | `docker` or `podman` (default: whichever is installed)   |
| `CONTAINER_OPTS`  | extra options, e.g. `-c ps2build` for a podman machine   |
| `EXFAT_TEST_WORK` | work directory (default `tests/exfat-qemu/work`, ignored by git) |

It prints `ALL TESTS PASSED` and exits 0, or `TESTS FAILED` with the
failing checks.  Guest logs are `work/phase<N>-<cluster>.log`, the kernel
build log is `work/build.log`.  GitHub Actions runs it on changes to
`fs/exfat` (`.github/workflows/exfat-tests.yml`).

## What is tested

`exfattest.c`, selected by `phase=` on the kernel command line:

1. **Phase 1**: files of 0 B to 1 MB with odd write sizes, overwrite in the
   middle, holes, truncate (shrink, extend, O_TRUNC), O_APPEND, shared mmap
   writes, directories, rename (directories, across directories, over a
   file, of an open file), unlink of an open file, case-insensitive lookup
   and O_EXCL, rejected names (`\`, `:`, control characters), 255/256
   character names, Unicode names, 600 entries with deletion during
   readdir, telldir/seekdir, read-only attribute, utime, exec from exFAT,
   4 concurrent writers, a 12 MB file in 24 MB of RAM, backward seeks, and
   filling the volume (ENOSPC).
2. **Phase 2**: a fresh boot verifies that everything persisted, plus
   `iocharset=iso8859-1`.
3. **Phase 3**: removes everything again.

## How it works

- `prepare-kernel.sh` unpacks the kernel.org tarball (SHA-256 checked),
  copies `fs/exfat`, applies the same `Config.in`/`Makefile`/NLS wiring as
  this tree, and writes the `.config`.
- QEMU writes its default `ramdisk_max` at setup offset 0x22c, where a boot
  protocol 2.02 kernel such as 2.4.17 still has code, so `setup.S` gets 8
  bytes of padding.  This only affects the test kernel.
- The root filesystem holds only the static test program and
  `/dev/console`, `/dev/null`.  glibc 2.2's static startup spins forever
  in init if `/dev/null` is missing.

When adding a test, keep `exfattest.c` C89 (it is built with gcc 2.95) and
make phase 2 check whatever phase 1 leaves behind.
