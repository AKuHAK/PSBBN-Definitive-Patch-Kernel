/*
 * ps2fs/super.c
 *
 * Copyright (c) 2002 Andrew Church <achurch@achurch.org>
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/locks.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include "ps2fs_fs.h"
#include "ps2_partition.h"

/*************************************************************************/

static void ps2fs_put_super(struct super_block *sb);
static void ps2fs_write_super(struct super_block *sb);
static int ps2fs_statfs(struct super_block *sb, struct statfs *sf);
static int ps2fs_remount_fs(struct super_block *sb, int *flags, char *data);

static struct super_operations ps2fs_sops = {
    read_inode:		ps2fs_read_inode,
    write_inode:	ps2fs_write_inode,
    delete_inode:	ps2fs_delete_inode,
    clear_inode:	ps2fs_clear_inode,
    put_super:		ps2fs_put_super,
    write_super:	ps2fs_write_super,
    statfs:		ps2fs_statfs,
    remount_fs:		ps2fs_remount_fs,
};

struct ps2fs_options {
    char partition[PS2_PART_IDMAX+2];
    int tzoffset;			/* minutes */
    int unsafe_write;
    uid_t uid;
    gid_t gid;
};

/*************************************************************************/
/*************************************************************************/

/* Parse options into `opt'; return 1 on success, or 0 if an unsupported
 * option is found. */

static int parse_options(char *options, struct ps2fs_options *opt)
{
    char *s, *value;

    while (options && *options) {
	s = strchr(options, ',');
	if (s)
	    *s++ = 0;
	value = strchr(options, '=');
	if (value)
	    *value++ = 0;
	if (strcmp(options, "partition") == 0 && value && *value) {
	    if (strlen(value) > PS2_PART_IDMAX) {
		printk("ps2fs: partition ID too long (%d chars max)\n",
		       PS2_PART_IDMAX);
		return 0;
	    }
	    strcpy(opt->partition, value);
	} else if (strcmp(options, "tzoffset") == 0 && value && *value) {
	    opt->tzoffset = simple_strtol(value, &value, 0);
	    if (*value) {
		printk("ps2fs: tzoffset option requires an integer\n");
		return 0;
	    }
	} else if (strcmp(options, "uid") == 0 && value && *value) {
	    opt->uid = simple_strtoul(value, &value, 0);
	    if (*value)
		return 0;
	} else if (strcmp(options, "gid") == 0 && value && *value) {
	    opt->gid = simple_strtoul(value, &value, 0);
	    if (*value)
		return 0;
	} else if (strcmp(options, "unsafe_write") == 0 && !value) {
	    opt->unsafe_write = 1;
	} else {
	    printk("ps2fs: unrecognized option `%s'\n", options);
	    return 0;
	}
	options = s;
    }
    return 1;
}

/*************************************************************************/

/* Read and check the PFS superblock; fills the geometry of sbi */
static int read_pfs_super(struct super_block *sb, int silent, __u32 *fsck_stat)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct buffer_head *bh;
    struct ps2fs_super_block *psb;
    __u32 zsize, nsubs, i;
    int err = -EINVAL;

    bh = bread(sb->s_dev, sbi->part_start[0] + PS2FS_SUPER_SECTOR, 512);
    if (!bh) {
	printk("ps2fs: unable to read superblock\n");
	return -EIO;
    }
    psb = (struct ps2fs_super_block *)bh->b_data;
    zsize = le32_to_cpu(psb->zone_size);
    nsubs = le32_to_cpu(psb->num_subs);
    if (le32_to_cpu(psb->magic) != PS2FS_SUPER_MAGIC) {
	if (!silent)
	    printk("ps2fs: bad magic number in superblock\n");
	goto out;
    }
    if (le32_to_cpu(psb->version) > PS2FS_FORMAT_VERSION) {
	printk("ps2fs: unknown format version %u\n",
	       le32_to_cpu(psb->version));
	goto out;
    }
    if (zsize < 2048 || zsize > 131072 || (zsize & (zsize - 1))) {
	printk("ps2fs: bad zone size %u\n", zsize);
	goto out;
    }
    if (nsubs + 1 > sbi->n_subparts) {
	printk("ps2fs: superblock claims %u sub-partitions, APA has %u\n",
	       nsubs, sbi->n_subparts - 1);
	goto out;
    }
    sbi->n_subparts = nsubs + 1;
    sbi->zone_size = zsize;
    for (i = 0; zsize > 512; i++)
	zsize >>= 1;
    sbi->sector_shift = i;
    sbi->block_shift = i - 1;
    sbi->log.number = le32_to_cpu(psb->log.number);
    sbi->log.subpart = le16_to_cpu(psb->log.subpart);
    sbi->log.count = le16_to_cpu(psb->log.count);
    sbi->root.number = le32_to_cpu(psb->root.number);
    sbi->root.subpart = le16_to_cpu(psb->root.subpart);
    sbi->root.count = le16_to_cpu(psb->root.count);
    if (sbi->log.subpart != 0 || sbi->log.count < 1
	|| ((sbi->log.number + sbi->log.count) << sbi->sector_shift)
	   > sbi->part_size[0]
	|| sbi->root.subpart >= sbi->n_subparts) {
	printk("ps2fs: bad journal or root location in superblock\n");
	goto out;
    }
    *fsck_stat = le32_to_cpu(psb->fsck_stat);
    err = 0;
  out:
    brelse(bh);
    return err;
}

/* Make the mount writable: refuse flagged volumes, replay the journal */
static int start_writing(struct super_block *sb, __u32 fsck_stat)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    int err;

    if (fsck_stat & PS2FS_FSCK_WRITE_ERROR) {
	printk(KERN_WARNING "ps2fs: volume is flagged with a write error; "
	       "check it on the PS2 (or with pfsshell) before writing\n");
	return -EROFS;
    }
    down(&sbi->lock);
    sbi->writable = 1;
    sbi->error = 0;
    err = ps2fs_journal_replay(sb);
    if (!err)
	err = ps2fs_count_free(sb);
    if (err)
	sbi->writable = 0;
    up(&sbi->lock);
    if (!err) {
	sb->s_flags |= MS_NOATIME | MS_NODIRATIME;
	printk(KERN_WARNING "ps2fs: unsafe_write: writing is experimental; "
	       "keep a backup of this partition\n");
    }
    return err;
}

struct super_block *ps2fs_read_super(struct super_block *sb, void *data,
				     int silent)
{
    struct ps2fs_sb_info *sbi;
    struct ps2fs_options opt;
    struct inode *root;
    __u32 fsck_stat = 0;
    int n;

    memset(&opt, 0, sizeof(opt));
    opt.tzoffset = 540;		/* the PS2 clock runs on JST */
    if (!parse_options((char *)data, &opt))
	return NULL;

    sbi = kmalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
	return NULL;
    memset(sbi, 0, sizeof(*sbi));
    init_MUTEX(&sbi->lock);
    sbi->tzoffset = opt.tzoffset * 60;
    sbi->uid = opt.uid;
    sbi->gid = opt.gid;
    sbi->unsafe_write = opt.unsafe_write;
    sb->u.generic_sbp = sbi;

    set_blocksize(sb->s_dev, 512);
    sb->s_blocksize = 512;
    sb->s_blocksize_bits = 9;
    sb->s_maxbytes = (loff_t)1 << 40;
    sb->s_magic = PS2FS_SUPER_MAGIC;
    sb->s_op = &ps2fs_sops;

    n = find_partition(sb->s_dev, 512, *opt.partition ? opt.partition : NULL,
		       sbi->part_start, sbi->part_size);
    if (n < 0)
	goto fail;
    sbi->n_subparts = n;
    if (read_pfs_super(sb, silent, &fsck_stat))
	goto fail;
    sbi->max_tblocks = ((sbi->log.count << sbi->sector_shift) - 2) / 2;
    if (sbi->max_tblocks > PS2FS_JOURNAL_SLOTS)
	sbi->max_tblocks = PS2FS_JOURNAL_SLOTS;

    /* PSBBN keeps its own data here (see README): read-only unless asked */
    if (!opt.unsafe_write)
	sb->s_flags |= MS_RDONLY;
    if (!(sb->s_flags & MS_RDONLY) && start_writing(sb, fsck_stat)) {
	printk(KERN_WARNING "ps2fs: mounting read-only\n");
	sb->s_flags |= MS_RDONLY;
    }
    if (!sbi->writable) {
	down(&sbi->lock);
	ps2fs_journal_check(sb);
	n = ps2fs_count_free(sb);
	up(&sbi->lock);
	if (n)
	    goto fail;
    }

    root = iget(sb, PS2FS_MAKE_INUM(sbi->root.number, sbi->root.subpart));
    if (!root || is_bad_inode(root) || !S_ISDIR(root->i_mode)) {
	ps2fs_error(sb, "ps2fs_read_super", "unable to read root directory");
	if (root)
	    iput(root);
	goto fail;
    }
    sb->s_root = d_alloc_root(root);
    if (!sb->s_root) {
	iput(root);
	goto fail;
    }
    return sb;

  fail:
    sb->u.generic_sbp = NULL;
    kfree(sbi);
    return NULL;
}

/*************************************************************************/

static void ps2fs_put_super(struct super_block *sb)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);

    down(&sbi->lock);
    if (sbi->writable)
	ps2fs_commit(sb);
    up(&sbi->lock);
    sb->u.generic_sbp = NULL;
    kfree(sbi);
}

/* Called by sync and by kupdate every few seconds while s_dirt is set */
static void ps2fs_write_super(struct super_block *sb)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);

    down(&sbi->lock);
    ps2fs_commit(sb);
    up(&sbi->lock);
}

static int ps2fs_statfs(struct super_block *sb, struct statfs *sf)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);

    sf->f_type = PS2FS_SUPER_MAGIC;
    sf->f_bsize = sbi->zone_size;
    down(&sbi->lock);
    sf->f_blocks = sbi->total_zones;
    sf->f_bfree = sbi->free_zones;
    up(&sbi->lock);
    sf->f_bavail = sf->f_bfree;
    sf->f_files = -1;
    sf->f_ffree = -1;
    sf->f_fsid.val[0] = -1;
    sf->f_fsid.val[1] = -1;
    sf->f_namelen = PS2FS_NAME_LEN;
    return 0;
}

static int ps2fs_remount_fs(struct super_block *sb, int *flags, char *data)
{
    struct ps2fs_sb_info *sbi = PS2FS_SB(sb);
    struct ps2fs_options opt;
    __u32 fsck_stat = 0;
    int err;

    memset(&opt, 0, sizeof(opt));
    if (!parse_options(data, &opt))
	return -EINVAL;
    if ((*flags & MS_RDONLY) && sbi->writable) {
	down(&sbi->lock);
	ps2fs_commit(sb);
	sbi->writable = 0;
	up(&sbi->lock);
    } else if (!(*flags & MS_RDONLY) && !sbi->writable) {
	/* read-write needs unsafe_write, now or at mount */
	if (!opt.unsafe_write && !sbi->unsafe_write)
	    return -EROFS;
	sbi->unsafe_write = 1;
	if ((err = read_pfs_super(sb, 0, &fsck_stat)))
	    return err;
	if ((err = start_writing(sb, fsck_stat)))
	    return err;
    }
    return 0;
}

/*************************************************************************/
/*************************************************************************/

static DECLARE_FSTYPE_DEV(ps2fs_fs_type, "ps2fs", ps2fs_read_super);

static int __init init_ps2fs_fs(void)
{
    return register_filesystem(&ps2fs_fs_type);
}

static void __exit exit_ps2fs_fs(void)
{
    unregister_filesystem(&ps2fs_fs_type);
}

MODULE_AUTHOR("Andrew Church <achurch@achurch.org>");
MODULE_DESCRIPTION("Playstation 2 filesystem");
MODULE_LICENSE("GPLish");

EXPORT_NO_SYMBOLS;

module_init(init_ps2fs_fs)
module_exit(exit_ps2fs_fs)
