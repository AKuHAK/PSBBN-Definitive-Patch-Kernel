/* Dump the used zones of a PFS partition (all sub-partitions) to stdout.
 *   pfsdump /dev/hda PARTITION > dump
 * Format: "PFSDUMP1", u32 nsubs+1, u32 zone size, then per subpart
 * u32 start sector, u32 sectors; then records u32 sub, u32 zone, u32 count
 * followed by count zones of data; a record with count 0 ends the dump.
 * Also prints used zones per subpart to stderr. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int fd;
static int rd(unsigned long long sector, void *buf, unsigned int len)
{
	return pread(fd, buf, len, (off_t)sector * 512) == (ssize_t)len ? 0 : -1;
}
static void out(const void *p, unsigned int n)
{
	if (fwrite(p, 1, n, stdout) != n) { perror("write"); exit(1); }
}

int main(int argc, char **argv)
{
	unsigned char h[1024], sb[512], *bm, *zbuf;
	unsigned int sector = 0, start[65], size[65], zsize, nz, z, run, used, w[3];
	int nsub = -1, i, s, ss;

	if (argc < 3) return 2;
	if ((fd = open(argv[1], O_RDONLY)) < 0) { perror("open"); return 1; }
	do {
		if (rd(sector, h, 1024) || memcmp(h + 4, "APA", 4)) { fprintf(stderr, "bad APA\n"); return 1; }
		if (!strncmp((char *)h + 16, argv[2], 32)) {
			start[0] = ((unsigned int *)h)[16];
			size[0] = ((unsigned int *)h)[17];
			for (nsub = 0; nsub < 64 && ((unsigned int *)(h + 512))[2 * nsub + 1]; nsub++) {
				start[nsub + 1] = ((unsigned int *)(h + 512))[2 * nsub];
				size[nsub + 1] = ((unsigned int *)(h + 512))[2 * nsub + 1];
			}
			break;
		}
		sector = ((unsigned int *)h)[2];
	} while (sector);
	if (nsub < 0) { fprintf(stderr, "no partition %s\n", argv[2]); return 1; }
	if (rd(start[0] + 8192, sb, 512) || ((unsigned int *)sb)[0] != 0x50465300U) { fprintf(stderr, "no PFS superblock\n"); return 1; }
	zsize = ((unsigned int *)sb)[4];
	if (((unsigned int *)sb)[5] < (unsigned int)nsub) nsub = ((unsigned int *)sb)[5];
	for (ss = 0; (512UL << ss) < zsize; ss++) ;
	out("PFSDUMP1", 8);
	w[0] = nsub + 1; w[1] = zsize; out(w, 8);
	for (s = 0; s <= nsub; s++) { w[0] = start[s]; w[1] = size[s]; out(w, 8); }
	zbuf = malloc(zsize);
	for (s = 0; s <= nsub; s++) {
		nz = size[s] >> ss;
		bm = calloc(1, nz / 8 + 1024);
		/* bitmap: 1 KiB blocks from block (zone/1024) (+4096 on the main partition) */
		if (rd(start[s] + ((zsize / 1024) + (s == 0 ? 4096 : 0)) * 2, bm, (nz / 8 + 1023) & ~1023U)) {
			fprintf(stderr, "bitmap read failed\n"); return 1;
		}
		used = 0;
		for (z = 0; z < nz; z += run) {
			run = 0;
			while (z + run < nz && (bm[(z + run) / 8] >> ((z + run) % 8) & 1)) run++;
			if (!run) { run = 1; continue; }
			w[0] = s; w[1] = z; w[2] = run; out(w, 12);
			for (i = 0; i < (int)run; i++) {
				if (rd(start[s] + ((z + i) << ss), zbuf, zsize)) { fprintf(stderr, "read failed\n"); return 1; }
				out(zbuf, zsize);
			}
			used += run;
		}
		fprintf(stderr, "subpart %d: %u of %u zones used\n", s, used, nz);
		free(bm);
	}
	w[0] = w[1] = w[2] = 0; out(w, 12);
	return 0;
}
