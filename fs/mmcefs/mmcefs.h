/*
 *  linux/fs/mmcefs/mmcefs.h
 *
 *  Filesystem for the SD card of a Multipurpose Memory Card Emulator
 *  (SD2PSX, MemCard PRO2, ...) in a PlayStation 2 memory card slot.
 *
 *  The card speaks the MMCE protocol over SIO2, which only the IOP can
 *  reach.  On the IOP, mmceman.irx exposes the card's files as the iomanX
 *  devices "mmce0:" and "mmce1:", and fileXio.irx serves iomanX to the EE
 *  over SIF RPC.  This driver is a fileXio client: every operation is a
 *  path or file descriptor request to the IOP.  The IRX modules are loaded
 *  from the root filesystem on the first mount if the IOP lacks them.
 *
 *  Locking: fx_lock() serialises all RPCs, protects the shared DMA buffers
 *  and the IOP descriptors kept in inodes, and makes a seek plus the read
 *  or write that follows it atomic.  It nests inside the page lock and
 *  i_sem; while holding it the driver only talks to the IOP and never
 *  allocates memory with __GFP_FS.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */
#ifndef _MMCEFS_H
#define _MMCEFS_H

#include <linux/types.h>
#include <linux/fs.h>

#define MMCEFS_SUPER_MAGIC	0x4d4d4345	/* "MMCE" */
#define MMCEFS_ROOT_INO		1
#define MMCEFS_PATH_MAX		512	/* fileXio path buffer, incl. "mmceN:" */
#define MMCEFS_NAME_MAX		255

/* iomanX open flags */
#define FX_O_RDONLY	0x0001
#define FX_O_WRONLY	0x0002
#define FX_O_RDWR	0x0003
#define FX_O_CREAT	0x0200
#define FX_O_TRUNC	0x0400
#define FX_O_EXCL	0x0800

/* iox_stat_t.mode */
#define FX_S_IFMT	0xF000
#define FX_S_IFDIR	0x1000
#define FX_S_IFREG	0x2000

/* iox_stat_t of ps2sdk (64 bytes) */
struct fx_stat {
	u32	mode;
	u32	attr;
	u32	size;
	u8	ctime[8];	/* -, sec, min, hour, day, month, year (LE16) */
	u8	atime[8];
	u8	mtime[8];
	u32	hisize;
	u32	priv[6];
};

/* iox_dirent_t of ps2sdk; fileXio transfers the first 320 bytes */
struct fx_dirent {
	struct fx_stat stat;
	char	name[256];
	u32	privdata;
};

struct mmcefs_sb_info {
	int		slot;		/* 0 or 1: device "mmce0:" / "mmce1:" */
	uid_t		uid;
	gid_t		gid;
	unsigned short	fmask;
	unsigned short	dmask;
};

struct mmcefs_inode_info {
	int		fd;		/* IOP file descriptor, or -1 */
	int		fd_write;	/* fd was opened for writing */
	int		openers;	/* struct files using fd */
	int		dead;		/* removed; never reopen by path */
};

static inline struct mmcefs_sb_info *MMCEFS_SB(struct super_block *sb)
{
	return (struct mmcefs_sb_info *)sb->u.generic_sbp;
}

static inline struct mmcefs_inode_info *MMCEFS_I(struct inode *inode)
{
	return (struct mmcefs_inode_info *)inode->u.generic_ip;
}

/*
 * fxio.c: fileXio RPC client.  All return a negative errno on failure and
 * must be called with fx_lock() held.
 */
#define FX_IOBUF	8192	/* largest fx_read()/fx_write() transfer */
extern int fx_init(void);		/* module init/exit, no lock */
extern void fx_exit(void);
extern void fx_lock(void);
extern void fx_unlock(void);
extern int fx_bind(void);
extern int fx_open(const char *path, int flags, int mode);
extern int fx_close(int fd);
extern int fx_read(int fd, void *buf, int size);
extern int fx_write(int fd, const void *buf, int size);
extern loff_t fx_lseek64(int fd, loff_t offset, int whence);
extern int fx_getstat(const char *path, struct fx_stat *st);
extern int fx_dopen(const char *path);
extern int fx_dread(int fd, struct fx_dirent *de);
extern int fx_dclose(int fd);
extern int fx_mkdir(const char *path, int mode);
extern int fx_rmdir(const char *path);
extern int fx_remove(const char *path);
extern int fx_rename(const char *from, const char *to);

/* super.c */
extern char *mmcefs_build_path(struct dentry *dentry);
extern void mmcefs_put_path(char *path);
extern struct inode *mmcefs_build_inode(struct super_block *sb,
					const struct fx_stat *st, int root);
extern time_t mmcefs_time(const u8 *t);
extern void mmcefs_set_blocks(struct inode *inode);

/* file.c */
extern struct inode_operations mmcefs_file_inode_operations;
extern struct file_operations mmcefs_file_operations;
extern struct address_space_operations mmcefs_aops;
extern int mmcefs_fd_get(struct inode *inode, struct dentry *dentry,
			 int write);
extern void mmcefs_fd_close(struct inode *inode);
extern int mmcefs_setattr(struct dentry *dentry, struct iattr *attr);

/* namei.c */
extern struct inode_operations mmcefs_dir_inode_operations;
extern struct file_operations mmcefs_dir_operations;
extern struct dentry_operations mmcefs_dentry_ops;

#endif /* _MMCEFS_H */
