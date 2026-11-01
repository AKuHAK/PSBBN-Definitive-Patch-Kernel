# PlayStation 2 PFS: on-disk format and modification rules

The specification `fs/ps2fs` was written against. It is derived from reading the ps2sdk
reverse-engineered reference (`iop/hdd/libpfs`, `iop/hdd/pfs`, `iop/hdd/fsck`,
`iop/hdd/fssk`, `iop/hdd/apa`). Everything below restates formats, rules,
algorithms and orderings in prose, tables and pseudocode. **No reference code is
reproduced.** ps2sdk is AFL-2.0; do not paste its code into GPL sources.

Items marked **UNSURE** say why. Items marked **REF-BUG** are behaviour of the
reference that looks like a defect. A new implementation should not copy them,
but it must tolerate volumes that they produced.

Layout facts were cross-checked against a volume written by `pfsshell`, which
links the same libpfs: the image made by `tests/ps2fs-qemu/mkimage.sh`,
partition `PP.TEST`, 256 MiB, 8 KiB
zones.

---

## 0. Conventions and terminology

| Term | Meaning |
|---|---|
| **sector** | 512 bytes, the ATA unit. All addresses passed to the block device are sectors. |
| **block** (or *metadata block*) | 1024 bytes = 2 sectors. This is the unit of every metadata structure: superblock area, inode, SEGI, bitmap chunk, directory chunk, attribute area, and journal slot. The reference calls it `pfsMetaSize`. Its block-to-sector shift is the constant 1. |
| **zone** | The allocation unit. `zsize` is a power of two from 2 KiB to 128 KiB (fsck and Sony's driver reject anything else). PSBBN and pfsshell volumes normally use 8 KiB. |
| **subpart** `s` | 0 is the main APA partition. 1..`num_subs` are its APA sub-partitions, in the order of the main header's `subs[]` array. |
| **zone number** `z` | Counted from 0 at the first sector of *its own* subpart. A zone is only meaningful together with its subpart. |
| **sector_scale** `ss` | log2(zsize / 512). Sector of zone z is `z << ss`. |
| **inode_scale** `is` | log2(zsize / 1024). Block of zone z is `z << is`. |

All multi-byte integers are **little-endian**: the IOP and the EE Linux kernel
are both little-endian MIPS, but use `le*_to_cpu` anyway. Structures are
packed with natural alignment and contain no implicit padding beyond what the
tables show.

Every sector address is **relative to the start of the (sub)partition**. The
APA layer translates `(subpart s, sector x)` to the absolute LBA
`part[s].start + x`.

---

## 1. Geometry

### 1.1 APA addressing of subparts

* Main partition header (1 KiB at the partition's first sector): `start` (u32)
  is at offset 0x40, `length` in sectors (u32) at 0x44, `nsub` (u32) at 0x4C.
  The `subs[64]` array of `{u32 start, u32 length}` (absolute LBA and sector
  count) is at offset 0x200.
* Subpart table: `part[0] = {main.start, main.length}` and
  `part[i] = main.subs[i-1]` for i = 1..nsub. Each sub-partition also carries
  its own APA header (flag 0x0001 = sub) in its first sector(s).
* The Sony hdd driver refuses PFS transfers to:
  * main-partition sectors `< 0x2000`. The first 4 MiB belong to APA: the
    header, and from sector 8 up to 0x1FFF the partition's "extended
    attribute" area, which PSBBN/HDD-OSD use for icons and attribute files.
    fsck v1.10 checks that this area can be read.
  * sub-partition sectors `< 2`, which hold the sub's APA header.
  * any transfer with `sector + count > part[s].length`.

  A PFS writer must never write into those ranges.
* **Subpart count.** Use `superblock.num_subs`. If APA `nsub` is greater, the
  extra subs have been added but are not yet PFS-formatted. On mount, Sony
  formats them: it writes their bitmap with zone 0 and the bitmap zones marked
  used, sets `num_subs = nsub`, and rewrites both superblocks. If APA `nsub` is
  smaller than `num_subs`, mount fails ("filesystem larger than partition").
  A Linux writer should refuse a read-write mount when the two counts differ,
  unless it implements the same formatting.

### 1.2 Main partition (subpart 0) layout

Let `B0` be the number of bitmap zones of the main partition (§1.6) and `L`
be `superblock.log.count`.

| Zones | Content |
|---|---|
| `0 .. Z4-1`, where `Z4 = 0x2000 >> ss` (4 MiB) | APA header and APA extended-attribute area. Not PFS. |
| `Z4` (sector 0x2000 = byte 4 MiB) | Superblock zone. Primary superblock in sector 8192; backup superblock in sector 8193. The rest of the zone is unused. |
| `Z4+1 .. Z4+B0` | Free-space bitmap of subpart 0 |
| `log.number .. log.number+L-1` (`log.number = Z4 + 1 + B0`) | Metadata journal (§6) |
| `root.number = log.number + L` | Root directory inode zone |
| `root.number + 1` | Root directory's first data zone (only at format time; afterwards it is just an ordinary allocated zone) |
| rest | Allocatable |

The test volume (256 MiB, zsize 8 KiB) has `Z4 = 512`, `B0 = 1`,
`log = {514, sub 0, 16}` and `root = {530, sub 0, 1}`, with the root data zone
at 531.

### 1.3 Sub-partition (subpart s ≥ 1) layout

| Zones | Content |
|---|---|
| 0 | APA sub header area (sectors 0..1), rest unused |
| `1 .. Bs` | Bitmap of this subpart |
| rest | Allocatable |

### 1.4 Superblock (sector 8192 of main; backup copy in sector 8193)

Only the first 40 bytes are defined. The rest of the 512-byte sector is zero.

| Off | Size | Field | Value / meaning |
|---|---|---|---|
| 0x00 | u32 | magic | `0x50465300` (bytes `00 53 46 50`) |
| 0x04 | u32 | version | Format version, `3`. Mount and fsck reject values `> 3`. |
| 0x08 | u32 | modver | `(major<<8)|minor` of the driver that formatted the volume. pfsshell/ps2sdk writes `0x0202`. Informational only. **UNSURE** which values real PSBBN volumes carry; never check it. |
| 0x0C | u32 | pfsFsckStat | Bit 0 = WRITE_ERROR, bit 1 = ERRORS_FIXED (§6.7) |
| 0x10 | u32 | zone_size | zsize in bytes |
| 0x14 | u32 | num_subs | Number of sub-partitions that are PFS-formatted |
| 0x18 | blockinfo | log | Journal location `{number, subpart(=0), count}` (§3.1) |
| 0x20 | blockinfo | root | Root inode `{number, subpart, count=1}` |

Mount-time checks made by Sony's driver:

* The magic must match.
* `version` must be ≤ 3.
* `zone_size` must be a power of two in [2048, 131072].
* If WRITE_ERROR is set *and* the mount flag 0x4 was given, the mount fails
  with EIO.
* The `num_subs` rules in §1.1 apply.

Sony reads **only the primary** copy. fsck falls back to the backup when the
primary cannot be read or has a bad magic, and then copies the backup over the
primary.

Who writes the superblock:

* **Format** writes the backup first, then the primary.
* **Adding subs** at mount writes the primary, then the backup.
* **The fsck-stat helper** rewrites only the primary.

Ordinary file operations never write the superblock. A Linux driver needs no
superblock writes except, optionally, setting WRITE_ERROR (§6.7).

### 1.5 Free-space accounting (zfree / free_zone)

Free counts are **not stored on disk**.

* At mount, for every subpart `s`, Sony counts the zero bits in the bitmap
  over the first `nz(s)` zones. Here `nz(s) = part[s].length >> ss`, and for a
  final partial bitmap chunk only `floor(rem/8)` bytes are counted.
* `free_zone[s]` holds the per-subpart count.
* `zfree` is the sum of all `free_zone[s]`.
* `total_zones` is the sum of `nz(s)`.

All three are kept in memory and updated on every allocation and free. A Linux
driver may recompute them the same way (statfs, allocator heuristics).

### 1.6 Format (for building test fixtures; not needed by a driver)

The steps, in order:

1. Allocate the superblock fields:
   * `log.number = Bmain + Z4 + 1`
   * `log.count = max(1, 0x20000 / zsize)`. The journal is always 128 KiB:
     2 header sectors plus 127 two-sector slots.
   * `root = {log.number + log.count, 0, 1}`
2. Write a reset journal header (§6.1) to sector `log.number << ss`.
3. Write the root directory's first block to `(root.number+1) << is`. It holds
   `"."` and `".."`, both pointing at the root (§4.3).
4. Write the root inode to block `root.number << is`, with these values:

   | Field | Value |
   |---|---|
   | mode | `0x11FF` |
   | uid, gid | 0, 0 |
   | attr | `0xA0` |
   | size | 512 |
   | `number_data`, `number_blocks` | 2, 2 |
   | `number_segdesg` | 1 |
   | `data[1]` | `{root+1, 0, 1}` |
   | times | now |

5. For every subpart, write the bitmap. All bits are 0 (or the optional
   "fragment" fill pattern), except that the first `R(s)` bits are set:
   * main: `R(0) = Z4 + log.count + 3 + Bmain`. That covers 4 MiB, the
     superblock zone, the bitmap, the log, the root inode and the root data
     zone.
   * sub: `R(s) = 1 + Bs`.

   Bitmap sectors start at sector `zsize/512` of a sub, or at
   `0x2000 + zsize/512` on main. `count_sectors(s)` sectors are written.
6. Write the backup superblock, then the primary.

The bitmap-size formula is quirky; fsck uses it to compute the reserved region:

```
zones   = length_in_sectors >> ss
bytes   = zones/8 + (zones mod 8)        # adds the remainder, not 1 (quirk)
sectors = bytes/512 + (bytes mod 512)    # same quirk
B       = ceil(sectors / (zsize/512))    # bitmap zones
```

For real partition sizes (multiples of 128 MiB) the quirk only inflates
`sectors`, and B still comes out as the expected 1 zone in common cases. Never
recompute B in a driver. The reserved bits are already set on disk.

---

## 2. The zone bitmap

### 2.1 Location and addressing

Each subpart has its own bitmap with one bit per zone of that subpart.

* It starts at **zone 1** of a sub-partition.
* On the main partition it starts at **byte 4 MiB + zsize** (zone `Z4+1`).

The bitmap is handled in 1 KiB chunks of 8192 bits:

```
chunk(z)   = z / 8192
word(z)    = (z mod 8192) / 32          # u32 LE index inside the chunk
bit(z)     = z mod 32                   # 1 << bit inside that word
block(s,c) = (1 << is) + c + (s == 0 ? 4096 : 0)   # 1 KiB block in subpart s
sector     = block(s,c) * 2
```

The bit order is therefore **LSB-first within little-endian u32 words**. This
is the same as LSB-first within bytes: zone z is byte `z/8`, bit `z%8` of the
bitmap stream. 1 means used, 0 means free.

### 2.2 Bits that must be set

These bits are set by format and must stay set:

* main: zones `0 .. Z4 + 1 + B0 + log.count - 1` (the APA area, superblock
  zone, bitmap and journal);
* sub: zones `0 .. Bs`;

In addition, every zone referenced by any inode entry
`data[0..number_data-1]` must be set. That includes each inode's own zone and
each SEGI zone (§3.5).

**No other bit may be set.** fsck rebuilds the bitmap from the tree and
compares it with the on-disk bitmap word by word (§9.1). A leaked zone is a
mismatch, and fsck repairs it by clearing the bit.

Bits at or beyond `nz(s)` (past the end of the partition) must stay 0.

**REF-BUG.** The reference search scans whole u32 words up to byte
`floor(rem/8)` of the last chunk. When the zone count is not a multiple of 32
it can therefore hand out zones that do not exist. APA sizes are multiples of
128 MiB, so the zone count is a multiple of 1024 and this never happens in
practice. A writer must still bound every search by `nz(s)`.

### 2.3 Allocation primitives used by the reference

Three primitives. All of them update the in-memory `free_zone[s]` and `zfree`
and dirty the touched bitmap blocks. Bitmap blocks are journaled metadata
(§6).

**(a) extend_contiguous(extent e, want)** grows an existing extent
`{number, subpart, count}` in place:

```
start = e.number + e.count
want  = min(want, 65535 - e.count)        # count is a u16
got   = 0
for z = start, start+1, ... while got < want and z < nz(e.subpart):
    if bit(z) set: stop
    set bit(z); got++
return got        # caller does e.count += got
```

**(b) find_run_and_take(sub, from, runlen, take_max)**:

* Scan zones `from, from+1, ...` of `sub` up to the end of the subpart. There
  is no wrap-around.
* Look for the first run of `runlen` consecutive free zones. Runs may span
  chunk boundaries.
* If one is found, set the first `take = min(runlen, take_max)` zones of the
  run and return `{run_start, sub, take}`.

**(c) search_alloc(hint{number, subpart, count=desired}, maxcount)** is
`pfsBitmapSearchFreeZone`:

```
if hint.subpart > num_subs: hint.subpart = 0
tries  = num_subs + 1 + (hint.number != 0 ? 1 : 0)
runlen = max(min(maxcount, 32), hint.count)
repeat tries times:
    n = runlen
    while n >= 1:
        if free_zone[hint.subpart] >= n and
           find_run_and_take(hint.subpart, hint.number, n, hint.count) succeeds:
            return extent (count may be < desired when n < desired)
        n = n / 2           # integer halving: runlen, runlen/2, ..., 1
    hint.subpart = (hint.subpart + 1) mod (num_subs + 1); hint.number = 0
fail: ENOSPC
```

How the search order works out:

* It first tries the hinted subpart from the hint position, looking for
  progressively shorter runs.
* It then tries every other subpart from zone 0.
* If the hint was non-zero, it finally retries the hinted subpart from zone 0.
* The allocated extent is placed at the **start** of a run of `runlen` free
  zones. `runlen` is larger than the desired count when the file is already
  bigger than the request, which leaves room for later contiguous growth.

The extent count is a u16. The reference stores `desired` into `count`
without clamping (REF-BUG for requests of 65536 zones or more). Clamp it to
65535.

### 2.4 Who allocates what, with which hints

| Purpose | Hint subpart / number | desired | maxcount (runlen) |
|---|---|---|---|
| Inode of a **file or symlink** | Parent dir's inode zone (its subpart and number) | 1 | 2. Looks for 2 free zones, so data can follow the inode. |
| Inode of a **directory** | Subpart chosen as in §2.5, number 0 | 1 | 2 |
| First data zone of a new directory | The new dir inode's zone | 1 | 0, so runlen = 1 |
| New SEGI | The zone after the current last extent | 1 | Current `number_blocks` of the file |
| New data extent (file write, directory growth) | The zone after the last extent (`data[last].number + count`, same subpart). For a file without data, the zone after the inode. | Zones still needed | `number_blocks` |
| Growing the last extent | In place; extend_contiguous | Zones still needed | – |

Exact allocation placement is **not** required for compatibility. Sony and
fsck accept any placement that satisfies §9. Following the policy keeps
fragmentation similar to Sony's.

### 2.5 Directory placement across subparts

The parent inode's u32 field at 0x3EC (`subpart`, §3.3) is a **round-robin
cursor**:

```
c = parent.subpart_cursor
c = (c < num_subs) ? c + 1 : 0       # advance first, then use
parent.subpart_cursor = c            # parent inode is rewritten anyway (times)
sub = c
overall = zfree*100 / total_zones
this    = free_zone[sub]*100 / nz(sub)
if this < overall and overall - this >= 11:
    sub = the subpart with the highest free_zone*100/nz (first strict maximum, default 0)
```

With `num_subs = 0` the result is always subpart 0. A new inode's cursor
starts at its own subpart.

### 2.6 Freeing

To free extent `{n, s, c}`, clear bits `n .. n+c-1` of subpart `s` (they may
span chunks), then add `c` to `free_zone[s]` and to `zfree`.

Cached blocks inside freed zones must be **dropped, not written back**. The
reference marks every cached 1 KiB block in `[n<<is, (n+c)<<is)` of that
subpart clean, so stale dirty metadata (for example a deleted directory's
entry block) is never flushed into a zone that may be reused.

---

## 3. Inodes and segment descriptors

### 3.1 blockinfo (8 bytes)

| Off | Size | Field |
|---|---|---|
| 0 | u32 | number: zone number within subpart |
| 4 | u16 | subpart: 0..num_subs |
| 6 | u16 | count: number of zones, 1..65535 |

### 3.2 datetime (8 bytes)

| Off | Size | Field |
|---|---|---|
| 0 | u8 | unused, 0 |
| 1 | u8 | sec 0..59 |
| 2 | u8 | min |
| 3 | u8 | hour |
| 4 | u8 | day 1..31 |
| 5 | u8 | month 1..12 |
| 6 | u16 | year, full (e.g. 2026) |

Values are plain binary. The IOP converts them from the RTC's BCD. See §3.11
for the time zone.

### 3.3 Inode (SEGD) layout: 1024 bytes, block 0 of the inode's zone

| Off | Size | Field | Notes |
|---|---|---|---|
| 0x000 | u32 | checksum | §3.4 |
| 0x004 | u32 | magic | `0x53454744` "SEGD" |
| 0x008 | blockinfo | inode_block | This inode's own location; `count = 1` |
| 0x010 | blockinfo | next_segment | First SEGI, or all zero |
| 0x018 | blockinfo | last_segment | Last descriptor of the chain: this inode itself, or the last SEGI |
| 0x020 | blockinfo | unused | 0 |
| 0x028 | blockinfo[114] | data[0..113] | `data[0]` = this inode's own zone (equal to inode_block). `data[1..113]` are extents. |
| 0x3B8 | u16 | mode | Type nibble and permission bits (§3.10) |
| 0x3BA | u16 | attr | §3.10 |
| 0x3BC | u16 | uid | 0xFFFF for everything Sony creates (the root made by format has 0) |
| 0x3BE | u16 | gid | Same as uid |
| 0x3C0 | datetime | atime | |
| 0x3C8 | datetime | ctime | |
| 0x3D0 | datetime | mtime | |
| 0x3D8 | u64 | size | Bytes. For directories, a multiple of 512. |
| 0x3E0 | u32 | number_blocks | Sum of `count` over entries `0..number_data-1` of the whole chain, descriptors included |
| 0x3E4 | u32 | number_data | Number of used entry indices in the whole chain, descriptors' self entries included |
| 0x3E8 | u32 | number_segdesg | Number of descriptors: 1 (this inode) plus the number of SEGIs |
| 0x3EC | u32 | subpart | Round-robin cursor for subdirectory placement (§2.5). Initialised to the inode's own subpart. fsck resets it to 0 when it repairs a file. |
| 0x3F0 | u32[4] | reserved | 0 |

The inode's zone also holds the **attribute area** (§5) in block 1 (byte
offset 1024 of the zone). The rest of the zone (blocks 2..) is unused. The
smallest zone (2 KiB) holds exactly the inode and the attribute area.

The inode number on disk is the pair `(subpart, zone)`. Directory entries
store exactly that pair.

### 3.4 Checksum (SEGD and SEGI)

Treat the 1024 bytes as 256 little-endian u32 words. The checksum is the sum
of words **1..255** (inclusive) mod 2^32, stored in word 0. It is recomputed
whenever the block is written.

On read, Sony checks magic (SEGD for inodes reached through a directory entry
or the root pointer, SEGI for blocks reached through `next_segment`) and the
checksum. A mismatch is an I/O error, and it **sets the WRITE_ERROR flag in
the superblock** (§6.7).

### 3.5 SEGI (indirect segment descriptor): 1024 bytes, block 0 of its own zone

| Off | Field | Value |
|---|---|---|
| 0x000 | checksum | Same algorithm as §3.4 |
| 0x004 | magic | `0x53454749` "SEGI" |
| 0x008 | inode_block | The owning SEGD's inode_block |
| 0x010 | next_segment | Next SEGI, or zero |
| 0x018 | last_segment | **Back link**: `data[0]` of the previous descriptor (the SEGD or the previous SEGI) |
| 0x020 | unused | 0 |
| 0x028.. | data[0..122] | 123 entries occupying the rest of the block (offsets 0x028..0x3FF). `data[0]` = this SEGI's own zone `{z, s, 1}`. `data[1..122]` are extents. |

All other bytes are zero when the SEGI is created. Entries 114..122 overlap
where `mode`, `attr` and the rest sit in a SEGD; a SEGI has none of those
fields.

**Global entry index.** Entry indices `i = 0 .. number_data-1` run across the
whole chain:

```
fix(i) = i                      if i < 114
       = (i - 114) mod 123      otherwise
descriptor(i) = 0 (the SEGD)              if i < 114
              = 1 + (i - 114) div 123     (k-th SEGI, 1-based)
```

* Index 0 is the SEGD's self entry, and indices 1..113 are its extents.
* Index 114 is SEGI#1's self entry, 115..236 its extents, 237 is SEGI#2's
  self entry, and so on.
* Every index with `fix(i) = 0` is a descriptor's self entry, of one zone. It
  is counted in `number_data` and `number_blocks`, but it holds no file data.

The chain is walked forward through `next_segment`, starting at the SEGD.

Invariants used by Sony's driver (fsck checks only some of them):

* `number_blocks = Σ data[i].count` over `i < number_data`.
* `number_segdesg = #{ i < number_data : fix(i) = 0 }`.
* `last_segment` = `data[0]` of `descriptor(number_data - 1)`.
* Data zones = `number_blocks - number_segdesg` ≥ `ceil(size / zsize)`.
  Files have no holes, and the zones may exceed the size (preallocation).
* Every used entry has `count ≥ 1`, `number ≥ 2`, `subpart ≤ num_subs`, and
  `(number + count) << ss ≤ part[subpart].length`.
* Entries at indices `≥ number_data` and `next_segment` beyond the last used
  descriptor are **ignored**. Sony leaves stale values there after a truncate.
* Avoid leaving `number_data - 1` pointing at a SEGI self entry, i.e. a SEGI
  with no extents. The reference can end up like that after a failed
  allocation, but its read path rejects it.

### 3.6 Mapping a byte offset to disk

```
zi  = off div zsize ; within = off mod zsize
d   = SEGD
for i = 1 .. number_data-1:
    if fix(i) == 0: d = load(d.next_segment, expect SEGI); continue
    e = d.data[fix(i)]
    if zi < e.count:
        subpart = e.subpart
        sector  = ((e.number + zi) << ss) + (within >> 9)
        return
    zi -= e.count
error: offset beyond allocation
```

Directories are read through the same mapping, one 1 KiB block at a time.
The dentry block at byte position `p` is block `((e.number + zi) << is) +
(within >> 10)`.

### 3.7 Growing a file (write at or past the allocated end)

Sony only grows a file by writing at EOF. `lseek` past `size` is rejected, so
**there are no sparse files**. When a Linux driver extends a file by truncate
or by writing past EOF, it must allocate every zone in between and zero the
bytes between the old EOF and the new data. Zones beyond `size` that already
exist (from preallocation) hold garbage.

Reference algorithm, given `need` = zones still missing for the bytes about to
be written:

```
grow(I, need):
    L = descriptor(number_data - 1)                  # = load(I.last_segment)
    if fix(number_data - 1) != 0:                    # last entry is a real extent
        got = extend_contiguous(L.data[fix(number_data-1)], need)
        L.data[...].count += got; I.number_blocks += got; need -= got
    while need > 0:
        if fix(number_data) == 0:                    # current descriptor full
            last = L.data[fix(number_data-1)]
            z = search_alloc({last.number+last.count, last.subpart, 1},
                             I.number_blocks)
            S = new zeroed 1 KiB block at z<<is: magic SEGI,
                inode_block = I.inode_block, last_segment = L.data[0],
                data[0] = {z.number, z.subpart, 1}
            L.next_segment   = S.data[0]
            I.last_segment   = S.data[0]
            I.number_data   += 1 ; I.number_blocks += 1 ; I.number_segdesg += 1
            L = S
        prev = L.data[fix(number_data-1)]            # last extent, or the self entry
        x = search_alloc({prev.number+prev.count, prev.subpart, need},
                         I.number_blocks)
        L.data[fix(number_data)] = x
        I.number_data += 1 ; I.number_blocks += x.count ; need -= x.count
        if need > 0:
            got = extend_contiguous(x, need) ; x.count += got
            I.number_blocks += got ; need -= got
```

How the reference sizes each step during `write(len)`:

* If the current position lies in the last extent and the remaining space in
  that extent is less than `len`, it first tries
  `extend_contiguous(ceil((len - remain) / zsize))`.
* Whenever it reaches the end of the last extent with bytes still to write, it
  allocates a new segment for `ceil(remaining_len / zsize)` zones.
* A brand-new file has `number_data = 1`. The first write allocates `data[1]`
  with the hint "zone after the inode".

After each chunk of data is written, if the position has passed `size`, set
`size = position` and mark the inode dirty. Data blocks are **not journaled**.

Blocks dirtied: the bitmap chunks, the SEGD (and the SEGI holding the last
extent, if any), the previous descriptor (its `next_segment`), and any new
SEGI.

Sony also offers explicit preallocation (`ioctl2 PIOCALLOC n`). It always
allocates `ceil(n / zsize)` more zones and leaves `size` unchanged.

### 3.8 Truncation and freeing excess zones (`pfsFreeZones`)

Sony uses this algorithm in two places:

* For `open(O_TRUNC)`, after setting `size = 0`.
* For `ioctl2 PIOCFREE`, which trims preallocated zones to `size`.

There is no other truncate. Sony's chstat with the size flag returns EACCES.
A Linux truncate should set `size` and then apply the same algorithm.

```
K = ceil(size / zsize)                       # data zones to keep
if I.number_blocks == I.number_segdesg + K: return
keep = K ; descs = 1 ; d = SEGD ; tail = none
i = 1
while i < number_data and keep > 0:
    if fix(i) == 0: d = next(d); descs += 1
    else:
        e = d.data[fix(i)]
        if keep < e.count:
            tail = {e.number + keep, e.subpart, e.count - keep}
            e.count = keep ; keep = 0 ; mark d dirty
        else: keep -= e.count
    i += 1
new_nd = i
I.number_data    = new_nd
I.number_segdesg = descs
I.number_blocks  = K + descs
I.last_segment   = d.data[0]                 # descriptor holding index new_nd-1
mark I dirty
if tail: free(tail)
for j = new_nd .. old_number_data-1:         # free everything past the cut
    if fix(j) == 0: d = next(d)              # entering a SEGI: its self entry
    free(d.data[fix(j)])                     # frees extents and SEGI zones alike
    drop cached blocks inside the freed range
```

Notes:

* `K = 0` keeps only index 0, the inode itself.
* The reference does not clear `next_segment` of the new last descriptor, nor
  the stale entries past `new_nd`. Clearing them is compatible and cleaner.
* The O_TRUNC path also clears `attr` bit CLOSED (0x80) and **re-initialises
  the attribute area** (§5) before freeing.

Blocks dirtied: the SEGD, the descriptor holding the cut extent, and the
bitmap chunks.

### 3.9 Freeing a whole inode (unlink, rmdir, rename-over)

Free every entry `data[fix(i)]` for `i = 0 .. number_data-1`, following the
chain. That frees the inode zone, every SEGI zone and every extent. Drop all
cached blocks inside those zones. The inode block itself is **not** rewritten
on disk: a stale SEGD stays in the freed zone, and nothing marks it deleted.

**REF-BUG (confirmed in ps2sdk; UNSURE whether Sony's binary has it).**
When ps2sdk's `pfsBitmapFreeInodeBlocks` crosses into a SEGI, it frees the
previous descriptor's `data[0]` a second time instead of the SEGI's. It then
stops, because its loop bound is re-read from the SEGI, whose `number_data`
field is 0. All SEGIs and the extents they hold therefore **leak**, and fsck
later repairs the leak as a bitmap mismatch. Implement the correct walk.
Reproduced with pfsshell on a file written by Linux ps2fs; the fix for
ps2sdk (and pfsshell) is `tests/ps2fs-qemu/ps2sdk-pfs-free-segi.patch`.

### 3.10 mode and attr

**mode** (u16). The type nibble uses **iomanX values, not Linux values**:

| PFS type (mode & 0xF000) | Meaning | Linux type |
|---|---|---|
| 0x1000 | directory | S_IFDIR 0040000 |
| 0x2000 | regular file | S_IFREG 0100000 |
| 0x4000 | symbolic link | S_IFLNK 0120000 |

The low 12 bits are the permissions as given by the creator:

* `rwx` = 0x1C0/0x038/0x007, plus 0x800/0x400/0x200 setuid/setgid/sticky.
* `open(O_CREAT)` stores `0x2000 | (mode & 0xFFF)`. pfsshell passes 0666, so
  files on the test volume have mode `0x21B6`.
* mkdir stores `0x1000 | (mode & 0xFFF)`. The test volume shows `0x11FF`.
* symlink stores `0x41FF`.

Sony essentially ignores the permission bits. Reading and writing are always
allowed except on a read-only mount. Only "execute" on a non-directory is
denied, when it has no x bit.

**attr** (u16):

| Bit | Name | Rule |
|---|---|---|
| 0x0001 | READABLE | User-settable via chstat |
| 0x0002 | WRITEABLE | User-settable |
| 0x0004 | EXECUTABLE | User-settable |
| 0x0008 | COPYPROTECT | User-settable |
| 0x0010 | – | User-settable |
| 0x0020 | SUBDIR | Set on directories at creation. chstat cannot change it. |
| 0x0040 | – | User-settable |
| 0x0080 | CLOSED | Directories: always set. Files: set when a write-mode descriptor is closed (§8), cleared (and flushed) when an existing file is opened for writing, cleared by O_TRUNC. chstat cannot change it. |
| 0x0100–0x0400 | – | User-settable |
| 0x0800 | PDA | User-settable |
| 0x1000 | PSX | User-settable |
| 0x2000 | – | User-settable |
| 0x4000 | HIDDEN | User-settable |

* A new file starts with attr 0. A new directory starts with `0xA0`. A new
  symlink starts with 0 and is never "closed".
* chstat rule: `attr = (old & 0x00A0) | (new & 0xFF5F)`.
* **UNSURE** whether PSBBN software treats files without CLOSED as incomplete.
  Mirror Sony's behaviour: clear the bit, persistently, on the first open for
  write; set it on the last close.

**uid/gid**: Sony stores 0xFFFF/0xFFFF on every file, directory and symlink
it creates. chstat cannot change them in v2.2. The ps2sdk header claims some
software ignores entries whose uid/gid are not 0xFFFF (**UNSURE**, unverified),
so a writer should use 0xFFFF. There are no link counts on disk.

### 3.11 Timestamps

**Source.** The IOP's CDVD real-time clock. By PS2 convention this holds
**Japan Standard Time (UTC+9)**, and the OSD applies the user's time-zone
setting on top. A Linux writer should therefore store `UTC + 9h` by default,
and allow a mount option to override it. The existing driver's `tzoffset` is
in minutes; Sony's semantics correspond to `tzoffset=540`.

**Evidence and caveats.**

* The test volume, written by pfsshell at 10:51 UTC, stores 01:51, which is
  `UTC − 9h`. pfsshell's host port subtracts 9 hours. It labels this
  "Convert to JST", but that is a sign error. Do not copy pfsshell.
* **UNSURE**: the JST convention is well established for the PS2 RTC, but it
  was not verified here against a real-console volume.

Which operations set which times:

| Operation | Effect |
|---|---|
| Inode creation (file, dir, symlink) | atime = ctime = mtime = now |
| Directory entry added or removed (create, mkdir, symlink, unlink, rmdir, rename) | Parent dir: mtime = now, ctime = atime = mtime |
| Rename across directories | Both parents set as above. A moved **directory** also gets atime = ctime = mtime = now, because its ".." changes. A moved file's own times do not change. |
| Close of a descriptor opened for writing | File: atime = ctime = mtime = now, **even if nothing was written**. Done before setting CLOSED. |
| O_TRUNC, write, read | No time change. Atime is never updated on read; mtime only at close. |
| chstat | Sets exactly the requested atime, ctime or mtime fields |

---

## 4. Directories

### 4.1 Directory entry (dentry)

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | u32 | inode | Zone number of the target inode. 0 means an empty slot. |
| 4 | u8 | sub | Subpart of the target inode |
| 5 | u8 | pLen | Name length, 0..255. 0 means an empty slot. |
| 6 | u16 | aLen | **Low 12 bits:** allocated length in bytes. **High 4 bits:** the target's type nibble (`mode & 0xF000`: 0x1000 dir, 0x2000 file, 0x4000 symlink). |
| 8 | pLen bytes | name | Raw bytes, **not** NUL-terminated. |

The minimal length of an entry is `used(pLen) = (pLen + 8 + 3) & ~3`: 12 for
names of 1–4 bytes, and 264 for 255 bytes. Every entry must satisfy:

* `(aLen & 0xFFF) mod 4 = 0`
* `(aLen & 0xFFF) ≥ used(pLen)`
* the entry does not cross a 512-byte boundary

The type nibble **must be correct**. fsck decides whether to recurse into a
directory, and whether to check a file's attribute block, from the
**dentry's** type bits (§9). A directory entry without 0x1000 would make fsck
skip the subtree. fsck would then see the subtree's zones as unreferenced
and free them.

### 4.2 Sector tiling

The directory's byte stream (§3.6) is a sequence of 512-byte sectors, `size /
512` of them. Only the first `size` bytes are meaningful.

* In each sector, entries start at offset 0 and the `aLen` values sum to
  exactly 512.
* The first entry of a sector can be an **empty placeholder**: `inode = 0`,
  `pLen = 0`, and its `aLen` and type bits keep their old values.
* Any other free space is **slack**: the bytes between `used(pLen)` and `aLen`
  inside a live entry.

The reference does not zero name padding or slack, and leaves stale bytes in
them. Writers **should zero-pad** every name up to `used(pLen)`. Two
reference checks compare names with C `strcmp` (§4.8), and fsck's own
initialisation writes NULs, so zero padding is both compatible and safer.

### 4.3 A new directory

* The inode has `size = 512`, `number_data = 2`, `number_blocks = 2`,
  `number_segdesg = 1`, and `data[1] = {data zone, sub, 1}`.
* The first 1 KiB block of the data zone is zero-filled. It is the only block
  written, so the rest of the zone keeps whatever it held.
* Its first sector then gets:
  * offset 0: `"."`, with `inode/sub` = self, `pLen = 1`, `aLen = 0x100C`
    (12 | dir), and bytes `'.' 0 0 0`.
  * offset 12: `".."`, with `inode/sub` = the parent's `inode_block`,
    `pLen = 2`, `aLen = 0x11F4` (500 | dir), and bytes `'.' '.' 0 0`.
* Sector 1 of that block is all zeros, but it lies outside `size`.
* The root's `".."` points to the root itself.

`"."` and `".."` are ordinary entries for lookup purposes. Sony
short-circuits `".."` in the root to the root.

### 4.4 Lookup

Lookup is a linear scan over the whole `size`, in on-disk order. The first
entry with `pLen = len(name)` and byte-equal name wins. The match is
**case-sensitive**, uses raw bytes with no character-set conversion, and does
not test `inode != 0`.

### 4.5 Inserting an entry (`pfsDirAddEntry`)

```
need = used(len(name))
for each entry e in order (offset < size):
    a = e.aLen & 0xFFF ; u = used(e.pLen)
    if e.inode == 0 and a >= need:            # empty placeholder, reuse whole
        target = e ; tlen = a ; break
    if a - u >= need:                         # split slack off a live entry
        e.aLen = (e.aLen & 0xF000) | u        # the existing entry keeps its type
        target = e + u ; tlen = a - u ; break
if none found:
    ensure capacity: if (number_blocks - number_segdesg)*zsize < size + 512:
        grow(dir, 1 zone)                     # §3.7 with need = 1
    target = byte position `size` (start of a fresh sector); tlen = 512
    size += 512
write target: inode = child.number, sub = child.subpart, pLen = len,
              aLen = tlen | (child.mode & 0xF000), name bytes (+ zero pad)
parent times = now (atime = ctime = mtime); mark parent inode and dentry block dirty
```

Notes:

* The predicate uses "inode == 0" for the reuse case but tests `pLen` to
  decide between reuse and split. A placeholder created by Sony has both at
  zero, so the two cases agree.
* `"."` (aLen 12) has no slack. In a fresh directory, the first insert splits
  `".."` and lands at offset 24, with `aLen = 488`.
* Placement is first-fit over the whole directory.
* A newly allocated directory zone is not zeroed. Only the 1 KiB block that
  holds the new sector is read, modified and written.

### 4.6 Removing an entry (`pfsDirRemoveEntry`)

```
find e by name (§4.4); off = byte offset of e within the directory
s0 = start of e's 512-byte sector; walk entries from s0 to find e's predecessor p in that sector
if p exists:
    p.aLen = (p.aLen & 0xF000) | ((p.aLen & 0xFFF) + (e.aLen & 0xFFF))   # merge into predecessor
else:                                   # e is the first entry of its sector
    e.inode = 0 ; e.pLen = 0            # placeholder; aLen and type bits untouched
    if off + (e.aLen & 0xFFF) >= size:  # the sector held only e and it is the last sector
        size -= (e.aLen & 0xFFF)        # i.e. size -= 512
parent times = now; dentry block and parent inode dirty
```

* A directory shrinks by at most one sector per removal, and only when its
  last sector becomes completely empty. It does not cascade to earlier empty
  sectors.
* Directory zones are **never freed** while the directory exists. A later
  insert that needs a new sector reuses them through the capacity check.

### 4.7 Size field and capacity

* `size` is in bytes, a multiple of 512, and at least 512.
* Capacity in bytes is `(number_blocks − number_segdesg) * zsize`.
* Directories grow one zone at a time. Sony tries to extend the last extent
  contiguously first, then falls back to a new extent placed after the last
  one (on the test volume, the 300-entry directory `many` ended up with
  extents at zones 1338, 1564 and 1789).

### 4.8 rmdir emptiness check, and other rmdir/unlink rules

* **Empty** means no entry within `size` has `pLen ≠ 0` and a name other than
  `"."` and `".."`. The reference compares names with C `strcmp` against the
  unterminated name bytes. A `"."` or `".."` entry that is not followed by a
  NUL byte would count as a file. Always zero-pad.
* rmdir also refuses the mount root and the current directory with EBUSY. The
  reference compares only the low 16 bits of the zone number (REF-BUG). It
  also refuses inodes that are currently open (EBUSY).
* unlink refuses directories (EISDIR). rmdir refuses non-directories
  (ENOTDIR).
* rmdir's intended "path ends with '.'" check never triggers (REF-BUG).

### 4.9 Rename (`pfsFioRename`)

Preconditions:

* Old and new names must not be `"."` or `".."` (EINVAL).
* The old entry must exist.
* If the target name exists:
  * Moving a directory: the target must be a directory (else ENOTDIR) and
    empty (else ENOTEMPTY).
  * Moving a file: the target must not be a directory (else EISDIR).
  * The target must not be open (EBUSY).
  * If the target is the same inode, rename is a no-op.
* When a directory moves to a different parent, walk `".."` upward from the
  new parent to the root. If the moved directory is met on the way, fail with
  EINVAL (a directory cannot move into its own subtree).
* The old and new parents must not be in use elsewhere (EBUSY). That is a
  cache-reference check in Sony's driver.

Steps, in the reference order:

1. If the target exists: remove its entry from the new parent (§4.6).
2. Remove the old entry from the old parent (§4.6).
3. Insert `(new name → old inode's inode_block, type = old inode's mode)` into
   the new parent (§4.5). Within the same directory this can reuse the space
   freed in step 2.
4. If a directory moved to a different parent: rewrite `inode` and `sub` of
   its `".."` entry, which is at byte 12 of its first data block.
5. Times (§3.11):
   * Old parent, if it differs from the new one: atime = ctime = mtime = now.
   * New parent: the same.
   * A moved directory: the same.
6. If a target was replaced: free its whole inode (§3.9).
7. Commit everything as one transaction (§6).

On error, Sony **discards** the dirty cached blocks: it invalidates them
instead of writing them. That is its rollback.

### 4.10 Names and paths

* A name component is 1..255 bytes. Any byte is allowed except `'/'` and
  NUL.
* Sony silently truncates a component longer than 255 bytes. A writer should
  return ENAMETOOLONG instead.
* A full path must be shorter than 1024 bytes.
* Leading, repeated and trailing slashes are collapsed.
* Names are case-sensitive raw bytes, and PFS enforces no encoding.
  **UNSURE**: which encoding PSBBN uses for non-ASCII names. The existing
  Linux driver assumes UTF-8 on disk and presents EUC-JP to user space.
* Creating `"."` or `".."` fails with EEXIST, because those entries exist.

### 4.11 readdir order

On-disk order. Entries with `pLen = 0` are skipped. `"."` and `".."` are
returned as stored.

---

## 5. Attribute area (aentry)

**Location.** Block 1 of the inode's zone: the 1 KiB block right after the
inode, at block `(z << is) + 1`.

**Entry format.** The entries tile the 1024 bytes:

| Off | Size | Field |
|---|---|---|
| 0 | u8 | kLen: key length. 0 means an empty slot. |
| 1 | u8 | vLen: value length |
| 2 | u16 | aLen: allocated length. A multiple of 4, and at least `(kLen + vLen + 7) & ~3`. |
| 4 | … | key bytes immediately followed by value bytes, not NUL-terminated |

**Initial state.**

* Regular files and symlinks: Sony writes the block at creation as all zeros
  except `aLen = 1024` at offset 2. It does the same on `open(O_TRUNC)`, so
  O_TRUNC wipes the attributes.
* Directories: Sony never initialises or touches the block. It may hold zeros
  or stale data.

**Operations.** These are ioctl2 calls on a file opened for writing:

* PIOCATTRADD
  * The key and value must be non-empty and each shorter than 256 bytes.
  * If the key already exists, the call fails with EEXIST.
  * Otherwise it is a first-fit insert, using the same reuse-or-split rule as
    §4.5. Reuse a slot with `kLen = 0` and enough room; otherwise split the
    slack of a live entry.
* PIOCATTRDEL merges the entry into its predecessor. If it is the first entry,
  the call sets `kLen = vLen = 0` instead.
* LOOKUP and READ do not modify anything.

All of these dirty the block, and the block is journaled like other metadata.

**fsck.** For every dentry whose type is "regular file", fsck checks only that
block 1 **can be read**. If it cannot, fsck reinitialises it (zero-fill and
`aLen = 1024`). It never parses the contents.

**Rules for a writer.**

* Initialise the block for new regular files and symlinks.
* Never put file data in it, and preserve it otherwise.
* Wiping it on truncate-to-zero is optional (Sony does it on O_TRUNC).
* Initialising it for new directories is harmless but not required.

Do not confuse this with the APA **partition** extended-attribute area
(sectors 8..0x1FFF of the main partition). That area is outside PFS and must
never be written.

---

## 6. Journal ("PFSL") and error flags

### 6.1 Layout

The journal lives on the main partition. Its first sector is
`J = log.number << ss`, and it spans `log.count` zones (128 KiB).

**Header**: 1024 bytes in sectors `J, J+1`:

| Off | Size | Field |
|---|---|---|
| 0 | u32 | magic `0x5046534C` "PFSL" |
| 4 | u16 | num: number of valid log entries, 0..127 |
| 6 | u16 | checksum |
| 8 | 127 × 8 | entries: u32 `sector`, the home sector within the subpart (always even: a 1 KiB block × 2); u16 `sub`, the home subpart; u16 `logSector`, the sector offset from J where the 2-sector image is stored |

**Images**: 2 sectors (one 1 KiB block) each, at `J + logSector`. Sony uses
`logSector = 2 + 2*k` for cache slot k, so images occupy `J+2 .. J+255`.

**Checksum**: the sum of u32 words **2..255** of the header (all 127 entries,
used or not), truncated to 16 bits. The word holding magic and the word
holding num/checksum are excluded.

**Reset state**: magic, then zeros everywhere (`num = 0`, `checksum = 0`).
That is self-consistent, because all-zero entries sum to 0.

### 6.2 What gets journaled

Every dirty metadata block is journaled:

* inodes (SEGD) and SEGIs (with their checksum recomputed at this point);
* bitmap chunks;
* directory blocks;
* attribute blocks.

**File data is never journaled.** Sony writes data straight to its zones,
before the metadata commit that follows the write call.

### 6.3 Commit protocol (`pfsCacheFlushAllDirty`)

1. If any metadata block of this mount is dirty:
   1. For each dirty block, append an entry `{sector = block*2, sub,
      logSector}` to the in-memory header. `num` = the number of dirty
      blocks.
   2. Write the images into the journal image area. Sony writes its whole
      cache array, clean slots included, as one contiguous transfer to
      `J+2`, `2 × nbuf` sectors. Only the dirty slots are referenced by
      entries, so a writer may store only the dirty images, at any
      `logSector`.
   3. Flush the ATA cache, then write the header with its checksum
      (2 sectors), then flush the ATA cache again.
   4. Write every dirty block to its home location.
2. Always (even if nothing was dirty): flush the ATA cache, write the **reset**
   header, and flush the ATA cache again.

A torn header write fails the checksum and is ignored at replay. Home writes
start only after the header is durable. So any crash leaves either the old
state or a state that replay completes. The "flush ATA cache" steps are the
hdd driver's FLUSH CACHE ioctl.

### 6.4 Replay at mount (Sony's driver and fsck alike)

1. Read the header.
2. If the read failed, or the magic or checksum is wrong, write a reset
   header and continue as if the journal were empty.
3. If `num = 0`, write a reset header.
4. Otherwise, for `k = 0 .. num-1`, copy the 2 sectors at `J + logSector[k]`
   to `(sub[k], sector[k])`. Then write a reset header.

### 6.5 When Sony commits

Sony's transactions are **not** strictly per operation:

* **Always:** after remove/rmdir, after rename, on `sync`, on unmount (cache
  close), and when the cache has to evict a dirty buffer. Eviction commits
  everything dirty, possibly in the middle of an operation. The cache holds 8
  to 127 one-KiB buffers; 127 is the maximum because of the 127 journal
  slots.
* **Only when mount flag 0x2 is set** (named `PFS_FIO_ATTR_WRITEABLE` in
  ps2sdk; it behaves as a write-through/sync flag, official name
  **UNSURE**):
  * after every `write()`;
  * at close of a written file;
  * after chstat;
  * after attribute ioctls;
  * when an existing file is opened for writing and its CLOSED bit is
    cleared.

  With this flag, partial-sector data writes also go straight to disk instead
  of the per-descriptor 512-byte buffer.
* **Never on their own:** create and mkdir leave their changes dirty in the
  cache until one of the events above. pfsshell mounts with flag 0.

### 6.6 Requirements for the Linux writer

1. **Replay before writing.** At a read-write mount, read the journal header.
   If it is valid with `num > 0`, perform the replay (§6.4) and reset the
   header before doing anything else.
   * Not replaying is dangerous. Sony would later replay *old* images over
     newer metadata written by Linux.
   * For a read-only mount, either replay in memory or at least log a
     warning, because the metadata may be pre-crash.
2. **Leave the journal clean.** Whenever no commit is in progress, the header
   must be in the reset state: valid magic, `num = 0`, entries and checksum
   0. Sony and fsck treat any valid header with `num > 0` as work to replay.
3. **Recommended commit protocol**, the same as Sony's. It gives crash safety
   and full compatibility. Per metadata transaction (one VFS operation, or a
   batch):
   1. Collect the dirty 1 KiB metadata blocks. At most 127 fit, because
      `logSector + 2` must stay ≤ `log.count << ss`.
   2. Write their images to `J + 2 + 2k`.
   3. Barrier, then write the header with `num` and the checksum.
   4. Barrier, then write the blocks home.
   5. Barrier, then write the reset header.

   If an operation dirties more than 127 blocks, split it into several
   transactions in an order that is safe on its own. A safe order is "write
   new metadata, then link it, then unlink/free"; Sony splits arbitrarily.
4. **Write data before metadata.** Write file data zones before committing
   the metadata that makes them reachable (ordered mode).
5. **Barriers on Linux 2.4.** Linux 2.4 has no generic barrier.
   * The minimum is synchronous `ll_rw_block` + `wait_on_buffer` in order.
   * With the drive's write cache enabled, ordering also needs ATA FLUSH
     CACHE, the equivalent of HIOCFLUSH. **UNSURE** how best to issue that
     from the PS2 IDE driver: an `ide_do_drive_cmd` with WIN_FLUSH_CACHE is
     the likely route.
6. **Never write freed metadata.** Do not write back cached metadata whose
   zone was freed in the same transaction (§2.6).

### 6.7 Superblock error flags (pfsFsckStat at offset 0x0C)

**Bit 0, WRITE_ERROR.**

* Sony sets it in the **primary** superblock only, with a read-modify-write
  of sector 8192 followed by a flush, on **any** metadata I/O failure:
  * a read or write error;
  * a SEGD/SEGI magic or checksum mismatch on read.
* It also calls the hdd driver's "set partition error". That records the
  partition's start LBA in APA sector 7 of the disk.
* After that the mount latches `lastError` and performs no more metadata I/O.
* A mount with flag 0x4 refuses volumes that have the bit set.
* fsck clears the bit, and the APA part-error record, when it finishes
  without unresolved errors.

**Bit 1, ERRORS_FIXED.**

* fsck sets it after fixing anything.
* devctl `PDIOC_GETFSCKSTAT` reads it; `PDIOC_CLRFSCKSTAT` clears it.

The backup superblock is never updated by these.

**Linux writer:**

* Refuse a read-write mount when bit 0 is set. Recommended; it mirrors
  flag 0x4.
* Never clear either bit.
* Optionally set bit 0 on metadata I/O errors, so PSBBN prompts for a check.

---

## 7. Symlinks, hard links, other types

**Symlink.** Created like a file:

* inode allocated near the parent, `mode = 0x41FF`, `attr = 0`;
* uid/gid 0xFFFF;
* `size = 0`, `number_data = number_blocks = 1`, no data zones;
* attribute area initialised;
* dentry type `0x4000`.

The **target path is stored inside the inode**: a NUL-terminated string
starting at offset 0x030 (`data[1]`), overlaying the unused `data[]` area.
Readlink returns `strlen` of that string. Because `number_data = 1`, those
bytes are never interpreted as extents, and the inode checksum covers them.

Sony does not bound the length (**UNSURE** of its official maximum). The
string must end before `mode` at 0x3B8, so a writer must limit the target to
**at most 903 bytes plus the NUL**.

Path resolution follows symlinks with a depth limit of 4 (ELOOP beyond).
chstat ignores mode changes on symlinks.

**Hard links.** There is no API, no link count, and the design assumes one
dentry per inode. Freeing on unlink is unconditional. Never create two
dentries for one inode: fsck would report the shared zones as "overlapped"
and truncate the second file.

**Device, FIFO and socket nodes.** Not supported. Only the three types in
§3.10 exist.

---

## 8. Per-operation step lists (reference order)

"Dirty X" means the 1 KiB block X joins the next journal transaction.

### 8.1 Create a regular file (`open(O_CREAT)` of a missing name)

1. Resolve the parent. The name must not exist (else EEXIST when O_EXCL or
   when called from mkdir/symlink). Check write access on the parent.
2. Allocate the inode zone: `search_alloc({parent.inode_block.number,
   parent.subpart, 1}, maxcount 2)`. Dirty the bitmap chunk.
3. Build the inode in block `z << is`, zero-filled first:
   * magic SEGD;
   * `inode_block = last_segment = data[0] = {z, s, 1}`;
   * `mode = 0x2000 | perm`, `uid = gid = 0xFFFF`, `attr = 0`, `size = 0`;
   * `number_data = number_blocks = number_segdesg = 1`;
   * `subpart = s`;
   * `atime = ctime = mtime = now`.

   Dirty it.
4. Attribute block `(z << is) + 1`: zero-fill, `aLen = 1024`. Dirty it.
5. Insert the dentry into the parent (§4.5). This may grow the parent
   (bitmap, parent inode/SEGI) and dirties the dentry block.
6. Parent times = now. Dirty the parent inode.
7. With Sony there is no commit here (§6.5). Linux: commit (§6.6).

### 8.2 Open an existing file for writing (Sony-specific state)

If `attr` has CLOSED: clear it, dirty the inode, and commit when flag 0x2 is
set.

### 8.3 Write / extend

1. Position at the offset (§3.6). It must be ≤ size.
2. If more zones are needed: grow (§3.7). This dirties the bitmap chunks, the
   SEGD, the descriptor holding the last extent, and any new SEGI with the
   previous descriptor's `next_segment`.
3. Write the data sectors directly.
4. If `position > size`: update `size` and dirty the SEGD.
5. Commit when flag 0x2 is set. Linux: commit after the data writes.

### 8.4 Close of a file opened for writing

1. Flush the buffered partial sector.
2. Set `atime = ctime = mtime = now` and `attr |= CLOSED`. Dirty the inode.
3. Commit when flag 0x2 is set.

### 8.5 Truncate

**Sony O_TRUNC:**

1. Re-initialise the attribute block.
2. Set `size = 0` and clear CLOSED.
3. Apply §3.8 with K = 0. This dirties the inode, the bitmap and possibly
   descriptors.
4. No time change until close.

**Linux truncate to n bytes:**

* Shrinking (`n < size`): set `size = n`, then apply §3.8 with
  `K = ceil(n/zsize)`. Optionally set mtime/ctime, which Sony would only do at
  close.
* Growing (`n > size`): allocate (§3.7) and zero-fill `[size, n)`, then set
  `size = n`.

### 8.6 Unlink

1. Resolve the parent and target. The target must not be a directory, and
   must not be open.
2. Remove the dentry (§4.6). Dirty the dentry block.
3. Parent times = now. Dirty the parent inode.
4. Discard pending changes to the target inode. Free the target's entire
   chain (§3.9). This dirties bitmap chunks and drops cached blocks in the
   freed zones. The inode block itself is not rewritten.
5. Commit (Sony always commits here).

### 8.7 mkdir

1. Resolve the parent. The name must not exist (EEXIST). Check write access.
2. Choose the subpart (§2.5). This updates the cursor in the parent inode.
   Allocate the inode zone with `search_alloc({0, sub, 1}, 2)`. Allocate the
   data zone with `search_alloc({inode.z, inode.s, 1}, 0)`. If the second
   allocation fails, free the first. Dirty the bitmap.
3. Build the inode as in 8.1, except:
   * `mode = 0x1000 | perm`, `attr = 0xA0`;
   * `size = 512`, `number_data = number_blocks = 2`;
   * `data[1] = {datazone, sub, 1}`.

   Dirty it.
4. Data block `(datazone << is)`: zero the 1 KiB, then write `"."` and `".."`
   (§4.3). Dirty it. The attribute block is not touched.
5. Insert the dentry into the parent with type 0x1000 (§4.5). Parent
   times = now. Dirty the parent.
6. Sony does not commit. Linux: commit.

### 8.8 rmdir

1. The target must be a directory, empty (§4.8), not the root, not the
   current directory, and not open.
2. Then follow unlink steps 2–5. The directory's data zones are freed along
   with it.

### 8.9 Rename (file or directory)

Follow §4.9. Dirtied blocks:

* the old and new parents' dentry blocks and inodes;
* the bitmap and parent descriptors, if the new parent grew;
* the moved directory's first data block and inode, if it changed parent;
* the bitmap, if a target was replaced.

Commit once.

### 8.10 chstat / chmod / utime

1. Load the inode. It must be writable (EROFS on a read-only mount).
2. Apply the changes:
   * MODE: `mode = (mode & 0xF000) | (new & 0x0FFF)`, skipped for symlinks.
   * ATTR: the §3.10 mask.
   * CT/AT/MT: copy the given datetimes verbatim.
   * SIZE: rejected with EACCES.
   * uid/gid: not changeable.
3. Dirty the inode. Commit when flag 0x2 is set.

Linux mapping:

* chmod changes only the low 12 bits.
* `utime` sets atime and mtime.
* Linux updating ctime on chmod or utime is a harmless deviation.
* chown should be rejected, or ignored to keep 0xFFFF.

### 8.11 symlink

Follow 8.1, with `mode = 0x41FF` and the target string copied to inode offset
0x030 before the dentry is inserted (dentry type 0x4000). Sony does not
commit here.

---

## 9. Invariants

### 9.1 What Sony's fsck verifies (PSBBN fsck v1.10 / FSCK.IRX v2.01)

**Superblock**

1. Superblock readable with a good magic; otherwise the backup is copied over
   it.
2. `version ≤ 3`.
3. `zone_size` a power of two in [2 KiB, 128 KiB].
4. `num_subs ≤` APA sub count; otherwise fsck can lower it.

**Journal and bitmap**

5. The journal is replayed and reset.
6. Every bitmap zone of every subpart can be read. Unreadable bitmap sectors
   are overwritten with 0xFF.
7. Reserved zones are pre-marked in fsck's private bitmap:
   * main: `0 .. Z4 + 1 + B0 + log.count - 1`;
   * sub: `0 .. Bs`.

**APA extended-attribute area** (fsck v1.10 only)

8. Sectors 8..0x1FFF of the main partition can be read; otherwise they are
   zero-filled.

**Each inode, starting at the root and recursing**

9. Magic `SEGD` and the checksum are valid.
10. For **every** index `i < number_data`, including `i = 0` and SEGI self
    entries:
    * `subpart ≤ num_subs`;
    * `count ≠ 0`;
    * `number ≥ 2`;
    * `(number + count) << ss ≤ part[subpart].length`.

    Otherwise: "bad zone" and an offer to "Remove rest of file".
11. Each SEGI reached through `next_segment` has magic `SEGI` and a valid
    checksum.
12. Directories: every 1 KiB block below `size` can be read. Files: every
    zone of every extent can be read.
13. Every referenced zone has its **on-disk** bitmap bit set. Otherwise fsck
    sets it ("not marked as used").
14. **No zone is referenced twice**, counting the reserved ranges and every
    inode. Otherwise: "overlapped zone", and the file is cut at that entry.
15. Regular files (by dentry type): the attribute block can be read.

**Each directory** (only the entries within `size`)

16. `aLen & 0xFFF` is a multiple of 4.
17. `aLen ≥ used(pLen)`.
18. No entry crosses a sector boundary.
19. Entries with `inode ≠ 0`:
    * `pLen = 1` and name `'.'`: `sub` and `inode` must equal the directory's
      own sub and `inode_block.number`.
    * `pLen = 2` and first byte `'.'`: must point to the parent. The root
      points to itself. **Note:** fsck only checks the first character, so
      any 2-byte name starting with `'.'` (for example `".a"`) is treated as
      `".."` and would be "fixed" to point at the parent (REF-BUG/quirk).
      Avoid creating such names, or accept the risk.
    * Any other entry: the target inode must be readable (otherwise the
      entry is removed), then it is checked recursively. fsck recurses when
      `dentry.aLen` has type 0x1000, and does the attribute check when it has
      0x2000.
20. Directory depth ≤ 63; deeper trees make fsck report an error.

**Whole volume**

21. **Bitmap equality.** After the walk, for each subpart, the on-disk bitmap
    must equal the computed bitmap (reserved plus referenced zones) **word
    for word** over `ceil(nz/32)` words. Leaked or extra bits are reported as
    "bitmap unmatch" and replaced with the computed value.
22. On success fsck clears WRITE_ERROR and the APA part-error record. If
    anything was fixed it sets ERRORS_FIXED.

**What fsck does not check.** It does not verify any of the following, but
Sony's driver relies on them, so a writer must keep them right:

* `number_blocks`, `number_segdesg` and `last_segment` consistency (§3.5);
* `size` against the allocation;
* the type nibble against the inode's mode;
* sector tiling beyond the per-entry checks;
* the attribute block's contents;
* `uid`/`gid`/`attr`.

**Quirks of the ps2sdk fsck sources** (**UNSURE** whether Sony's binary has
them):

* The bitmap-check helper adds the 4 MiB main-partition offset for **every**
  subpart.
* The final comparison indexes the computed bitmap by subpart index instead of
  by the subpart's zone offset.

Both make the RE'd fsck unreliable on multi-subpart volumes. Validate
multi-subpart output with extra care, for example with your own checker that
follows §9.

**fsck's repair "Remove rest of file"** sets:

* `number_data` = the bad index;
* `number_blocks` = the zones counted so far;
* `number_segdesg` = the descriptors so far;
* `last_segment` = the current descriptor;
* zero for that descriptor's `next_segment`;
* subpart cursor = 0;
* for files, CLOSED cleared.

### 9.2 Self-check a writer should run in tests

For every inode reachable from the root (directory entries with `inode ≠ 0`,
excluding `"."` and `".."`):

* Verify 9.1 items 9–19 and §3.5.
* Check `number_blocks == Σcount` and `number_segdesg == #self entries`.
* Check `last_segment` equals the self entry of the last descriptor.
* Check `(number_blocks − number_segdesg) * zsize ≥ size`.
* For directories, check `size mod 512 == 0` and that every sector's `aLen`
  values sum to exactly 512.
* Check that the dentry type equals `mode & 0xF000`.

Then rebuild the bitmap and compare it with disk (item 21). Check that the
journal header is in the reset state.

pfsshell (`put`, `mkdir`, `rm`, `rename`) produces reference volumes for
comparison. A host build of the ps2sdk fsck is in
`tests/ps2fs-qemu/pfsfsck/`.

### 9.3 Known REF-BUGs and quirks to tolerate but not reproduce

* Whole-inode free leaks all SEGIs and their extents (§3.9).
* Truncate leaves stale `next_segment` values and stale entries past
  `number_data`.
* Extent count is not clamped to 65535 on a new segment.
* The last-word bitmap scan can exceed `nz` (§2.2).
* The rmdir "." check is dead code. rmdir compares root/cwd with 16-bit
  truncated zone numbers.
* Name padding and slack are not zeroed, and emptiness is tested with
  `strcmp`.
* pfsshell's host build stores timestamps as UTC−9 h.
* Transactions can be split by cache eviction.
* fsck treats `".x"` names as `".."`, and has the multi-subpart quirks in
  §9.1.

---

## Appendix A: Constants

| Name | Value |
|---|---|
| Superblock magic | `0x50465300` |
| Journal magic | `0x5046534C` |
| SEGD magic | `0x53454744` |
| SEGI magic | `0x53454749` |
| Format version | 3 |
| Superblock / backup sector | 8192 / 8193 (main partition) |
| Main reserved prefix | 0x2000 sectors (4 MiB) |
| Metadata block | 1024 B |
| Bitmap chunk | 1024 B = 8192 zones |
| SEGD extent slots | `data[1..113]` (114 entries including self) |
| SEGI extent slots | `data[1..122]` (123 entries including self) |
| Max extent count | 65535 zones |
| Journal slots | 127 × 1 KiB images + 1 KiB header (128 KiB) |
| Dentry sector | 512 B |
| Name length | 1..255 |
| Directory type nibble | 0x1000 |
| File type nibble | 0x2000 |
| Symlink type nibble | 0x4000 |
| New dir attr | 0xA0 |
| CLOSED | 0x80 |
| uid/gid for created objects | 0xFFFF |
| pfsFsckStat bits | 0x1 WRITE_ERROR, 0x2 ERRORS_FIXED |
| Sony mount flags | 0x1 read-only, 0x2 write-through (name UNSURE), 0x4 refuse when WRITE_ERROR |

## Appendix B: Worked layout, test volume `PP.TEST` (256 MiB, zsize 8 KiB, ss = 4, is = 3)

* Superblock: magic `0x50465300`, version 3, modver `0x0202`, fsckstat 0,
  zone 8192, num_subs 0.
* `log = {514, 0, 16}`, so the journal starts at partition sector 8224.
* `root = {530, 0, 1}`.
* The bitmap starts at partition sector 8208, and its first bytes are `FF…`.
  Reserved zones 0..531 = 512 + 1 + 1 + 16 + 2.
* Root inode: `data[0] = {530,0,1}`, `data[1] = {531,0,1}`, mode `0x11FF`,
  attr `0xA0`, uid 0, size 512, `nb = 2`, `nd = 2`, `ns = 1`.
* Root dentries:
  * `"."`: `aLen 0x100C`.
  * `".."`: `0x100C` after the split.
  * `"sizes"`: `0x1010` (zone 532).
  * `"many"`, `"a"`, and a 200-byte name with `aLen 0x21C0` (448) closing the
    sector.
* Files: mode `0x21B6`, attr `0x80`, uid `0xFFFF`, attribute block = zeros
  with `aLen = 1024`.
  * `f_5242880`: inode 696 with a single extent `{697, 0, 640}`, `nb = 641`.
    This shows the "data right after the inode" placement and contiguous
    growth.
