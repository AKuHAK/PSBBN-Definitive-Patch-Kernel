# APA + MBR partition table test

`fs/partitions/ps2.c` reads the APA partition chain of a PS2 disk and gives
each `__linux.N` partition the minor N.  It also reads a classic MBR from
sector 0, inside the APA header (as APA-Jail places it), and gives its
primary entries the last four minors of the disk: `hda60`..`hda63` for
entries 1..4.  An entry is used only if it lies on the disk and overlaps no
APA partition except those whose name starts with `__exfat`, which reserve
the space on the APA side; even there it must leave their APA headers (the
first 4 MiB of the main partition, 4 KiB of each sub) alone.  Type 0x17
(the APA-Jail entry over the APA area) is skipped.  If the APA chain is
broken, no MBR partition is registered.

`parttest.c` compiles `ps2.c` unchanged in user space, with the few kernel
interfaces it uses stubbed in `stub/`.  `cases.py` builds a 16 GiB APA disk
with pfsshell (`__linux.1`, a PFS partition with sub-partitions, `__exfat`,
`__exfat.big` with a sub, `__linux.63`), then writes different MBRs into
sector 0 and checks the registered partitions and warnings against the APA
chain read from the image.

    tests/ps2-partitions/run.sh       # docker or podman

Without a container: build pfsshell, put a `pfsshell` wrapper and
`gcc -std=gnu89 -I stub -o <work>/parttest parttest.c` into a work
directory, and run `python3 cases.py <work>`.
