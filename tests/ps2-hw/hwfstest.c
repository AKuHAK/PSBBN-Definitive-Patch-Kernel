/* Filesystem test for real hardware (PS2 Linux, gcc 2.95, glibc 2.2).
 *
 *   hwfstest DIR write  FLAGS BIGMB   build a known tree under DIR
 *   hwfstest DIR verify FLAGS BIGMB   check it (after umount/mount)
 *   hwfstest DIR clean                remove DIR
 *
 * FLAGS: letters for what the filesystem supports
 *   R rename   S symlink   T truncate to a smaller non-zero size
 *   M chmod/utime persist  C case-insensitive names   W shared mmap write
 *   P print CKSUM lines (for checking on a PC)
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <utime.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/vfs.h>

static int npass, nfail;
static char D[512];
static const char *flags = "";

#define HAS(c) (strchr(flags, c) != NULL)

static void ok(const char *n) { printf("PASS %s\n", n); npass++; }
static void bad(const char *n, const char *fmt, long a, long b)
{
	printf("FAIL %s: ", n); printf(fmt, a, b); printf(" (errno=%d %s)\n", errno, strerror(errno));
	nfail++;
}
#define CHECK(name, cond, fmt, a, b) do { if (cond) ok(name); else bad(name, fmt, (long)(a), (long)(b)); } while (0)

static unsigned char pat(long long off, int seed) { return (unsigned char)((off * 7 + seed + (off >> 9)) & 0xff); }

static char *P(const char *rel)
{
	static char buf[8][1024];
	static int n;
	n = (n + 1) % 8;
	sprintf(buf[n], "%s/%s", D, rel);
	return buf[n];
}

static double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* 0 if [0,size) of the file is the pattern and the size matches */
static long long check_pattern(const char *path, long long size, int seed)
{
	static unsigned char buf[65536];
	long long off = 0;
	struct stat st;
	int fd = open(path, O_RDONLY), n, i;
	if (fd < 0) return -1;
	if (fstat(fd, &st) || st.st_size != size) { close(fd); return -2; }
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < n; i++)
			if (buf[i] != pat(off + i, seed)) { close(fd); return off + i + 1; }
		off += n;
	}
	close(fd);
	return off == size ? 0 : -3;
}

static int put_pattern(int fd, long long off, long long len, int seed)
{
	static unsigned char buf[65536];
	long long done = 0;
	int n, i;
	if (lseek(fd, off, SEEK_SET) != off) return -1;
	while (done < len) {
		n = len - done > 3001 ? 3001 : (int)(len - done);
		for (i = 0; i < n; i++) buf[i] = pat(off + done + i, seed);
		if (write(fd, buf, n) != n) return -1;
		done += n;
	}
	return 0;
}

static int make_file(const char *path, long long size, int seed)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644), r;
	if (fd < 0) return -1;
	r = put_pattern(fd, 0, size, seed);
	if (close(fd)) r = -1;
	return r;
}

static int zero_range(const char *path, long long from, long long to)
{
	unsigned char buf[4096];
	int fd = open(path, O_RDONLY), n, i;
	if (fd < 0 || lseek(fd, from, SEEK_SET) != from) return 0;
	while (from < to && (n = read(fd, buf, to - from > 4096 ? 4096 : (int)(to - from))) > 0) {
		for (i = 0; i < n; i++) if (buf[i]) { close(fd); return 0; }
		from += n;
	}
	close(fd);
	return from == to;
}

static long long fsize(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long long)st.st_size; }

static int count_dir(const char *p)
{
	DIR *d = opendir(p); struct dirent *e; int n = 0;
	if (!d) return -1;
	while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
	closedir(d);
	return n;
}

static int rm_rf(const char *path)
{
	char p[1024];
	struct stat st;
	DIR *d;
	struct dirent *e;
	int r = 0;
	if (lstat(path, &st)) return -1;
	if (!S_ISDIR(st.st_mode)) return unlink(path);
	if (!(d = opendir(path))) return -1;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		sprintf(p, "%s/%s", path, e->d_name);
		if (rm_rf(p)) r = -1;
	}
	closedir(d);
	if (rmdir(path)) r = -1;
	return r;
}

/* POSIX cksum */
static unsigned long crctab[256];
static void crc_init(void)
{
	unsigned long c; int i, j;
	for (i = 0; i < 256; i++) {
		c = (unsigned long)i << 24;
		for (j = 0; j < 8; j++) c = c & 0x80000000UL ? (c << 1) ^ 0x04C11DB7UL : c << 1;
		crctab[i] = c & 0xffffffffUL;
	}
}
static void print_cksum(const char *rel)
{
	unsigned long crc = 0, len = 0, l;
	static unsigned char buf[65536];
	int fd = open(P(rel), O_RDONLY), n, i;
	if (fd < 0) { printf("CKSUM %s missing\n", rel); return; }
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < n; i++) crc = ((crc << 8) ^ crctab[((crc >> 24) ^ buf[i]) & 0xff]) & 0xffffffffUL;
		len += n;
	}
	close(fd);
	for (l = len; l; l >>= 8) crc = ((crc << 8) ^ crctab[((crc >> 24) ^ (l & 0xff)) & 0xff]) & 0xffffffffUL;
	printf("CKSUM %s %lu %lu\n", rel, (~crc) & 0xffffffffUL, len);
}

static const long long sizes[] = { 0, 1, 511, 512, 513, 4095, 4096, 4097, 8191, 8192, 8193, 65537, 1048579 };
#define NSIZES (int)(sizeof(sizes) / sizeof(sizes[0]))
#define NMANY 400
#define NFRAG 150
#define CHUNK 8192

static char longname[256];

/*************************************************************************/

static void do_write(long long big)
{
	char p[1024], q[64];
	int fd, fb, r, i, errat;
	double t;
	unsigned char *map;
	struct stat st;
	struct utimbuf ut;
	pid_t pid[2];

	r = mkdir(D, 0755);
	CHECK("mkdir test dir", r == 0, "r=%ld%ld", r, 0);
	r = mkdir(P("sizes"), 0755);
	errat = r ? -1 : 0;
	for (i = 0; i < NSIZES && !errat; i++) {
		sprintf(q, "sizes/f_%lld", sizes[i]);
		if (make_file(P(q), sizes[i], i) || check_pattern(P(q), sizes[i], i)) errat = i + 1;
	}
	CHECK("files of 0 B .. 1 MB", errat == 0, "at=%ld%ld", errat, 0);

	fd = open(P("sizes/f_65537"), O_RDWR);
	memset(p, 0xAB, 1000);
	r = 0;
	for (i = 0; i < 9; i++) if (pwrite(fd, p, 1000, 20000 + i * 1000) != 1000) r = -1;
	close(fd);
	CHECK("overwrite in the middle", r == 0 && fsize(P("sizes/f_65537")) == 65537, "r=%ld%ld", r, 0);

	r = make_file(P("app"), 5000, 3);
	fd = open(P("app"), O_WRONLY | O_APPEND);
	r |= fd < 0 || put_pattern(fd, 5000, 10000, 3);
	if (fd >= 0) close(fd);
	CHECK("append", r == 0 && check_pattern(P("app"), 15000, 3) == 0, "r=%ld%ld", r, 0);

	if (HAS('T')) {
		r = make_file(P("trunc"), 100000, 4) | truncate(P("trunc"), 30000);
		CHECK("truncate shrink", r == 0 && check_pattern(P("trunc"), 30000, 4) == 0, "r=%ld%ld", r, 0);
	} else {
		r = make_file(P("trunc"), 30000, 4);
	}
	r = make_file(P("zeroed"), 50000, 5) | truncate(P("zeroed"), 0);
	CHECK("truncate to 0", r == 0 && fsize(P("zeroed")) == 0, "r=%ld size=%ld", r, fsize(P("zeroed")));
	r = make_file(P("ext"), 1000, 6) | truncate(P("ext"), 50000);
	CHECK("truncate extend", r == 0 && fsize(P("ext")) == 50000 && zero_range(P("ext"), 1000, 50000), "r=%ld%ld", r, 0);
	fd = open(P("sparse"), O_WRONLY | O_CREAT, 0644);
	r = fd >= 0 && lseek(fd, 200000, SEEK_SET) == 200000 ? write(fd, "end", 3) : -1;
	if (fd >= 0) close(fd);
	CHECK("write past the end", r == 3 && fsize(P("sparse")) == 200003 && zero_range(P("sparse"), 0, 200000), "r=%ld%ld", r, 0);

	t = now();
	r = make_file(P("big"), big, 50);
	sync();
	t = now() - t;
	CHECK("big file write", r == 0, "r=%ld%ld", r, 0);
	printf("SPEED write %lld bytes %.1f s %.0f KB/s\n", big, t, big / 1024.0 / t);

	/* two files growing in turn: fragmented allocation */
	fd = open(P("fragA"), O_WRONLY | O_CREAT, 0644);
	fb = open(P("fragB"), O_WRONLY | O_CREAT, 0644);
	errat = fd < 0 || fb < 0;
	for (i = 0; i < NFRAG && !errat; i++)
		errat = put_pattern(fd, (long long)i * CHUNK, CHUNK, 21) || put_pattern(fb, (long long)i * CHUNK, CHUNK, 22);
	if (fd >= 0) close(fd);
	if (fb >= 0) close(fb);
	CHECK("interleaved growth", !errat && check_pattern(P("fragA"), NFRAG * CHUNK, 21) == 0
	      && check_pattern(P("fragB"), NFRAG * CHUNK, 22) == 0, "e=%ld%ld", errat, 0);

	/* two processes writing at once */
	for (i = 0; i < 2; i++) {
		if ((pid[i] = fork()) == 0) {
			sprintf(q, "par%d", i);
			_exit(make_file(P(q), 1500000, 30 + i) ? 1 : 0);
		}
	}
	r = 0;
	for (i = 0; i < 2; i++) {
		int status = 1;
		waitpid(pid[i], &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status)) r = 1;
	}
	CHECK("two writers at once", r == 0 && check_pattern(P("par0"), 1500000, 30) == 0
	      && check_pattern(P("par1"), 1500000, 31) == 0, "r=%ld%ld", r, 0);

	/* a big directory, half deleted, slots reused */
	r = mkdir(P("many"), 0755);
	errat = r ? -1 : 0;
	for (i = 1; i <= NMANY && !errat; i++) {
		sprintf(q, "many/file_%03d", i);
		if (make_file(P(q), (i * 37) % 5000, i)) errat = i;
	}
	CHECK("400 files", errat == 0 && count_dir(P("many")) == NMANY, "at=%ld n=%ld", errat, count_dir(P("many")));
	for (i = 2; i <= NMANY && !errat; i += 2) {
		sprintf(q, "many/file_%03d", i);
		if (unlink(P(q))) errat = i;
	}
	for (i = 1; i <= 50 && !errat; i++) {
		sprintf(q, "many/new_%03d", i);
		if (make_file(P(q), i * 100, i + 100)) errat = i;
	}
	CHECK("unlink 200, create 50", errat == 0 && count_dir(P("many")) == NMANY / 2 + 50, "at=%ld n=%ld", errat, count_dir(P("many")));

	r = mkdir(P("nest"), 0755) | mkdir(P("nest/a"), 0755) | mkdir(P("nest/a/b"), 0755) | mkdir(P("nest/a/b/c"), 0755);
	r |= make_file(P("nest/a/b/c/deep"), 12345, 77);
	CHECK("nested dirs", r == 0, "r=%ld%ld", r, 0);
	r = mkdir(P("nest"), 0755);
	CHECK("mkdir existing", r < 0 && errno == EEXIST, "r=%ld errno=%ld", r, errno);
	r = rmdir(P("nest"));
	CHECK("rmdir non-empty", r < 0 && (errno == ENOTEMPTY || errno == EEXIST), "r=%ld errno=%ld", r, errno);
	r = mkdir(P("empty"), 0755) | rmdir(P("empty"));
	CHECK("rmdir", r == 0 && fsize(P("empty")) < 0, "r=%ld%ld", r, 0);
	fd = open(P("app"), O_WRONLY | O_CREAT | O_EXCL, 0644);
	CHECK("O_EXCL on existing", fd < 0 && errno == EEXIST, "fd=%ld errno=%ld", fd, errno);
	sprintf(p, "%s", longname);
	r = make_file(P(p), 33, 5);
	CHECK("255-character name", r == 0 && check_pattern(P(p), 33, 5) == 0, "r=%ld%ld", r, 0);

	if (HAS('C')) {
		CHECK("case-insensitive lookup", check_pattern(P("NEST/A/B/C/DEEP"), 12345, 77) == 0, "%ld%ld", 0, 0);
		fd = open(P("APP"), O_WRONLY | O_CREAT | O_EXCL, 0644);
		CHECK("case-insensitive O_EXCL", fd < 0 && errno == EEXIST, "fd=%ld errno=%ld", fd, errno);
	}

	make_file(P("ren1"), 4000, 8);
	make_file(P("victim"), 50000, 9);
	make_file(P("src"), 777, 10);
	r = mkdir(P("mv"), 0755) | make_file(P("mv/inmv"), 100, 11);
	if (HAS('R')) {
		r = rename(P("ren1"), P("ren2")) | rename(P("ren2"), P("nest/a/ren3"));
		CHECK("rename file (same dir, across dirs)", r == 0 && check_pattern(P("nest/a/ren3"), 4000, 8) == 0 && fsize(P("ren1")) < 0, "r=%ld%ld", r, 0);
		r = rename(P("mv"), P("nest/mvd"));
		CHECK("rename dir across dirs", r == 0 && check_pattern(P("nest/mvd/inmv"), 100, 11) == 0, "r=%ld%ld", r, 0);
		r = rename(P("src"), P("victim"));
		CHECK("rename over a file", r == 0 && check_pattern(P("victim"), 777, 10) == 0 && fsize(P("src")) < 0, "r=%ld%ld", r, 0);
	} else {
		r = rename(P("ren1"), P("ren2"));
		CHECK("rename refused, nothing lost", r < 0 && check_pattern(P("ren1"), 4000, 8) == 0 && fsize(P("ren2")) < 0, "r=%ld errno=%ld", r, errno);
		r = rename(P("src"), P("victim"));
		CHECK("rename over a file refused, nothing lost", r < 0 && check_pattern(P("victim"), 50000, 9) == 0
		      && check_pattern(P("src"), 777, 10) == 0, "r=%ld errno=%ld", r, errno);
	}

	if (HAS('S')) {
		r = symlink("big", P("link"));
		i = readlink(P("link"), p, sizeof(p));
		CHECK("symlink", r == 0 && i == 3 && !memcmp(p, "big", 3) && fsize(P("link")) == big, "r=%ld len=%ld", r, i);
	}
	if (HAS('M')) {
		ut.actime = ut.modtime = 1000000000;
		r = chmod(P("sizes/f_1"), 0600) | utime(P("sizes/f_1"), &ut);
		CHECK("chmod, utime", r == 0 && stat(P("sizes/f_1"), &st) == 0 && st.st_mtime == 1000000000, "r=%ld%ld", r, 0);
	}

	fd = open(P("tmpopen"), O_RDWR | O_CREAT, 0644);
	put_pattern(fd, 0, 100000, 12);
	r = unlink(P("tmpopen"));
	i = put_pattern(fd, 100000, 50000, 12);
	CHECK("unlink an open file", r == 0 && i == 0 && fsize(P("tmpopen")) < 0 && lseek(fd, 0, SEEK_END) == 150000, "r=%ld i=%ld", r, i);
	close(fd);

	fd = open(P("mm"), O_RDWR | O_CREAT, 0644);
	r = fd >= 0 ? put_pattern(fd, 0, 65536, 13) : -1;
	map = fd >= 0 ? mmap(0, 65536, PROT_READ | (HAS('W') ? PROT_WRITE : 0), MAP_SHARED, fd, 0) : MAP_FAILED;
	if (map != MAP_FAILED) {
		for (i = 0; i < 65536; i++) if (map[i] != pat(i, 13)) { r = 2; break; }
		if (HAS('W')) {
			memset(map + 1000, 0xCD, 3000);
			r |= msync(map, 65536, MS_SYNC);
		}
		munmap(map, 65536);
	} else {
		r = 3;
	}
	if (fd >= 0) close(fd);
	CHECK(HAS('W') ? "mmap read and write" : "mmap read", r == 0, "r=%ld%ld", r, 0);
	sync();
}

/*************************************************************************/

static void do_verify(long long big)
{
	char p[1024], q[64];
	int i, r, errat, fd;
	double t;
	unsigned char *map;
	struct stat st;

	errat = 0;
	for (i = 0; i < NSIZES; i++) {
		long long m;
		sprintf(q, "sizes/f_%lld", sizes[i]);
		if (sizes[i] == 65537) {
			int k;
			unsigned char b[65537];
			fd = open(P(q), O_RDONLY);
			if (fd < 0 || read(fd, b, 65537) != 65537) errat = i + 1;
			else for (k = 0; k < 65537; k++)
				if (b[k] != (k >= 20000 && k < 29000 ? 0xAB : pat(k, i))) { errat = i + 1; break; }
			if (fd >= 0) close(fd);
			continue;
		}
		m = check_pattern(P(q), sizes[i], i);
		if (m) errat = i + 1;
	}
	CHECK("sizes and overwrite persisted", errat == 0, "at=%ld%ld", errat, 0);
	CHECK("append persisted", check_pattern(P("app"), 15000, 3) == 0, "%ld%ld", 0, 0);
	CHECK("truncations persisted", check_pattern(P("trunc"), 30000, 4) == 0 && fsize(P("zeroed")) == 0
	      && fsize(P("ext")) == 50000 && check_pattern(P("ext"), 50000, 6) != 0 && zero_range(P("ext"), 1000, 50000), "%ld%ld", 0, 0);
	CHECK("sparse persisted", fsize(P("sparse")) == 200003 && zero_range(P("sparse"), 0, 200000), "%ld%ld", 0, 0);
	t = now();
	r = (int)check_pattern(P("big"), big, 50);
	t = now() - t;
	CHECK("big file", r == 0, "m=%ld%ld", r, 0);
	printf("SPEED read %lld bytes %.1f s %.0f KB/s\n", big, t, big / 1024.0 / t);
	CHECK("fragmented files", check_pattern(P("fragA"), NFRAG * CHUNK, 21) == 0 && check_pattern(P("fragB"), NFRAG * CHUNK, 22) == 0, "%ld%ld", 0, 0);
	CHECK("parallel files", check_pattern(P("par0"), 1500000, 30) == 0 && check_pattern(P("par1"), 1500000, 31) == 0, "%ld%ld", 0, 0);
	errat = 0;
	for (i = 1; i <= NMANY; i++) {
		sprintf(q, "many/file_%03d", i);
		if (i % 2 ? check_pattern(P(q), (i * 37) % 5000, i) != 0 : fsize(P(q)) >= 0) errat = i;
	}
	for (i = 1; i <= 50; i++) {
		sprintf(q, "many/new_%03d", i);
		if (check_pattern(P(q), i * 100, i + 100)) errat = 1000 + i;
	}
	CHECK("big directory", errat == 0 && count_dir(P("many")) == NMANY / 2 + 50, "at=%ld n=%ld", errat, count_dir(P("many")));
	CHECK("nested", check_pattern(P("nest/a/b/c/deep"), 12345, 77) == 0 && fsize(P("empty")) < 0, "%ld%ld", 0, 0);
	CHECK("long name", check_pattern(P(longname), 33, 5) == 0, "%ld%ld", 0, 0);
	if (HAS('R')) {
		CHECK("renames persisted", check_pattern(P("nest/a/ren3"), 4000, 8) == 0 && check_pattern(P("nest/mvd/inmv"), 100, 11) == 0
		      && check_pattern(P("victim"), 777, 10) == 0 && fsize(P("src")) < 0 && fsize(P("mv")) < 0, "%ld%ld", 0, 0);
	} else {
		CHECK("files kept after refused rename", check_pattern(P("ren1"), 4000, 8) == 0 && check_pattern(P("victim"), 50000, 9) == 0
		      && check_pattern(P("src"), 777, 10) == 0, "%ld%ld", 0, 0);
	}
	if (HAS('S')) {
		i = readlink(P("link"), p, sizeof(p));
		CHECK("symlink persisted", i == 3 && !memcmp(p, "big", 3), "len=%ld%ld", i, 0);
	}
	if (HAS('M')) {
		stat(P("sizes/f_1"), &st);
		CHECK("mtime persisted", st.st_mtime == 1000000000, "mtime=%ld%ld", st.st_mtime, 0);
	}
	CHECK("unlinked open file gone", fsize(P("tmpopen")) < 0, "%ld%ld", 0, 0);
	fd = open(P("mm"), O_RDONLY);
	map = fd >= 0 ? mmap(0, 65536, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
	r = map == MAP_FAILED;
	if (!r) {
		for (i = 0; i < 65536; i++)
			if (map[i] != (HAS('W') && i >= 1000 && i < 4000 ? 0xCD : pat(i, 13))) { r = 2; break; }
		munmap(map, 65536);
	}
	if (fd >= 0) close(fd);
	CHECK("mmap file persisted", r == 0, "r=%ld%ld", r, 0);

	if (HAS('P')) {
		for (i = 0; i < NSIZES; i++) { sprintf(q, "sizes/f_%lld", sizes[i]); print_cksum(q); }
		print_cksum("app"); print_cksum("trunc"); print_cksum("ext"); print_cksum("sparse");
		print_cksum("big"); print_cksum("fragA"); print_cksum("fragB"); print_cksum("par0"); print_cksum("par1");
		print_cksum("many/file_399"); print_cksum("many/new_050"); print_cksum("nest/a/b/c/deep");
		print_cksum("victim"); print_cksum("mm");
	}
}

int main(int argc, char **argv)
{
	long long big;

	setvbuf(stdout, NULL, _IONBF, 0);
	crc_init();
	memset(longname, 'L', 255); longname[255] = 0;
	if (argc < 3) {
		fprintf(stderr, "usage: hwfstest DIR write|verify|clean [FLAGS] [BIGMB]\n");
		return 2;
	}
	strcpy(D, argv[1]);
	flags = argc > 3 ? argv[3] : "";
	big = (argc > 4 ? atoll(argv[4]) : 16) * 1024 * 1024 + 77;
	if (!strcmp(argv[2], "write")) do_write(big);
	else if (!strcmp(argv[2], "verify")) do_verify(big);
	else if (!strcmp(argv[2], "clean")) {
		int r = rm_rf(D);
		CHECK("remove the test tree", r == 0 && fsize(D) < 0, "r=%ld%ld", r, 0);
	}
	printf("RESULT pass=%d fail=%d\n", npass, nfail);
	return nfail != 0;
}
