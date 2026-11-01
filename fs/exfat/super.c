/*
 *  linux/fs/exfat/super.c
 *
 *  exFAT filesystem for Linux 2.4: superblock and inode handling.
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
#include <linux/string.h>
#include <linux/blkdev.h>
#include <linux/locks.h>
#include <linux/errno.h>

#include "exfat_fs.h"

/* Recompute i_mode from the mount masks and the read-only attribute */
void exfat_set_mode(struct inode *inode)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	umode_t mode;

	if (S_ISDIR(inode->i_mode))
		mode = S_IFDIR | (S_IRWXUGO & ~sbi->opts.dmask);
	else
		mode = S_IFREG | (S_IRWXUGO & ~sbi->opts.fmask);
	if (ei->attr & AM_RDO)
		mode &= ~S_IWUGO;
	inode->i_mode = mode;
}

void exfat_set_blocks(struct inode *inode)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	loff_t alloc;

	alloc = (inode->i_size + sbi->clu_size - 1) &
		~(loff_t)(sbi->clu_size - 1);
	inode->i_blocks = alloc >> 9;
}

/*
 * Make an inode for the object described by @fno, or for the root directory
 * if @fno is NULL.  Called without sbi->lock (new_inode() may reclaim).
 */
struct inode *exfat_build_inode(struct super_block *sb, const FILINFO *fno)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct exfat_inode_info *ei;
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;
	ei = kmalloc(sizeof(*ei), GFP_KERNEL);
	if (!ei) {
		iput(inode);
		return NULL;
	}
	memset(ei, 0, sizeof(*ei));
	inode->u.generic_ip = ei;

	inode->i_ino = fno ? iunique(sb, EXFAT_ROOT_INO) : EXFAT_ROOT_INO;
	inode->i_uid = sbi->opts.uid;
	inode->i_gid = sbi->opts.gid;
	inode->i_nlink = 1;	/* subdirectories are not counted (cf. find -noleaf) */
	inode->i_blksize = PAGE_CACHE_SIZE;

	if (!fno || (fno->fattrib & AM_DIR)) {
		ei->attr = fno ? fno->fattrib : AM_DIR;
		inode->i_mode = S_IFDIR;
		inode->i_size = sbi->clu_size;
		inode->i_op = &exfat_dir_inode_operations;
		inode->i_fop = &exfat_dir_operations;
	} else {
		ei->attr = fno->fattrib;
		inode->i_mode = S_IFREG;
		inode->i_size = fno->fsize;
		inode->i_op = &exfat_file_inode_operations;
		inode->i_fop = &exfat_file_operations;
		inode->i_data.a_ops = &exfat_aops;
	}
	exfat_set_mode(inode);
	exfat_set_blocks(inode);
	if (fno)
		inode->i_mtime = exfat_time_fat2unix(fno->fdate, fno->ftime);
	else
		inode->i_mtime = 0;	/* the root has no directory entry */
	inode->i_atime = inode->i_ctime = inode->i_mtime;
	insert_inode_hash(inode);	/* needed for writeback of dirty inodes */
	return inode;
}

static void exfat_write_inode(struct inode *inode, int wait)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);

	if (!ei || !ei->fil)
		return;
	down(&sbi->lock);
	if (ei->fil)
		f_sync(ei->fil);	/* size and timestamps to the directory entry */
	up(&sbi->lock);
}

static void exfat_clear_inode(struct inode *inode)
{
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);

	if (!ei)
		return;
	if (ei->fil) {
		down(&sbi->lock);
		f_close(ei->fil);
		up(&sbi->lock);
		kfree(ei->fil);
	}
	if (ei->cltbl)
		kfree(ei->cltbl);
	kfree(ei);
	inode->u.generic_ip = NULL;
}

static void exfat_free_sbi(struct exfat_sb_info *sbi)
{
	if (sbi->nls)
		unload_nls(sbi->nls);
	if (sbi->opts.iocharset)
		kfree(sbi->opts.iocharset);
	kfree(sbi);
}

static void exfat_put_super(struct super_block *sb)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	WCHAR path[3];

	exfat_vol_path(sbi, path);
	down(&sbi->lock);
	f_mount(NULL, path, 0);
	up(&sbi->lock);
	exfat_vol_detach(sbi);
	exfat_free_sbi(sbi);
	sb->u.generic_sbp = NULL;
}

static int exfat_statfs(struct super_block *sb, struct statfs *buf)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	DWORD nfree = 0;
	FATFS *fs;
	WCHAR path[3];
	FRESULT res;

	exfat_vol_path(sbi, path);
	down(&sbi->lock);
	res = f_getfree(path, &nfree, &fs);
	up(&sbi->lock);
	if (res != FR_OK)
		return exfat_errno(res);
	buf->f_type = EXFAT_SUPER_MAGIC;
	buf->f_bsize = sbi->clu_size;
	buf->f_blocks = sbi->fatfs.n_fatent - 2;
	buf->f_bfree = nfree;
	buf->f_bavail = nfree;
	buf->f_namelen = EXFAT_NAME_MAX;
	return 0;
}

static int exfat_remount(struct super_block *sb, int *flags, char *data)
{
	/* the VFS checks for files open for writing before going read-only */
	return 0;
}

static struct super_operations exfat_sops = {
	put_inode:	force_delete,	/* nothing to keep once unused */
	write_inode:	exfat_write_inode,
	clear_inode:	exfat_clear_inode,
	put_super:	exfat_put_super,
	statfs:		exfat_statfs,
	remount_fs:	exfat_remount,
};

static int exfat_parse_options(char *options, struct exfat_mount_options *opts)
{
	char *next, *value, *end;
	unsigned long val;

	opts->uid = current->uid;
	opts->gid = current->gid;
	opts->fmask = opts->dmask = current->fs->umask;
	opts->quiet = 0;
	opts->iocharset = NULL;

	while (options && *options) {
		next = strchr(options, ',');
		if (next)
			*next++ = 0;
		value = strchr(options, '=');
		if (value)
			*value++ = 0;

		if (!strcmp(options, "uid") || !strcmp(options, "gid") ||
		    !strcmp(options, "umask") || !strcmp(options, "fmask") ||
		    !strcmp(options, "dmask")) {
			if (!value || !*value)
				goto bad_value;
			val = simple_strtoul(value, &end, options[1] == 'i' ?
					     0 : 8);
			if (*end)
				goto bad_value;
			if (!strcmp(options, "uid"))
				opts->uid = val;
			else if (!strcmp(options, "gid"))
				opts->gid = val;
			else if (!strcmp(options, "fmask"))
				opts->fmask = val & S_IRWXUGO;
			else if (!strcmp(options, "dmask"))
				opts->dmask = val & S_IRWXUGO;
			else
				opts->fmask = opts->dmask = val & S_IRWXUGO;
		} else if (!strcmp(options, "iocharset")) {
			if (!value || !*value)
				goto bad_value;
			if (opts->iocharset)
				kfree(opts->iocharset);
			opts->iocharset = NULL;
			if (strcmp(value, "utf8")) {
				opts->iocharset = kmalloc(strlen(value) + 1,
							  GFP_KERNEL);
				if (!opts->iocharset)
					return -ENOMEM;
				strcpy(opts->iocharset, value);
			}
		} else if (!strcmp(options, "utf8")) {
			if (opts->iocharset)
				kfree(opts->iocharset);
			opts->iocharset = NULL;
		} else if (!strcmp(options, "quiet")) {
			opts->quiet = 1;
		} else {
			printk(KERN_ERR "exfat: unknown mount option %s\n",
			       options);
			return -EINVAL;
		}
		options = next;
	}
	return 0;

bad_value:
	printk(KERN_ERR "exfat: bad value for mount option %s\n", options);
	return -EINVAL;
}

static struct super_block *exfat_read_super(struct super_block *sb,
					    void *data, int silent)
{
	struct exfat_sb_info *sbi;
	struct inode *root;
	WCHAR path[3];
	FRESULT res;

	sbi = kmalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return NULL;
	memset(sbi, 0, sizeof(*sbi));
	init_MUTEX(&sbi->lock);
	sbi->sb = sb;
	sbi->dev = sb->s_dev;
	sb->u.generic_sbp = sbi;

	if (exfat_parse_options((char *)data, &sbi->opts))
		goto out_free;
	if (sbi->opts.iocharset) {
		sbi->nls = load_nls(sbi->opts.iocharset);
		if (!sbi->nls) {
			printk(KERN_ERR "exfat: cannot load NLS charset %s\n",
			       sbi->opts.iocharset);
			goto out_free;
		}
	}

	if (get_hardsect_size(sb->s_dev) > EXFAT_SECTOR_SIZE ||
	    set_blocksize(sb->s_dev, EXFAT_SECTOR_SIZE)) {
		if (!silent)
			printk(KERN_ERR "exfat: %s: only 512-byte sector "
			       "devices are supported\n", kdevname(sb->s_dev));
		goto out_free;
	}
	sb->s_blocksize = EXFAT_SECTOR_SIZE;
	sb->s_blocksize_bits = EXFAT_SECTOR_BITS;

	if (exfat_vol_attach(sbi)) {
		printk(KERN_ERR "exfat: too many mounted volumes (max %d)\n",
		       FF_VOLUMES);
		goto out_free;
	}
	exfat_vol_path(sbi, path);
	down(&sbi->lock);
	res = f_mount(&sbi->fatfs, path, 1);
	if (res == FR_OK && sbi->fatfs.fs_type != FS_EXFAT) {
		f_mount(NULL, path, 0);	/* FAT12/16/32: leave it to vfat */
		res = FR_NO_FILESYSTEM;
	}
	up(&sbi->lock);
	if (res != FR_OK) {
		if (!silent)
			printk(KERN_INFO "exfat: no exFAT filesystem on %s "
			       "(%d)\n", kdevname(sb->s_dev), res);
		goto out_detach;
	}
	sbi->clu_size = (unsigned long)sbi->fatfs.csize << EXFAT_SECTOR_BITS;

	sb->s_magic = EXFAT_SUPER_MAGIC;
	sb->s_op = &exfat_sops;
	/* limited by the 32-bit page cache index */
	sb->s_maxbytes = ((u64)PAGE_CACHE_SIZE << (BITS_PER_LONG - 1)) - 1;

	root = exfat_build_inode(sb, NULL);
	if (!root)
		goto out_unmount;
	sb->s_root = d_alloc_root(root);
	if (!sb->s_root) {
		iput(root);
		goto out_unmount;
	}
	sb->s_root->d_op = &exfat_dentry_ops;
	return sb;

out_unmount:
	printk(KERN_ERR "exfat: cannot allocate the root inode\n");
	down(&sbi->lock);
	f_mount(NULL, path, 0);
	up(&sbi->lock);
out_detach:
	exfat_vol_detach(sbi);
out_free:
	exfat_free_sbi(sbi);
	sb->u.generic_sbp = NULL;
	return NULL;
}

static DECLARE_FSTYPE_DEV(exfat_fs_type, "exfat", exfat_read_super);

static int __init init_exfat_fs(void)
{
	return register_filesystem(&exfat_fs_type);
}

static void __exit exit_exfat_fs(void)
{
	unregister_filesystem(&exfat_fs_type);
}

EXPORT_NO_SYMBOLS;

module_init(init_exfat_fs)
module_exit(exit_exfat_fs)

MODULE_DESCRIPTION("exFAT filesystem support (FatFs based)");
MODULE_LICENSE("GPL");
