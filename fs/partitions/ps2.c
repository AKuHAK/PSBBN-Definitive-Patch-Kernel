/*
 *  fs/partitions/ps2.c
 *  support for PlayStation 2 partition(APA) 
 *
 *        Copyright (C) 2002  Sony Computer Entertainment Inc.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 *
 * $Id: ps2.c,v 1.1.2.2 2002/07/19 08:45:08 inamoto Exp $
 */


#include <linux/genhd.h>
#include <linux/blk.h>
#include "./ps2.h"
#include <linux/slab.h>
#include <linux/string.h>

static unsigned int get_ptable_blocksize(kdev_t dev)
{
  int ret = 1024;

  /*
   * See whether the low-level driver has given us a minumum blocksize.
   * If so, check to see whether it is larger than the default of 1024.
   */
  if (!blksize_size[MAJOR(dev)])
    {
      return ret;
    }

  /*
   * Check for certain special power of two sizes that we allow.
   * With anything larger than 1024, we must force the blocksize up to
   * the natural blocksize for the device so that we don't have to try
   * and read partial sectors.  Anything smaller should be just fine.
   */
  switch( blksize_size[MAJOR(dev)][MINOR(dev)] )
    {
    case 2048:
      ret = 2048;
      break;
    case 4096:
      ret = 4096;
      break;
    case 8192:
      ret = 8192;
      break;
    case 1024:
    case 512:
    case 256:
    case 0:
      /*
       * These are all OK.
       */
      break;
    default:
      panic("Strange blocksize for partition table\n");
    }

  return ret;

}



struct ps2_mbr_part {
	unsigned long start;
	unsigned long nr_sects;
	char apa_id[PS2_PART_NID + 1];	/* APA partition it clashes with */
};

static unsigned long ps2_get_le32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long) p[3] << 24);
}

/* 1 if the sector ranges [s1, s1 + n1) and [s2, s2 + n2) intersect */
static int ps2_overlap(unsigned long s1, unsigned long n1,
		       unsigned long s2, unsigned long n2)
{
	if (n1 == 0 || n2 == 0)
		return 0;
	return s1 <= s2 ? s2 - s1 < n1 : s1 - s2 < n2;
}

/* The MBR entries of sector 0 that are partitions on this disk */
static void ps2_mbr_read(const unsigned char *sect0, unsigned long disk_sects,
			 struct ps2_mbr_part *mp)
{
	const unsigned char *e;
	unsigned long start, n;
	int i;

	memset(mp, 0, sizeof(*mp) * PS2_MBR_PARTS);
	if (sect0[510] != 0x55 || sect0[511] != 0xaa)
		return;
	for (i = 0; i < PS2_MBR_PARTS; i++) {
		e = sect0 + PS2_MBR_TABLE + 16 * i;
		start = ps2_get_le32(e + 8);
		n = ps2_get_le32(e + 12);
		if (e[4] == 0 || e[4] == PS2_MBR_TYPE_JAIL || n == 0 ||
		    start >= disk_sects || n > disk_sects - start)
			continue;
		mp[i].start = start;
		mp[i].nr_sects = n;
	}
}

/*
 * Drop the MBR entries that overlap the APA partition of main header pp.
 * A reserving partition may hold them, but not its APA header areas
 * (resv_m sectors of the main partition, resv_s of each sub).
 */
static void ps2_mbr_exclude(struct ps2_partition *pp, struct ps2_mbr_part *mp,
			    int resv_m, int resv_s)
{
	int i, j, hit, reserve;
	unsigned long nm, ns;

	reserve = strncmp(pp->id, PS2_MBR_RESERVE_ID,
			  strlen(PS2_MBR_RESERVE_ID)) == 0;
	nm = reserve ? resv_m : pp->nsector;
	for (i = 0; i < PS2_MBR_PARTS; i++) {
		if (mp[i].nr_sects == 0)
			continue;
		hit = ps2_overlap(pp->start, nm, mp[i].start, mp[i].nr_sects);
		for (j = 0; !hit && j < pp->nsub && j < PS2_PART_MAXSUB; j++) {
			ns = reserve ? resv_s : pp->subs[j].nsector;
			hit = ps2_overlap(pp->subs[j].start, ns,
					  mp[i].start, mp[i].nr_sects);
		}
		if (hit) {
			mp[i].nr_sects = 0;
			memcpy(mp[i].apa_id, pp->id, PS2_PART_NID);
			mp[i].apa_id[PS2_PART_NID] = '\0';
		}
	}
}

/* Give the MBR entries left the last PS2_MBR_PARTS minors of the disk */
static int ps2_mbr_add(struct gendisk *hd, kdev_t dev, struct ps2_mbr_part *mp)
{
	struct hd_struct *part;
	int i, added = 0;

	for (i = 0; i < PS2_MBR_PARTS; i++) {
		if (mp[i].nr_sects == 0)
			continue;
		part = &hd->part[MINOR(dev) + hd->max_p - PS2_MBR_PARTS + i];
		if (part->nr_sects != 0) {
			/* the number is taken by an APA __linux.N partition */
			mp[i].nr_sects = 0;
			sprintf(mp[i].apa_id, "%s%d", PS2_LINUX_ID,
				hd->max_p - PS2_MBR_PARTS + i);
			continue;
		}
		if (part->hash) {
			kfree(part->hash);
			part->hash = NULL;
		}
		if (part->seg) {
			kfree(part->seg);
			part->seg = NULL;
		}
		part->nr_segs = 0;
		part->start_sect = mp[i].start;
		part->nr_sects = mp[i].nr_sects;
		added++;
	}
	return added;
}

static int ps2_partition_one(struct gendisk *hd, kdev_t dev,
			     struct ps2_partition *pp, int resv_m, int resv_s)
{
	int i, pno;
	unsigned long nr_sects;
	struct hd_struct *part;
	struct hd_seg_struct *seg, *sp;
	char *p, *pe;
	int resv0;


	if (pp->magic != PS2_PARTITION_MAGIC)
		return 0;

	if ((pp->flag & PS2_PART_FLAG_SUB) != 0)
		return 1;

	pe = &pp->id[PS2_PART_NID];
	if (pp->id[0] != '\0' && pp->id[1] != '\0' &&
		strncmp((char*)(&pp->id), PS2_LINUX_ID, strlen(PS2_LINUX_ID)) == 0) {
		/* PS2 Linux partition */
		resv0 = resv_m;
		p = &pp->id[strlen(PS2_LINUX_ID)];
	} else
		return 1;	/* not PS2 Linux partition */

	pno = 0;
	while (p < pe && *p >= '0' && *p <= '9')
		pno = pno * 10 + (*p++ - '0');
	if (pno == 0)
		pno = 1;
	if (pno < 1 || pno > hd->max_p - 1)
		return 1;

        part = &hd->part[MINOR(dev) + pno];


	if (part->hash) {
		kfree(part->hash);
		part->hash = NULL;
	}
	if (part->seg) {
		kfree(part->seg);
		part->seg = NULL;
	}
	if ((part->hash = (struct hd_seg_struct **)kmalloc(sizeof(struct hd_seg_struct *) * PS2_SEGMENT_HASH_ENTRIES, GFP_KERNEL)) == NULL)
		return 0;
	seg = (struct hd_seg_struct *)kmalloc(sizeof(struct hd_seg_struct) * (pp->nsub + 1), GFP_KERNEL);
	if (seg == NULL) {
		kfree(part->hash);
		part->hash = NULL;
		return 0;
	}
	part->nr_segs = pp->nsub + 1;
	part->seg = seg;
	seg->start_sect = part->start_sect = pp->start + resv0;
	seg->nr_sects = nr_sects = pp->nsector - resv0;
	seg->offset = 0;
	seg++;
	for (i = 0; i < pp->nsub; i++) {
		seg->start_sect = pp->subs[i].start + resv_s;
		seg->nr_sects = pp->subs[i].nsector - resv_s;
		seg->offset = nr_sects;
		nr_sects += seg->nr_sects;
		seg++;
	}
	part->nr_sects = nr_sects;
	part->hash_unit = (nr_sects + PS2_SEGMENT_HASH_ENTRIES - 1) / PS2_SEGMENT_HASH_ENTRIES;
	sp = part->seg;
	nr_sects = 0;
	for (i = 0; i < PS2_SEGMENT_HASH_ENTRIES; i++) {
		while (nr_sects < part->nr_sects &&
		       nr_sects > sp->offset + sp->nr_sects)
			sp++;
		part->hash[i] = sp;
		nr_sects += part->hash_unit;
	}

	return 1;
}



int ps2_partition(struct gendisk *hd, struct block_device *bdev , unsigned long first_sector, int first_part_minor)
{
	struct buffer_head *bh;
	int dev_bsize, dev_ssize, stob, resv_m, resv_s;
	struct ps2_partition *pp;
	struct ps2_mbr_part mp[PS2_MBR_PARTS];
	unsigned long sect;
	int i, complete, mbr_parts;
	char buf[8];
	kdev_t dev;


	dev = to_kdev_t(bdev->bd_dev);

	dev_bsize = get_ptable_blocksize(dev);
	dev_ssize = 512;
	if (hardsect_size[MAJOR(dev)] != 0)
		dev_ssize = hardsect_size[MAJOR(dev)][MINOR(dev)];
	stob = dev_bsize / dev_ssize;
	if (stob == 0)
		stob = 1;
	resv_m = PS2_PART_RESV_MAIN / dev_ssize;
	if (resv_m == 0)
		resv_m = 1;
	resv_s = PS2_PART_RESV_SUB / dev_ssize;
	if (resv_s == 0)
		resv_s = 1;

	sect = 0;
	complete = 0;
	do {
		if ((bh = bread(dev, sect / stob, dev_bsize)) == 0) {
			printk("%s: unable to read sector %lu\n",
				kdevname(dev), sect);
			return -1;
		}
		pp = (struct ps2_partition *)bh->b_data;
		if (sect == 0) {
			if (memcmp(pp->mbr.magic, PS2_MBR_MAGIC, 32) != 0 ||
			    pp->mbr.version > PS2_MBR_VERSION) {
				brelse(bh);
				return 0;
			}
			ps2_mbr_read((unsigned char *) bh->b_data,
				     hd->part[MINOR(dev)].nr_sects, mp);
		}
		if (pp->magic == PS2_PARTITION_MAGIC &&
		    (pp->flag & PS2_PART_FLAG_SUB) == 0)
			ps2_mbr_exclude(pp, mp, resv_m, resv_s);
		if (!ps2_partition_one(hd, dev, pp, resv_m, resv_s)) {
			brelse(bh);
			break;
		}
		sect = pp->next;
		brelse(bh);
		if (sect == 0)
			complete = 1;
	} while (sect != 0);

	/* MBR partitions only when the whole APA chain was checked */
	mbr_parts = 0;
	if (complete && hd->max_p > PS2_MBR_PARTS)
		mbr_parts = ps2_mbr_add(hd, dev, mp);

        printk(" [APA]");
	for (i = 1; i < hd->max_p; i++) {
		if (mbr_parts && i == hd->max_p - PS2_MBR_PARTS)
			printk(" [MBR]");
		if (hd->part[MINOR(dev) + i].nr_sects != 0) {
                        printk(" %s", disk_name(hd, MINOR(dev) + i, buf)); /* new */
                }
                                
	}
	printk ("\n");

	for (i = 0; complete && i < PS2_MBR_PARTS; i++)
		if (mp[i].apa_id[0])
			printk(KERN_WARNING "%s: MBR partition %d clashes with APA partition %s, not used\n",
			       kdevname(dev), i + 1, mp[i].apa_id);

	return 1;
}
