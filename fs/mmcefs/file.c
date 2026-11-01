/*
 *  linux/fs/mmcefs/file.c
 *
 *  MMCE SD card filesystem: regular files.  Data goes through the page
 *  cache; ->readpage and ->commit_write transfer it with the inode's IOP
 *  file descriptor, so only shared mmaps leave dirty pages behind.
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
#include <linux/errno.h>

#include "mmcefs.h"

static u8 mmcefs_zero[PAGE_CACHE_SIZE];

/*
 * Make sure @inode has an open IOP descriptor, writable if @write is set.
 * @dentry (an alias of @inode) gives the path.  Called without fx_lock().
 */
int mmcefs_fd_get(struct inode *inode, struct dentry *dentry, int write)
{
	struct mmcefs_inode_info *mi = MMCEFS_I(inode);
	char *path;
	int fd, err = 0;

	if (mi->dead)
		return -ESTALE;
	if (mi->fd >= 0 && (!write || mi->fd_write))
		return 0;
	if (!dentry)
		return -EIO;
	path = mmcefs_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);

	fx_lock();
	if (mi->dead) {
		err = -ESTALE;
	} else if (mi->fd < 0 || (write && !mi->fd_write)) {
		if (mi->fd >= 0) {
			fx_close(mi->fd);
			mi->fd = -1;
		}
		fd = fx_open(path, FX_O_RDWR, 0666);
		mi->fd_write = fd >= 0;
		if (fd < 0 && !write)
			fd = fx_open(path, FX_O_RDONLY, 0);
		if (fd >= 0)
			mi->fd = fd;
		else
			err = fd;
	}
	fx_unlock();
	mmcefs_put_path(path);
	return err;
}

static int mmcefs_fd_get_alias(struct inode *inode, int write)
{
	struct dentry *dentry;
	int err;

	if (MMCEFS_I(inode)->fd >= 0 && !write)
		return 0;
	dentry = d_find_alias(inode);
	err = mmcefs_fd_get(inode, dentry, write);
	if (dentry)
		dput(dentry);
	return err;
}

void mmcefs_fd_close(struct inode *inode)
{
	struct mmcefs_inode_info *mi = MMCEFS_I(inode);

	fx_lock();
	if (mi->fd >= 0) {
		fx_close(mi->fd);
		mi->fd = -1;
		mi->fd_write = 0;
	}
	fx_unlock();
}

/* Write @count bytes at @pos with fx_lock() held; returns bytes or error */
static int mmcefs_pwrite(int fd, loff_t pos, const char *buf, int count)
{
	loff_t rv;
	int done = 0, n, w;

	rv = fx_lseek64(fd, pos, 0);
	if (rv < 0)
		return (int)rv;
	while (done < count) {
		n = count - done < FX_IOBUF ? count - done : FX_IOBUF;
		w = fx_write(fd, buf + done, n);
		if (w < 0)
			return done ? done : w;
		done += w;
		if (w < n)
			break;
	}
	return done;
}

/* Fill a locked page from the file; zeroes beyond end of file */
static int mmcefs_fill_page(struct inode *inode, struct dentry *dentry,
			    struct page *page)
{
	struct mmcefs_inode_info *mi = MMCEFS_I(inode);
	loff_t pos = (loff_t)page->index << PAGE_CACHE_SHIFT;
	loff_t rv;
	char *kaddr = kmap(page);
	int done = 0, n, err = 0;

	if (pos < inode->i_size) {
		err = dentry ? mmcefs_fd_get(inode, dentry, 0) :
			mmcefs_fd_get_alias(inode, 0);
		if (!err) {
			fx_lock();
			if (mi->fd < 0) {
				err = -EIO;
			} else {
				rv = fx_lseek64(mi->fd, pos, 0);
				if (rv < 0)
					err = (int)rv;
			}
			while (!err && done < PAGE_CACHE_SIZE) {
				n = fx_read(mi->fd, kaddr + done,
					    PAGE_CACHE_SIZE - done);
				if (n < 0)
					err = n;
				if (n <= 0)
					break;
				done += n;
			}
			fx_unlock();
		}
	}
	if (!err) {
		memset(kaddr + done, 0, PAGE_CACHE_SIZE - done);
		flush_dcache_page(page);
		SetPageUptodate(page);
	}
	kunmap(page);
	return err;
}

static int mmcefs_readpage(struct file *file, struct page *page)
{
	int err;

	err = mmcefs_fill_page(page->mapping->host,
			       file ? file->f_dentry : NULL, page);
	if (err)
		SetPageError(page);
	UnlockPage(page);
	return err;
}

/* Extend the file with zeroes to @size; fx_lock() held */
static int mmcefs_extend(struct inode *inode, loff_t size)
{
	struct mmcefs_inode_info *mi = MMCEFS_I(inode);
	int n, w;

	while (inode->i_size < size) {
		n = size - inode->i_size < (loff_t)sizeof(mmcefs_zero) ?
			(int)(size - inode->i_size) : sizeof(mmcefs_zero);
		w = mmcefs_pwrite(mi->fd, inode->i_size, mmcefs_zero, n);
		if (w > 0)
			inode->i_size += w;
		if (w < n)
			return w < 0 ? w : -ENOSPC;
	}
	return 0;
}

static int mmcefs_write_range(struct inode *inode, struct dentry *dentry,
			      struct page *page, unsigned offset,
			      unsigned count)
{
	struct mmcefs_inode_info *mi = MMCEFS_I(inode);
	loff_t pos = ((loff_t)page->index << PAGE_CACHE_SHIFT) + offset;
	char *kaddr;
	int err, w = 0;

	err = dentry ? mmcefs_fd_get(inode, dentry, 1) :
		mmcefs_fd_get_alias(inode, 1);
	if (err)
		return err;
	kaddr = kmap(page);
	fx_lock();
	if (mi->fd < 0 || !mi->fd_write)
		err = -EIO;
	else if (pos > inode->i_size)
		err = mmcefs_extend(inode, pos);
	if (!err) {
		w = mmcefs_pwrite(mi->fd, pos, kaddr + offset, count);
		if (w < 0)
			err = w;
		else if (pos + w > inode->i_size)
			inode->i_size = pos + w;
	}
	fx_unlock();
	kunmap(page);
	if (!err && w < (int)count)
		err = -ENOSPC;
	return err;
}

static int mmcefs_writepage(struct page *page)
{
	struct inode *inode = page->mapping->host;
	unsigned long end_index = inode->i_size >> PAGE_CACHE_SHIFT;
	unsigned count = PAGE_CACHE_SIZE;
	int err = 0;

	if (page->index >= end_index)
		count = inode->i_size & (PAGE_CACHE_SIZE - 1);
	if (page->index <= end_index && count)
		err = mmcefs_write_range(inode, NULL, page, 0, count);
	if (err)
		printk(KERN_WARNING "mmcefs: writeback of inode %lu failed "
		       "(%d)\n", inode->i_ino, err);
	UnlockPage(page);
	return err;
}

static int mmcefs_prepare_write(struct file *file, struct page *page,
				unsigned from, unsigned to)
{
	char *kaddr;

	if (Page_Uptodate(page))
		return 0;
	if (from == 0 && to == PAGE_CACHE_SIZE) {
		/* a fault in copy_from_user still commits the whole range */
		kaddr = kmap(page);
		memset(kaddr, 0, PAGE_CACHE_SIZE);
		kunmap(page);
		return 0;
	}
	return mmcefs_fill_page(page->mapping->host, file->f_dentry, page);
}

static int mmcefs_commit_write(struct file *file, struct page *page,
			       unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	int err;

	if (from == to)
		return 0;
	err = mmcefs_write_range(inode, file->f_dentry, page, from, to - from);
	if (!err && from == 0 && to == PAGE_CACHE_SIZE)
		SetPageUptodate(page);
	mmcefs_set_blocks(inode);
	return err;
}

struct address_space_operations mmcefs_aops = {
	readpage:	mmcefs_readpage,
	writepage:	mmcefs_writepage,
	prepare_write:	mmcefs_prepare_write,
	commit_write:	mmcefs_commit_write,
};

static int mmcefs_file_open(struct inode *inode, struct file *filp)
{
	int err;

	err = mmcefs_fd_get(inode, filp->f_dentry,
			    (filp->f_mode & FMODE_WRITE) != 0);
	if (err)
		return err;
	fx_lock();
	MMCEFS_I(inode)->openers++;
	fx_unlock();
	return 0;
}

static int mmcefs_file_release(struct inode *inode, struct file *filp)
{
	int last;

	fx_lock();
	last = --MMCEFS_I(inode)->openers == 0;
	fx_unlock();
	if (last) {
		/* no mappings are left; write what they dirtied */
		filemap_fdatasync(inode->i_mapping);
		filemap_fdatawait(inode->i_mapping);
		if (MMCEFS_I(inode)->openers == 0)
			mmcefs_fd_close(inode);
	}
	return 0;
}

static int mmcefs_file_fsync(struct file *filp, struct dentry *dentry,
			     int datasync)
{
	/* the VFS wrote the dirty pages; writes reach the card directly */
	return 0;
}

struct file_operations mmcefs_file_operations = {
	llseek:		generic_file_llseek,
	read:		generic_file_read,
	write:		generic_file_write,
	mmap:		generic_file_mmap,
	open:		mmcefs_file_open,
	release:	mmcefs_file_release,
	fsync:		mmcefs_file_fsync,
};

static int mmcefs_truncate(struct inode *inode, struct dentry *dentry,
			   loff_t size)
{
	struct mmcefs_inode_info *mi = MMCEFS_I(inode);
	char *path;
	int err, fd;

	if (size == inode->i_size)
		return 0;
	if (size > inode->i_size) {
		err = mmcefs_fd_get(inode, dentry, 1);
		if (err)
			return err;
		fx_lock();
		err = mi->fd >= 0 ? mmcefs_extend(inode, size) : -EIO;
		fx_unlock();
		mmcefs_set_blocks(inode);
		return err;
	}
	/* iomanX has no ftruncate; only O_TRUNC to empty is possible */
	if (size != 0)
		return -EOPNOTSUPP;
	path = mmcefs_build_path(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	filemap_fdatasync(inode->i_mapping);
	filemap_fdatawait(inode->i_mapping);
	err = vmtruncate(inode, 0);
	if (!err) {
		fx_lock();
		if (mi->fd >= 0) {
			fx_close(mi->fd);
			mi->fd = -1;
		}
		fd = fx_open(path, FX_O_RDWR | FX_O_TRUNC, 0666);
		if (fd >= 0) {
			mi->fd = fd;
			mi->fd_write = 1;
		} else
			err = fd;
		fx_unlock();
	}
	mmcefs_put_path(path);
	mmcefs_set_blocks(inode);
	return err;
}

int mmcefs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	int err;

	err = inode_change_ok(inode, attr);
	if (err)
		return err;
	if (attr->ia_valid & ATTR_SIZE) {
		if (S_ISDIR(inode->i_mode))
			return -EISDIR;
		err = mmcefs_truncate(inode, dentry, attr->ia_size);
		if (err)
			return err;
		inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	}
	/*
	 * The MMCE protocol cannot store owners, modes or times; keep
	 * them in core only so tools like cp -p and tar do not fail.
	 */
	if (attr->ia_valid & ATTR_MTIME)
		inode->i_mtime = attr->ia_mtime;
	if (attr->ia_valid & ATTR_ATIME)
		inode->i_atime = attr->ia_atime;
	return 0;
}

struct inode_operations mmcefs_file_inode_operations = {
	setattr:	mmcefs_setattr,
};
