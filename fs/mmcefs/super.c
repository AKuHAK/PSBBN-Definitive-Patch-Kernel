/*
 *  linux/fs/mmcefs/super.c
 *
 *  MMCE SD card filesystem: mounting, IOP module loading and inodes.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/errno.h>
#include <asm/semaphore.h>
#include <asm/ps2/loadfile.h>

#include "mmcefs.h"

extern struct timezone sys_tz;

/* Directory with iomanX.irx, fileXio.irx and mmceman.irx */
static char *irx_dir = "/lib/modules/irx";
MODULE_PARM(irx_dir, "s");
MODULE_PARM_DESC(irx_dir, "directory holding iomanX.irx, fileXio.irx and mmceman.irx");
static char *irx_iop = "mc0:/LINUXIRX";
MODULE_PARM(irx_iop, "s");
MODULE_PARM_DESC(irx_iop, "IOP path of the same files, for IOPs that cannot load from memory");

static DECLARE_MUTEX(mmcefs_load_sem);

/*
 * Build "mmceN:/dir/name" for @dentry, which may be negative.  Returns a
 * kmalloc'ed string or an ERR_PTR.  Called without fx_lock().
 */
char *mmcefs_build_path(struct dentry *dentry)
{
	struct mmcefs_sb_info *sbi = MMCEFS_SB(dentry->d_sb);
	struct dentry *d;
	char *buf, *p;
	int len, err = 0;

	buf = kmalloc(MMCEFS_PATH_MAX, GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);
	p = buf + MMCEFS_PATH_MAX;
	*--p = 0;
	spin_lock(&dcache_lock);
	for (d = dentry; !IS_ROOT(d); d = d->d_parent) {
		len = d->d_name.len;
		if (p - buf < len + 1 + 8) {
			err = -ENAMETOOLONG;
			break;
		}
		p -= len;
		memcpy(p, d->d_name.name, len);
		*--p = '/';
	}
	spin_unlock(&dcache_lock);
	if (err) {
		kfree(buf);
		return ERR_PTR(err);
	}
	if (!*p)
		*--p = '/';		/* the root directory */
	p -= 6;
	memcpy(p, "mmce0:", 6);
	p[4] = '0' + sbi->slot;
	memmove(buf, p, buf + MMCEFS_PATH_MAX - p);
	return buf;
}

void mmcefs_put_path(char *path)
{
	kfree(path);
}

/* iox time: -, sec, min, hour, day, month, year (LE16); local time */
time_t mmcefs_time(const u8 *t)
{
	static const int mdays[] =
		{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	int year = t[6] | (t[7] << 8), month = t[5], day = t[4];
	long days;

	if (year < 1970 || year > 2037 || month < 1 || month > 12 ||
	    day < 1 || day > 31)
		return 0;
	days = (year - 1970) * 365L + (year - 1969) / 4 + mdays[month - 1] +
		day - 1;
	if (month > 2 && (year % 4) == 0)
		days++;
	return days * 86400 + t[3] * 3600 + t[2] * 60 + t[1] +
		sys_tz.tz_minuteswest * 60;
}

void mmcefs_set_blocks(struct inode *inode)
{
	inode->i_blocks = (inode->i_size + 511) >> 9;
}

/* Make an inode for @st, or for the root directory.  No fx_lock(). */
struct inode *mmcefs_build_inode(struct super_block *sb,
				 const struct fx_stat *st, int root)
{
	struct mmcefs_sb_info *sbi = MMCEFS_SB(sb);
	struct mmcefs_inode_info *mi;
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;
	mi = kmalloc(sizeof(*mi), GFP_KERNEL);
	if (!mi) {
		iput(inode);
		return NULL;
	}
	mi->fd = -1;
	mi->fd_write = 0;
	mi->openers = 0;
	mi->dead = 0;
	inode->u.generic_ip = mi;

	inode->i_ino = root ? MMCEFS_ROOT_INO : iunique(sb, MMCEFS_ROOT_INO);
	inode->i_uid = sbi->uid;
	inode->i_gid = sbi->gid;
	inode->i_nlink = 1;
	inode->i_blksize = PAGE_CACHE_SIZE;
	if (root || (st->mode & FX_S_IFMT) == FX_S_IFDIR) {
		inode->i_mode = S_IFDIR | (S_IRWXUGO & ~sbi->dmask);
		inode->i_size = 0;
		inode->i_op = &mmcefs_dir_inode_operations;
		inode->i_fop = &mmcefs_dir_operations;
	} else {
		inode->i_mode = S_IFREG | (S_IRWXUGO & ~sbi->fmask);
		inode->i_size = ((loff_t)st->hisize << 32) | st->size;
		inode->i_op = &mmcefs_file_inode_operations;
		inode->i_fop = &mmcefs_file_operations;
		inode->i_data.a_ops = &mmcefs_aops;
	}
	inode->i_mtime = root ? 0 : mmcefs_time(st->mtime);
	inode->i_atime = inode->i_ctime = inode->i_mtime;
	mmcefs_set_blocks(inode);
	insert_inode_hash(inode);	/* for writeback of dirty inodes */
	return inode;
}

static void mmcefs_clear_inode(struct inode *inode)
{
	struct mmcefs_inode_info *mi = MMCEFS_I(inode);

	if (!mi)
		return;
	mmcefs_fd_close(inode);
	kfree(mi);
	inode->u.generic_ip = NULL;
}

static void mmcefs_put_super(struct super_block *sb)
{
	kfree(sb->u.generic_sbp);
	sb->u.generic_sbp = NULL;
}

static int mmcefs_statfs(struct super_block *sb, struct statfs *buf)
{
	/* the MMCE protocol does not report capacity or free space */
	buf->f_type = MMCEFS_SUPER_MAGIC;
	buf->f_bsize = 512;
	buf->f_namelen = MMCEFS_NAME_MAX;
	return 0;
}

static struct super_operations mmcefs_sops = {
	put_inode:	force_delete,
	clear_inode:	mmcefs_clear_inode,
	put_super:	mmcefs_put_super,
	statfs:		mmcefs_statfs,
};

/* Read @name from irx_dir and start it on the IOP */
/* Send irx_dir/name to the IOP from EE memory */
static int mmcefs_load_irx_file(const char *name, char *path, int *modres)
{
	struct file *f;
	char *image;
	loff_t size;
	int rv;

	sprintf(path, "%s/%s", irx_dir, name);
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	size = f->f_dentry->d_inode->i_size;
	if (size <= 0 || size > 1024 * 1024) {
		filp_close(f, NULL);
		return -EINVAL;
	}
	image = vmalloc(size);
	if (!image) {
		filp_close(f, NULL);
		return -ENOMEM;
	}
	rv = kernel_read(f, 0, image, size);
	filp_close(f, NULL);
	if (rv == size)
		rv = ps2_load_module_buffer(image, size, NULL, 0, modres);
	else if (rv >= 0)
		rv = -EIO;
	vfree(image);
	return rv;
}

/*
 * Load an IOP module: from irx_dir through EE memory, or, once the IOP has
 * refused that (PSBBN's LOADFILE cannot load from memory) or irx_dir lacks
 * the files, from irx_iop, a directory the IOP reads itself (by default on
 * the memory card).
 */
static int mmcefs_load_irx(const char *name)
{
	static int iop_only;
	char path[256];
	int rv = -ENOSYS, modres = -1;

	if (strlen(irx_dir) + strlen(name) + 2 > sizeof(path) ||
	    strlen(irx_iop) + strlen(name) + 2 > sizeof(path))
		return -ENAMETOOLONG;
	if (!iop_only) {
		rv = mmcefs_load_irx_file(name, path, &modres);
		if (rv == -ENOSYS || rv == -ENOENT) {
			printk(KERN_INFO "mmcefs: %s; loading modules from "
			       "%s\n", rv == -ENOSYS ? "the IOP cannot load "
			       "modules from memory" : "no IRX files in irx_dir",
			       irx_iop);
			iop_only = 1;
		}
	}
	if (iop_only) {
		sprintf(path, "%s/%s", irx_iop, name);
		rv = ps2_load_module(path, NULL, 0, &modres);
	}
	if (rv < 0)
		printk(KERN_ERR "mmcefs: loading %s failed (%d)\n", path, rv);
	else if (modres == 1)
		printk(KERN_WARNING "mmcefs: %s did not stay resident\n", path);
	else
		printk(KERN_INFO "mmcefs: loaded %s (IOP module %d)\n", path, rv);
	return rv < 0 ? rv : 0;
}

/*
 * Make sure fileXio is serving and the "mmceN:" device exists, loading
 * the IOP modules that are missing.  Called without fx_lock() (it reads
 * files), then probes with it.
 */
static int mmcefs_prepare_iop(int slot, int load)
{
	char dev[8];
	int rv;

	sprintf(dev, "mmce%d:/", slot);
	down(&mmcefs_load_sem);
	fx_lock();
	rv = fx_bind();
	fx_unlock();
	if (rv == -ENODEV && load) {
		/* iomanX.irx may already be resident; fileXio decides */
		mmcefs_load_irx("iomanX.irx");
		mmcefs_load_irx("fileXio.irx");
		fx_lock();
		rv = fx_bind();
		fx_unlock();
	}
	if (rv) {
		printk(KERN_ERR "mmcefs: fileXio RPC service not available "
		       "(%d)\n", rv);
		goto out;
	}

	fx_lock();
	rv = fx_dopen(dev);
	if (rv >= 0)
		fx_dclose(rv);
	fx_unlock();
	if (rv == -ENODEV && load) {
		mmcefs_load_irx("mmceman.irx");
		fx_lock();
		rv = fx_dopen(dev);
		if (rv >= 0)
			fx_dclose(rv);
		fx_unlock();
	}
	if (rv == -ENODEV)
		printk(KERN_ERR "mmcefs: no %s device: is mmceman.irx loaded "
		       "and an MMCE in slot %d?\n", dev, slot + 1);
	else if (rv < 0)
		printk(KERN_ERR "mmcefs: cannot open %s (%d)\n", dev, rv);
out:
	up(&mmcefs_load_sem);
	return rv < 0 ? rv : 0;
}

static int mmcefs_parse_options(char *options, struct mmcefs_sb_info *sbi,
				int *load)
{
	char *next, *value, *end;
	unsigned long val;

	while (options && *options) {
		next = strchr(options, ',');
		if (next)
			*next++ = 0;
		value = strchr(options, '=');
		if (value)
			*value++ = 0;
		if (!strcmp(options, "noload")) {
			*load = 0;
		} else if (!strcmp(options, "slot") || !strcmp(options, "uid") ||
			   !strcmp(options, "gid") || !strcmp(options, "umask") ||
			   !strcmp(options, "fmask") ||
			   !strcmp(options, "dmask")) {
			if (!value || !*value)
				goto bad;
			val = simple_strtoul(value, &end, options[1] == 'm' ||
					     options[0] == 'f' ||
					     options[0] == 'd' ? 8 : 0);
			if (*end)
				goto bad;
			if (!strcmp(options, "slot")) {
				if (val > 1)
					goto bad;
				sbi->slot = val;
			} else if (!strcmp(options, "uid"))
				sbi->uid = val;
			else if (!strcmp(options, "gid"))
				sbi->gid = val;
			else if (!strcmp(options, "fmask"))
				sbi->fmask = val & S_IRWXUGO;
			else if (!strcmp(options, "dmask"))
				sbi->dmask = val & S_IRWXUGO;
			else
				sbi->fmask = sbi->dmask = val & S_IRWXUGO;
		} else {
			printk(KERN_ERR "mmcefs: unknown mount option %s\n",
			       options);
			return -EINVAL;
		}
		options = next;
	}
	return 0;
bad:
	printk(KERN_ERR "mmcefs: bad value for mount option %s\n", options);
	return -EINVAL;
}

static struct super_block *mmcefs_read_super(struct super_block *sb,
					     void *data, int silent)
{
	struct mmcefs_sb_info *sbi;
	struct inode *root;
	int load = 1;

	sbi = kmalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return NULL;
	sbi->slot = 0;
	sbi->uid = current->uid;
	sbi->gid = current->gid;
	sbi->fmask = sbi->dmask = current->fs->umask;
	sb->u.generic_sbp = sbi;
	if (mmcefs_parse_options((char *)data, sbi, &load) ||
	    mmcefs_prepare_iop(sbi->slot, load))
		goto out;

	sb->s_blocksize = 512;
	sb->s_blocksize_bits = 9;
	sb->s_magic = MMCEFS_SUPER_MAGIC;
	sb->s_op = &mmcefs_sops;
	sb->s_maxbytes = ((u64)PAGE_CACHE_SIZE << (BITS_PER_LONG - 1)) - 1;
	root = mmcefs_build_inode(sb, NULL, 1);
	if (!root)
		goto out;
	sb->s_root = d_alloc_root(root);
	if (!sb->s_root) {
		iput(root);
		goto out;
	}
	sb->s_root->d_op = &mmcefs_dentry_ops;
	return sb;
out:
	kfree(sbi);
	sb->u.generic_sbp = NULL;
	return NULL;
}

static DECLARE_FSTYPE(mmcefs_fs_type, "mmcefs", mmcefs_read_super, 0);

static int __init init_mmcefs(void)
{
	int err;

	err = fx_init();
	if (err)
		return err;
	err = register_filesystem(&mmcefs_fs_type);
	if (err)
		fx_exit();
	return err;
}

static void __exit exit_mmcefs(void)
{
	unregister_filesystem(&mmcefs_fs_type);
	fx_exit();
}

EXPORT_NO_SYMBOLS;

module_init(init_mmcefs)
module_exit(exit_mmcefs)

MODULE_DESCRIPTION("MMCE (SD2PSX, MemCard PRO2) SD card filesystem");
MODULE_LICENSE("GPL");
