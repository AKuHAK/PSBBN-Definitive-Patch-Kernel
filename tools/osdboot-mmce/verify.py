#!/usr/bin/env python3
"""Check a loader built by osdboot_mmce.py against the original:
  verify.py osdboot.elf osdboot_mmce.elf
Lists the files of image 234 that changed, and runs the IRX's own unpacker
(function 0xb20, in the unicorn MIPS emulator: pip install unicorn) on the
old and the new data."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from osdboot_mmce import unpack, romdir as _romdir
from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN
from unicorn.mips_const import *
from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN
from unicorn.mips_const import *
def romdir(img):
    out, off = [], 0
    for name, ext, data in _romdir(img):
        out.append((name, ext, len(data), off))
        off += (len(data) + 15) & ~15
    return out
orig = open(sys.argv[1], "rb").read()
new = open(sys.argv[2], "rb").read()
def irx_of(elf):
    phnum = struct.unpack_from("<H", elf, 44)[0]
    segs = [struct.unpack_from("<8I", elf, 0x34 + 32 * i) for i in range(phnum)]
    def off(va):
        for t, o, v, p, fs, ms, fl, al in segs:
            if v <= va < v + fs: return o + va - v
    ent = struct.unpack_from("<6I", elf, off(0x10465b8))
    return elf[off(ent[2]):off(ent[2]) + ent[3] - ent[2]]
oirx, nirx = irx_of(orig), irx_of(new)
oimg = unpack(oirx[0x90 + 0x1690:]); nimg = unpack(nirx[0x90 + 0x1690:])
o_ents = {n: oimg[off:off + s] for n, e, s, off in romdir(oimg)}
n_ents = {n: nimg[off:off + s] for n, e, s, off in romdir(nimg)}
print("original files that differ in the new image:", [n for n in o_ents if o_ents[n] != n_ents.get(n)])

def emulate(irx):
    mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
    ld = struct.unpack_from("<I", irx, 0x34 + 32 + 16)[0]
    code = irx[0x90:0x90 + ld]
    total = struct.unpack_from("<I", irx, 0x90 + 0x1690)[0]
    BASE, OUT, STACK = 0x100000, 0x400000, 0x300000
    mu.mem_map(0, 0x800000)
    mu.mem_write(BASE, code)
    shoff = struct.unpack_from("<I", irx, 32)[0]; shnum = struct.unpack_from("<H", irx, 48)[0]
    sh = [struct.unpack_from("<10I", irx, shoff + 40 * i) for i in range(shnum)]
    for s in sh:
        if s[1] != 9: continue
        rels = [struct.unpack_from("<II", irx, s[4] + 8 * i) for i in range(s[5] // 8)]
        k = 0
        while k < len(rels):
            o, info = rels[k]; t = info & 0xff
            w = struct.unpack_from("<I", mu.mem_read(BASE + o, 4))[0]
            if t == 2: w = (w + BASE) & 0xffffffff
            elif t == 4: w = (w & 0xfc000000) | ((((w & 0x3ffffff) << 2) + BASE) >> 2 & 0x3ffffff)
            elif t == 5:
                o2 = rels[k + 1][0]; w2 = struct.unpack_from("<I", mu.mem_read(BASE + o2, 4))[0]
                v = ((w & 0xffff) << 16) + ((w2 & 0xffff) ^ 0x8000) - 0x8000 + BASE
                w = (w & 0xffff0000) | (((v + 0x8000) >> 16) & 0xffff)
                w2 = (w2 & 0xffff0000) | (v & 0xffff)
                mu.mem_write(BASE + o2, struct.pack("<I", w2)); k += 1
            mu.mem_write(BASE + o, struct.pack("<I", w)); k += 1
    mu.reg_write(UC_MIPS_REG_A0, BASE + 0x1690)
    mu.reg_write(UC_MIPS_REG_A1, OUT)
    mu.reg_write(UC_MIPS_REG_SP, STACK)
    mu.reg_write(UC_MIPS_REG_RA, 0x7fff00)
    mu.emu_start(BASE + 0xb20, 0x7fff00)
    return mu.reg_read(UC_MIPS_REG_V0), bytes(mu.mem_read(OUT, total))

r, out = emulate(oirx); print("original IRX, its own unpacker: returned", r, "= python unpack:", out == oimg)
r, out = emulate(nirx); print("new IRX, its own unpacker: returned", r, "= new image:", out == nimg)
