/*
 *  linux/fs/exfat/file.c
 *
 *  exFAT filesystem for Linux 2.4: regular files.
 *
 *  File data goes through the page cache; ->readpage and ->commit_write
 *  move it to and from the inode's FatFs file object.  Writes are passed
 *  to FatFs immediately, so pages are only dirtied by shared mmaps.
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
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/locks.h>
#include <linux/errno.h>

#include "exfat_fs.h"

#define EXFAT_CLMT_MIN		64	/* clusters before fast seek pays off */
#define EXFAT_CLMT_MAX		4096	/* largest link map (DWORDs) */

/* Source of zeroes for filling holes */
static BYTE exfat_zero_buf[PAGE_CACHE_SIZE];

/*
 * Make sure @inode has an open FatFs file object, with write access if
 * @write is set.  @dentry is an alias of @inode used to build the path if
 * the file has to be opened.  Called without sbi->lock.
 */
int exfat_fil_get(struct inode *inode, struct dentry *dentry, int write)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	WCHAR *path;
	FIL *fil;
	FRESULT res;
	BYTE mode;
	int err = 0;

	if (ei->flags & EXFAT_I_DEAD)
		return -ESTALE;
	if (ei->fil && (!write || (ei->fil->flag & FA_WRITE)))
		return 0;
	if (!dentry)
		return -EIO;
	path = exfat_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	fil = kmalloc(sizeof(*fil), GFP_KERNEL);
	if (!fil) {
		exfat_put_path(path);
		return -ENOMEM;
	}

	/* open read-write whenever possible so later writers can share it */
	mode = FA_READ;
	if (write || (!(inode->i_sb->s_flags & MS_RDONLY) &&
		      !(ei->attr & AM_RDO)))
		mode |= FA_WRITE;

	down(&sbi->lock);
	if (ei->flags & EXFAT_I_DEAD) {
		err = -ESTALE;
	} else if (!ei->fil || (write && !(ei->fil->flag & FA_WRITE))) {
		if (ei->fil) {		/* reopen a read-only object for writing */
			f_close(ei->fil);
			kfree(ei->fil);
			ei->fil = NULL;
		}
		res = f_open(fil, path, mode);
		if (res == FR_DENIED && !write && (mode & FA_WRITE))
			res = f_open(fil, path, FA_READ);
		if (res == FR_OK) {
			ei->fil = fil;
			fil = NULL;
		} else
			err = exfat_errno(res);
	}
	up(&sbi->lock);

	if (fil)
		kfree(fil);
	exfat_put_path(path);
	return err;
}

/* As exfat_fil_get(), for callers that have no dentry at hand */
static int exfat_fil_get_alias(struct inode *inode, int write)
{
	struct dentry *dentry;
	int err;

	if (EXFAT_I(inode)->fil && !write)
		return 0;
	dentry = d_find_alias(inode);
	err = exfat_fil_get(inode, dentry, write);
	if (dentry)
		dput(dentry);
	return err;
}

/*
 * Build the cluster link map of a file so that backward seeks do not have
 * to walk the cluster chain from its start.  Best effort; called with
 * sbi->lock held.
 */
static void exfat_fil_map(struct exfat_inode_info *ei)
{
	FIL *fp = ei->fil;
	DWORD need;

	if (!ei->cltbl) {
		ei->cltbl = kmalloc(32 * sizeof(DWORD), GFP_NOFS);
		if (!ei->cltbl)
			return;
		ei->cltbl_len = 32;
	}
	ei->cltbl[0] = ei->cltbl_len;
	fp->cltbl = ei->cltbl;
	if (f_lseek(fp, CREATE_LINKMAP) == FR_OK)
		return;
	fp->cltbl = NULL;
	need = ei->cltbl[0];		/* required size, if that was the error */
	if (need <= ei->cltbl_len || need > EXFAT_CLMT_MAX)
		return;
	kfree(ei->cltbl);
	ei->cltbl = kmalloc(need * sizeof(DWORD), GFP_NOFS);
	if (!ei->cltbl) {
		ei->cltbl_len = 0;
		return;
	}
	ei->cltbl_len = need;
	ei->cltbl[0] = need;
	fp->cltbl = ei->cltbl;
	if (f_lseek(fp, CREATE_LINKMAP) != FR_OK)
		fp->cltbl = NULL;
}

/*
 * Move the file pointer, with sbi->lock held.  The link map is dropped
 * before anything that may change the cluster chain.
 */
static FRESULT exfat_fil_seek(struct inode *inode, loff_t pos, int write)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	FIL *fp = ei->fil;

	if (write)
		fp->cltbl = NULL;
	else if (!fp->cltbl && (FSIZE_t)pos < fp->fptr &&
		 fp->obj.objsize > (FSIZE_t)EXFAT_CLMT_MIN * sbi->clu_size)
		exfat_fil_map(ei);
	return f_lseek(fp, pos);
}

/* Extend the file with zeroes up to @size; sbi->lock held */
static FRESULT exfat_fil_extend(struct inode *inode, FSIZE_t size)
{
	FIL *fp = EXFAT_I(inode)->fil;
	FRESULT res;
	UINT n, bw;

	res = exfat_fil_seek(inode, fp->obj.objsize, 1);
	while (res == FR_OK && fp->fptr < size) {
		n = size - fp->fptr < sizeof(exfat_zero_buf) ?
			(UINT)(size - fp->fptr) : sizeof(exfat_zero_buf);
		res = f_write(fp, exfat_zero_buf, n, &bw);
		if (res == FR_OK && bw < n)
			return FR_DENIED;	/* volume full */
	}
	return res;
}

/* Fill a locked page from the file; zeroes beyond end of file */
static int exfat_fill_page(struct inode *inode, struct dentry *dentry,
			   struct page *page)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	loff_t pos = (loff_t)page->index << PAGE_CACHE_SHIFT;
	char *kaddr;
	FRESULT res = FR_OK;
	UINT br = 0;
	int err = 0;

	kaddr = kmap(page);
	if (pos < inode->i_size) {
		err = dentry ? exfat_fil_get(inode, dentry, 0) :
			exfat_fil_get_alias(inode, 0);
		if (!err) {
			down(&sbi->lock);
			if (ei->fil) {
				res = exfat_fil_seek(inode, pos, 0);
				if (res == FR_OK)
					res = f_read(ei->fil, kaddr,
						     PAGE_CACHE_SIZE, &br);
			} else
				res = FR_INVALID_OBJECT;	/* closed meanwhile */
			up(&sbi->lock);
			err = exfat_errno(res);
		}
	}
	if (!err) {
		memset(kaddr + br, 0, PAGE_CACHE_SIZE - br);
		flush_dcache_page(page);
		SetPageUptodate(page);
	}
	kunmap(page);
	return err;
}

static int exfat_readpage(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	int err;

	err = exfat_fill_page(inode, file ? file->f_dentry : NULL, page);
	if (err)
		SetPageError(page);
	UnlockPage(page);
	return err;
}

/*
 * Write @count bytes at @offset of @page to the file.  Extends the file if
 * needed, filling any gap with zeroes.  Called without sbi->lock.
 */
static int exfat_write_range(struct inode *inode, struct dentry *dentry,
			     struct page *page, unsigned offset,
			     unsigned count)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	loff_t pos = ((loff_t)page->index << PAGE_CACHE_SHIFT) + offset;
	char *kaddr;
	FRESULT res;
	UINT bw = 0;
	int err;

	err = dentry ? exfat_fil_get(inode, dentry, 1) :
		exfat_fil_get_alias(inode, 1);
	if (err)
		return err;
	kaddr = kmap(page);
	down(&sbi->lock);
	if (!ei->fil)
		res = FR_INVALID_OBJECT;
	else if ((FSIZE_t)pos > ei->fil->obj.objsize)
		res = exfat_fil_extend(inode, pos);
	else
		res = FR_OK;
	if (res == FR_OK)
		res = exfat_fil_seek(inode, pos, 1);
	if (res == FR_OK)
		res = f_write(ei->fil, kaddr + offset, count, &bw);
	/* keep i_size in step with what FatFs has allocated */
	if (ei->fil && ei->fil->obj.objsize > (FSIZE_t)inode->i_size)
		inode->i_size = ei->fil->obj.objsize;
	up(&sbi->lock);
	kunmap(page);

	if (res == FR_DENIED)
		return -ENOSPC;
	if (res != FR_OK)
		return exfat_errno(res);
	if (bw < count)
		return -ENOSPC;
	return 0;
}

static int exfat_writepage(struct page *page)
{
	struct inode *inode = page->mapping->host;
	unsigned long end_index = inode->i_size >> PAGE_CACHE_SHIFT;
	unsigned count = PAGE_CACHE_SIZE;
	int err = 0;

	if (page->index >= end_index)
		count = inode->i_size & (PAGE_CACHE_SIZE - 1);
	if (page->index <= end_index && count)
		err = exfat_write_range(inode, NULL, page, 0, count);
	if (err)
		printk(KERN_WARNING "exfat: writeback of inode %lu failed "
		       "(%d)\n", inode->i_ino, err);
	UnlockPage(page);
	return err;
}

static int exfat_prepare_write(struct file *file, struct page *page,
			       unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	char *kaddr;

	if (Page_Uptodate(page))
		return 0;
	if (from == 0 && to == PAGE_CACHE_SIZE) {
		/*
		 * A fault in copy_from_user still commits the whole range,
		 * so never leave stale memory in it.
		 */
		kaddr = kmap(page);
		memset(kaddr, 0, PAGE_CACHE_SIZE);
		kunmap(page);
		return 0;
	}
	return exfat_fill_page(inode, file->f_dentry, page);
}

static int exfat_commit_write(struct file *file, struct page *page,
			      unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	int err;

	if (from == to)
		return 0;
	err = exfat_write_range(inode, file->f_dentry, page, from, to - from);
	if (!err && from == 0 && to == PAGE_CACHE_SIZE)
		SetPageUptodate(page);
	exfat_set_blocks(inode);
	mark_inode_dirty(inode);
	return err;
}

struct address_space_operations exfat_aops = {
	readpage:	exfat_readpage,
	writepage:	exfat_writepage,
	prepare_write:	exfat_prepare_write,
	commit_write:	exfat_commit_write,
};

static int exfat_file_open(struct inode *inode, struct file *filp)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	int err;

	err = exfat_fil_get(inode, filp->f_dentry,
			    (filp->f_mode & FMODE_WRITE) != 0);
	if (err)
		return err;
	down(&sbi->lock);
	EXFAT_I(inode)->openers++;
	up(&sbi->lock);
	return 0;
}

/* Write dirty mmap pages and the directory entry on every close */
static int exfat_file_flush(struct file *filp)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	FRESULT res = FR_OK;

	if (!(filp->f_mode & FMODE_WRITE))
		return 0;
	filemap_fdatasync(inode->i_mapping);
	filemap_fdatawait(inode->i_mapping);
	down(&sbi->lock);
	if (ei->fil)
		res = f_sync(ei->fil);
	up(&sbi->lock);
	return exfat_errno(res);
}

static int exfat_file_release(struct inode *inode, struct file *filp)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	int last;

	down(&sbi->lock);
	last = --ei->openers == 0;
	up(&sbi->lock);
	if (!last)
		return 0;
	/* no mappings are left; write what they dirtied, then close */
	filemap_fdatasync(inode->i_mapping);
	filemap_fdatawait(inode->i_mapping);
	down(&sbi->lock);
	if (ei->openers == 0 && ei->fil) {
		f_close(ei->fil);
		kfree(ei->fil);
		ei->fil = NULL;
	}
	up(&sbi->lock);
	return 0;
}

static int exfat_file_fsync(struct file *filp, struct dentry *dentry,
			    int datasync)
{
	struct inode *inode = dentry->d_inode;
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	FRESULT res = FR_OK;
	int err;

	/* the VFS has already written the dirty pages */
	down(&sbi->lock);
	if (ei->fil)
		res = f_sync(ei->fil);
	up(&sbi->lock);
	err = fsync_no_super(sbi->dev);
	return res != FR_OK ? exfat_errno(res) : err;
}

struct file_operations exfat_file_operations = {
	llseek:		generic_file_llseek,
	read:		generic_file_read,
	write:		generic_file_write,
	mmap:		generic_file_mmap,
	open:		exfat_file_open,
	flush:		exfat_file_flush,
	release:	exfat_file_release,
	fsync:		exfat_file_fsync,
};

static int exfat_truncate(struct inode *inode, struct dentry *dentry,
			  loff_t size)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	FSIZE_t now;
	FRESULT res;
	int err;

	if (size == inode->i_size)
		return 0;
	if (size > inode->i_sb->s_maxbytes)
		return -EFBIG;
	err = exfat_fil_get(inode, dentry, 1);
	if (err)
		return err;
	if (size < inode->i_size) {
		/* drop the cached pages first so writeback cannot regrow it */
		err = vmtruncate(inode, size);
		if (err)
			return err;
	}
	down(&sbi->lock);
	if (!ei->fil) {
		res = FR_INVALID_OBJECT;
	} else if ((FSIZE_t)size < ei->fil->obj.objsize) {
		res = exfat_fil_seek(inode, size, 1);
		if (res == FR_OK)
			res = f_truncate(ei->fil);
	} else {
		res = exfat_fil_extend(inode, size);
	}
	if (ei->fil && res == FR_OK)
		res = f_sync(ei->fil);
	now = ei->fil ? ei->fil->obj.objsize : 0;
	up(&sbi->lock);

	if (ei->fil && now > (FSIZE_t)inode->i_size)
		vmtruncate(inode, now);	/* the part that was extended */
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	exfat_set_blocks(inode);
	mark_inode_dirty(inode);
	if (res == FR_DENIED)
		return -ENOSPC;
	return exfat_errno(res);
}

int exfat_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	unsigned int valid = attr->ia_valid;
	WCHAR *path = NULL;
	FILINFO *fno = NULL;
	FRESULT res = FR_OK;
	BYTE rdo;
	int err;

	err = inode_change_ok(inode, attr);
	if (!err && (((valid & ATTR_UID) && attr->ia_uid != sbi->opts.uid) ||
		     ((valid & ATTR_GID) && attr->ia_gid != sbi->opts.gid) ||
		     ((valid & ATTR_MODE) &&
		      (attr->ia_mode & ~(S_IFREG | S_IFDIR | S_IRWXUGO)))))
		err = -EPERM;
	if (err)
		return sbi->opts.quiet ? 0 : err;

	if (valid & ATTR_SIZE) {
		if (S_ISDIR(inode->i_mode))
			return -EISDIR;
		err = exfat_truncate(inode, dentry, attr->ia_size);
		if (err)
			return err;
	}

	/* the root directory has no directory entry to update */
	if (IS_ROOT(dentry) || !(valid & (ATTR_MODE | ATTR_MTIME)))
		goto in_core;

	path = exfat_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	fno = kmalloc(sizeof(*fno), GFP_KERNEL);
	if (!fno) {
		exfat_put_path(path);
		return -ENOMEM;
	}
	rdo = (valid & ATTR_MODE) && !(attr->ia_mode & S_IWUSR) ? AM_RDO : 0;

	down(&sbi->lock);
	if (ei->flags & EXFAT_I_DEAD)
		res = FR_NO_FILE;
	if (res == FR_OK && (valid & ATTR_MTIME)) {
		/* a later f_sync of pending writes must not undo the time */
		if (ei->fil)
			res = f_sync(ei->fil);
		exfat_time_unix2fat(attr->ia_mtime, &fno->fdate, &fno->ftime);
		if (res == FR_OK)
			res = f_utime(path, fno);
	}
	if (res == FR_OK && (valid & ATTR_MODE) &&
	    rdo != (ei->attr & AM_RDO)) {
		res = f_chmod(path, rdo, AM_RDO);
		if (res == FR_OK)
			ei->attr = (ei->attr & ~AM_RDO) | rdo;
	}
	up(&sbi->lock);
	kfree(fno);
	exfat_put_path(path);
	if (res != FR_OK)
		return exfat_errno(res);

in_core:
	if (valid & ATTR_MTIME)
		inode->i_mtime = attr->ia_mtime;
	if (valid & ATTR_ATIME)
		inode->i_atime = attr->ia_atime;
	if (valid & ATTR_CTIME)
		inode->i_ctime = attr->ia_ctime;
	exfat_set_mode(inode);
	return 0;
}

struct inode_operations exfat_file_inode_operations = {
	setattr:	exfat_setattr,
};
