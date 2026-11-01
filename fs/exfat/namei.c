/*
 *  linux/fs/exfat/namei.c
 *
 *  exFAT filesystem for Linux 2.4: directories and name handling.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/locks.h>
#include <linux/errno.h>

#include "exfat_fs.h"

/*
 * exFAT names are case-insensitive, and FatFs ignores trailing dots and
 * spaces.  The dcache must agree, or one file could get two dentries (and
 * two inodes with separate FatFs file objects).  Case folding uses the same
 * ff_wtoupper() that FatFs uses to compare names.
 */

static int exfat_strip_len(const struct qstr *q)
{
	int len = q->len;

	while (len && (q->name[len - 1] == '.' || q->name[len - 1] == ' '))
		len--;
	return len;
}

/* Next folded UTF-16 unit(s) of a name; invalid bytes are taken as-is */
static int exfat_fold_next(struct exfat_sb_info *sbi, const unsigned char *s,
			   int len, DWORD *u1, DWORD *u2)
{
	DWORD c;
	int n;

	n = exfat_next_char(sbi, s, len, &c);
	if (n <= 0) {
		*u1 = 0x80000000 | s[0];
		*u2 = 0;
		return 1;
	}
	if (c >= 0x10000) {
		c -= 0x10000;
		*u1 = 0xd800 | (c >> 10);
		*u2 = 0xdc00 | (c & 0x3ff);
	} else {
		*u1 = ff_wtoupper(c);
		*u2 = 0;
	}
	return n;
}

static int exfat_d_hash(struct dentry *dentry, struct qstr *qstr)
{
	struct exfat_sb_info *sbi = EXFAT_SB(dentry->d_sb);
	const unsigned char *name = qstr->name;
	int len = exfat_strip_len(qstr), n;
	unsigned long hash = init_name_hash();
	DWORD u1, u2;

	while (len > 0) {
		n = exfat_fold_next(sbi, name, len, &u1, &u2);
		hash = partial_name_hash(u1, hash);
		if (u2)
			hash = partial_name_hash(u2, hash);
		name += n;
		len -= n;
	}
	qstr->hash = end_name_hash(hash);
	return 0;
}

static int exfat_d_compare(struct dentry *dentry, struct qstr *a,
			   struct qstr *b)
{
	struct exfat_sb_info *sbi = EXFAT_SB(dentry->d_sb);
	const unsigned char *pa = a->name, *pb = b->name;
	int la = exfat_strip_len(a), lb = exfat_strip_len(b), na, nb;
	DWORD a1, a2, b1, b2;

	while (la > 0 && lb > 0) {
		na = exfat_fold_next(sbi, pa, la, &a1, &a2);
		nb = exfat_fold_next(sbi, pb, lb, &b1, &b2);
		if (a1 != b1 || a2 != b2)
			return 1;
		pa += na;
		la -= na;
		pb += nb;
		lb -= nb;
	}
	return la || lb;
}

/*
 * A negative dentry may have been created by a lookup that used different
 * case; reusing it for a create would store that case on disk.  Always
 * look negative dentries up again so creates get the name as typed.
 */
static int exfat_d_revalidate(struct dentry *dentry, int flags)
{
	return dentry->d_inode != NULL;
}

struct dentry_operations exfat_dentry_ops = {
	d_revalidate:	exfat_d_revalidate,
	d_hash:		exfat_d_hash,
	d_compare:	exfat_d_compare,
};

static struct dentry *exfat_lookup(struct inode *dir, struct dentry *dentry)
{
	struct exfat_sb_info *sbi = EXFAT_SB(dir->i_sb);
	struct inode *inode = NULL;
	FILINFO *fno;
	WCHAR *path;
	FRESULT res;

	dentry->d_op = &exfat_dentry_ops;
	if (EXFAT_I(dir)->flags & EXFAT_I_DEAD)
		goto out;
	path = exfat_build_path(dentry);
	if (IS_ERR(path)) {
		if (PTR_ERR(path) == -EINVAL)
			goto out;	/* a name exFAT cannot hold: not found */
		return ERR_PTR(PTR_ERR(path));
	}
	fno = kmalloc(sizeof(*fno), GFP_KERNEL);
	if (!fno) {
		exfat_put_path(path);
		return ERR_PTR(-ENOMEM);
	}
	down(&sbi->lock);
	res = f_stat(path, fno);
	up(&sbi->lock);
	exfat_put_path(path);
	if (res == FR_OK) {
		inode = exfat_build_inode(dir->i_sb, fno);
		kfree(fno);
		if (!inode)
			return ERR_PTR(-ENOMEM);
	} else {
		kfree(fno);
		if (res != FR_NO_FILE && res != FR_NO_PATH &&
		    res != FR_INVALID_NAME)
			return ERR_PTR(exfat_errno(res));
	}
out:
	d_add(dentry, inode);
	return NULL;
}

/* Instantiate @dentry with a new, empty object */
static int exfat_instantiate(struct inode *dir, struct dentry *dentry,
			     BYTE attr)
{
	struct inode *inode;
	FILINFO *fno;

	fno = kmalloc(sizeof(*fno), GFP_KERNEL);
	if (!fno)
		return -ENOMEM;
	memset(fno, 0, sizeof(*fno));
	fno->fattrib = attr;
	exfat_time_unix2fat(CURRENT_TIME, &fno->fdate, &fno->ftime);
	inode = exfat_build_inode(dir->i_sb, fno);
	kfree(fno);
	if (!inode)
		return -ENOMEM;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	d_instantiate(dentry, inode);
	dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	return 0;
}

static int exfat_create(struct inode *dir, struct dentry *dentry, int mode)
{
	struct exfat_sb_info *sbi = EXFAT_SB(dir->i_sb);
	WCHAR *path;
	FIL *fil;
	FRESULT res;

	if (EXFAT_I(dir)->flags & EXFAT_I_DEAD)
		return -ENOENT;
	path = exfat_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	fil = kmalloc(sizeof(*fil), GFP_KERNEL);
	if (!fil) {
		exfat_put_path(path);
		return -ENOMEM;
	}
	down(&sbi->lock);
	res = f_open(fil, path, FA_CREATE_NEW | FA_READ | FA_WRITE);
	if (res == FR_OK)
		res = f_close(fil);
	up(&sbi->lock);
	kfree(fil);
	exfat_put_path(path);
	if (res == FR_DENIED)
		return -ENOSPC;		/* the directory cannot grow */
	if (res != FR_OK)
		return exfat_errno(res);
	return exfat_instantiate(dir, dentry, AM_ARC);
}

static int exfat_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct exfat_sb_info *sbi = EXFAT_SB(dir->i_sb);
	WCHAR *path;
	FRESULT res;

	if (EXFAT_I(dir)->flags & EXFAT_I_DEAD)
		return -ENOENT;
	path = exfat_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	down(&sbi->lock);
	res = f_mkdir(path);
	up(&sbi->lock);
	exfat_put_path(path);
	if (res == FR_DENIED)
		return -ENOSPC;
	if (res != FR_OK)
		return exfat_errno(res);
	return exfat_instantiate(dir, dentry, AM_DIR);
}

/*
 * Remove the object at @path, with sbi->lock held.  Linux allows removing
 * read-only files, FatFs does not, so the attribute is cleared first.
 */
static FRESULT exfat_remove(struct inode *inode, const WCHAR *path)
{
	struct exfat_inode_info *ei = EXFAT_I(inode);
	FRESULT res;

	if (ei->fil) {			/* reopened since the flush; close again */
		f_close(ei->fil);
		kfree(ei->fil);
		ei->fil = NULL;
	}
	if (ei->attr & AM_RDO) {
		res = f_chmod(path, 0, AM_RDO);
		if (res != FR_OK)
			return res;
		ei->attr &= ~AM_RDO;
	}
	res = f_unlink(path);
	if (res == FR_OK) {
		ei->flags |= EXFAT_I_DEAD;
		inode->i_nlink = 0;
	}
	return res;
}

/* Write back an inode's dirty pages before its file object goes away */
static void exfat_flush_pages(struct inode *inode)
{
	if (S_ISREG(inode->i_mode)) {
		filemap_fdatasync(inode->i_mapping);
		filemap_fdatawait(inode->i_mapping);
	}
}

static int exfat_unlink_common(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct exfat_sb_info *sbi = EXFAT_SB(dir->i_sb);
	WCHAR *path;
	FRESULT res;

	path = exfat_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	exfat_flush_pages(inode);
	down(&sbi->lock);
	res = exfat_remove(inode, path);
	up(&sbi->lock);
	exfat_put_path(path);
	if (res == FR_DENIED && S_ISDIR(inode->i_mode))
		return -ENOTEMPTY;
	if (res != FR_OK)
		return exfat_errno(res);
	inode->i_ctime = dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	return 0;
}

static int exfat_unlink(struct inode *dir, struct dentry *dentry)
{
	return exfat_unlink_common(dir, dentry);
}

static int exfat_rmdir(struct inode *dir, struct dentry *dentry)
{
	return exfat_unlink_common(dir, dentry);
}

static int exfat_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	struct exfat_sb_info *sbi = EXFAT_SB(old_dir->i_sb);
	struct inode *inode = old_dentry->d_inode;
	struct inode *target = new_dentry->d_inode;
	struct exfat_inode_info *ei = EXFAT_I(inode);
	WCHAR *old_path, *new_path;
	BYTE mode = 0;
	FRESULT res = FR_OK, res2;

	old_path = exfat_build_path(old_dentry);
	if (IS_ERR(old_path))
		return PTR_ERR(old_path);
	new_path = exfat_build_path(new_dentry);
	if (IS_ERR(new_path)) {
		exfat_put_path(old_path);
		return PTR_ERR(new_path);
	}
	exfat_flush_pages(inode);
	if (target)
		exfat_flush_pages(target);

	down(&sbi->lock);
	if (target)		/* exFAT cannot replace atomically */
		res = exfat_remove(target, new_path);
	/*
	 * An open file object refers to the old directory entry, which
	 * f_rename() moves; close it around the rename and reopen it.
	 */
	if (res == FR_OK && ei->fil) {
		mode = ei->fil->flag & (FA_READ | FA_WRITE);
		res = f_close(ei->fil);
		if (res != FR_OK) {
			kfree(ei->fil);
			ei->fil = NULL;
		}
	}
	if (res == FR_OK) {
		res = f_rename(old_path, new_path);
		if (ei->fil) {
			res2 = f_open(ei->fil, res == FR_OK ? new_path : old_path,
				      mode);
			if (res2 != FR_OK) {
				kfree(ei->fil);
				ei->fil = NULL;
			}
		}
	}
	up(&sbi->lock);
	exfat_put_path(old_path);
	exfat_put_path(new_path);

	if (res == FR_DENIED)
		return target && S_ISDIR(target->i_mode) ? -ENOTEMPTY : -ENOSPC;
	if (res != FR_OK)
		return exfat_errno(res);
	if (target)
		target->i_ctime = CURRENT_TIME;
	inode->i_ctime = CURRENT_TIME;
	old_dir->i_mtime = old_dir->i_ctime = CURRENT_TIME;
	new_dir->i_mtime = new_dir->i_ctime = CURRENT_TIME;
	return 0;
}

struct inode_operations exfat_dir_inode_operations = {
	create:		exfat_create,
	lookup:		exfat_lookup,
	unlink:		exfat_unlink,
	mkdir:		exfat_mkdir,
	rmdir:		exfat_rmdir,
	rename:		exfat_rename,
	setattr:	exfat_setattr,
};

/*
 * Directory reading.  f_pos 0 and 1 are "." and "..", an entry's f_pos is
 * 2 + the byte offset of its entry set in the directory, so positions stay
 * valid when other entries are removed.  An open directory keeps its FatFs
 * directory object, so sequential reads continue where they left off; any
 * other position (glibc seeks back when its buffer is full) is reached by
 * rewinding to the first entry at or after it.
 */
struct exfat_dir_priv {
	DIR		dir;
	FILINFO		fno;
	loff_t		pos;		/* f_pos that continues from dir */
	loff_t		epos;		/* f_pos of the pending entry */
	int		pending;	/* fno is read but was not delivered */
	unsigned char	name[EXFAT_NAMEBUF];
};

/* Read the next entry into priv->fno; 1 if there is one.  sbi->lock held. */
static int exfat_dir_next(struct exfat_dir_priv *priv, FRESULT *res)
{
	*res = f_readdir(&priv->dir, &priv->fno);
	if (*res != FR_OK || !priv->fno.fname[0])
		return 0;
	priv->epos = 2 + (loff_t)priv->dir.blk_ofs;
	priv->pending = 1;
	return 1;
}

static int exfat_dir_open(struct inode *inode, struct file *filp)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_dir_priv *priv;
	WCHAR *path;
	FRESULT res;

	if (EXFAT_I(inode)->flags & EXFAT_I_DEAD)
		return -ENOENT;
	path = exfat_build_path(filp->f_dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	priv = kmalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		exfat_put_path(path);
		return -ENOMEM;
	}
	down(&sbi->lock);
	res = f_opendir(&priv->dir, path);
	up(&sbi->lock);
	exfat_put_path(path);
	if (res != FR_OK) {
		kfree(priv);
		return exfat_errno(res);
	}
	priv->pos = 2;
	priv->pending = 0;
	filp->private_data = priv;
	return 0;
}

static int exfat_dir_release(struct inode *inode, struct file *filp)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_dir_priv *priv = filp->private_data;

	if (priv) {
		down(&sbi->lock);
		f_closedir(&priv->dir);
		up(&sbi->lock);
		kfree(priv);
		filp->private_data = NULL;
	}
	return 0;
}

static int exfat_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_dir_priv *priv = filp->private_data;
	loff_t pos = filp->f_pos;
	FRESULT res = FR_OK;
	int len;

	if (pos == 0) {
		if (filldir(dirent, ".", 1, pos, inode->i_ino, DT_DIR) < 0)
			return 0;
		filp->f_pos = ++pos;
	}
	if (pos == 1) {
		if (filldir(dirent, "..", 2, pos,
			    filp->f_dentry->d_parent->d_inode->i_ino,
			    DT_DIR) < 0)
			return 0;
		filp->f_pos = ++pos;
	}
	if (EXFAT_I(inode)->flags & EXFAT_I_DEAD)
		return 0;

	if (pos != priv->pos) {		/* seeked: rewind, find the entry */
		down(&sbi->lock);
		res = f_readdir(&priv->dir, NULL);
		priv->pending = 0;
		while (res == FR_OK && exfat_dir_next(priv, &res) &&
		       priv->epos < pos)
			priv->pending = 0;
		up(&sbi->lock);
	}
	/*
	 * filldir() copies to user space and may fault into this
	 * filesystem, so it is called without sbi->lock.
	 */
	while (res == FR_OK) {
		if (!priv->pending) {
			down(&sbi->lock);
			len = exfat_dir_next(priv, &res);
			up(&sbi->lock);
			if (!len)
				break;
		}
		len = exfat_uni_to_name(sbi, priv->fno.fname, priv->name,
					sizeof(priv->name));
		/* d_ino must be nonzero; real inode numbers come from lookup */
		if (filldir(dirent, (char *)priv->name, len, priv->epos,
			    ((inode->i_ino << 12) + priv->epos) | 1,
			    (priv->fno.fattrib & AM_DIR) ? DT_DIR : DT_REG) < 0) {
			pos = priv->epos;
			break;
		}
		priv->pending = 0;
		pos = priv->epos + 1;
	}
	priv->pos = pos;
	filp->f_pos = pos;
	return exfat_errno(res);
}

static int exfat_dir_fsync(struct file *filp, struct dentry *dentry,
			   int datasync)
{
	return fsync_no_super(EXFAT_SB(dentry->d_sb)->dev);
}

struct file_operations exfat_dir_operations = {
	read:		generic_read_dir,
	readdir:	exfat_readdir,
	open:		exfat_dir_open,
	release:	exfat_dir_release,
	fsync:		exfat_dir_fsync,
};
