/* List the APA partitions of a PS2 disk: apalist /dev/hda
 * With "dump FILE", also write every partition header (1 KiB at its start
 * sector) to FILE as records: u32 sector, then the 1024 bytes. */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned char h[1024];
	unsigned long sector = 0, next, start, len, nsub, i, sum;
	unsigned short type, flags;
	char id[33];
	FILE *dump = NULL;
	int fd = open(argc > 1 ? argv[1] : "/dev/hda", O_RDONLY), n = 0;

	if (fd < 0) { perror("open"); return 1; }
	if (argc > 3 && !strcmp(argv[2], "dump")) dump = fopen(argv[3], "wb");
	do {
		if (lseek(fd, (off_t)sector * 512, SEEK_SET) < 0 || read(fd, h, 1024) != 1024) { perror("read"); return 1; }
		if (memcmp(h + 4, "APA", 4)) { printf("no APA magic at %lu\n", sector); return 1; }
		for (sum = 0, i = 1; i < 256; i++) sum += ((unsigned long *)h)[i];
		next = ((unsigned long *)h)[2];
		start = ((unsigned long *)h)[16];
		len = ((unsigned long *)h)[17];
		type = ((unsigned short *)h)[36];
		flags = ((unsigned short *)h)[37];
		nsub = ((unsigned long *)h)[19];
		memcpy(id, h + 16, 32); id[32] = 0;
		printf("%-32s start %10lu len %10lu (%6lu MB) type %04x flags %04x nsub %lu%s\n",
		       id, start, len, len / 2048, type, flags, nsub, sum == ((unsigned long *)h)[0] ? "" : " BADSUM");
		if (dump) { fwrite(&sector, 4, 1, dump); fwrite(h, 1024, 1, dump); }
		n++;
		sector = next;
	} while (sector != 0 && n < 10000);
	if (dump) fclose(dump);
	return 0;
}
