#ifndef STUB_BLK_H
#define STUB_BLK_H
struct buffer_head {
	char *b_data;
};
extern int *blksize_size[256];
extern int *hardsect_size[256];
struct buffer_head *bread(kdev_t dev, int block, int size);
void brelse(struct buffer_head *bh);
void panic(const char *msg);
#endif
