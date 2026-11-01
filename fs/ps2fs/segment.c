/* Inode zone chains.
 *
 * An inode's data[] entries continue in SEGI blocks linked through
 * next_segment.  The whole chain is kept in memory (ps2fs_inode_info.ext,
 * indexed by the global entry index): entry 0 is the inode itself, entry
 * 114 + 123*k is the k-th SEGI's own zone, all others are data extents.
 * Functions here keep the on-disk chain, number_data, number_blocks,
 * number_segdesg and last_segment consistent, and are called with
 * sbi->lock held except ps2fs_free_chain_memory().
 */

#include <linux/config.h>
#include <linux/fs.h>
#include <linux/locks.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/stddef.h>

#include "ps2fs_fs.h"

#define RAW(inode)	(&PS2FS_I(inode)->raw)

static __u32 nd_of(struct inode *inode)
{
    return le32_to_cpu(RAW(inode)->number_data);
}

/* Index of descriptor d's own entry (d = 0 is the inode) */
static __u32 desc_index(__u32 d)
{
    return d ? PS2FS_SEGD_ENTRIES + (d - 1) * PS2FS_SEGI_ENTRIES : 0;
}

static int ensure_cap(struct inode *inode, __u32 n)
{
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    struct ps2fs_blockinfo *p;
    __u32 cap;

    if (n <= ii->ext_cap)
	return 0;
    cap = n + 32;
    p = kmalloc(cap * sizeof(*p), GFP_NOFS);
    if (!p)
	return -ENOMEM;
    memset(p, 0, cap * sizeof(*p));
    if (ii->ext) {
	memcpy(p, ii->ext, ii->ext_cap * sizeof(*p));
	kfree(ii->ext);
    }
    ii->ext = p;
    ii->ext_cap = cap;
    return 0;
}

void ps2fs_free_chain_memory(struct inode *inode)
{
    struct ps2fs_inode_info *ii = PS2FS_I(inode);

    if (ii->ext)
	kfree(ii->ext);
    ii->ext = NULL;
    ii->ext_cap = 0;
}

/* Recompute the checksum of a SEGI held in a transaction block */
static void segi_checksum(struct ps2fs_tblock *tb)
{
    __u32 *w0 = (__u32 *)tb->bh[0]->b_data, *w1 = (__u32 *)tb->bh[1]->b_data;
    __u32 sum = 0;
    int i;

    for (i = 1; i < 128; i++)
	sum += le32_to_cpu(w0[i]);
    for (i = 0; i < 128; i++)
	sum += le32_to_cpu(w1[i]);
    w0[0] = cpu_to_le32(sum);
}

/* SEGI number d (>= 1) in the open transaction */
static struct ps2fs_tblock *segi_get(struct inode *inode, __u32 d, int noload)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    struct ps2fs_blockinfo *self = &PS2FS_I(inode)->ext[desc_index(d)];

    return ps2fs_tblock_get(inode->i_sb, le16_to_cpu(self->subpart),
			    le32_to_cpu(self->number) << sbi->block_shift,
			    noload);
}

/* Pointer to byte `off' of a 1 KiB block split over two buffers */
static void *tb_ptr(struct ps2fs_tblock *tb, __u32 off)
{
    return tb->bh[off / 512]->b_data + off % 512;
}

/* Store entry i of the chain in memory and in its descriptor */
static int set_entry(struct inode *inode, __u32 i,
		     const struct ps2fs_blockinfo *e)
{
    struct ps2fs_tblock *tb;
    __u32 d = ps2fs_desc(i);

    PS2FS_I(inode)->ext[i] = *e;
    if (!d) {
	RAW(inode)->data[i] = *e;
	return 0;
    }
    if (!(tb = segi_get(inode, d, 0)))
	return -EIO;
    memcpy(tb_ptr(tb, offsetof(struct ps2fs_segi, data) +
		  ps2fs_fix(i) * sizeof(*e)), e, sizeof(*e));
    segi_checksum(tb);
    return 0;
}

/* Set next_segment of descriptor d */
static int set_next(struct inode *inode, __u32 d,
		    const struct ps2fs_blockinfo *next)
{
    struct ps2fs_tblock *tb;

    if (!d) {
	RAW(inode)->next_segment = *next;
	return 0;
    }
    if (!(tb = segi_get(inode, d, 0)))
	return -EIO;
    memcpy(tb_ptr(tb, offsetof(struct ps2fs_segi, next_segment)), next,
	   sizeof(*next));
    segi_checksum(tb);
    return 0;
}

/* Write the in-memory inode to its block (with a fresh checksum) */
int ps2fs_write_raw(struct inode *inode)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    struct ps2fs_inode *raw = RAW(inode);
    struct ps2fs_tblock *tb;

    tb = ps2fs_tblock_get(inode->i_sb, PS2FS_SUBPART(inode->i_ino),
			  PS2FS_ZONE(inode->i_ino) << sbi->block_shift, 1);
    if (!tb)
	return -EIO;
    raw->magic = cpu_to_le32(PS2FS_SEGD_MAGIC);
    raw->checksum = cpu_to_le32(ps2fs_checksum(raw));
    memcpy(tb->bh[0]->b_data, raw, 512);
    memcpy(tb->bh[1]->b_data, (char *)raw + 512, 512);
    return 0;
}

/*************************************************************************/

/* Check that an entry can be a zone run of this volume */
static int entry_ok(struct ps2fs_sb_info *sbi, const struct ps2fs_blockinfo *e)
{
    __u16 sub = le16_to_cpu(e->subpart);
    __u32 n = le32_to_cpu(e->number), c = le16_to_cpu(e->count);

    return sub < sbi->n_subparts && n >= 2 && c >= 1
	&& ((n + c) << sbi->sector_shift) <= sbi->part_size[sub];
}

/* Load the zone chain of an inode whose raw copy has been read */
int ps2fs_load_chain(struct inode *inode)
{
    struct super_block *sb = inode->i_sb;
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    struct ps2fs_inode *raw = &ii->raw;
    struct ps2fs_segi *segi;
    struct ps2fs_blockinfo next;
    __u32 nd = le32_to_cpu(raw->number_data), i, n, blocks = 0, descs = 0;
    int err = 0;

    if (nd < 1 || nd > 0x1000000 || ensure_cap(inode, nd))
	return -EIO;
    n = nd < PS2FS_SEGD_ENTRIES ? nd : PS2FS_SEGD_ENTRIES;
    memcpy(ii->ext, raw->data, n * sizeof(*ii->ext));
    next = raw->next_segment;
    segi = NULL;
    for (i = PS2FS_SEGD_ENTRIES; i < nd; i += PS2FS_SEGI_ENTRIES) {
	if (!segi && !(segi = kmalloc(sizeof(*segi), GFP_NOFS)))
	    return -ENOMEM;
	if (!entry_ok(sbi, &next)
	    || ps2fs_read_block(sb, le16_to_cpu(next.subpart),
				le32_to_cpu(next.number) << sbi->block_shift,
				segi)
	    || le32_to_cpu(segi->magic) != PS2FS_SEGI_MAGIC
	    || le32_to_cpu(segi->checksum) != ps2fs_checksum(segi)) {
	    ps2fs_error(sb, "ps2fs_load_chain", "inode 0x%08lX: bad SEGI",
			inode->i_ino);
	    err = -EIO;
	    break;
	}
	n = nd - i < PS2FS_SEGI_ENTRIES ? nd - i : PS2FS_SEGI_ENTRIES;
	memcpy(&ii->ext[i], segi->data, n * sizeof(*ii->ext));
	next = segi->next_segment;
    }
    if (segi)
	kfree(segi);
    if (err)
	return err;
    for (i = 0; i < nd; i++) {
	if (!entry_ok(sbi, &ii->ext[i])) {
	    ps2fs_error(sb, "ps2fs_load_chain", "inode 0x%08lX: bad zone "
			"entry %u", inode->i_ino, i);
	    return -EIO;
	}
	blocks += le16_to_cpu(ii->ext[i].count);
	if (!ps2fs_fix(i))
	    descs++;
    }
    if (blocks != le32_to_cpu(raw->number_blocks)
	|| descs != le32_to_cpu(raw->number_segdesg))
	ps2fs_warning(sb, "ps2fs_load_chain", "inode 0x%08lX: block counts "
		      "do not match its zones", inode->i_ino);
    return 0;
}

/* Data zones (descriptors excluded) */
__u32 ps2fs_data_zones(struct inode *inode)
{
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    __u32 i, nd = nd_of(inode), zones = 0;

    for (i = 1; i < nd; i++)
	if (ps2fs_fix(i))
	    zones += le16_to_cpu(ii->ext[i].count);
    return zones;
}

/* Zone number `zi' of the file data -> (subpart, zone); -1 if past the end */
int ps2fs_map(struct inode *inode, __u32 zi, __u16 *sub, __u32 *zone)
{
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    __u32 i, nd = nd_of(inode), c;

    for (i = 1; i < nd; i++) {
	if (!ps2fs_fix(i))
	    continue;
	c = le16_to_cpu(ii->ext[i].count);
	if (zi < c) {
	    *sub = le16_to_cpu(ii->ext[i].subpart);
	    *zone = le32_to_cpu(ii->ext[i].number) + zi;
	    return 0;
	}
	zi -= c;
    }
    return -1;
}

/*************************************************************************/

/* Add `need' data zones to the end of the file (Sony's growth policy:
 * extend the last run in place, else allocate after it).  Partial growth
 * is kept on failure; the inode stays consistent either way. */
int ps2fs_grow(struct inode *inode, __u32 need)
{
    struct super_block *sb = inode->i_sb;
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    struct ps2fs_inode *raw = &ii->raw;
    struct ps2fs_blockinfo e, prev;
    struct ps2fs_tblock *tb;
    struct ps2fs_segi *s;
    __u32 nd = nd_of(inode), blocks = le32_to_cpu(raw->number_blocks);
    __u32 descs = le32_to_cpu(raw->number_segdesg);
    int got, err = 0;

    if (ps2fs_fix(nd - 1)) {
	e = ii->ext[nd - 1];
	got = ps2fs_extend_run(sb, &e, need);
	if (got < 0)
	    return got;
	if (got) {
	    if ((err = set_entry(inode, nd - 1, &e)))
		return err;
	    blocks += got;
	    need -= got;
	}
    }
    while (need > 0) {
	if ((err = ensure_cap(inode, nd + 1)))
	    break;
	if (!ps2fs_fix(nd)) {
	    /* The last descriptor is full: chain a new SEGI after it */
	    prev = ii->ext[nd - 1];
	    e.number = cpu_to_le32(le32_to_cpu(prev.number)
				   + le16_to_cpu(prev.count));
	    e.subpart = prev.subpart;
	    e.count = cpu_to_le16(1);
	    if ((err = ps2fs_alloc(sb, &e, blocks)))
		break;
	    ii->ext[nd] = e;
	    tb = segi_get(inode, ps2fs_desc(nd), 1);
	    if (!tb) {
		ps2fs_free(sb, &e);
		err = -EIO;
		break;
	    }
	    s = (struct ps2fs_segi *)tb->bh[0]->b_data;
	    s->magic = cpu_to_le32(PS2FS_SEGI_MAGIC);
	    s->inode_block = raw->inode_block;
	    s->last_segment = ii->ext[desc_index(ps2fs_desc(nd - 1))];
	    s->data[0] = e;
	    segi_checksum(tb);
	    if ((err = set_next(inode, ps2fs_desc(nd - 1), &e)))
		break;
	    raw->last_segment = e;
	    nd++;
	    blocks++;
	    descs++;
	}
	prev = ii->ext[nd - 1];
	e.number = cpu_to_le32(le32_to_cpu(prev.number) + le16_to_cpu(prev.count));
	e.subpart = prev.subpart;
	e.count = cpu_to_le16(need > PS2FS_MAX_EXTENT ? PS2FS_MAX_EXTENT : need);
	if ((err = ps2fs_alloc(sb, &e, blocks)))
	    break;
	if ((err = set_entry(inode, nd, &e)))
	    break;
	nd++;
	blocks += le16_to_cpu(e.count);
	need -= le16_to_cpu(e.count);
	if (need > 0) {
	    got = ps2fs_extend_run(sb, &e, need);
	    if (got > 0) {
		if ((err = set_entry(inode, nd - 1, &e)))
		    break;
		blocks += got;
		need -= got;
	    }
	}
    }
    raw->number_data = cpu_to_le32(nd);
    raw->number_blocks = cpu_to_le32(blocks);
    raw->number_segdesg = cpu_to_le32(descs);
    if (ps2fs_write_raw(inode) && !err)
	err = -EIO;
    return err;
}

/* Keep only the first `keep' data zones, freeing the rest (and the SEGIs
 * no longer needed).  Unlike Sony's driver, entries past the new end and
 * the stale next_segment are cleared. */
int ps2fs_shrink(struct inode *inode, __u32 keep)
{
    struct super_block *sb = inode->i_sb;
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    struct ps2fs_inode *raw = &ii->raw;
    struct ps2fs_blockinfo e, zero;
    __u32 nd = nd_of(inode), i, j, descs = 1, c;
    int err = 0;

    if (le32_to_cpu(raw->number_blocks) ==
	le32_to_cpu(raw->number_segdesg) + keep)
	return 0;
    memset(&zero, 0, sizeof(zero));
    for (i = 1; i < nd && keep > 0; i++) {
	if (!ps2fs_fix(i)) {
	    descs++;
	    continue;
	}
	c = le16_to_cpu(ii->ext[i].count);
	if (keep < c) {
	    /* cut this run: free its tail */
	    e = ii->ext[i];
	    e.number = cpu_to_le32(le32_to_cpu(e.number) + keep);
	    e.count = cpu_to_le16(c - keep);
	    ii->ext[i].count = cpu_to_le16(keep);
	    if ((err = set_entry(inode, i, &ii->ext[i])))
		return err;
	    if ((err = ps2fs_free(sb, &e)))
		return err;
	    keep = 0;
	} else {
	    keep -= c;
	}
    }
    /* entries i..nd-1 go away: data runs and SEGIs alike */
    for (j = i; j < nd; j++) {
	if ((err = ps2fs_free(sb, &ii->ext[j])))
	    return err;
	if (ps2fs_desc(j) == ps2fs_desc(i - 1)) {
	    /* clear stale entries in the descriptor that stays */
	    if ((err = set_entry(inode, j, &zero)))
		return err;
	}
	ii->ext[j] = zero;
    }
    if (i < nd && (err = set_next(inode, ps2fs_desc(i - 1), &zero)))
	return err;
    raw->number_data = cpu_to_le32(i);
    raw->number_segdesg = cpu_to_le32(descs);
    c = 0;
    for (j = 0; j < i; j++)
	c += le16_to_cpu(ii->ext[j].count);
    raw->number_blocks = cpu_to_le32(c);
    raw->last_segment = ii->ext[desc_index(ps2fs_desc(i - 1))];
    return ps2fs_write_raw(inode);
}

/* Free every zone of an inode: data runs, SEGIs and the inode zone */
int ps2fs_free_inode_zones(struct inode *inode)
{
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    __u32 i, nd = nd_of(inode);
    int err = 0;

    for (i = 0; i < nd && !err; i++)
	err = ps2fs_free(inode->i_sb, &ii->ext[i]);
    return err;
}
