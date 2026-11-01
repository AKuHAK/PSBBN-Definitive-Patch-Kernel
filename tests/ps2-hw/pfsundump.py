"""pfsundump.py DUMP HEADERS OUTIMG [BITMAPOUT]: sparse disk image from a pfsdump stream.
HEADERS is the output of "apalist /dev/hda dump FILE"; the image has the size
of the PS2 disk it came from (DISK_SECTORS below), for pfsshell/pfsfsck."""
DISK_SECTORS = 78242976
import struct, sys
d = open(sys.argv[1], 'rb')
assert d.read(8) == b'PFSDUMP1'
n, zs = struct.unpack('<II', d.read(8))
parts = [struct.unpack('<II', d.read(8)) for _ in range(n)]
f = open(sys.argv[3], 'wb'); f.truncate(DISK_SECTORS * 512)
h = open(sys.argv[2], 'rb').read()
for i in range(0, len(h), 1028):
    sec = struct.unpack_from('<I', h, i)[0]
    f.seek(sec * 512); f.write(h[i + 4:i + 1028])
used = {}
while True:
    s, z, c = struct.unpack('<III', d.read(12))
    if not c: break
    f.seek(parts[s][0] * 512 + z * zs); f.write(d.read(c * zs))
    used.setdefault(s, []).append((z, c))
f.close()
for s in sorted(used): print("subpart", s, "used", sum(c for z, c in used[s]))
if len(sys.argv) > 4:
    open(sys.argv[4], 'w').write(repr(used))
