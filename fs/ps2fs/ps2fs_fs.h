#ifndef _LINUX_PS2FS_FS_H
#define _LINUX_PS2FS_FS_H

#include <linux/version.h>

/*
 * PlayStation 2 filesystem (PFS) on-disk format and driver definitions.
 *
 * Units: a sector is 512 bytes; a metadata block is 1024 bytes (2 sectors;
 * superblock area, inodes, segment descriptors, bitmap chunks, directory
 * chunks, attribute areas, journal slots); a zone (2..128 KiB) is the
 * allocation unit.  Zone numbers count from the start of their own
 * subpart: 0 is the main APA partition, 1..num_subs its sub-partitions.
 * All on-disk integers are little-endian.
 *
 * An inode number is (subpart << 24 | zone of the inode).
 */

/*************************************************************************/

#define PS2FS_SUPER_MAGIC	0x50465300	/* "PFS\0" */
#define PS2FS_JOURNAL_MAGIC	0x5046534C	/* "PFSL" */
#define PS2FS_SEGD_MAGIC	0x53454744	/* "SEGD": inode */
#define PS2FS_SEGI_MAGIC	0x53454749	/* "SEGI": indirect descriptor */
#define PS2FS_FORMAT_VERSION	3

#define PS2FS_SUPER_SECTOR	8192	/* in the main partition; backup +1 */
#define PS2FS_APA_RESERVED	0x2000	/* main-partition sectors owned by APA */

/* Main partition plus up to 64 sub-partitions */
#define PS2FS_MAX_SUBPARTS	65

#define PS2FS_NAME_LEN		255
#define PS2FS_SEGD_ENTRIES	114	/* data[] slots in an inode */
#define PS2FS_SEGI_ENTRIES	123	/* data[] slots in an indirect block */
#define PS2FS_MAX_EXTENT	65535	/* zones in one data[] entry */
#define PS2FS_JOURNAL_SLOTS	127
#define PS2FS_SYMLINK_MAX	903	/* bytes of target stored in the inode */

/* Type nibble of inode mode and of directory entries (iomanX values) */
#define PS2FS_TYPE_MASK		0xF000
#define PS2FS_TYPE_DIR		0x1000
#define PS2FS_TYPE_FILE		0x2000
#define PS2FS_TYPE_SYMLINK	0x4000

/* Inode attr bits */
#define PS2FS_ATTR_SUBDIR	0x0020
#define PS2FS_ATTR_CLOSED	0x0080
#define PS2FS_ATTR_NEWDIR	0x00A0

/* Superblock pfsFsckStat bits */
#define PS2FS_FSCK_WRITE_ERROR	0x0001

/* uid/gid Sony stores on everything it creates */
#define PS2FS_ID_NONE		0xFFFF

/* Inode number <-> (subpart, zone) */
#define PS2FS_SUBPART(ino)	((ino) >> 24)
#define PS2FS_ZONE(ino)		((ino) & 0xFFFFFF)
#define PS2FS_MAKE_INUM(zone,sub) ((((unsigned long)(sub)) << 24) | (zone))

/*************************************************************************/

/* Zone run: number, subpart, count */
struct ps2fs_blockinfo {
    __u32 number;
    __u16 subpart;
    __u16 count;
};

/* Date/time (plain binary, local time of the console clock) */
struct ps2fs_datetime {
    __u8 unused;
    __u8 sec;
    __u8 min;
    __u8 hour;
    __u8 day;
    __u8 month;
    __u16 year;
};

/* Superblock (first 40 bytes of sector 8192 of the main partition) */
struct ps2fs_super_block {
    __u32 magic;
    __u32 version;
    __u32 modver;
    __u32 fsck_stat;
    __u32 zone_size;
    __u32 num_subs;
    struct ps2fs_blockinfo log;		/* journal */
    struct ps2fs_blockinfo root;	/* root directory inode */
};

/* Inode (SEGD), block 0 of its zone; block 1 is the attribute area */
struct ps2fs_inode {
    __u32 checksum;			/* sum of words 1..255 */
    __u32 magic;
    struct ps2fs_blockinfo inode_block;	/* this inode */
    struct ps2fs_blockinfo next_segment; /* first SEGI */
    struct ps2fs_blockinfo last_segment; /* last descriptor of the chain */
    struct ps2fs_blockinfo unused;
    struct ps2fs_blockinfo data[PS2FS_SEGD_ENTRIES]; /* data[0]: this inode */
    __u16 mode;
    __u16 attr;
    __u16 uid;
    __u16 gid;
    struct ps2fs_datetime atime;
    struct ps2fs_datetime ctime;
    struct ps2fs_datetime mtime;
    __u32 size_lo;
    __u32 size_hi;
    __u32 number_blocks;		/* zones in all entries incl. descriptors */
    __u32 number_data;			/* used entries over the whole chain */
    __u32 number_segdesg;		/* descriptors: 1 + number of SEGIs */
    __u32 subpart;			/* subdirectory placement cursor */
    __u32 reserved[4];
};

/* Indirect segment descriptor (SEGI), block 0 of its own zone */
struct ps2fs_segi {
    __u32 checksum;
    __u32 magic;
    struct ps2fs_blockinfo inode_block;	/* owning inode */
    struct ps2fs_blockinfo next_segment;
    struct ps2fs_blockinfo last_segment; /* previous descriptor */
    struct ps2fs_blockinfo unused;
    struct ps2fs_blockinfo data[PS2FS_SEGI_ENTRIES]; /* data[0]: this SEGI */
};

/* Directory entry; entries tile each 512-byte sector exactly */
struct ps2fs_dirent {
    __u32 inode;			/* zone of the target inode; 0 = empty */
    __u8 subpart;
    __u8 namelen;			/* 0 = empty */
    __u16 size_type;			/* allocated length | type nibble */
    char name[0];			/* not terminated, padded to 4 bytes */
};

#define PS2FS_DIRENT_SIZEMASK	0x0FFF
#define PS2FS_DIRENT_USED(len)	(((len) + 8 + 3) & ~3)

/* Journal header, sectors 0-1 of the journal */
struct ps2fs_journal {
    __u32 magic;
    __u16 num;
    __u16 checksum;			/* low 16 bits of the sum of words 2..255 */
    struct {
	__u32 sector;			/* home sector within its subpart */
	__u16 sub;
	__u16 log_sector;		/* image offset from the journal start */
    } log[PS2FS_JOURNAL_SLOTS];
};

/*************************************************************************/

#ifdef __KERNEL__

#include <linux/fs.h>
#include <asm/semaphore.h>

#define dprintk(fmt,rest...)

/* One 1 KiB metadata block held in a transaction */
struct ps2fs_tblock {
    __u16 sub;
    __u32 block;			/* 1 KiB block number within the subpart */
    struct buffer_head *bh[2];
};

struct ps2fs_sb_info {
    struct semaphore lock;		/* all metadata access and allocation */
    __u32 n_subparts;			/* num_subs + 1 */
    __u32 part_start[PS2FS_MAX_SUBPARTS];	/* device sectors */
    __u32 part_size[PS2FS_MAX_SUBPARTS];	/* sectors */
    __u32 zone_size;
    __u32 sector_shift;			/* log2(zone / 512) */
    __u32 block_shift;			/* log2(zone / 1024) */
    struct ps2fs_blockinfo log;		/* from the superblock, CPU order */
    struct ps2fs_blockinfo root;
    __u32 total_zones;
    __u32 free_zones;
    __u32 free_zone[PS2FS_MAX_SUBPARTS];
    __s32 tzoffset;			/* seconds east of UTC */
    uid_t uid;
    gid_t gid;
    int unsafe_write;			/* mount option */
    int writable;			/* unsafe_write and mounted read-write */
    int error;				/* metadata I/O failed; refuse writes */
    struct ps2fs_tblock tblocks[PS2FS_JOURNAL_SLOTS];
    int ntblocks;			/* blocks in the open transaction */
    int max_tblocks;			/* journal capacity */
};

struct ps2fs_inode_info {
    struct ps2fs_inode raw;		/* copy of the on-disk inode */
    struct ps2fs_blockinfo *ext;	/* entries 0..number_data-1 of the chain */
    __u32 ext_cap;
    unsigned long mmu_private;		/* bytes allocated and initialised */
    int writers;			/* open for writing */
};

static inline struct ps2fs_sb_info *PS2FS_SB(struct super_block *sb)
{
    return (struct ps2fs_sb_info *)sb->u.generic_sbp;
}

static inline struct ps2fs_inode_info *PS2FS_I(struct inode *inode)
{
    return (struct ps2fs_inode_info *)inode->u.generic_ip;
}

/* Global chain index -> slot in its descriptor, and the descriptor */
static inline __u32 ps2fs_fix(__u32 i)
{
    return i < PS2FS_SEGD_ENTRIES ? i
	: (i - PS2FS_SEGD_ENTRIES) % PS2FS_SEGI_ENTRIES;
}
static inline __u32 ps2fs_desc(__u32 i)
{
    return i < PS2FS_SEGD_ENTRIES ? 0
	: 1 + (i - PS2FS_SEGD_ENTRIES) / PS2FS_SEGI_ENTRIES;
}

/* journal.c: metadata blocks and transactions (sbi->lock held) */
extern struct buffer_head *ps2fs_bread(struct super_block *sb, __u16 sub,
				       __u32 sector);
extern int ps2fs_read_block(struct super_block *sb, __u16 sub, __u32 block,
			    void *buf);
extern struct ps2fs_tblock *ps2fs_tblock_get(struct super_block *sb,
					     __u16 sub, __u32 block, int noload);
extern void ps2fs_tblock_drop(struct super_block *sb, __u16 sub,
			      __u32 first, __u32 count);
extern int ps2fs_commit(struct super_block *sb);
extern int ps2fs_journal_replay(struct super_block *sb);
extern int ps2fs_journal_check(struct super_block *sb);
extern __u32 ps2fs_checksum(const void *block);
extern void ps2fs_metadata_error(struct super_block *sb, const char *what);

/* bitmap.c (sbi->lock held) */
extern int ps2fs_count_free(struct super_block *sb);
extern int ps2fs_extend_run(struct super_block *sb, struct ps2fs_blockinfo *e,
			    __u32 want);
extern int ps2fs_alloc(struct super_block *sb, struct ps2fs_blockinfo *hint,
		       __u32 maxcount);
extern int ps2fs_free(struct super_block *sb, const struct ps2fs_blockinfo *e);

/* segment.c (sbi->lock held unless noted) */
extern int ps2fs_load_chain(struct inode *inode);
extern void ps2fs_free_chain_memory(struct inode *inode);
extern int ps2fs_map(struct inode *inode, __u32 zone_index, __u16 *sub,
		     __u32 *zone);
extern __u32 ps2fs_data_zones(struct inode *inode);
extern int ps2fs_grow(struct inode *inode, __u32 need);
extern int ps2fs_shrink(struct inode *inode, __u32 keep);
extern int ps2fs_free_inode_zones(struct inode *inode);
extern int ps2fs_write_raw(struct inode *inode);

/* inode.c */
extern struct address_space_operations ps2fs_aops;
extern struct inode_operations ps2fs_file_iops;
extern struct inode_operations ps2fs_symlink_iops;
extern struct file_operations ps2fs_fops;
extern void ps2fs_read_inode(struct inode *inode);
extern void ps2fs_write_inode(struct inode *inode, int wait);
extern void ps2fs_delete_inode(struct inode *inode);
extern void ps2fs_clear_inode(struct inode *inode);
extern void ps2fs_set_inode_ops(struct inode *inode);
extern int ps2fs_setattr(struct dentry *dentry, struct iattr *attr);
extern int ps2fs_new_inode(struct inode *dir, int type, int perm,
			   const char *symlink, struct ps2fs_blockinfo *where);

/* dir.c */
extern struct inode_operations ps2fs_dir_iops;
extern struct file_operations ps2fs_dops;

/* misc.c */
extern void ps2fs_warning(struct super_block *sb, const char *function,
			  const char *fmt, ...);
extern void ps2fs_error(struct super_block *sb, const char *function,
			const char *fmt, ...);
extern time_t from_ps2time(const struct ps2fs_datetime *dt, __s32 tzoffset);
extern void to_ps2time(struct ps2fs_datetime *dt, time_t time,
		       __s32 tzoffset);

/* unicode.c */
extern void translate_from_unicode(unsigned char *buf);
extern int translate_to_unicode(const struct dentry *dentry,
				unsigned char *dest, int destlen);

#endif /* __KERNEL__ */

#endif /* _LINUX_PS2FS_FS_H */
