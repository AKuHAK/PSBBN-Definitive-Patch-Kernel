/*
 * ps2fs/inode.c: inodes, regular files and symbolic links
 *
 * Copyright (c) 2002 Andrew Church <achurch@achurch.org>
 *
 * File data goes through the page cache (get_block maps 512-byte blocks
 * to zones); the inode itself is metadata and changes only inside a
 * journal transaction (journal.c).  Zones are allocated a zone at a time
 * while the file grows, and the size on disk never exceeds what is
 * allocated and initialised (mmu_private, as in the FAT driver).
 */

#include <linux/config.h>
#include <linux/fs.h>
#include <linux/locks.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/string.h>

#include "ps2fs_fs.h"

/*************************************************************************/

static int ps2fs_readlink(struct dentry *dentry, char *buf, int buflen);
static int ps2fs_follow_link(struct dentry *dentry, struct nameidata *nd);
static void ps2fs_truncate(struct inode *inode);
static int ps2fs_file_open(struct inode *inode, struct file *file);
static int ps2fs_file_release(struct inode *inode, struct file *file);
static int ps2fs_fsync(struct file *file, struct dentry *dentry, int datasync);

/*************************************************************************/

/* Target of a symlink, stored in the inode from offset 0x30 */
#define SYMLINK_TARGET(raw)	((char *)(raw) + 0x30)

static void raw_set_now(struct inode *inode, struct ps2fs_inode *raw)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);

    inode->i_atime = inode->i_ctime = inode->i_mtime = CURRENT_TIME;
    to_ps2time(&raw->atime, inode->i_atime, sbi->tzoffset);
    raw->ctime = raw->mtime = raw->atime;
}

/* Copy times and (for regular files) the size into the raw inode */
static void raw_update(struct inode *inode)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    loff_t size;

    to_ps2time(&ii->raw.atime, inode->i_atime, sbi->tzoffset);
    to_ps2time(&ii->raw.ctime, inode->i_ctime, sbi->tzoffset);
    to_ps2time(&ii->raw.mtime, inode->i_mtime, sbi->tzoffset);
    if (S_ISREG(inode->i_mode)) {
	/* never claim bytes that are not allocated and written */
	size = inode->i_size;
	if (size > ii->mmu_private)
	    size = ii->mmu_private;
	ii->raw.size_lo = cpu_to_le32((__u32)size);
	ii->raw.size_hi = cpu_to_le32((__u32)(size >> 32));
    }
}

/* Linux mode from the PFS mode.  Sony ignores the permission bits except
 * for execute, so reading is always allowed. */
static mode_t linux_mode(struct ps2fs_inode *raw)
{
    __u16 mode = le16_to_cpu(raw->mode);

    switch (mode & PS2FS_TYPE_MASK) {
      case PS2FS_TYPE_DIR:
	return S_IFDIR | (mode & 07777) | 0555;
      case PS2FS_TYPE_SYMLINK:
	return S_IFLNK | 0777;
      default:
	return S_IFREG | (mode & 07777) | 0444;
    }
}

void ps2fs_set_inode_ops(struct inode *inode)
{
    if (S_ISDIR(inode->i_mode)) {
	inode->i_op = &ps2fs_dir_iops;
	inode->i_fop = &ps2fs_dops;
    } else if (S_ISLNK(inode->i_mode)) {
	inode->i_op = &ps2fs_symlink_iops;
    } else {
	inode->i_op = &ps2fs_file_iops;
	inode->i_fop = &ps2fs_fops;
	inode->i_mapping->a_ops = &ps2fs_aops;
    }
}

/*************************************************************************/

void ps2fs_read_inode(struct inode *inode)
{
    struct super_block *sb = inode->i_sb;
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_inode_info *ii;
    struct ps2fs_inode *raw;
    __u16 sub = PS2FS_SUBPART(inode->i_ino);
    __u32 zone = PS2FS_ZONE(inode->i_ino), type;
    int err;

    ii = kmalloc(sizeof(*ii), GFP_KERNEL);
    if (!ii) {
	make_bad_inode(inode);
	return;
    }
    memset(ii, 0, sizeof(*ii));
    inode->u.generic_ip = ii;
    raw = &ii->raw;

    if (sub >= sbi->n_subparts || zone < 2
	|| ((zone + 1) << sbi->sector_shift) > sbi->part_size[sub]) {
	ps2fs_error(sb, "ps2fs_read_inode", "bad inode number 0x%08lX",
		    inode->i_ino);
	goto bad;
    }
    down(&sbi->lock);
    err = ps2fs_read_block(sb, sub, zone << sbi->block_shift, raw);
    if (!err && (le32_to_cpu(raw->magic) != PS2FS_SEGD_MAGIC
		 || le32_to_cpu(raw->checksum) != ps2fs_checksum(raw)
		 || le32_to_cpu(raw->inode_block.number) != zone
		 || le16_to_cpu(raw->inode_block.subpart) != sub)) {
	ps2fs_error(sb, "ps2fs_read_inode", "inode 0x%08lX is corrupt",
		    inode->i_ino);
	err = -EIO;
    }
    if (!err)
	err = ps2fs_load_chain(inode);
    up(&sbi->lock);
    if (err)
	goto bad;

    type = le16_to_cpu(raw->mode) & PS2FS_TYPE_MASK;
    if (type != PS2FS_TYPE_DIR && type != PS2FS_TYPE_FILE
	&& type != PS2FS_TYPE_SYMLINK) {
	ps2fs_error(sb, "ps2fs_read_inode", "inode 0x%08lX has unknown type "
		    "0x%04X", inode->i_ino, type);
	goto bad;
    }
    inode->i_mode = linux_mode(raw);
    inode->i_nlink = 1;
    inode->i_uid = sbi->uid;
    inode->i_gid = sbi->gid;
    if (type == PS2FS_TYPE_SYMLINK) {
	SYMLINK_TARGET(raw)[PS2FS_SYMLINK_MAX] = 0;
	inode->i_size = strlen(SYMLINK_TARGET(raw));
    } else {
	inode->i_size = le32_to_cpu(raw->size_lo)
	    | ((loff_t)le32_to_cpu(raw->size_hi) << 32);
	if (inode->i_size > (loff_t)ps2fs_data_zones(inode) * sbi->zone_size) {
	    ps2fs_error(sb, "ps2fs_read_inode", "inode 0x%08lX: size beyond "
			"its zones", inode->i_ino);
	    goto bad;
	}
    }
    ii->mmu_private = inode->i_size;
    inode->i_atime = from_ps2time(&raw->atime, sbi->tzoffset);
    inode->i_ctime = from_ps2time(&raw->ctime, sbi->tzoffset);
    inode->i_mtime = from_ps2time(&raw->mtime, sbi->tzoffset);
    inode->i_blksize = sbi->zone_size;
    inode->i_blocks = le32_to_cpu(raw->number_blocks) << sbi->sector_shift;
    ps2fs_set_inode_ops(inode);
    return;

  bad:
    ps2fs_free_chain_memory(inode);
    kfree(ii);
    inode->u.generic_ip = NULL;
    make_bad_inode(inode);
}

void ps2fs_write_inode(struct inode *inode, int wait)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);

    if (is_bad_inode(inode) || !PS2FS_I(inode) || !inode->i_nlink
	|| !sbi->writable)
	return;
    down(&sbi->lock);
    raw_update(inode);
    if (!ps2fs_write_raw(inode) && wait)
	ps2fs_commit(inode->i_sb);
    up(&sbi->lock);
}

/* Last reference to an unlinked inode: free all of its zones */
void ps2fs_delete_inode(struct inode *inode)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);

    if (!is_bad_inode(inode) && PS2FS_I(inode) && sbi->writable) {
	down(&sbi->lock);
	if (!ps2fs_free_inode_zones(inode))
	    ps2fs_commit(inode->i_sb);
	up(&sbi->lock);
    }
    clear_inode(inode);
}

void ps2fs_clear_inode(struct inode *inode)
{
    if (PS2FS_I(inode)) {
	ps2fs_free_chain_memory(inode);
	kfree(PS2FS_I(inode));
	inode->u.generic_ip = NULL;
    }
}

/*************************************************************************/

/* Build a new inode (and the first block of a directory) in the open
 * transaction.  Called with sbi->lock held; the caller links it into the
 * directory and then reads it with iget(). */

/* Subpart for a new directory: the parent's round-robin cursor, unless
 * that subpart is much fuller than the volume as a whole */
static __u16 choose_subpart(struct super_block *sb, struct ps2fs_inode *praw)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    __u32 c = le32_to_cpu(praw->subpart), i, best = 0, pct, max = 0;
    __u32 overall, this;

    c = c < sbi->n_subparts - 1 ? c + 1 : 0;
    praw->subpart = cpu_to_le32(c);
    overall = sbi->free_zones / (sbi->total_zones / 100 + 1);
    this = sbi->free_zone[c]
	/ ((sbi->part_size[c] >> sbi->sector_shift) / 100 + 1);
    if (this >= overall || overall - this < 11)
	return c;
    for (i = 0; i < sbi->n_subparts; i++) {
	pct = sbi->free_zone[i]
	    / ((sbi->part_size[i] >> sbi->sector_shift) / 100 + 1);
	if (pct > max) {
	    max = pct;
	    best = i;
	}
    }
    return best;
}

static void put_dirent(char *p, const struct ps2fs_blockinfo *target,
		       const char *name, int len, __u16 size_type)
{
    struct ps2fs_dirent *de = (struct ps2fs_dirent *)p;

    de->inode = target->number;
    de->subpart = le16_to_cpu(target->subpart);
    de->namelen = len;
    de->size_type = cpu_to_le16(size_type);
    memcpy(de->name, name, len);
}

int ps2fs_new_inode(struct inode *dir, int type, int perm,
		    const char *symlink, struct ps2fs_blockinfo *where)
{
    struct super_block *sb = dir->i_sb;
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_inode *praw = &PS2FS_I(dir)->raw, *raw;
    struct ps2fs_blockinfo e, d;
    struct ps2fs_tblock *tb;
    int err = -EIO;

    if (type == PS2FS_TYPE_DIR) {
	e.number = 0;
	e.subpart = cpu_to_le16(choose_subpart(sb, praw));
    } else {
	e = praw->inode_block;
    }
    e.count = cpu_to_le16(1);
    if ((err = ps2fs_alloc(sb, &e, 2)))
	return err;
    d = e;
    if (type == PS2FS_TYPE_DIR && (err = ps2fs_alloc(sb, &d, 0))) {
	ps2fs_free(sb, &e);
	return err;
    }

    raw = kmalloc(sizeof(*raw), GFP_NOFS);
    if (!raw) {
	err = -ENOMEM;
	goto fail;
    }
    memset(raw, 0, sizeof(*raw));
    raw->magic = cpu_to_le32(PS2FS_SEGD_MAGIC);
    raw->inode_block = raw->last_segment = raw->data[0] = e;
    raw->mode = cpu_to_le16(type | (perm & 07777));
    raw->uid = raw->gid = cpu_to_le16(PS2FS_ID_NONE);
    to_ps2time(&raw->atime, CURRENT_TIME, sbi->tzoffset);
    raw->ctime = raw->mtime = raw->atime;
    raw->number_data = raw->number_blocks = cpu_to_le32(1);
    raw->number_segdesg = cpu_to_le32(1);
    raw->subpart = cpu_to_le32(le16_to_cpu(e.subpart));
    if (type == PS2FS_TYPE_DIR) {
	raw->attr = cpu_to_le16(PS2FS_ATTR_NEWDIR);
	raw->size_lo = cpu_to_le32(512);
	raw->data[1] = d;
	raw->number_data = raw->number_blocks = cpu_to_le32(2);
    } else if (type == PS2FS_TYPE_SYMLINK) {
	raw->mode = cpu_to_le16(PS2FS_TYPE_SYMLINK | 0777);
	strcpy(SYMLINK_TARGET(raw), symlink);
    }
    raw->checksum = cpu_to_le32(ps2fs_checksum(raw));

    tb = ps2fs_tblock_get(sb, le16_to_cpu(e.subpart),
			  le32_to_cpu(e.number) << sbi->block_shift, 1);
    if (!tb)
	goto fail_free;
    memcpy(tb->bh[0]->b_data, raw, 512);
    memcpy(tb->bh[1]->b_data, (char *)raw + 512, 512);

    if (type == PS2FS_TYPE_DIR) {
	tb = ps2fs_tblock_get(sb, le16_to_cpu(d.subpart),
			      le32_to_cpu(d.number) << sbi->block_shift, 1);
	if (!tb)
	    goto fail_free;
	put_dirent(tb->bh[0]->b_data, &e, ".", 1, PS2FS_TYPE_DIR | 12);
	put_dirent(tb->bh[0]->b_data + 12, &praw->inode_block, "..", 2,
		   PS2FS_TYPE_DIR | 500);
    } else {
	/* empty attribute area */
	tb = ps2fs_tblock_get(sb, le16_to_cpu(e.subpart),
			      (le32_to_cpu(e.number) << sbi->block_shift) + 1, 1);
	if (!tb)
	    goto fail_free;
	((__u16 *)tb->bh[0]->b_data)[1] = cpu_to_le16(1024);
    }
    kfree(raw);
    *where = e;
    return 0;

  fail_free:
    kfree(raw);
  fail:
    ps2fs_free(sb, &e);
    if (type == PS2FS_TYPE_DIR)
	ps2fs_free(sb, &d);
    return err;
}

/*************************************************************************/

static int ps2fs_get_block(struct inode *inode, long iblock,
			   struct buffer_head *bh_result, int create)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    unsigned long pos = (unsigned long)iblock << 9;
    __u32 zi = iblock >> sbi->sector_shift, zone;
    __u16 sub;
    int err = 0;

    if (iblock < 0)
	return -EIO;
    down(&sbi->lock);
    if (pos < ii->mmu_private || !create) {
	if (pos < ii->mmu_private && !ps2fs_map(inode, zi, &sub, &zone)) {
	    bh_result->b_dev = inode->i_dev;
	    bh_result->b_blocknr = sbi->part_start[sub]
		+ (zone << sbi->sector_shift)
		+ (iblock & ((1 << sbi->sector_shift) - 1));
	    bh_result->b_state |= 1UL << BH_Mapped;
	}
	goto out;
    }
    /* cont_prepare_write() extends files one block at a time */
    if (pos != ii->mmu_private) {
	err = -EIO;
	goto out;
    }
    if (!sbi->writable || sbi->error) {
	err = -EROFS;
	goto out;
    }
    if (ps2fs_map(inode, zi, &sub, &zone)) {
	if ((err = ps2fs_grow(inode, 1)))
	    goto out;
	if (ps2fs_map(inode, zi, &sub, &zone)) {
	    err = -EIO;
	    goto out;
	}
	inode->i_blocks = le32_to_cpu(ii->raw.number_blocks)
	    << sbi->sector_shift;
    }
    bh_result->b_dev = inode->i_dev;
    bh_result->b_blocknr = sbi->part_start[sub] + (zone << sbi->sector_shift)
	+ (iblock & ((1 << sbi->sector_shift) - 1));
    bh_result->b_state |= (1UL << BH_Mapped) | (1UL << BH_New);
    ii->mmu_private += 512;
  out:
    up(&sbi->lock);
    return err;
}

static int ps2fs_readpage(struct file *file, struct page *page)
{
    return block_read_full_page(page, ps2fs_get_block);
}

static int ps2fs_writepage(struct page *page)
{
    return block_write_full_page(page, ps2fs_get_block);
}

static int ps2fs_prepare_write(struct file *file, struct page *page,
			       unsigned from, unsigned to)
{
    return cont_prepare_write(page, from, to, ps2fs_get_block,
			      &PS2FS_I(page->mapping->host)->mmu_private);
}

static int ps2fs_bmap(struct address_space *mapping, long block)
{
    return generic_block_bmap(mapping, block, ps2fs_get_block);
}

struct address_space_operations ps2fs_aops = {
    readpage:		ps2fs_readpage,
    writepage:		ps2fs_writepage,
    sync_page:		block_sync_page,
    prepare_write:	ps2fs_prepare_write,
    commit_write:	generic_commit_write,
    bmap:		ps2fs_bmap,
};

/*************************************************************************/

/* Called by vmtruncate() once i_size is set (with the kernel lock held) */
static void ps2fs_truncate(struct inode *inode)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    __u32 keep;

    if (!S_ISREG(inode->i_mode) || !sbi->writable)
	return;
    down(&sbi->lock);
    if (ii->mmu_private > inode->i_size)
	ii->mmu_private = inode->i_size;
    keep = (ii->mmu_private + sbi->zone_size - 1) >> (sbi->sector_shift + 9);
    inode->i_mtime = inode->i_ctime = CURRENT_TIME;
    raw_update(inode);
    if (!ps2fs_shrink(inode, keep))
	ps2fs_write_raw(inode);
    inode->i_blocks = le32_to_cpu(ii->raw.number_blocks) << sbi->sector_shift;
    ps2fs_commit(inode->i_sb);
    up(&sbi->lock);
}

/* Allocate and zero-fill a file up to `size' bytes (generic_cont_expand()
 * of later kernels); the caller sets the size itself afterwards */
static int cont_expand(struct inode *inode, loff_t size)
{
    struct page *page;
    unsigned long limit = current->rlim[RLIMIT_FSIZE].rlim_cur;
    unsigned offset;
    int err;

    if (limit != RLIM_INFINITY && size > (loff_t)limit) {
	send_sig(SIGXFSZ, current, 0);
	return -EFBIG;
    }
    if (size > inode->i_sb->s_maxbytes)
	return -EFBIG;
    /* prepare_write skips a block when from == to == its start */
    offset = (size & (PAGE_CACHE_SIZE - 1)) | 1;
    page = grab_cache_page(inode->i_mapping, size >> PAGE_CACHE_SHIFT);
    if (!page)
	return -ENOMEM;
    err = ps2fs_prepare_write(NULL, page, offset, offset);
    if (!err)
	err = generic_commit_write(NULL, page, offset, offset);
    UnlockPage(page);
    page_cache_release(page);
    return err > 0 ? 0 : err;
}

int ps2fs_setattr(struct dentry *dentry, struct iattr *attr)
{
    struct inode *inode = dentry->d_inode;
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    int err;

    /* PFS keeps no owners: Sony stores 0xFFFF everywhere */
    if (((attr->ia_valid & ATTR_UID) && attr->ia_uid != inode->i_uid)
	|| ((attr->ia_valid & ATTR_GID) && attr->ia_gid != inode->i_gid))
	return -EPERM;
    attr->ia_valid &= ~(ATTR_UID | ATTR_GID);
    if ((err = inode_change_ok(inode, attr)))
	return err;
    if (attr->ia_valid & ATTR_SIZE) {
	if (!S_ISREG(inode->i_mode))
	    return -EINVAL;
	if (attr->ia_size > inode->i_size
	    && (err = cont_expand(inode, attr->ia_size)))
	    return err;
    }
    if ((err = inode_setattr(inode, attr)))
	return err;
    if ((attr->ia_valid & ATTR_MODE) && !S_ISLNK(inode->i_mode)) {
	down(&sbi->lock);
	ii->raw.mode = cpu_to_le16((le16_to_cpu(ii->raw.mode)
				    & PS2FS_TYPE_MASK)
				   | (attr->ia_mode & 07777));
	up(&sbi->lock);
	inode->i_mode = linux_mode(&ii->raw);
    }
    /* times and mode reach the disk through write_inode */
    return 0;
}

/*************************************************************************/

/* Sony clears CLOSED (persistently) while a file is open for writing and
 * sets it again, with fresh times, at the last close */
static int ps2fs_file_open(struct inode *inode, struct file *file)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    int err = 0;

    if (!(file->f_mode & FMODE_WRITE))
	return 0;
    down(&sbi->lock);
    if (ii->writers++ == 0
	&& (le16_to_cpu(ii->raw.attr) & PS2FS_ATTR_CLOSED)) {
	ii->raw.attr &= cpu_to_le16(~PS2FS_ATTR_CLOSED);
	err = ps2fs_write_raw(inode);
	if (!err)
	    err = ps2fs_commit(inode->i_sb);
	if (err)
	    ii->writers--;
    }
    up(&sbi->lock);
    return err;
}

static int ps2fs_file_release(struct inode *inode, struct file *file)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    struct ps2fs_inode_info *ii = PS2FS_I(inode);

    if (!(file->f_mode & FMODE_WRITE))
	return 0;
    down(&sbi->lock);
    if (--ii->writers == 0 && inode->i_nlink && sbi->writable) {
	raw_set_now(inode, &ii->raw);
	raw_update(inode);
	ii->raw.attr |= cpu_to_le16(PS2FS_ATTR_CLOSED);
	if (!ps2fs_write_raw(inode))
	    ps2fs_commit(inode->i_sb);
    }
    up(&sbi->lock);
    return 0;
}

static int ps2fs_fsync(struct file *file, struct dentry *dentry, int datasync)
{
    struct inode *inode = dentry->d_inode;
    struct ps2fs_sb_info *sbi = PS2FS_SB(inode->i_sb);
    int err;

    down(&sbi->lock);
    raw_update(inode);
    err = ps2fs_write_raw(inode);
    if (!err)
	err = ps2fs_commit(inode->i_sb);	/* writes the data first */
    up(&sbi->lock);
    return err;
}

struct file_operations ps2fs_fops = {
    llseek:	generic_file_llseek,
    read:	generic_file_read,
    write:	generic_file_write,
    mmap:	generic_file_mmap,
    open:	ps2fs_file_open,
    release:	ps2fs_file_release,
    fsync:	ps2fs_fsync,
};

struct inode_operations ps2fs_file_iops = {
    truncate:	ps2fs_truncate,
    setattr:	ps2fs_setattr,
};

/*************************************************************************/

static int ps2fs_readlink(struct dentry *dentry, char *buf, int buflen)
{
    return vfs_readlink(dentry, buf, buflen,
			SYMLINK_TARGET(&PS2FS_I(dentry->d_inode)->raw));
}

static int ps2fs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
    return vfs_follow_link(nd, SYMLINK_TARGET(&PS2FS_I(dentry->d_inode)->raw));
}

struct inode_operations ps2fs_symlink_iops = {
    readlink:		ps2fs_readlink,
    follow_link:	ps2fs_follow_link,
    setattr:		ps2fs_setattr,
};
