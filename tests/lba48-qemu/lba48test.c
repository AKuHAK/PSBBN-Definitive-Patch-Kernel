/*
 * 48-bit LBA test, /sbin/init of the QEMU guest.  /dev/hdb is a disk larger
 * than 2 TiB: the IDE driver must report 2^32 - 1 sectors and read and write
 * every sector at its own address: PIO, DMA, PIO with READ/WRITE MULTIPLE.  Each sector written
 * holds its LBA and the pass, so that the host can check the addresses in
 * the image afterwards (check.py, from the REGION lines).  /dev/hdb2, above 128 GiB, holds exFAT.
 *
 * Environment from the kernel command line: expect=<sectors of /dev/hdb>
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/sysmacros.h>

#ifndef BLKGETSIZE
#define BLKGETSIZE	0x1260
#define BLKFLSBUF	0x1261
#endif
#define HDIO_GET_MULTCOUNT	0x0304
#define HDIO_GET_DMA	0x030b
#define HDIO_SET_MULTCOUNT	0x0321
#define HDIO_SET_DMA	0x0326

#define NSECT		64	/* sectors per region */

static int npass, nfail;

static void check(const char *what, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (ok)
		npass++;
	else
		nfail++;
}

static void stamp(unsigned char *buf, unsigned long lba, int pass)
{
	int i;

	for (i = 0; i < 512; i += 32)
		sprintf((char *) buf + i, "LBA %010lu PASS %d ......\n", lba, pass);
}

/* Write NSECT stamped sectors from lba with one write(), read them back */
static void region(int fd, unsigned long lba, int pass, const char *name)
{
	static unsigned char wbuf[NSECT * 512], rbuf[NSECT * 512];
	char what[128];
	off_t off = (off_t) lba * 512;
	int i;

	for (i = 0; i < NSECT; i++)
		stamp(wbuf + i * 512, lba + i, pass);
	printf("REGION %lu %d\n", lba, NSECT);
	sprintf(what, "pass %d %s: write %d sectors at LBA %lu", pass, name,
		NSECT, lba);
	check(what, lseek(fd, off, SEEK_SET) == off &&
	      write(fd, wbuf, sizeof(wbuf)) == (ssize_t) sizeof(wbuf) &&
	      fsync(fd) == 0);
	ioctl(fd, BLKFLSBUF, 0);	/* drop the buffer cache: read the disk */
	memset(rbuf, 0, sizeof(rbuf));
	sprintf(what, "pass %d %s: read back", pass, name);
	check(what, lseek(fd, off, SEEK_SET) == off &&
	      read(fd, rbuf, sizeof(rbuf)) == (ssize_t) sizeof(rbuf) &&
	      memcmp(rbuf, wbuf, sizeof(rbuf)) == 0);
}

static void exfat(void)
{
	static unsigned char buf[65536], rbuf[65536];
	unsigned int i, j;
	int fd, ok;

	mkdir("/mnt", 0755);
	check("mount /dev/hdb2 (exFAT above 128 GiB)",
	      mount("/dev/hdb2", "/mnt", "exfat", 0, "") == 0);
	fd = open("/mnt/big.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ok = fd >= 0;
	for (i = 0; ok && i < 128; i++) {	/* 8 MiB */
		for (j = 0; j < sizeof(buf); j++)
			buf[j] = (unsigned char) (i * 7 + j);
		ok = write(fd, buf, sizeof(buf)) == (ssize_t) sizeof(buf);
	}
	check("exFAT: write 8 MiB file", ok && close(fd) == 0);
	check("umount", umount("/mnt") == 0);
	check("mount again", mount("/dev/hdb2", "/mnt", "exfat", 0, "") == 0);
	fd = open("/mnt/big.bin", O_RDONLY);
	ok = fd >= 0;
	for (i = 0; ok && i < 128; i++) {
		for (j = 0; j < sizeof(buf); j++)
			buf[j] = (unsigned char) (i * 7 + j);
		ok = read(fd, rbuf, sizeof(rbuf)) == (ssize_t) sizeof(rbuf) &&
			memcmp(rbuf, buf, sizeof(buf)) == 0;
	}
	check("exFAT: read the file back", ok);
	if (fd >= 0)
		close(fd);
	check("umount", umount("/mnt") == 0);
}

int main(void)
{
	const char *expect = getenv("expect");
	unsigned long size = 0, want;
	int fd, cfd, dma, mult, pass;

	mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
	cfd = open("/dev/console", O_RDWR);
	if (cfd >= 0) {
		dup2(cfd, 0); dup2(cfd, 1); dup2(cfd, 2);
		if (cfd > 2)
			close(cfd);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	mknod("/dev/hdb", S_IFBLK | 0600, makedev(3, 64));
	mknod("/dev/hdb2", S_IFBLK | 0600, makedev(3, 66));

	want = expect ? strtoul(expect, NULL, 0) : 0;
	fd = open("/dev/hdb", O_RDWR);
	check("open /dev/hdb", fd >= 0);
	if (fd >= 0) {
		ioctl(fd, BLKGETSIZE, &size);
		printf("hdb: %lu sectors, expected %lu\n", size, want);
		check("capacity", size == want);
		for (pass = 0; pass < 3; pass++) {
			ioctl(fd, HDIO_SET_DMA, pass == 1);
			ioctl(fd, HDIO_SET_MULTCOUNT, pass == 2 ? 16 : 0);
			dma = mult = -1;
			ioctl(fd, HDIO_GET_DMA, &dma);
			ioctl(fd, HDIO_GET_MULTCOUNT, &mult);
			printf("pass %d: using_dma=%d multcount=%d\n", pass, dma, mult);
			check(pass == 1 ? "DMA on" : "DMA off", dma == (pass == 1));
			check(pass == 2 ? "multcount 16" : "multcount 0",
			      mult == (pass == 2 ? 16 : 0));
			/* boundary regions cross the boundary in every pass */
			region(fd, 1000 + pass * 4096, pass, "28-bit");
			region(fd, 0x0fffff00UL - pass * 4096, pass, "below 2^28");
			region(fd, 0x0fffffe0UL - pass * 8, pass, "across 2^28");
			region(fd, 0x10001000UL + pass * 4096, pass, "above 2^28");
			region(fd, 0x7fffffe0UL - pass * 8, pass, "across 2^31");
			/*
			 * The block layer sizes devices in 1 KiB units, so the
			 * last sector of an odd-sized disk (2^32 - 1 here) is
			 * not reachable through /dev/hdb.
			 */
			region(fd, (size & ~1UL) - NSECT - pass * 4096, pass, "end of disk");
		}
		close(fd);
	}
	exfat();
	printf("RESULT pass=%d fail=%d\n", npass, nfail);
	sync();
	reboot(RB_AUTOBOOT);
	return 0;
}
