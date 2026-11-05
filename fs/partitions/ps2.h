/*
 *  fs/partitions/ps2.h
 *  support for PlayStation 2 partition(APA) 
 *
 *        Copyright (C) 2002  Sony Computer Entertainment Inc.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 *
 * $Id: ps2.h,v 1.1.2.2 2002/07/19 08:45:08 inamoto Exp $
 */


#define PS2_SEGMENT_HASH_ENTRIES	16

#define PS2_PARTITION_MAGIC	0x00415041
#define PS2_MBR_VERSION		0x00000002
#define PS2_MBR_MAGIC		"Sony Computer Entertainment Inc."

#define PS2_PART_NID		32
#define PS2_PART_NPASSWD	8
#define PS2_PART_NNAME		128
#define PS2_PART_MAXSUB		64
#define PS2_PART_MAXSUB_SHIFT	6

#define PS2_PART_FLAG_SUB	0x0001

#define PS2_PART_RESV_MAIN	(4 * 1024 * 1024)
#define PS2_PART_RESV_SUB	(4 * 1024)

struct ps2_partition {
	__u32	sum;
	__u32	magic;
	__u32	next;
	__u32	prev;
	char	id[PS2_PART_NID];
	char	rpwd[PS2_PART_NPASSWD];
	char	fpwd[PS2_PART_NPASSWD];
	__u32	start;
	__u32	nsector;
	__u16	fstype;
	__u16	flag;
	__u32	nsub;
	char	date[8];
	__u32	main;
	__u32	number;
	__u32	modver;
	__u32	rsvd[7];
	char	name[PS2_PART_NNAME];
	struct ps2_mbr {
		char	magic[32];
		__u32	version;
		__u32	nsector;
		char	date[8];
		__u32	osd_start;
		__u32	osd_count;
		char	rsvd[200];
	} mbr;
	struct ps2_subpart {
		__u32	start;
		__u32	nsector;
	} subs[PS2_PART_MAXSUB];
};

#define PS2_LINUX_ID		"__linux."

/*
 * A classic MBR in sector 0, inside the APA header (as APA-Jail places
 * it).  Its primary partitions get the last PS2_MBR_PARTS minors of the
 * disk (hda60..hda63), apart from the __linux.N numbers.  An entry is used
 * only if it lies on the disk and overlaps no APA partition other than
 * those whose name starts with PS2_MBR_RESERVE_ID, which reserve its space
 * on the APA side; even there it must leave their APA headers alone.
 * The APA-Jail entry (type 0x17) spans the APA area and is skipped.
 */
#define PS2_MBR_PARTS		4
#define PS2_MBR_TABLE		0x1be
#define PS2_MBR_TYPE_JAIL	0x17
#define PS2_MBR_RESERVE_ID	"__exfat"

int ps2_partition(struct gendisk *hd, struct block_device *bdev , unsigned long first_sector, int first_part_minor);
