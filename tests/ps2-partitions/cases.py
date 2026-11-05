#!/usr/bin/env python3
"""MBR partitions next to APA in fs/partitions/ps2.c: build an APA disk with
pfsshell, put different MBRs into sector 0 and check what parttest (ps2.c in
user space) registers.  Usage: cases.py <workdir with pfsshell and parttest>
"""
import os
import struct
import subprocess
import sys

WORK = sys.argv[1]
PFSSHELL = os.path.join(WORK, "pfsshell")
PARTTEST = os.path.join(WORK, "parttest")
IMG = os.path.join(WORK, "apa.img")
DISK = 16 << 30
RESV_M = 8192          # APA header area of a main partition, sectors
FAILED = []


def build_base():
    cmds = "\n".join([
        "device %s" % IMG,
        "initialize yes",
        "mkpart __linux.1 128M EXT2",
        "mkpart PP.BIG 4G PFS",
        "mkpart __exfat 1G MBR",
        "mkpart __exfat.big 2G MBR",
        "mkpart __linux.63 128M EXT2",
        "exit", ""])
    if os.path.exists(IMG):
        os.remove(IMG)
    with open(IMG, "wb") as f:
        f.truncate(DISK)
    out = subprocess.run([PFSSHELL], input=cmds, capture_output=True,
                         text=True).stdout
    with open(os.path.join(WORK, "pfsshell.log"), "w") as f:
        f.write(out)
    errors = [l for l in out.splitlines()
              if "(!)" in l and "Unable to open hdd0" not in l]
    if errors:
        sys.exit("pfsshell reported errors:\n" + "\n".join(errors))


def apa_parts(path):
    """Main partitions of the APA chain: {id: (start, nsector, [subs])}"""
    parts, sect = {}, 0
    with open(path, "rb") as f:
        while True:
            f.seek(sect * 512)
            h = f.read(1024)
            magic, nxt = struct.unpack_from("<II", h, 4)
            if magic != 0x00415041:
                sys.exit("bad APA header at sector %d" % sect)
            pid = h[16:48].split(b"\0")[0].decode()
            start, nsector = struct.unpack_from("<II", h, 0x40)
            flag, nsub = struct.unpack_from("<HI", h, 0x4a)
            if not flag & 1:
                subs = [struct.unpack_from("<II", h, 0x200 + 8 * i)
                        for i in range(nsub)]
                parts[pid] = (start, nsector, subs)
            if nxt == 0:
                return parts
            sect = nxt


def apa_end(parts):
    end = 0
    for start, n, subs in parts.values():
        end = max([end, start + n] + [s + m for s, m in subs])
    return end


def write_mbr(entries, signature=True):
    """Sector 0 of the pfsshell image with this MBR (the image is shared)"""
    with open(IMG, "r+b") as f:
        f.seek(0)
        f.write(SECT0)
        f.seek(0x1BE)
        for e in (entries + [None] * 4)[:4]:
            if e is None:
                f.write(bytes(16))
            else:
                ptype, start, n = e
                f.write(struct.pack("<B3sB3sII", 0, b"\0\0\0", ptype,
                                    b"\0\0\0", start, n))
        f.write(b"\x55\xaa" if signature else b"\0\0")


def run():
    out = subprocess.run([PARTTEST, IMG], capture_output=True,
                         text=True).stdout
    parts, warn = {}, []
    for line in out.splitlines():
        w = line.split()
        if w[:1] == ["part"]:
            parts[int(w[1])] = (int(w[2]), int(w[3]))
        if "clashes with APA partition" in line:
            warn.append(line.split("partition ")[-1].split(",")[0])
    return out, parts, warn


def case(name, entries, expect, warnings=(), signature=True, corrupt=None):
    write_mbr(entries, signature)
    if corrupt is not None:
        with open(IMG, "r+b") as f:
            f.seek(corrupt * 512 + 4)
            magic = f.read(4)
            f.seek(corrupt * 512 + 4)
            f.write(bytes(4))
    out, parts, warn = run()
    if corrupt is not None:
        with open(IMG, "r+b") as f:
            f.seek(corrupt * 512 + 4)
            f.write(magic)
    mbr = {m: p for m, p in parts.items() if m >= 60 and m not in APA_MINORS}
    ok = mbr == expect and sorted(warn) == sorted(warnings)
    if corrupt is None:
        ok = ok and all(parts.get(m) == p for m, p in APA_MINORS.items())
    print("%s %s" % ("PASS" if ok else "FAIL", name))
    if not ok:
        FAILED.append(name)
        print("  expected MBR %s, warnings %s" % (expect, list(warnings)))
        print("  " + out.replace("\n", "\n  "))


build_base()
with open(IMG, "rb") as f:
    SECT0 = f.read(512)
APA = apa_parts(IMG)
END = apa_end(APA)
lnx_s, lnx_n, _ = APA["__linux.1"]
big_s, big_n, big_subs = APA["PP.BIG"]
ex_s, ex_n, ex_subs = APA["__exfat"]
eb_s, eb_n, eb_subs = APA["__exfat.big"]
emp_s, emp_n, _ = APA["__empty"]
assert big_subs and eb_subs and not ex_subs, \
    "PP.BIG and __exfat.big need subs, __exfat none"
assert END + (1 << 21) < DISK // 512, "no free space after APA"
APA_MINORS = {1: (lnx_s + RESV_M, lnx_n - RESV_M)}
write_mbr([], signature=False)
_, p, _ = run()
APA_MINORS[63] = p.get(63)
print("APA: __linux.1 %d+%d, PP.BIG %d+%d subs %s, __exfat %d+%d, end %d"
      % (lnx_s, lnx_n, big_s, big_n, big_subs, ex_s, ex_n, END))

ex_data = (ex_s + RESV_M, ex_n - RESV_M)
free = (END + 2048, DISK // 512 - END - 4096)
case("no MBR signature: APA only", [], {}, signature=False)
case("APA-Jail: 0x17 over APA skipped, exFAT in __exfat data area",
     [(0x17, 0, END), (0x07, ) + ex_data], {61: ex_data})
case("exFAT in unallocated space after APA",
     [(0x07, ) + free], {60: free})
case("two entries: __exfat data area and free space",
     [None, None, (0x07, ) + ex_data, (0x0c, ) + free],
     {62: ex_data},
     warnings=["__linux.63"])
case("entry over the __exfat APA header area",
     [(0x07, ex_s, ex_n)], {}, warnings=["__exfat"])
case("entry over __linux.1", [(0x07, lnx_s + RESV_M, 2048)], {},
     warnings=["__linux.1"])
case("entry over a sub-partition of PP.BIG only",
     [(0x07, big_subs[0][0] + 64, 2048)], {}, warnings=["PP.BIG"])
case("entry at PC alignment (sector 2048) over __mbr",
     [(0x07, 2048, 1 << 20)], {}, warnings=["__mbr"])
case("entry inside the main data area of __exfat.big",
     [(0x07, eb_s + RESV_M, eb_n - RESV_M)], {60: (eb_s + RESV_M, eb_n - RESV_M)})
case("entry across a sub-partition header of __exfat.big",
     [(0x07, eb_s + RESV_M, eb_n - RESV_M + 4096)], {},
     warnings=["__exfat.big"])
case("entry over a deleted APA partition (__empty)",
     [(0x07, emp_s + 4096, 4096)], {}, warnings=["__empty"])
case("entry past the end of the disk ignored",
     [(0x07, DISK // 512 - 1024, 4096)], {})
case("broken APA chain: no MBR partitions",
     [(0x07, ) + free], {}, corrupt=big_s)

if FAILED:
    print("FAILED: %d case(s)" % len(FAILED))
    sys.exit(1)
print("ALL CASES PASSED")
