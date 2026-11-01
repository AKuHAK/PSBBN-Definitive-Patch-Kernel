"""pfscheck.py DUMP: check a PFS volume (pfsdump stream) against the rules
of Documentation/filesystems/ps2fs-format.md section 9, including volumes
with sub-partitions, where ps2sdk's fsck is unreliable.  Prints problems,
a summary, and exits 1 if anything is wrong."""
import struct, sys

d = open(sys.argv[1], 'rb')
assert d.read(8) == b'PFSDUMP1'
nparts, zs = struct.unpack('<II', d.read(8))
parts = [struct.unpack('<II', d.read(8)) for _ in range(nparts)]
zones = {}                      # (sub, zone) -> bytes
while True:
    s, z, c = struct.unpack('<III', d.read(12))
    if not c:
        break
    for i in range(c):
        zones[(s, z + i)] = d.read(zs)
ss = (zs // 512).bit_length() - 1
nz = [p[1] >> ss for p in parts]
errors = []
def err(msg):
    errors.append(msg)
    if len(errors) <= 40:
        print('ERROR', msg)

def block(sub, blk):            # 1 KiB block
    z, off = divmod(blk * 1024, zs)
    return zones.get((sub, z), b'\0' * zs)[off:off + 1024]

sb = zones[(0, 0x2000 >> ss)][:40]
magic, ver, modver, fsck, zsize, nsubs = struct.unpack_from('<6I', sb)
log = struct.unpack_from('<IHH', sb, 24)
root = struct.unpack_from('<IHH', sb, 32)
if magic != 0x50465300 or zsize != zs or nsubs + 1 != nparts:
    err('superblock magic %x zone %d subs %d' % (magic, zsize, nsubs))
if fsck & 1:
    err('WRITE_ERROR flag set in the superblock')

ref = [bytearray(n) for n in nz]        # computed bitmap (1 per zone)
def mark(sub, z, c, what):
    for i in range(c):
        if ref[sub][z + i]:
            err('zone %d/%d referenced twice (%s)' % (sub, z + i, what))
        ref[sub][z + i] = 1
z4 = 0x2000 >> ss
mark(0, 0, log[0] + log[2], 'reserved')
for s in range(1, nparts):
    bs = (nz[s] // 8 + zs - 1) // zs
    mark(s, 0, 1 + bs, 'reserved')

def checksum(b):
    w = struct.unpack('<256I', b)
    return sum(w[1:]) & 0xffffffff == w[0]

stats = {'files': 0, 'dirs': 0, 'links': 0, 'segi': 0, 'maxext': 0, 'subs_used': set()}

def load_inode(sub, zone, path):
    b = block(sub, zone * (zs // 1024))
    if b[4:8] != b'DGES' or not checksum(b):
        err('%s: inode %d/%d bad magic or checksum' % (path, sub, zone))
        return None
    ib = struct.unpack_from('<IHH', b, 8)
    if ib[:2] != (zone, sub):
        err('%s: inode_block %s is not itself' % (path, ib))
    nxt = struct.unpack_from('<IHH', b, 16)
    last = struct.unpack_from('<IHH', b, 24)
    mode, attr = struct.unpack_from('<HH', b, 0x3B8)
    size = struct.unpack_from('<Q', b, 0x3D8)[0]
    nblocks, ndata, nsegd = struct.unpack_from('<III', b, 0x3E0)
    ents = [struct.unpack_from('<IHH', b, 0x28 + 8 * i) for i in range(min(ndata, 114))]
    descs = [ents[0]] if ents else []
    i = 114
    while i < ndata:
        if not nxt[2]:
            err('%s: chain ends before number_data' % path); break
        s2 = block(nxt[1], nxt[0] * (zs // 1024))
        if s2[4:8] != b'IGES' or not checksum(s2):
            err('%s: bad SEGI %s' % (path, nxt)); break
        stats['segi'] += 1
        segents = [struct.unpack_from('<IHH', s2, 0x28 + 8 * k) for k in range(min(ndata - i, 123))]
        if segents[0] != nxt:
            err('%s: SEGI data[0] is not itself' % path)
        descs.append(segents[0])
        ents += segents
        nxt = struct.unpack_from('<IHH', s2, 16)
        i += 123
    total = 0
    for k, (n, s, c) in enumerate(ents):
        if s >= nparts or c == 0 or n < 2 or (n + c) << ss > parts[s][1]:
            err('%s: bad zone entry %d %s' % (path, k, (n, s, c))); return None
        mark(s, n, c, path)
        total += c
        stats['subs_used'].add(s)
    if total != nblocks:
        err('%s: number_blocks %d, zones %d' % (path, nblocks, total))
    if len(descs) != nsegd:
        err('%s: number_segdesg %d, descriptors %d' % (path, nsegd, len(descs)))
    if descs and last != descs[-1]:
        err('%s: last_segment %s, last descriptor %s' % (path, last, descs[-1]))
    data = [e for k, e in enumerate(ents) if k != 0 and not (k >= 114 and (k - 114) % 123 == 0)]
    stats['maxext'] = max(stats['maxext'], len(data))
    if (mode & 0xF000) != 0x4000 and sum(c for n, s, c in data) * zs < size:
        err('%s: size %d beyond its zones' % (path, size))
    return {'mode': mode, 'size': size, 'data': data, 'ib': (zone, sub)}

def read_stream(ino, size):
    out = bytearray()
    for n, s, c in ino['data']:
        for z in range(n, n + c):
            out += zones.get((s, z), b'\0' * zs)
            if len(out) >= size:
                return bytes(out[:size])
    return bytes(out[:size])

def walk(sub, zone, path, parent, depth):
    ino = load_inode(sub, zone, path)
    if not ino:
        return
    t = ino['mode'] & 0xF000
    if t == 0x2000:
        stats['files'] += 1; return
    if t == 0x4000:
        stats['links'] += 1; return
    if t != 0x1000:
        err('%s: unknown type %04x' % (path, ino['mode'])); return
    stats['dirs'] += 1
    if ino['size'] % 512 or ino['size'] < 512:
        err('%s: directory size %d' % (path, ino['size']))
    raw = read_stream(ino, ino['size'])
    for sec in range(0, len(raw), 512):
        off = 0
        while off < 512:
            inum, esub, plen, alen_t = struct.unpack_from('<IBBH', raw, sec + off)
            alen = alen_t & 0xFFF
            if alen % 4 or alen < ((plen + 8 + 3) & ~3) or off + alen > 512 or alen == 0:
                err('%s: bad entry at %d' % (path, sec + off)); break
            name = raw[sec + off + 8: sec + off + 8 + plen]
            if inum and plen:
                if name == b'.':
                    if (inum, esub) != ino['ib']:
                        err('%s: "." does not point to itself' % path)
                elif name == b'..':
                    if (inum, esub) != parent:
                        err('%s: ".." %s, parent %s' % (path, (inum, esub), parent))
                else:
                    cb = block(esub, inum * (zs // 1024))
                    cmode = struct.unpack_from('<H', cb, 0x3B8)[0]
                    if (cmode & 0xF000) != (alen_t & 0xF000):
                        err('%s/%s: dentry type %04x, inode type %04x' % (path, name, alen_t & 0xF000, cmode & 0xF000))
                    if depth > 63:
                        err('%s: too deep' % path)
                    else:
                        walk(esub, inum, path + '/' + name.decode('latin-1'), ino['ib'], depth + 1)
            off += alen
        if off != 512 and not errors:
            err('%s: sector %d not tiled' % (path, sec))

walk(root[1], root[0], '', (root[0], root[1]), 0)
# on-disk bitmaps must equal the computed ones exactly
for s in range(nparts):
    bs0 = (zs // 1024 + (4096 if s == 0 else 0)) * 1024
    nbytes = (nz[s] + 7) // 8
    bm = bytearray()
    pos = bs0
    while len(bm) < nbytes:
        z, off = divmod(pos, zs)
        chunk = zones.get((s, z), b'\0' * zs)[off:]
        bm += chunk
        pos += len(chunk)
    bad = 0
    for z in range(nz[s]):
        on = bm[z // 8] >> (z % 8) & 1
        if on != ref[s][z]:
            bad += 1
            if bad <= 5:
                err('subpart %d zone %d: bitmap %d, referenced %d' % (s, z, on, ref[s][z]))
    if bad > 5:
        err('subpart %d: %d bitmap mismatches in all' % (s, bad))
print('pfscheck: %d files, %d dirs, %d symlinks, %d SEGIs, max %d extents, zones used per subpart %s, inodes in subparts %s: %s' % (
    stats['files'], stats['dirs'], stats['links'], stats['segi'], stats['maxext'],
    [sum(r) for r in ref], sorted(stats['subs_used']), 'CLEAN' if not errors else '%d ERRORS' % len(errors)))
sys.exit(1 if errors else 0)
