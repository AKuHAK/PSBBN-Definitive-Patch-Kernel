#!/usr/bin/env python3
"""Add IOP modules to the PS2 Linux IOP image of PSBBN's boot loader.

PSBBN starts Linux with Sony's osdboot.elf (__system:/osdboot.elf, also used
as p2lboot/BB.ELF).  Before jumping to the kernel it reboots the IOP with
the packed IOPRP image "234" embedded in it (see README.md).  The IOP of
that environment cannot load modules at run time from EE memory or from
the memory card, so modules Linux needs on the IOP (iomanX, fileXio and
mmceman for fs/mmcefs) have to be part of that image.  This script:

  1. unpacks image "234", adds the modules to its ROMDIR and loads them
     from its IOPBTCONF (before EESYNC2), and packs it again;
  2. rebuilds the IRX that carries the image (bigger data section);
  3. puts it in a new segment of osdboot.elf after .bss, points the image
     table at it and moves the start of the heap behind it;
  4. optionally disables the kernel CRC check (--no-crc-check).

Usage: osdboot_mmce.py osdboot.elf out.elf iomanX.irx fileXio.irx mmceman.irx
"""
import argparse
import hashlib
import struct
import sys

ORIG_MD5 = "1649fba52ce8eb81dd9e5172921c14ed"   # PSBBN __system:/osdboot.elf

TEXT_BASE = 0x1000000          # VA of the single PT_LOAD of osdboot.elf
BSS_END = 0x12c4a78            # end of .bss, start of the heap
CURBRK = 0x11027dc             # sbrk()'s current break, = BSS_END
SETUPHEAP_LUI, SETUPHEAP_ADDIU = 0x100006c, 0x1000074
IMAGE_TABLE_234 = 0x10465b8    # {id, name, irx start, irx end, sbios start, end}
CRC_BRANCH = 0x1009f88         # bnez $v0 after the kernel cksum compare
CRC_ORIG, CRC_PATCHED = bytes.fromhex("07004014"), bytes.fromhex("07000010")

# IRX "234": offsets inside the module (addresses from 0, file = addr + 0x90)
IRX_LOAD_OFF = 0x90
IRX_DATA_ADDR = 0x1690
IRX_ENDM4_HI, IRX_ENDM4_LO = 0xe50, 0xe54     # lui/addiu of (data end - 4)
IRX_END_HI, IRX_END_LO = 0xfdc, 0xfe0         # lui/addiu of (data end)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


# ---- packed image format (decoded from the IRX's own unpacker) ----------

def unpack(blob):
    total, outsize, keylen = struct.unpack_from("<III", blob, 0)
    key = blob[16:16 + keylen]
    src, k, out, left, ctrl = 16 + keylen, 0, bytearray(), 0, 0

    def rd():
        nonlocal src, k
        b = blob[src] ^ key[k]
        src += 1
        k = (k + 1) % keylen
        return b
    while len(out) < outsize:
        if left == 0:
            ctrl = (rd() << 24) | (rd() << 16) | (rd() << 8) | rd()
            shift = ctrl & 3
            lshift, mask, left = 14 - shift, 0x3fff >> shift, 30
        b = rd()
        if ctrl & 0x80000000:
            v = (b << 8) | rd()
            off, n = (v & mask) + 1, (v >> lshift) + 3
            for _ in range(n):
                out.append(out[-off])
        else:
            out.append(b)
        ctrl = (ctrl << 1) & 0xffffffff
        left -= 1
    return bytes(out)


def pack(data, key, xbyte, shift=2):
    lshift, mask = 14 - shift, 0x3fff >> shift
    maxlen = (0xffff >> lshift) + 3
    items, head, i, n = [], {}, 0, len(data)
    while i < n:
        best_len = best_off = 0
        if i + 3 <= n:
            for j in reversed(head.get(data[i:i + 3], [])[-64:]):
                off = i - j
                if off > mask + 1:
                    break
                l = 0
                while l < maxlen and i + l < n and data[j + l] == data[i + l]:
                    l += 1
                if l > best_len:
                    best_len, best_off = l, off
                    if l == maxlen:
                        break
        if best_len >= 3:
            v = ((best_len - 3) << lshift) | (best_off - 1)
            items.append((1, bytes([v >> 8, v & 0xff])))
            step = best_len
        else:
            items.append((0, data[i:i + 1]))
            step = 1
        for p in range(i, min(i + step, n - 2)):
            head.setdefault(data[p:p + 3], []).append(p)
        i += step
    raw = bytearray()
    for g in range(0, len(items), 30):
        grp = items[g:g + 30]
        ctrl = shift
        for bit, (ref, _) in enumerate(grp):
            if ref:
                ctrl |= 1 << (31 - bit)
        raw += struct.pack(">I", ctrl)
        for _, b in grp:
            raw += b
    enc = bytes(c ^ key[i % len(key)] for i, c in enumerate(raw))
    return struct.pack("<IIIB3x", len(data), len(data), len(key), xbyte) + key + enc


# ---- IOPRP image ---------------------------------------------------------

def romdir(img):
    ents, off, i = [], 0, 0
    while img[i:i + 10].split(b"\0")[0]:
        name = img[i:i + 10].split(b"\0")[0].decode()
        ext, size = struct.unpack_from("<HI", img, i + 10)
        ents.append([name, ext, img[off:off + size]])
        off += (size + 15) & ~15
        i += 16
    return ents


def build_ioprp(ents):
    ents = [list(e) for e in ents]
    names = [e[0] for e in ents]
    ents[names.index("ROMDIR")][2] = b"\0" * (16 * (len(ents) + 1))
    rd = bytearray()
    for name, ext, data in ents:
        rd += name.encode().ljust(10, b"\0") + struct.pack("<HI", ext, len(data))
    rd += b"\0" * 16
    ents[names.index("ROMDIR")][2] = bytes(rd)
    out = bytearray()
    for name, ext, data in ents:
        out += data + b"\0" * (-len(data) % 16)
    # the last file is not padded in Sony's images; keep the exact size
    last = ents[-1][2]
    return bytes(out[:len(out) - (-len(last) % 16)])


def add_modules(img, mods, before="EESYNC2"):
    ents = romdir(img)
    names = [e[0] for e in ents]
    conf_i = names.index("IOPBTCONF")
    conf = ents[conf_i][2].decode()
    lines = conf.split("\n")
    if before not in lines:
        sys.exit("IOPBTCONF has no %s line" % before)
    for name, _ in mods:
        if name in names:
            sys.exit("%s is already in the image" % name)
    at = lines.index(before)
    lines[at:at] = [name for name, _ in mods]
    ents[conf_i][2] = "\n".join(lines).encode()
    for name, data in mods:
        ents.append([name, 0, data])
    return build_ioprp(ents), ents[conf_i][2].decode()


# ---- IRX carrying the image ----------------------------------------------

def set_hi_lo(b, hi_off, lo_off, value):
    hi = u32(b, IRX_LOAD_OFF + hi_off)
    lo = u32(b, IRX_LOAD_OFF + lo_off)
    if hi >> 26 != 15 or lo >> 26 != 9:
        sys.exit("unexpected instructions in the IRX")
    struct.pack_into("<I", b, IRX_LOAD_OFF + hi_off,
                     (hi & 0xffff0000) | (((value + 0x8000) >> 16) & 0xffff))
    struct.pack_into("<I", b, IRX_LOAD_OFF + lo_off,
                     (lo & 0xffff0000) | (value & 0xffff))


def rebuild_irx(irx, new_data):
    shoff, = struct.unpack_from("<I", irx, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", irx, 46)
    sh = [list(struct.unpack_from("<10I", irx, shoff + i * shentsize)) for i in range(shnum)]
    names = [irx[sh[shstrndx][4] + s[0]:].split(b"\0")[0].decode() for s in sh]
    data_i = names.index(".data")
    old_size = sh[data_i][5]
    if sh[data_i][3] != IRX_DATA_ADDR or sh[data_i][4] != IRX_LOAD_OFF + IRX_DATA_ADDR:
        sys.exit("unexpected .data placement in the IRX")
    new_data = new_data + b"\0" * (-len(new_data) % 16)
    delta = len(new_data) - old_size
    data_off = sh[data_i][4]
    tail_off = data_off + old_size
    out = bytearray(irx[:data_off]) + new_data + irx[tail_off:]
    end = IRX_DATA_ADDR + len(new_data)
    # sections after .data move by delta; .bss starts at the new end
    for i, s in enumerate(sh):
        if i == data_i:
            s[5] = len(new_data)
        elif s[4] >= tail_off:
            s[4] += delta
        if names[i] == ".bss":
            s[3] = end
    shoff += delta
    struct.pack_into("<I", out, 32, shoff)
    for i, s in enumerate(sh):
        struct.pack_into("<10I", out, shoff + i * shentsize, *s)
    # PT_LOAD sizes and the .iopmod sizes / gp
    phoff, = struct.unpack_from("<I", out, 28)
    phnum, = struct.unpack_from("<H", out, 44)
    for i in range(phnum):
        p = phoff + 32 * i
        ptype, poff = struct.unpack_from("<II", out, p)
        if ptype == 1:
            struct.pack_into("<II", out, p + 16, end, end)
        elif ptype == 0x70000080:
            iopmod = poff
    text_size = u32(out, iopmod + 12)
    struct.pack_into("<I", out, iopmod + 8, end + 0x7ff0)            # gp
    struct.pack_into("<I", out, iopmod + 16, end - text_size)        # data
    set_hi_lo(out, IRX_ENDM4_HI, IRX_ENDM4_LO, end - 4)
    set_hi_lo(out, IRX_END_HI, IRX_END_LO, end)
    return bytes(out)


# ---- osdboot.elf ----------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("osdboot")
    ap.add_argument("out")
    ap.add_argument("iomanx")
    ap.add_argument("filexio")
    ap.add_argument("mmceman")
    ap.add_argument("--poweroff",
                    help="path to poweroff.irx for front-panel power button support on Slim PS2")
    ap.add_argument("--no-crc-check", action="store_true",
                    help="boot kernels without the trailing cksum too")
    ap.add_argument("--any-input", action="store_true",
                    help="do not insist on the known osdboot.elf")
    a = ap.parse_args()

    elf = bytearray(open(a.osdboot, "rb").read())
    if hashlib.md5(elf).hexdigest() != ORIG_MD5 and not a.any_input:
        sys.exit("%s is not the known PSBBN osdboot.elf (md5 %s)" % (a.osdboot, ORIG_MD5))
    phoff, = struct.unpack_from("<I", elf, 28)
    phnum, = struct.unpack_from("<H", elf, 44)
    ptype, poff, pva, ppa, pfs, pms = struct.unpack_from("<6I", elf, phoff)
    if phnum != 1 or pva != TEXT_BASE or u32(elf, 24) != 0x1000008:
        sys.exit("unexpected osdboot.elf layout")

    def off(va):
        return poff + va - pva

    ent = [u32(elf, off(IMAGE_TABLE_234) + 4 * i) for i in range(6)]
    name = elf[off(ent[1]):off(ent[1]) + 4].split(b"\0")[0]
    if ent[0] != 1 or name != b"234":
        sys.exit("image table entry 234 not found")
    irx = bytes(elf[off(ent[2]):off(ent[3])])
    blob = irx[IRX_LOAD_OFF + IRX_DATA_ADDR:]
    total, outsize, keylen = struct.unpack_from("<III", blob, 0)
    key, xbyte = blob[16:16 + keylen], blob[12]
    img = unpack(blob)
    if len(img) != outsize:
        sys.exit("unpacking failed")

    mods = [("IOMANX", open(a.iomanx, "rb").read()),
            ("FILEXIO", open(a.filexio, "rb").read()),
            ("MMCEMAN", open(a.mmceman, "rb").read())]
    if a.poweroff:
        mods.append(("POWEROFF", open(a.poweroff, "rb").read()))
    new_img, conf = add_modules(img, mods)
    packed = pack(new_img, key, xbyte)
    if unpack(packed) != new_img:
        sys.exit("repacking does not round-trip")
    new_irx = rebuild_irx(irx, packed)

    # second PT_LOAD after .bss; the heap starts behind it
    new_va = (BSS_END + 0xfff) & ~0xfff
    file_off = (len(elf) + 0xfff) & ~0xfff
    heap = (new_va + len(new_irx) + 0xff) & ~0xff
    elf += b"\0" * (file_off - len(elf)) + new_irx
    if phoff + 64 > poff:
        sys.exit("no room for a second program header")
    struct.pack_into("<8I", elf, phoff + 32, 1, file_off, new_va, new_va,
                     len(new_irx), len(new_irx), 7, 0x1000)
    struct.pack_into("<H", elf, 44, 2)
    struct.pack_into("<II", elf, off(IMAGE_TABLE_234) + 8, new_va, new_va + len(new_irx))
    if u32(elf, off(CURBRK)) != BSS_END:
        sys.exit("sbrk start not where expected")
    struct.pack_into("<I", elf, off(CURBRK), heap)
    lui, addiu = u32(elf, off(SETUPHEAP_LUI)), u32(elf, off(SETUPHEAP_ADDIU))
    if (lui & 0xffff) != ((BSS_END + 0x8000) >> 16) or (addiu & 0xffff) != BSS_END & 0xffff:
        sys.exit("SetupHeap arguments not where expected")
    struct.pack_into("<I", elf, off(SETUPHEAP_LUI), (lui & 0xffff0000) | ((heap + 0x8000) >> 16))
    struct.pack_into("<I", elf, off(SETUPHEAP_ADDIU), (addiu & 0xffff0000) | (heap & 0xffff))
    if a.no_crc_check:
        o = off(CRC_BRANCH)
        if elf[o:o + 4] != CRC_ORIG:
            sys.exit("kernel CRC branch not where expected")
        elf[o:o + 4] = CRC_PATCHED
    open(a.out, "wb").write(elf)

    print("IOPBTCONF of the Linux IOP image is now:")
    print("  " + conf.strip().replace("\n", "\n  "))
    print("image 234: %d -> %d bytes unpacked, IRX %d -> %d bytes" %
          (len(img), len(new_img), len(irx), len(new_irx)))
    print("new segment at %#x, heap from %#x; %s written%s" %
          (new_va, heap, a.out, ", kernel CRC check off" if a.no_crc_check else ""))


if __name__ == "__main__":
    main()
