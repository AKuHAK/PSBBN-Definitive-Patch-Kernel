#!/usr/bin/env python3
"""Write a DOS MBR with the given primary partitions into a disk image.
Usage: mbr.py <image> <type>:<start>:<sectors> ..."""
import struct
import sys

with open(sys.argv[1], "r+b") as f:
    f.seek(0x1BE)
    entries = sys.argv[2:] + [None] * (4 - len(sys.argv[2:]))
    for e in entries:
        if e is None or e == "-":
            f.write(bytes(16))
            continue
        t, start, n = (int(x, 0) for x in e.split(":"))
        f.write(struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", t,
                            b"\xfe\xff\xff", start, n))
    f.write(b"\x55\xaa")
