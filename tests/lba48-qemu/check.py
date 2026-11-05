#!/usr/bin/env python3
"""Check on the host that every sector the guest wrote (the REGION lines of
its log) holds its own LBA, i.e. landed at the right address of the image.
Usage: check.py <disk image> <guest log>"""
import sys

img, log = sys.argv[1:3]
regions = []
for line in open(log, "rb").read().decode("latin-1").splitlines():
    w = line.strip().split()
    if len(w) == 3 and w[0] == "REGION":
        regions.append((int(w[1]), int(w[2])))
bad = 0
with open(img, "rb") as f:
    for lba, n in regions:
        for s in range(lba, lba + n):
            f.seek(s * 512)
            head = f.read(14).decode("latin-1")
            if head != "LBA %010d" % s:
                if bad < 10:
                    print("FAIL sector %d holds %r" % (s, head))
                bad += 1
print("%s host check: %d regions, %d sectors at the wrong address"
      % ("PASS" if regions and not bad else "FAIL", len(regions), bad))
sys.exit(1 if bad or not regions else 0)
