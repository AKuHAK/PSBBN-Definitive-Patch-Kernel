/*
 * Run fs/partitions/ps2.c on a disk image in user space and print the
 * partitions it registers.  Usage: parttest <image>
 */
#include <sys/types.h>
#include "linux/genhd.h"
#include "linux/blk.h"
#include "linux/slab.h"

static FILE *img;
int *blksize_size[256];
int *hardsect_size[256];

struct buffer_head *bread(kdev_t dev, int block, int size)
{
	struct buffer_head *bh = malloc(sizeof(*bh));

	bh->b_data = calloc(1, size);
	if (fseeko(img, (off_t) block * size, SEEK_SET) != 0 ||
	    fread(bh->b_data, size, 1, img) != 1) {
		free(bh->b_data);
		free(bh);
		return NULL;
	}
	return bh;
}

void brelse(struct buffer_head *bh)
{
	free(bh->b_data);
	free(bh);
}

void panic(const char *msg)
{
	printf("panic: %s\n", msg);
	exit(2);
}

char *disk_name(struct gendisk *hd, int minor, char *buf)
{
	sprintf(buf, "hda%d", minor);
	return buf;
}

const char *kdevname(kdev_t dev)
{
	return "hda";
}

#include "../../fs/partitions/ps2.c"

int main(int argc, char **argv)
{
	static struct hd_struct part[64];
	struct gendisk hd;
	struct block_device bdev;
	int i, j, r;

	if (argc != 2 || (img = fopen(argv[1], "rb")) == NULL) {
		fprintf(stderr, "usage: parttest <image>\n");
		return 2;
	}
	fseeko(img, 0, SEEK_END);
	part[0].nr_sects = ftello(img) / 512;
	hd.max_p = 64;
	hd.part = part;
	bdev.bd_dev = MKDEV(3, 0);
	printf("hda:");
	r = ps2_partition(&hd, &bdev, 0, 1);
	printf("result %d\n", r);
	for (i = 1; i < 64; i++) {
		if (part[i].nr_sects == 0)
			continue;
		printf("part %d %lu %lu", i, part[i].start_sect, part[i].nr_sects);
		for (j = 0; part[i].nr_segs > 1 && j < part[i].nr_segs; j++)
			printf(" seg %lu+%lu@%lu", part[i].seg[j].start_sect,
			       part[i].seg[j].nr_sects, part[i].seg[j].offset);
		printf("\n");
	}
	return 0;
}
