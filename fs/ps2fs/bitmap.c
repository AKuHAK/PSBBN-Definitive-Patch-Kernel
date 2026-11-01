/* Zone bitmap: one bit per zone of each subpart, set = used.
 *
 * The bitmap of subpart s starts at zone 1 (main partition: at 4 MiB plus
 * one zone) and is handled in 1 KiB chunks of 8192 zones, LSB first.
 * Free counts are not stored on disk; they are counted at mount.
 * Allocation follows the policy of Sony's driver so that fragmentation
 * stays similar.  All functions are called with sbi->lock held.
 */

#include <linux/config.h>
#include <linux/fs.h>
#include <linux/locks.h>
#include <linux/string.h>

#include "ps2fs_fs.h"

/*************************************************************************/

static __u32 zones_in(struct ps2fs_sb_info *sbi, __u16 sub)
{
    return sbi->part_size[sub] >> sbi->sector_shift;
}

/* 1 KiB block holding the bitmap chunk of zone z */
static __u32 chunk_block(struct ps2fs_sb_info *sbi, __u16 sub, __u32 z)
{
    return (1 << sbi->block_shift) + z / 8192 + (sub == 0 ? 4096 : 0);
}

/* Read-only view of the bitmap sector holding zone z */
struct bmap_cursor {
    struct buffer_head *bh;
    __u16 sub;
    __u32 sector;
};

static int cursor_bit(struct super_block *sb, struct bmap_cursor *c,
		      __u16 sub, __u32 z)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    __u32 sector = chunk_block(sbi, sub, z) * 2 + (z % 8192) / 4096;
    __u32 b = z % 4096;

    if (!c->bh || c->sub != sub || c->sector != sector) {
	if (c->bh)
	    brelse(c->bh);
	c->bh = ps2fs_bread(sb, sub, sector);
	if (!c->bh)
	    return -EIO;
	c->sub = sub;
	c->sector = sector;
    }
    return (((__u8 *)c->bh->b_data)[b / 8] >> (b % 8)) & 1;
}

static void cursor_done(struct bmap_cursor *c)
{
    if (c->bh)
	brelse(c->bh);
    c->bh = NULL;
}

/* Set or clear the bits of zones first..first+count-1 */
static int set_bits(struct super_block *sb, __u16 sub, __u32 first,
		    __u32 count, int value)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_tblock *tb = NULL;
    __u32 z, block = 0;
    __u8 *p;

    for (z = first; z < first + count; z++) {
	if (!tb || chunk_block(sbi, sub, z) != block) {
	    block = chunk_block(sbi, sub, z);
	    tb = ps2fs_tblock_get(sb, sub, block, 0);
	    if (!tb)
		return -EIO;
	}
	p = (__u8 *)tb->bh[(z % 8192) / 4096]->b_data + (z % 4096) / 8;
	if (value)
	    *p |= 1 << (z % 8);
	else
	    *p &= ~(1 << (z % 8));
    }
    return 0;
}

/*************************************************************************/

/* Count free zones of every subpart (at mount) */
int ps2fs_count_free(struct super_block *sb)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct buffer_head *bh;
    __u16 sub;
    __u32 z, nz, used, b, n;
    const __u8 *p;

    sbi->total_zones = sbi->free_zones = 0;
    for (sub = 0; sub < sbi->n_subparts; sub++) {
	nz = zones_in(sbi, sub);
	used = 0;
	/* one bitmap sector (4096 zones) at a time, a byte at a time */
	for (z = 0; z < nz; z += 4096) {
	    bh = ps2fs_bread(sb, sub, chunk_block(sbi, sub, z) * 2
			     + (z % 8192) / 4096);
	    if (!bh)
		return -EIO;
	    p = (const __u8 *)bh->b_data;
	    n = nz - z < 4096 ? nz - z : 4096;
	    for (b = 0; b < n; b++)
		used += (p[b / 8] >> (b % 8)) & 1;
	    brelse(bh);
	}
	sbi->free_zone[sub] = nz - used;
	sbi->total_zones += nz;
	sbi->free_zones += nz - used;
    }
    return 0;
}

/* Grow extent e in place by up to `want' free zones directly after it.
 * Returns the number of zones added (e->count is updated) or <0. */
int ps2fs_extend_run(struct super_block *sb, struct ps2fs_blockinfo *e,
		     __u32 want)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct bmap_cursor c = { NULL, 0, 0 };
    __u16 sub = le16_to_cpu(e->subpart);
    __u32 count = le16_to_cpu(e->count);
    __u32 start = le32_to_cpu(e->number) + count;
    __u32 nz = zones_in(sbi, sub), got = 0;
    int bit = 0, err;

    if (want > PS2FS_MAX_EXTENT - count)
	want = PS2FS_MAX_EXTENT - count;
    while (got < want && start + got < nz) {
	if ((bit = cursor_bit(sb, &c, sub, start + got)) != 0)
	    break;
	got++;
    }
    cursor_done(&c);
    if (bit < 0)
	return bit;
    if (!got)
	return 0;
    if ((err = set_bits(sb, sub, start, got, 1)) < 0)
	return err;
    e->count = cpu_to_le16(count + got);
    sbi->free_zone[sub] -= got;
    sbi->free_zones -= got;
    return got;
}

/* First run of `runlen' free zones of `sub' from zone `from'; takes up to
 * `take' zones of it.  Returns 1 and fills r, 0 if none, or <0. */
static int take_run(struct super_block *sb, __u16 sub, __u32 from,
		    __u32 runlen, __u32 take, struct ps2fs_blockinfo *r)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct bmap_cursor c = { NULL, 0, 0 };
    __u32 nz = zones_in(sbi, sub), z, start = from, len = 0;
    int bit, err;

    for (z = from; z < nz; z++) {
	if ((bit = cursor_bit(sb, &c, sub, z)) < 0) {
	    cursor_done(&c);
	    return bit;
	}
	if (bit) {
	    len = 0;
	    start = z + 1;
	} else if (++len >= runlen) {
	    break;
	}
    }
    cursor_done(&c);
    if (len < runlen)
	return 0;
    if (take > runlen)
	take = runlen;
    if ((err = set_bits(sb, sub, start, take, 1)) < 0)
	return err;
    sbi->free_zone[sub] -= take;
    sbi->free_zones -= take;
    r->number = cpu_to_le32(start);
    r->subpart = cpu_to_le16(sub);
    r->count = cpu_to_le16(take);
    return 1;
}

/* Allocate zones near hint {number, subpart, count = zones wanted}: look
 * for a run of max(min(maxcount, 32), wanted) zones, then shorter ones, in
 * the hinted subpart from the hint, then the other subparts from zone 0,
 * then the hinted subpart from 0.  On success *hint is the new extent
 * (which may be shorter than wanted). */
int ps2fs_alloc(struct super_block *sb, struct ps2fs_blockinfo *hint,
		__u32 maxcount)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    __u16 sub = le16_to_cpu(hint->subpart);
    __u32 from = le32_to_cpu(hint->number);
    __u32 want = le16_to_cpu(hint->count), runlen, n;
    int tries, rv;

    if (sub >= sbi->n_subparts)
	sub = 0;
    if (want == 0)
	want = 1;
    runlen = maxcount < 32 ? maxcount : 32;
    if (runlen < want)
	runlen = want;
    tries = sbi->n_subparts + (from ? 1 : 0);
    while (tries-- > 0) {
	for (n = runlen; n >= 1; n /= 2) {
	    if (sbi->free_zone[sub] < n)
		continue;
	    rv = take_run(sb, sub, from, n, want, hint);
	    if (rv)
		return rv < 0 ? rv : 0;
	}
	sub = (sub + 1) % sbi->n_subparts;
	from = 0;
    }
    return -ENOSPC;
}

/* Free extent e and forget pending changes to metadata inside it */
int ps2fs_free(struct super_block *sb, const struct ps2fs_blockinfo *e)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    __u16 sub = le16_to_cpu(e->subpart);
    __u32 number = le32_to_cpu(e->number), count = le16_to_cpu(e->count);
    int err;

    if (sub >= sbi->n_subparts || number < 2 || !count
	|| number + count > zones_in(sbi, sub)) {
	ps2fs_error(sb, "ps2fs_free", "bad extent %u+%u in subpart %u",
		    number, count, sub);
	return -EIO;
    }
    ps2fs_tblock_drop(sb, sub, number << sbi->block_shift,
		      count << sbi->block_shift);
    if ((err = set_bits(sb, sub, number, count, 0)) < 0)
	return err;
    sbi->free_zone[sub] += count;
    sbi->free_zones += count;
    return 0;
}
