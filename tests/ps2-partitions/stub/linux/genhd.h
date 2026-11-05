/* Just enough of the kernel for fs/partitions/ps2.c in user space */
#ifndef STUB_GENHD_H
#define STUB_GENHD_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int __u32;
typedef unsigned short __u16;
typedef int kdev_t;

#define MAJOR(dev)	((dev) >> 8)
#define MINOR(dev)	((dev) & 0xff)
#define MKDEV(ma, mi)	(((ma) << 8) | (mi))
#define to_kdev_t(dev)	(dev)

#define KERN_WARNING	"<4>"
#define printk		printf

struct hd_seg_struct {
	unsigned long start_sect;
	unsigned long nr_sects;
	unsigned long offset;
};

struct hd_struct {
	unsigned long start_sect;
	unsigned long nr_sects;
	int nr_segs;
	struct hd_seg_struct *seg;
	unsigned long hash_unit;
	struct hd_seg_struct **hash;
};

struct gendisk {
	int max_p;
	struct hd_struct *part;
};

struct block_device {
	int bd_dev;
};

char *disk_name(struct gendisk *hd, int minor, char *buf);
const char *kdevname(kdev_t dev);
#endif
