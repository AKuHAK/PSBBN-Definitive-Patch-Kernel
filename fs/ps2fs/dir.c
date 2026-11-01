/*
 * ps2fs/dir.c: directories
 *
 * Copyright (c) 2002 Andrew Church <achurch@achurch.org>
 *
 * A directory is a stream of 512-byte sectors, each tiled exactly by
 * entries (see ps2fs_fs.h).  Directory blocks are metadata: they are read
 * through the buffer cache and changed only inside a journal transaction.
 * Insertion and removal follow Sony's driver (first fit, merge into the
 * predecessor) so that PSBBN, fsck and Linux see the same layout.
 */

#include <linux/config.h>
#include <linux/fs.h>
#include <linux/locks.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ps2fs_fs.h"

/*************************************************************************/

/* Blocks one directory operation can put in a transaction, short of
 * freeing zones (bitmap chunks are then committed as they fill) */
#define OP_TBLOCKS	24

/* Position of an entry */
struct dir_pos {
    __u32 n;			/* sector index within the directory */
    __u16 sub;			/* where that sector is */
    __u32 sector;
    int off;			/* entry offset within the sector */
    int prev;			/* previous entry in the sector, or -1 */
};

#define DE(buf, off)	((struct ps2fs_dirent *)((char *)(buf) + (off)))
#define ALEN(de)	(le16_to_cpu((de)->size_type) & PS2FS_DIRENT_SIZEMASK)

static __u32 dir_size(struct inode *dir)
{
    return le32_to_cpu(PS2FS_I(dir)->raw.size_lo);
}

static void set_dir_size(struct inode *dir, __u32 size)
{
    PS2FS_I(dir)->raw.size_lo = cpu_to_le32(size);
    dir->i_size = size;
}

/* Directory sector n -> (subpart, sector within it) */
static int dir_sector(struct inode *dir, __u32 n, __u16 *sub, __u32 *sector)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(dir->i_sb);
    __u32 zone;

    if (ps2fs_map(dir, n >> sbi->sector_shift, sub, &zone)) {
	ps2fs_error(dir->i_sb, "dir_sector", "directory 0x%08lX: size beyond "
		    "its zones", dir->i_ino);
	return -EIO;
    }
    *sector = (zone << sbi->sector_shift) + (n & ((1 << sbi->sector_shift) - 1));
    return 0;
}

/* Check that the entry at `off' fits the sector tiling */
static int entry_ok(struct inode *dir, const char *buf, int off)
{
    struct ps2fs_dirent *de = DE(buf, off);
    int alen = ALEN(de);

    if (alen % 4 || alen < PS2FS_DIRENT_USED(de->namelen)
	|| off + alen > 512) {
	ps2fs_error(dir->i_sb, "ps2fs_dir", "directory 0x%08lX: bad entry",
		    dir->i_ino);
	return 0;
    }
    return 1;
}

/* Copy directory sector n into buf (sbi->lock held) */
static int read_dir_sector(struct inode *dir, __u32 n, char *buf,
			   struct dir_pos *pos)
{
    struct buffer_head *bh;
    int err;

    if ((err = dir_sector(dir, n, &pos->sub, &pos->sector)))
	return err;
    bh = ps2fs_bread(dir->i_sb, pos->sub, pos->sector);
    if (!bh)
	return -EIO;
    memcpy(buf, bh->b_data, 512);
    brelse(bh);
    pos->n = n;
    return 0;
}

/* Sector of pos in the open transaction */
static char *dir_tsector(struct inode *dir, struct dir_pos *pos, int noload)
{
    struct ps2fs_tblock *tb;

    tb = ps2fs_tblock_get(dir->i_sb, pos->sub, pos->sector / 2, noload);
    return tb ? tb->bh[pos->sector & 1]->b_data : NULL;
}

/*************************************************************************/

/* Find `name' in dir.  Returns 1 and fills pos (and *de if given), 0 if
 * not found, or <0.  sbi->lock held. */
static int find_entry(struct inode *dir, const char *name, int len,
		      struct dir_pos *pos, struct ps2fs_dirent *found)
{
    char *buf;
    __u32 n, nsec = dir_size(dir) / 512;
    int off, prev, err = 0;

    buf = kmalloc(512, GFP_NOFS);
    if (!buf)
	return -ENOMEM;
    for (n = 0; n < nsec && !err; n++) {
	if ((err = read_dir_sector(dir, n, buf, pos)))
	    break;
	for (off = 0, prev = -1; off < 512; prev = off, off += ALEN(DE(buf, off))) {
	    if (!entry_ok(dir, buf, off)) {
		err = -EIO;
		break;
	    }
	    if (DE(buf, off)->namelen == len
		&& !memcmp(DE(buf, off)->name, name, len)) {
		pos->off = off;
		pos->prev = prev;
		if (found)
		    *found = *DE(buf, off);
		err = 1;
		break;
	    }
	}
    }
    kfree(buf);
    return err;
}

/* Does dir hold anything but "." and ".."?  1 = empty, 0 = not, <0 */
static int dir_empty(struct inode *dir)
{
    struct dir_pos pos;
    struct ps2fs_dirent *de;
    char *buf;
    __u32 n, nsec = dir_size(dir) / 512;
    int off, rv = 1;

    buf = kmalloc(512, GFP_NOFS);
    if (!buf)
	return -ENOMEM;
    for (n = 0; n < nsec && rv == 1; n++) {
	if ((rv = read_dir_sector(dir, n, buf, &pos)))
	    break;
	rv = 1;
	for (off = 0; off < 512; off += ALEN(de)) {
	    de = DE(buf, off);
	    if (!entry_ok(dir, buf, off)) {
		rv = -EIO;
		break;
	    }
	    if (de->namelen && !(de->namelen == 1 && de->name[0] == '.')
		&& !(de->namelen == 2 && de->name[0] == '.'
		     && de->name[1] == '.')) {
		rv = 0;
		break;
	    }
	}
    }
    kfree(buf);
    return rv;
}

/* Insert an entry (first fit, else a new sector at the end) */
static int add_entry(struct inode *dir, const char *name, int len,
		     const struct ps2fs_blockinfo *target, __u16 type)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(dir->i_sb);
    struct ps2fs_inode *raw = &PS2FS_I(dir)->raw;
    struct ps2fs_dirent *de;
    struct dir_pos pos;
    char *buf, *p;
    __u32 n, nsec = dir_size(dir) / 512, cap;
    int need = PS2FS_DIRENT_USED(len), off, alen, used, tlen = 0, err = 0;

    buf = kmalloc(512, GFP_NOFS);
    if (!buf)
	return -ENOMEM;
    for (n = 0; n < nsec && !tlen && !err; n++) {
	if ((err = read_dir_sector(dir, n, buf, &pos)))
	    break;
	for (off = 0; off < 512; off += alen) {
	    if (!entry_ok(dir, buf, off)) {
		err = -EIO;
		break;
	    }
	    de = DE(buf, off);
	    alen = ALEN(de);
	    used = PS2FS_DIRENT_USED(de->namelen);
	    if ((!de->namelen || !de->inode) && alen >= need) {
		pos.off = off;			/* reuse an empty slot */
		tlen = alen;
		break;
	    }
	    if (de->namelen && alen - used >= need) {
		if (!(p = dir_tsector(dir, &pos, 0))) {
		    err = -EIO;
		    break;
		}
		DE(p, off)->size_type = cpu_to_le16(
		    (le16_to_cpu(de->size_type) & PS2FS_TYPE_MASK) | used);
		pos.off = off + used;		/* split its slack */
		tlen = alen - used;
		break;
	    }
	}
    }
    kfree(buf);
    if (err)
	return err;

    if (!tlen) {
	/* append a sector, growing the directory by a zone if needed */
	cap = (le32_to_cpu(raw->number_blocks)
	       - le32_to_cpu(raw->number_segdesg)) * sbi->zone_size;
	if (cap < dir_size(dir) + 512) {
	    if ((err = ps2fs_grow(dir, 1)))
		return err;
	    dir->i_blocks = le32_to_cpu(raw->number_blocks) << sbi->sector_shift;
	}
	if ((err = dir_sector(dir, nsec, &pos.sub, &pos.sector)))
	    return err;
	pos.off = 0;
	tlen = 512;
	/* the other half of the block is beyond the size if the new
	 * sector starts a block */
	if (!(p = dir_tsector(dir, &pos, !(pos.sector & 1))))
	    return -EIO;
	set_dir_size(dir, dir_size(dir) + 512);
    } else if (!(p = dir_tsector(dir, &pos, 0))) {
	return -EIO;
    }
    de = DE(p, pos.off);
    memset(de, 0, need);
    de->inode = target->number;
    de->subpart = le16_to_cpu(target->subpart);
    de->namelen = len;
    de->size_type = cpu_to_le16(type | tlen);
    memcpy(de->name, name, len);
    return 0;
}

/* Remove the entry at pos: merge it into its predecessor, or make it an
 * empty slot (dropping the last sector once it is empty) */
static int remove_entry(struct inode *dir, struct dir_pos *pos)
{
    struct ps2fs_dirent *de, *pde;
    char *p;
    int alen;

    if (!(p = dir_tsector(dir, pos, 0)))
	return -EIO;
    de = DE(p, pos->off);
    alen = ALEN(de);
    if (pos->prev >= 0) {
	pde = DE(p, pos->prev);
	pde->size_type = cpu_to_le16(le16_to_cpu(pde->size_type) + alen);
    } else {
	de->inode = 0;
	de->namelen = 0;
	if (pos->n * 512 + alen >= dir_size(dir))
	    set_dir_size(dir, pos->n * 512);
    }
    return 0;
}

/* Times of a changed directory; its inode joins the transaction */
static int dir_touch(struct inode *dir)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(dir->i_sb);
    struct ps2fs_inode *raw = &PS2FS_I(dir)->raw;

    dir->i_atime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
    to_ps2time(&raw->atime, dir->i_mtime, sbi->tzoffset);
    raw->ctime = raw->mtime = raw->atime;
    return ps2fs_write_raw(dir);
}

/*************************************************************************/

/* Name on disk (UTF-8) of a dentry (EUC-JP); returns its length or <0 */
static int disk_name(struct dentry *dentry, char *buf)
{
    int len;

    if (!translate_to_unicode(dentry, buf, PS2FS_NAME_LEN + 1))
	return -ENAMETOOLONG;
    len = strlen(buf);
    if (!len)
	return -EINVAL;
    return len;
}

/* Start a directory change: room in the journal, a usable volume */
static int begin_op(struct super_block *sb)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);

    if (!sbi->writable)
	return -EROFS;
    if (sbi->error)
	return -EIO;
    if (sbi->ntblocks + OP_TBLOCKS > sbi->max_tblocks)
	return ps2fs_commit(sb);
    return 0;
}

static struct dentry *ps2fs_lookup(struct inode *dir, struct dentry *dentry)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(dir->i_sb);
    struct ps2fs_dirent de;
    struct dir_pos pos;
    struct inode *inode = NULL;
    char name[PS2FS_NAME_LEN + 1];
    int len, rv;

    len = disk_name(dentry, name);
    if (len == -ENAMETOOLONG)
	return ERR_PTR(len);
    if (len > 0) {
	down(&sbi->lock);
	rv = find_entry(dir, name, len, &pos, &de);
	up(&sbi->lock);
	if (rv < 0)
	    return ERR_PTR(rv);
	if (rv && de.inode) {
	    inode = iget(dir->i_sb, PS2FS_MAKE_INUM(le32_to_cpu(de.inode),
						    de.subpart));
	    if (!inode)
		return ERR_PTR(-EACCES);
	    if (is_bad_inode(inode)) {
		iput(inode);
		return ERR_PTR(-EIO);
	    }
	}
    }
    d_add(dentry, inode);
    return NULL;
}

static int ps2fs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
    struct inode *dir = filp->f_dentry->d_inode;
    struct ps2fs_sb_info *sbi = PS2FS_SB(dir->i_sb);
    struct ps2fs_dirent *de;
    struct dir_pos pos;
    char *buf, *name;
    __u32 n;
    int off, start, err = 0;
    unsigned int type;

    buf = kmalloc(512 + PS2FS_NAME_LEN + 1, GFP_KERNEL);
    if (!buf)
	return -ENOMEM;
    name = buf + 512;
    while (filp->f_pos < dir->i_size) {
	n = filp->f_pos / 512;
	start = filp->f_pos % 512;
	/* copy the sector so that filldir runs without the lock */
	down(&sbi->lock);
	err = n < dir_size(dir) / 512 ? read_dir_sector(dir, n, buf, &pos) : 1;
	up(&sbi->lock);
	if (err)
	    break;
	for (off = 0; off < 512; off += ALEN(de)) {
	    de = DE(buf, off);
	    if (!entry_ok(dir, buf, off)) {
		err = -EIO;
		break;
	    }
	    if (off < start || !de->namelen)
		continue;
	    memcpy(name, de->name, de->namelen);
	    name[de->namelen] = 0;
	    translate_from_unicode(name);
	    switch (le16_to_cpu(de->size_type) & PS2FS_TYPE_MASK) {
	      case PS2FS_TYPE_DIR:	type = DT_DIR; break;
	      case PS2FS_TYPE_SYMLINK:	type = DT_LNK; break;
	      case PS2FS_TYPE_FILE:	type = DT_REG; break;
	      default:			type = DT_UNKNOWN; break;
	    }
	    if (filldir(dirent, name, strlen(name), (loff_t)n * 512 + off,
			PS2FS_MAKE_INUM(le32_to_cpu(de->inode), de->subpart),
			type) < 0)
		goto out;
	    filp->f_pos = (loff_t)n * 512 + off + ALEN(de);
	}
	if (err)
	    break;
	filp->f_pos = (loff_t)(n + 1) * 512;
    }
  out:
    kfree(buf);
    return err < 0 ? err : 0;
}

/*************************************************************************/

/* Create an inode of `type' and link it as dentry */
static int make_node(struct inode *dir, struct dentry *dentry, int type,
		     int perm, const char *symlink)
{
    struct super_block *sb = dir->i_sb;
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_blockinfo e;
    struct dir_pos pos;
    struct inode *inode;
    char name[PS2FS_NAME_LEN + 1];
    int len, err, created = 0, linked = 0;

    if ((len = disk_name(dentry, name)) < 0)
	return len;
    /* fsck takes any 2-byte name starting with '.' for ".." */
    if (len == 2 && name[0] == '.')
	return -EINVAL;
    down(&sbi->lock);
    if ((err = begin_op(sb)))
	goto out;
    err = find_entry(dir, name, len, &pos, NULL);
    if (err) {
	if (err > 0)
	    err = -EEXIST;
	goto out;
    }
    if ((err = ps2fs_new_inode(dir, type, perm, symlink, &e)))
	goto out;
    created = 1;
    if (!(err = add_entry(dir, name, len, &e, type))) {
	linked = 1;
	err = dir_touch(dir);
    }
  out:
    up(&sbi->lock);
    if (!created)
	return err;
    inode = iget(sb, PS2FS_MAKE_INUM(le32_to_cpu(e.number),
				     le16_to_cpu(e.subpart)));
    if (!inode)
	return err ? err : -EIO;
    if (is_bad_inode(inode) || err) {
	/* not linked: delete_inode frees what new_inode allocated */
	if (!linked && !is_bad_inode(inode))
	    inode->i_nlink = 0;
	iput(inode);
	return err ? err : -EIO;
    }
    d_instantiate(dentry, inode);
    return 0;
}

static int ps2fs_create(struct inode *dir, struct dentry *dentry, int mode)
{
    return make_node(dir, dentry, PS2FS_TYPE_FILE, mode, NULL);
}

static int ps2fs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
    return make_node(dir, dentry, PS2FS_TYPE_DIR, mode, NULL);
}

static int ps2fs_symlink(struct inode *dir, struct dentry *dentry,
			 const char *symname)
{
    if (strlen(symname) > PS2FS_SYMLINK_MAX)
	return -ENAMETOOLONG;
    return make_node(dir, dentry, PS2FS_TYPE_SYMLINK, 0777, symname);
}

/* Find dentry's entry and check that it names dentry's inode */
static int find_own_entry(struct inode *dir, struct dentry *dentry,
			  struct dir_pos *pos)
{
    struct ps2fs_dirent de;
    char name[PS2FS_NAME_LEN + 1];
    int len, rv;

    if ((len = disk_name(dentry, name)) < 0)
	return len;
    rv = find_entry(dir, name, len, pos, &de);
    if (rv < 0)
	return rv;
    if (!rv || PS2FS_MAKE_INUM(le32_to_cpu(de.inode), de.subpart)
	!= dentry->d_inode->i_ino) {
	ps2fs_error(dir->i_sb, "find_own_entry", "directory 0x%08lX changed "
		    "under us", dir->i_ino);
	return -EIO;
    }
    return 0;
}

/* Unlink or rmdir: the zones are freed by delete_inode at the last iput */
static int remove_node(struct inode *dir, struct dentry *dentry, int isdir)
{
    struct super_block *sb = dir->i_sb;
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct inode *inode = dentry->d_inode;
    struct dir_pos pos;
    int err;

    down(&sbi->lock);
    if ((err = begin_op(sb)))
	goto out;
    if (isdir && (err = dir_empty(inode)) <= 0) {
	if (!err)
	    err = -ENOTEMPTY;
	goto out;
    }
    if ((err = find_own_entry(dir, dentry, &pos)))
	goto out;
    if ((err = remove_entry(dir, &pos)) || (err = dir_touch(dir)))
	goto out;
    inode->i_nlink = 0;
    inode->i_ctime = dir->i_ctime;
  out:
    up(&sbi->lock);
    return err;
}

static int ps2fs_unlink(struct inode *dir, struct dentry *dentry)
{
    return remove_node(dir, dentry, 0);
}

static int ps2fs_rmdir(struct inode *dir, struct dentry *dentry)
{
    return remove_node(dir, dentry, 1);
}

static int ps2fs_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
    struct super_block *sb = old_dir->i_sb;
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct inode *inode = old_dentry->d_inode;
    struct inode *target = new_dentry->d_inode;
    struct ps2fs_inode_info *ii = PS2FS_I(inode);
    struct dir_pos pos;
    char name[PS2FS_NAME_LEN + 1];
    char *p;
    __u16 type = le16_to_cpu(ii->raw.mode) & PS2FS_TYPE_MASK;
    int len, err;

    if ((len = disk_name(new_dentry, name)) < 0)
	return len;
    if (len == 2 && name[0] == '.')
	return -EINVAL;
    down(&sbi->lock);
    if ((err = begin_op(sb)))
	goto out;
    /* the only allocation is growing new_dir; fail before changing
     * anything if that could fail */
    if (sbi->free_zones < 2) {
	err = -ENOSPC;
	goto out;
    }
    if (target) {
	if (S_ISDIR(target->i_mode) && (err = dir_empty(target)) <= 0) {
	    if (!err)
		err = -ENOTEMPTY;
	    goto out;
	}
	if ((err = find_own_entry(new_dir, new_dentry, &pos))
	    || (err = remove_entry(new_dir, &pos)))
	    goto out;
    }
    if ((err = find_own_entry(old_dir, old_dentry, &pos))
	|| (err = remove_entry(old_dir, &pos))
	|| (err = add_entry(new_dir, name, len, &ii->raw.inode_block, type)))
	goto out;
    if (S_ISDIR(inode->i_mode) && old_dir != new_dir) {
	/* repoint "..", at byte 12 of the first sector */
	if ((err = dir_sector(inode, 0, &pos.sub, &pos.sector)))
	    goto out;
	if (!(p = dir_tsector(inode, &pos, 0))) {
	    err = -EIO;
	    goto out;
	}
	DE(p, 12)->inode = PS2FS_I(new_dir)->raw.inode_block.number;
	DE(p, 12)->subpart = le16_to_cpu(PS2FS_I(new_dir)->raw.inode_block.subpart);
	if ((err = dir_touch(inode)))
	    goto out;
    }
    if (old_dir != new_dir && (err = dir_touch(old_dir)))
	goto out;
    if ((err = dir_touch(new_dir)))
	goto out;
    if (target) {
	target->i_nlink = 0;
	target->i_ctime = new_dir->i_ctime;
    }
  out:
    up(&sbi->lock);
    return err;
}

static int ps2fs_dir_fsync(struct file *file, struct dentry *dentry,
			   int datasync)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(dentry->d_inode->i_sb);
    int err;

    down(&sbi->lock);
    err = ps2fs_commit(dentry->d_inode->i_sb);
    up(&sbi->lock);
    return err;
}

/*************************************************************************/

struct file_operations ps2fs_dops = {
    read:	generic_read_dir,
    readdir:	ps2fs_readdir,
    fsync:	ps2fs_dir_fsync,
};

struct inode_operations ps2fs_dir_iops = {
    lookup:	ps2fs_lookup,
    create:	ps2fs_create,
    mkdir:	ps2fs_mkdir,
    symlink:	ps2fs_symlink,
    unlink:	ps2fs_unlink,
    rmdir:	ps2fs_rmdir,
    rename:	ps2fs_rename,
    setattr:	ps2fs_setattr,
};
