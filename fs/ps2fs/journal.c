/* Metadata blocks and the PFS journal ("PFSL").
 *
 * Metadata is changed in 1 KiB blocks held in the buffer cache but not
 * marked dirty, so nothing reaches the disk on its own.  ps2fs_commit()
 * writes them the way Sony's driver does: file data first, then the
 * block images into the journal, the journal header, the blocks at home,
 * and finally the reset header.  A crash at any point leaves either the
 * old state or a journal that Sony's driver (or ours) replays.
 *
 * All functions here are called with sbi->lock held.
 */

#include <linux/config.h>
#include <linux/fs.h>
#include <linux/locks.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/byteorder.h>

#include "ps2fs_fs.h"

/*************************************************************************/

/* Sum of words 1..255 of a 1 KiB inode or SEGI block */
__u32 ps2fs_checksum(const void *block)
{
    const __u32 *w = block;
    __u32 sum = 0;
    int i;

    for (i = 1; i < 256; i++)
	sum += le32_to_cpu(w[i]);
    return sum;
}

/* Low 16 bits of the sum of words 2..255 of a journal header */
static __u16 journal_checksum(const struct ps2fs_journal *j)
{
    const __u32 *w = (const __u32 *)j;
    __u32 sum = 0;
    int i;

    for (i = 2; i < 256; i++)
	sum += le32_to_cpu(w[i]);
    return sum & 0xFFFF;
}

/* Is a PFS write to (sub, sector) allowed at all? */
static int sector_ok(struct ps2fs_sb_info *sbi, __u16 sub, __u32 sector,
		     int write)
{
    if (sub >= sbi->n_subparts || sector >= sbi->part_size[sub])
	return 0;
    if (write && sector < (sub == 0 ? PS2FS_APA_RESERVED : 2))
	return 0;		/* APA header and attribute area */
    return 1;
}

struct buffer_head *ps2fs_bread(struct super_block *sb, __u16 sub,
				__u32 sector)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct buffer_head *bh;

    if (!sector_ok(sbi, sub, sector, 0)) {
	ps2fs_error(sb, "ps2fs_bread", "sector %u of subpart %u out of range",
		    sector, sub);
	return NULL;
    }
    bh = bread(sb->s_dev, sbi->part_start[sub] + sector, 512);
    if (!bh)
	ps2fs_metadata_error(sb, "read");
    return bh;
}

/* Copy 1 KiB block `block' of subpart `sub' into buf */
int ps2fs_read_block(struct super_block *sb, __u16 sub, __u32 block,
		     void *buf)
{
    struct buffer_head *bh;
    int i;

    for (i = 0; i < 2; i++) {
	bh = ps2fs_bread(sb, sub, block * 2 + i);
	if (!bh)
	    return -EIO;
	memcpy((char *)buf + i * 512, bh->b_data, 512);
	brelse(bh);
    }
    return 0;
}

/*************************************************************************/

/* Write `n' buffers and wait for them.  The buffers must be up to date. */
static int write_sync(struct buffer_head **bhs, int n)
{
    int i, err = 0;

    for (i = 0; i < n; i++)
	mark_buffer_dirty(bhs[i]);
    ll_rw_block(WRITE, n, bhs);
    for (i = 0; i < n; i++) {
	wait_on_buffer(bhs[i]);
	if (!buffer_uptodate(bhs[i]))
	    err = -EIO;
    }
    return err;
}

/* Write a 1 KiB image to the journal area, sectors J+off and J+off+1 */
static int journal_write(struct super_block *sb, __u32 off, const void *data)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct buffer_head *bhs[2];
    __u32 j = sbi->log.number << sbi->sector_shift;
    int i, err;

    for (i = 0; i < 2; i++) {
	bhs[i] = getblk(sb->s_dev, sbi->part_start[0] + j + off + i, 512);
	if (!bhs[i]) {
	    if (i)
		brelse(bhs[0]);
	    return -EIO;
	}
	wait_on_buffer(bhs[i]);
	memcpy(bhs[i]->b_data, (const char *)data + i * 512, 512);
	mark_buffer_uptodate(bhs[i], 1);
    }
    err = write_sync(bhs, 2);
    brelse(bhs[0]);
    brelse(bhs[1]);
    return err;
}

static int journal_reset(struct super_block *sb)
{
    struct ps2fs_journal *jh;
    int err;

    jh = kmalloc(sizeof(*jh), GFP_NOFS);
    if (!jh)
	return -ENOMEM;
    memset(jh, 0, sizeof(*jh));
    jh->magic = cpu_to_le32(PS2FS_JOURNAL_MAGIC);
    err = journal_write(sb, 0, jh);
    kfree(jh);
    return err;
}

/*************************************************************************/

/* Get block `block' of subpart `sub' into the open transaction.  With
 * `noload' the block is not read but zero-filled (a newly allocated block).
 * Commits first if the journal could not take another block. */
struct ps2fs_tblock *ps2fs_tblock_get(struct super_block *sb, __u16 sub,
				      __u32 block, int noload)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_tblock *tb;
    int i;

    for (i = 0; i < sbi->ntblocks; i++) {
	tb = &sbi->tblocks[i];
	if (tb->sub == sub && tb->block == block) {
	    if (noload) {
		memset(tb->bh[0]->b_data, 0, 512);
		memset(tb->bh[1]->b_data, 0, 512);
	    }
	    return tb;
	}
    }
    if (sbi->error || !sector_ok(sbi, sub, block * 2, 1)) {
	ps2fs_error(sb, "ps2fs_tblock_get", "refusing block %u of subpart %u",
		    block, sub);
	return NULL;
    }
    if (sbi->ntblocks >= sbi->max_tblocks && ps2fs_commit(sb))
	return NULL;
    tb = &sbi->tblocks[sbi->ntblocks];
    for (i = 0; i < 2; i++) {
	if (noload) {
	    tb->bh[i] = getblk(sb->s_dev,
			       sbi->part_start[sub] + block * 2 + i, 512);
	    if (tb->bh[i]) {
		wait_on_buffer(tb->bh[i]);
		memset(tb->bh[i]->b_data, 0, 512);
		mark_buffer_uptodate(tb->bh[i], 1);
	    }
	} else {
	    tb->bh[i] = ps2fs_bread(sb, sub, block * 2 + i);
	}
	if (!tb->bh[i]) {
	    if (i)
		brelse(tb->bh[0]);
	    return NULL;
	}
    }
    tb->sub = sub;
    tb->block = block;
    sbi->ntblocks++;
    sb->s_dirt = 1;
    return tb;
}

/* Forget changes to blocks inside a freed zone range so that they are
 * never written back (block numbers: first .. first+count-1). */
void ps2fs_tblock_drop(struct super_block *sb, __u16 sub, __u32 first,
		       __u32 count)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_tblock *tb;
    int i, j;

    for (i = 0; i < sbi->ntblocks; ) {
	tb = &sbi->tblocks[i];
	if (tb->sub != sub || tb->block < first || tb->block >= first + count) {
	    i++;
	    continue;
	}
	for (j = 0; j < 2; j++) {
	    /* the cached copy no longer matches the disk */
	    mark_buffer_uptodate(tb->bh[j], 0);
	    brelse(tb->bh[j]);
	}
	*tb = sbi->tblocks[--sbi->ntblocks];
    }
}

/* Commit the open transaction (see the comment at the top) */
int ps2fs_commit(struct super_block *sb)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_journal *jh;
    struct buffer_head *bhs[2 * PS2FS_JOURNAL_SLOTS];
    char *image;
    int i, n = sbi->ntblocks, err = 0;

    sb->s_dirt = 0;
    if (!n)
	return 0;
    jh = kmalloc(sizeof(*jh), GFP_NOFS);
    image = kmalloc(1024, GFP_NOFS);
    if (!jh || !image) {
	err = -ENOMEM;
	goto out;
    }

    /* File data first, so committed metadata never points at stale data
     * (the metadata buffers are clean and stay where they are). */
    fsync_no_super(sb->s_dev);

    memset(jh, 0, sizeof(*jh));
    jh->magic = cpu_to_le32(PS2FS_JOURNAL_MAGIC);
    jh->num = cpu_to_le16(n);
    for (i = 0; i < n && !err; i++) {
	struct ps2fs_tblock *tb = &sbi->tblocks[i];

	memcpy(image, tb->bh[0]->b_data, 512);
	memcpy(image + 512, tb->bh[1]->b_data, 512);
	jh->log[i].sector = cpu_to_le32(tb->block * 2);
	jh->log[i].sub = cpu_to_le16(tb->sub);
	jh->log[i].log_sector = cpu_to_le16(2 + 2 * i);
	err = journal_write(sb, 2 + 2 * i, image);
    }
    if (!err) {
	jh->checksum = cpu_to_le16(journal_checksum(jh));
	err = journal_write(sb, 0, jh);
    }
    if (!err) {
	for (i = 0; i < n; i++) {
	    bhs[2 * i] = sbi->tblocks[i].bh[0];
	    bhs[2 * i + 1] = sbi->tblocks[i].bh[1];
	}
	err = write_sync(bhs, 2 * n);
    }
    if (!err)
	err = journal_reset(sb);

  out:
    for (i = 0; i < n; i++) {
	if (err) {
	    mark_buffer_uptodate(sbi->tblocks[i].bh[0], 0);
	    mark_buffer_uptodate(sbi->tblocks[i].bh[1], 0);
	}
	brelse(sbi->tblocks[i].bh[0]);
	brelse(sbi->tblocks[i].bh[1]);
    }
    sbi->ntblocks = 0;
    if (jh)
	kfree(jh);
    if (image)
	kfree(image);
    if (err)
	ps2fs_metadata_error(sb, "journal commit");
    return err;
}

/*************************************************************************/

/* Read and validate the journal header; returns entries pending, or <0 */
static int journal_read(struct super_block *sb, struct ps2fs_journal *jh)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    __u32 j = sbi->log.number << sbi->sector_shift;

    if (ps2fs_read_block(sb, 0, j / 2, jh))
	return -EIO;
    if (le32_to_cpu(jh->magic) != PS2FS_JOURNAL_MAGIC
	|| le16_to_cpu(jh->checksum) != journal_checksum(jh)
	|| le16_to_cpu(jh->num) > PS2FS_JOURNAL_SLOTS)
	return 0;		/* torn or never written: nothing to replay */
    return le16_to_cpu(jh->num);
}

/* Warn about a pending journal on a read-only mount */
int ps2fs_journal_check(struct super_block *sb)
{
    struct ps2fs_journal *jh = kmalloc(sizeof(*jh), GFP_KERNEL);
    int n;

    if (!jh)
	return -ENOMEM;
    n = journal_read(sb, jh);
    kfree(jh);
    if (n > 0)
	printk(KERN_WARNING "ps2fs: journal holds %d unfinished updates; "
	       "mount read-write once to apply them\n", n);
    return n;
}

/* Apply a pending journal and reset it, as Sony's driver does at mount */
int ps2fs_journal_replay(struct super_block *sb)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_journal *jh;
    char *image;
    __u32 j = sbi->log.number << sbi->sector_shift;
    __u32 jsize = sbi->log.count << sbi->sector_shift;
    int n, i, k, err = 0;

    jh = kmalloc(sizeof(*jh), GFP_KERNEL);
    image = kmalloc(1024, GFP_KERNEL);
    if (!jh || !image) {
	err = -ENOMEM;
	goto out;
    }
    n = journal_read(sb, jh);
    if (n < 0) {
	err = n;
	goto out;
    }
    for (i = 0; i < n && !err; i++) {
	__u32 home = le32_to_cpu(jh->log[i].sector);
	__u16 sub = le16_to_cpu(jh->log[i].sub);
	__u32 ls = le16_to_cpu(jh->log[i].log_sector);
	struct buffer_head *bhs[2];

	if (ls + 2 > jsize || !sector_ok(sbi, sub, home + 1, 1)
	    || !sector_ok(sbi, sub, home, 1)) {
	    printk(KERN_ERR "ps2fs: bad journal entry %d, not replayed\n", i);
	    continue;
	}
	if (ps2fs_read_block(sb, 0, (j + ls) / 2, image)) {
	    err = -EIO;
	    break;
	}
	for (k = 0; k < 2; k++) {
	    bhs[k] = getblk(sb->s_dev, sbi->part_start[sub] + home + k, 512);
	    wait_on_buffer(bhs[k]);
	    memcpy(bhs[k]->b_data, image + k * 512, 512);
	    mark_buffer_uptodate(bhs[k], 1);
	}
	err = write_sync(bhs, 2);
	brelse(bhs[0]);
	brelse(bhs[1]);
    }
    if (!err)
	err = journal_reset(sb);
    if (!err && n > 0)
	printk(KERN_INFO "ps2fs: replayed %d journal entries\n", n);
  out:
    if (jh)
	kfree(jh);
    if (image)
	kfree(image);
    return err;
}

/*************************************************************************/

/* A metadata read or write failed: stop writing and, like Sony's driver,
 * flag the volume so that PSBBN offers to check it. */
void ps2fs_metadata_error(struct super_block *sb, const char *what)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct buffer_head *bh;
    struct ps2fs_super_block *psb;

    if (sbi->error)
	return;
    sbi->error = 1;
    printk(KERN_ERR "ps2fs: metadata %s error; no further writes\n", what);
    if (!sbi->writable)
	return;
    bh = bread(sb->s_dev, sbi->part_start[0] + PS2FS_SUPER_SECTOR, 512);
    if (!bh)
	return;
    psb = (struct ps2fs_super_block *)bh->b_data;
    if (le32_to_cpu(psb->magic) == PS2FS_SUPER_MAGIC) {
	psb->fsck_stat |= cpu_to_le32(PS2FS_FSCK_WRITE_ERROR);
	write_sync(&bh, 1);
    }
    brelse(bh);
}
