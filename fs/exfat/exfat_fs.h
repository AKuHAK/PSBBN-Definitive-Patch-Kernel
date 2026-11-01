/*
 *  linux/fs/exfat/exfat_fs.h
 *
 *  exFAT filesystem for Linux 2.4, built on the FatFs module by ChaN.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 *
 *  Locking:
 *    FatFs is not reentrant, so every FatFs call on a volume is made with
 *    that volume's sbi->lock held.  The lock nests inside the page lock and
 *    inside i_sem; while holding it the driver never takes a page lock or
 *    i_sem and never allocates memory with __GFP_FS (FatFs buffers come
 *    from ff_memalloc() with GFP_NOFS), so memory reclaim cannot re-enter
 *    the driver through ->writepage or ->clear_inode.
 *
 *  Open files:
 *    exFAT has no inode numbers, so files are addressed by path.  Each
 *    inode owns at most one FatFs file object (FIL); it is opened on demand
 *    and closed by the last ->release once dirty pages have been written.
 *    Unlink and rename close or reopen it while holding sbi->lock, and an
 *    inode whose object was removed from disk is marked EXFAT_I_DEAD so it
 *    can never reopen a different file that later reuses its name.
 */
#ifndef _EXFAT_FS_H
#define _EXFAT_FS_H

#include <linux/fs.h>
#include <linux/nls.h>
#include <asm/semaphore.h>

#include "ff.h"

#define EXFAT_SUPER_MAGIC	0x2011BAB0	/* same value as Linux 5.7+ */
#define EXFAT_ROOT_INO		1
#define EXFAT_SECTOR_SIZE	512
#define EXFAT_SECTOR_BITS	9
#define EXFAT_NAME_MAX		255	/* UTF-16 units per name */
#define EXFAT_PATH_MAX		1024	/* UTF-16 units per FatFs path */
#define EXFAT_NAMEBUF		(EXFAT_NAME_MAX * NLS_MAX_CHARSET_SIZE + 1)

struct exfat_mount_options {
	uid_t		uid;
	gid_t		gid;
	unsigned short	fmask;
	unsigned short	dmask;
	unsigned	quiet:1;	/* chown/chmod failures are not errors */
	char		*iocharset;	/* NULL means UTF-8 */
};

struct exfat_sb_info {
	FATFS		fatfs;		/* FatFs volume object */
	struct semaphore lock;		/* serialises FatFs calls */
	struct super_block *sb;
	kdev_t		dev;
	int		vol;		/* FatFs logical drive number */
	unsigned long	clu_size;	/* bytes per cluster */
	struct nls_table *nls;		/* NULL means UTF-8 */
	struct exfat_mount_options opts;
};

struct exfat_inode_info {
	FIL		*fil;		/* open FatFs file, or NULL */
	int		openers;	/* struct files using fil */
	unsigned long	flags;
	BYTE		attr;		/* FatFs AM_* attribute bits */
	DWORD		*cltbl;		/* fast seek cluster link map */
	DWORD		cltbl_len;	/* its size in DWORDs */
};

#define EXFAT_I_DEAD	1	/* removed from disk; never reopen by path */

static inline struct exfat_sb_info *EXFAT_SB(struct super_block *sb)
{
	return (struct exfat_sb_info *)sb->u.generic_sbp;
}

static inline struct exfat_inode_info *EXFAT_I(struct inode *inode)
{
	return (struct exfat_inode_info *)inode->u.generic_ip;
}

/* misc.c */
extern int exfat_vol_attach(struct exfat_sb_info *sbi);
extern void exfat_vol_detach(struct exfat_sb_info *sbi);
extern void exfat_vol_path(struct exfat_sb_info *sbi, WCHAR *path);
extern int exfat_errno(FRESULT res);
extern time_t exfat_time_fat2unix(WORD fdate, WORD ftime);
extern void exfat_time_unix2fat(time_t t, WORD *fdate, WORD *ftime);
extern int exfat_name_to_uni(struct exfat_sb_info *sbi,
			     const unsigned char *name, int len,
			     WCHAR *uni, int max);
extern int exfat_uni_to_name(struct exfat_sb_info *sbi, const WCHAR *uni,
			     unsigned char *name, int max);
extern int exfat_next_char(struct exfat_sb_info *sbi,
			   const unsigned char *name, int len, DWORD *c);
extern WCHAR *exfat_build_path(struct dentry *dentry);
extern void exfat_put_path(WCHAR *path);

/* super.c */
extern struct inode *exfat_build_inode(struct super_block *sb,
				       const FILINFO *fno);
extern void exfat_set_mode(struct inode *inode);
extern void exfat_set_blocks(struct inode *inode);

/* file.c */
extern struct inode_operations exfat_file_inode_operations;
extern struct file_operations exfat_file_operations;
extern struct address_space_operations exfat_aops;
extern int exfat_fil_get(struct inode *inode, struct dentry *dentry,
			 int write);
extern int exfat_setattr(struct dentry *dentry, struct iattr *attr);

/* namei.c */
extern struct inode_operations exfat_dir_inode_operations;
extern struct file_operations exfat_dir_operations;
extern struct dentry_operations exfat_dentry_ops;

#endif /* _EXFAT_FS_H */
