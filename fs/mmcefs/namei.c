/*
 *  linux/fs/mmcefs/namei.c
 *
 *  MMCE SD card filesystem: directories.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/mm.h>
#include <linux/errno.h>

#include "mmcefs.h"

/*
 * The card's FAT/exFAT names are case-insensitive, so the dcache folds
 * ASCII case: otherwise one file could get two dentries and two inodes,
 * each with its own IOP descriptor.
 */
static int mmcefs_d_hash(struct dentry *dentry, struct qstr *qstr)
{
	unsigned long hash = init_name_hash();
	unsigned int i;

	for (i = 0; i < qstr->len; i++)
		hash = partial_name_hash(tolower(qstr->name[i]), hash);
	qstr->hash = end_name_hash(hash);
	return 0;
}

static int mmcefs_d_compare(struct dentry *dentry, struct qstr *a,
			    struct qstr *b)
{
	unsigned int i;

	if (a->len != b->len)
		return 1;
	for (i = 0; i < a->len; i++)
		if (tolower(a->name[i]) != tolower(b->name[i]))
			return 1;
	return 0;
}

/* Negative dentries could carry another case into a create; recheck */
static int mmcefs_d_revalidate(struct dentry *dentry, int flags)
{
	return dentry->d_inode != NULL;
}

struct dentry_operations mmcefs_dentry_ops = {
	d_revalidate:	mmcefs_d_revalidate,
	d_hash:		mmcefs_d_hash,
	d_compare:	mmcefs_d_compare,
};

/* Names the card's FAT filesystem cannot store */
static int mmcefs_bad_name(struct qstr *q)
{
	unsigned int i;

	if (q->len > MMCEFS_NAME_MAX)
		return -ENAMETOOLONG;
	for (i = 0; i < q->len; i++)
		if (q->name[i] < 0x20 || strchr("\\:*?\"<>|", q->name[i]))
			return -EINVAL;
	return 0;
}

static struct dentry *mmcefs_lookup(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = NULL;
	struct fx_stat *st;
	char *path;
	int err;

	dentry->d_op = &mmcefs_dentry_ops;
	err = mmcefs_bad_name(&dentry->d_name);
	if (err == -ENAMETOOLONG)
		return ERR_PTR(err);
	if (err || MMCEFS_I(dir)->dead)
		goto out;
	path = mmcefs_build_path(dentry);
	if (IS_ERR(path))
		return ERR_PTR(PTR_ERR(path));
	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st) {
		mmcefs_put_path(path);
		return ERR_PTR(-ENOMEM);
	}
	fx_lock();
	err = fx_getstat(path, st);
	fx_unlock();
	mmcefs_put_path(path);
	if (!err) {
		inode = mmcefs_build_inode(dir->i_sb, st, 0);
		if (!inode)
			err = -ENOMEM;
	}
	kfree(st);
	if (err && err != -ENOENT)
		return ERR_PTR(err);
out:
	d_add(dentry, inode);
	return NULL;
}

static int mmcefs_instantiate(struct inode *dir, struct dentry *dentry,
			      int is_dir)
{
	struct fx_stat st;
	struct inode *inode;

	memset(&st, 0, sizeof(st));
	st.mode = is_dir ? FX_S_IFDIR : FX_S_IFREG;
	inode = mmcefs_build_inode(dir->i_sb, &st, 0);
	if (!inode)
		return -ENOMEM;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	d_instantiate(dentry, inode);
	dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	return 0;
}

static int mmcefs_create(struct inode *dir, struct dentry *dentry, int mode)
{
	char *path;
	int err;

	err = mmcefs_bad_name(&dentry->d_name);
	if (err)
		return err;
	path = mmcefs_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	fx_lock();
	err = fx_open(path, FX_O_WRONLY | FX_O_CREAT | FX_O_EXCL, 0666);
	if (err >= 0)
		err = fx_close(err);
	fx_unlock();
	mmcefs_put_path(path);
	return err ? err : mmcefs_instantiate(dir, dentry, 0);
}

static int mmcefs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	char *path;
	int err;

	err = mmcefs_bad_name(&dentry->d_name);
	if (err)
		return err;
	path = mmcefs_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	fx_lock();
	err = fx_mkdir(path, 0777);
	fx_unlock();
	mmcefs_put_path(path);
	return err ? err : mmcefs_instantiate(dir, dentry, 1);
}

/* Close the inode's descriptor before its file goes away or moves */
static void mmcefs_release_fd(struct inode *inode)
{
	if (S_ISREG(inode->i_mode)) {
		filemap_fdatasync(inode->i_mapping);
		filemap_fdatawait(inode->i_mapping);
		mmcefs_fd_close(inode);
	}
}

static int mmcefs_remove(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	char *path;
	int err;

	path = mmcefs_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	mmcefs_release_fd(inode);
	fx_lock();
	if (MMCEFS_I(inode)->fd >= 0) {	/* reopened meanwhile */
		fx_close(MMCEFS_I(inode)->fd);
		MMCEFS_I(inode)->fd = -1;
	}
	err = S_ISDIR(inode->i_mode) ? fx_rmdir(path) : fx_remove(path);
	if (!err) {
		MMCEFS_I(inode)->dead = 1;
		inode->i_nlink = 0;
	}
	fx_unlock();
	mmcefs_put_path(path);
	if (!err)
		dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	return err;
}

static int mmcefs_rename(struct inode *old_dir, struct dentry *old_dentry,
			 struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *inode = old_dentry->d_inode;
	char *from, *to;
	int err;

	err = mmcefs_bad_name(&new_dentry->d_name);
	if (err)
		return err;
	from = mmcefs_build_path(old_dentry);
	if (IS_ERR(from))
		return PTR_ERR(from);
	to = mmcefs_build_path(new_dentry);
	if (IS_ERR(to)) {
		mmcefs_put_path(from);
		return PTR_ERR(to);
	}
	/* the descriptor is reopened by the new path when needed */
	mmcefs_release_fd(inode);
	fx_lock();
	err = fx_rename(from, to);
	fx_unlock();
	/*
	 * Remove an existing target only once the device has shown it can
	 * rename (mmceman cannot), so a failing rename never loses it.
	 */
	if (err == -EEXIST && new_dentry->d_inode) {
		err = mmcefs_remove(new_dir, new_dentry);
		if (!err) {
			fx_lock();
			err = fx_rename(from, to);
			fx_unlock();
		}
	}
	mmcefs_put_path(from);
	mmcefs_put_path(to);
	if (!err)
		old_dir->i_mtime = new_dir->i_mtime = CURRENT_TIME;
	return err;
}

struct inode_operations mmcefs_dir_inode_operations = {
	create:		mmcefs_create,
	lookup:		mmcefs_lookup,
	unlink:		mmcefs_remove,
	mkdir:		mmcefs_mkdir,
	rmdir:		mmcefs_remove,
	rename:		mmcefs_rename,
	setattr:	mmcefs_setattr,
};

/*
 * f_pos 0 and 1 are "." and "..", n >= 2 is the (n-2)th entry.  An open
 * directory keeps its IOP directory descriptor and reads sequentially;
 * other positions are reached by reopening and skipping.
 */
struct mmcefs_dir_priv {
	int		fd;		/* IOP directory descriptor */
	loff_t		pos;		/* f_pos of the next entry */
	int		pending;	/* de was read but not delivered */
	struct fx_dirent de;
	char		*path;
};

static int mmcefs_dir_open(struct inode *inode, struct file *filp)
{
	struct mmcefs_dir_priv *priv;
	char *path;
	int fd;

	if (MMCEFS_I(inode)->dead)
		return -ENOENT;
	path = mmcefs_build_path(filp->f_dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	priv = kmalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		mmcefs_put_path(path);
		return -ENOMEM;
	}
	fx_lock();
	fd = fx_dopen(path);
	fx_unlock();
	if (fd < 0) {
		mmcefs_put_path(path);
		kfree(priv);
		return fd;
	}
	priv->fd = fd;
	priv->pos = 2;
	priv->pending = 0;
	priv->path = path;
	filp->private_data = priv;
	return 0;
}

static int mmcefs_dir_release(struct inode *inode, struct file *filp)
{
	struct mmcefs_dir_priv *priv = filp->private_data;

	if (priv) {
		fx_lock();
		if (priv->fd >= 0)
			fx_dclose(priv->fd);
		fx_unlock();
		mmcefs_put_path(priv->path);
		kfree(priv);
		filp->private_data = NULL;
	}
	return 0;
}

/* Next real entry ("." and ".." from the card are skipped); fx_lock() held */
static int mmcefs_next_entry(struct mmcefs_dir_priv *priv)
{
	int rv;

	do {
		rv = fx_dread(priv->fd, &priv->de);
	} while (rv > 0 && (!strcmp(priv->de.name, ".") ||
			    !strcmp(priv->de.name, "..")));
	return rv;
}

static int mmcefs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct mmcefs_dir_priv *priv = filp->private_data;
	loff_t pos = filp->f_pos;
	int rv = 0, type;

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
	if (MMCEFS_I(inode)->dead)
		return 0;

	if (pos != priv->pos) {		/* seeked: reopen and skip */
		fx_lock();
		if (priv->fd >= 0)
			fx_dclose(priv->fd);
		priv->fd = fx_dopen(priv->path);
		priv->pos = 2;
		priv->pending = 0;
		while (priv->fd >= 0 && priv->pos < pos &&
		       mmcefs_next_entry(priv) > 0)
			priv->pos++;
		fx_unlock();
		if (priv->fd < 0)
			return priv->fd;
		pos = priv->pos;
	}
	/* filldir() copies to user space, so it runs without fx_lock() */
	for (;;) {
		if (!priv->pending) {
			fx_lock();
			rv = mmcefs_next_entry(priv);
			fx_unlock();
			if (rv <= 0)
				break;
			priv->pending = 1;
		}
		type = (priv->de.stat.mode & FX_S_IFMT) == FX_S_IFDIR ?
			DT_DIR : DT_REG;
		if (filldir(dirent, priv->de.name, strlen(priv->de.name), pos,
			    ((inode->i_ino << 12) + pos) | 1, type) < 0)
			break;
		priv->pending = 0;
		priv->pos = ++pos;
	}
	filp->f_pos = pos;
	return rv < 0 ? rv : 0;
}

struct file_operations mmcefs_dir_operations = {
	read:		generic_read_dir,
	readdir:	mmcefs_readdir,
	open:		mmcefs_dir_open,
	release:	mmcefs_dir_release,
};
