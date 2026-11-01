/* In-guest test suite for the 2.4 ps2fs (PFS) driver; runs as /sbin/init.
 * Kernel command line "phase=N" selects the phase. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/vfs.h>
#include <sys/reboot.h>
#include <sys/sysmacros.h>
#include <utime.h>

#define M "/mnt"
static int npass, nfail;

static void ok(const char *n) { printf("PASS %s\n", n); npass++; }
static void bad(const char *n, const char *fmt, long a, long b)
{
	printf("FAIL %s: ", n); printf(fmt, a, b); printf(" (errno=%d %s)\n", errno, strerror(errno));
	nfail++;
}
#define CHECK(name, cond, fmt, a, b) do { if (cond) ok(name); else bad(name, fmt, (long)(a), (long)(b)); } while (0)

static unsigned char pat(long long off, int seed) { return (unsigned char)((off * 7 + seed + (off >> 9)) & 0xff); }

/* 0 if file equals pattern, offset+1 of first mismatch, or negative */
static long long check_pattern(const char *path, long long size, int seed)
{
	unsigned char buf[3001];
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

/* POSIX cksum (CRC-32, as printed by cksum(1)) */
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
static void print_cksum(const char *path, const char *name)
{
	unsigned long crc = 0, len = 0, l;
	unsigned char buf[4096];
	int fd = open(path, O_RDONLY), n, i;
	if (fd < 0) { printf("CKSUM %s missing\n", name); return; }
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < n; i++) crc = ((crc << 8) ^ crctab[((crc >> 24) ^ buf[i]) & 0xff]) & 0xffffffffUL;
		len += n;
	}
	close(fd);
	for (l = len; l; l >>= 8) crc = ((crc << 8) ^ crctab[((crc >> 24) ^ (l & 0xff)) & 0xff]) & 0xffffffffUL;
	printf("CKSUM %s %lu %lu\n", name, (~crc) & 0xffffffffUL, len);
}

static long fsize(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long)st.st_size; }

static int count_dir(const char *p)
{
	DIR *d = opendir(p); struct dirent *e; int n = 0;
	if (!d) return -1;
	while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
	closedir(d);
	return n;
}

static int dir_has(const char *dir, const char *name)
{
	DIR *d = opendir(dir); struct dirent *e; int f = 0;
	if (!d) return 0;
	while ((e = readdir(d))) if (!strcmp(e->d_name, name)) f = 1;
	closedir(d);
	return f;
}

/* Write `len' pattern bytes at `off' (pattern offsets are file offsets) */
static int put_pattern(int fd, long long off, long long len, int seed)
{
	unsigned char buf[3001];
	long long done = 0;
	int n, i;
	if (lseek(fd, off, SEEK_SET) != off) return -1;
	while (done < len) {
		n = len - done > (long long)sizeof(buf) ? (int)sizeof(buf) : (int)(len - done);
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

/* 1 if [from, to) of a file is all zero */
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

/* "LS dir name[/|@]" for each entry, in readdir (= on-disk) order, for
 * comparison with pfsshell's ls */
static void print_ls(const char *dir)
{
	char p[1024];
	struct stat st;
	DIR *d;
	struct dirent *e;
	const char *label = *dir ? dir : "/";
	sprintf(p, M "%s", dir);
	if (!(d = opendir(p))) { printf("LS %s ?\n", label); return; }
	while ((e = readdir(d))) {
		sprintf(p, M "%s/%s", dir, e->d_name);
		if (lstat(p, &st)) { printf("LS %s %s?\n", label, e->d_name); continue; }
		printf("LS %s %s%s\n", label, e->d_name,
		       S_ISDIR(st.st_mode) ? "/" : S_ISLNK(st.st_mode) ? "@" : "");
	}
	closedir(d);
}

static const long long sizes[] = { 0, 1, 511, 512, 513, 4095, 4096, 4097, 8191, 8192, 8193, 65537, 1048579, 5242880 };
#define NSIZES (int)(sizeof(sizes) / sizeof(sizes[0]))
#define MANY "entry_%04d_with_a_rather_long_name_to_fill_directory_sectors"
#define MANY2 M "/lx/many2/file_%03d_padding_padding_padding"
#define ZONE 8192
#define NFRAG 300

/* Checks of the pfsshell-made content; `changed' after phase 2 */
static void read_checks(const char *tag, int changed)
{
	char p[600], q[700], L[201];
	int i, r, fd;
	long long m;
	struct stat st;
	struct statfs sf;
	unsigned char *map;
	DIR *d;
	struct dirent *e;
	long pos;
	char n1[5][300], n2[5][300];

	for (i = 0; i < NSIZES; i++) {
		if (changed && (sizes[i] == 1 || sizes[i] == 0 || sizes[i] == 65537 ||
		    sizes[i] == 8193 || sizes[i] == 5242880))
			continue;	/* modified in phase 2 */
		sprintf(p, M "/sizes/f_%lld", sizes[i]);
		m = check_pattern(p, sizes[i], i);
		sprintf(q, "%s read size %lld", tag, sizes[i]);
		CHECK(q, m == 0, "m=%ld%ld", m, 0);
	}
	sprintf(q, "%s many count", tag);
	r = count_dir(M "/many");
	CHECK(q, r == (changed ? 250 : 300), "count=%ld%ld", r, 0);
	r = 0;
	for (i = 1; i <= 300; i += changed ? 74 : 37) {
		sprintf(p, M "/many/" MANY, i);
		if (check_pattern(p, i, i)) r = i;
	}
	sprintf(q, "%s many contents", tag);
	CHECK(q, r == 0, "bad entry %ld%ld", r, 0);
	sprintf(q, "%s nested", tag);
	CHECK(q, check_pattern(M "/a/b/c/deep.bin", 12345, 77) == 0, "%ld%ld", 0, 0);
	memset(L, 'L', 200); L[200] = 0;
	sprintf(p, M "/%s", L);
	sprintf(q, "%s 200-char name", tag);
	CHECK(q, dir_has(M, L) && check_pattern(p, 33, 5) == 0, "%ld%ld", 0, 0);
	stat(M "/a", &st);
	sprintf(q, "%s dir mode", tag);
	CHECK(q, S_ISDIR(st.st_mode), "mode=%lo%ld", st.st_mode, 0);
	stat(M "/sizes/f_4096", &st);
	sprintf(q, "%s file mode", tag);
	CHECK(q, S_ISREG(st.st_mode) && st.st_mtime > 1700000000, "mode=%lo mtime=%ld", st.st_mode, st.st_mtime);
	r = statfs(M, &sf);
	sprintf(q, "%s statfs", tag);
	CHECK(q, r == 0 && sf.f_blocks > 0 && sf.f_bfree > 0 && sf.f_bfree < sf.f_blocks, "blocks=%ld free=%ld", sf.f_blocks, sf.f_bfree);
	fd = open(M "/sizes/f_65537", O_RDONLY);
	map = fd >= 0 ? mmap(0, 65537, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
	r = map == MAP_FAILED;
	if (!r) {
		for (i = 0; i < 65537; i++)
			if (map[i] != (changed && i >= 20000 && i < 29000 ? 0xAB : pat(i, 11))) { r = 2; break; }
	}
	if (map != MAP_FAILED) munmap(map, 65537);
	if (fd >= 0) close(fd);
	sprintf(q, "%s mmap read", tag);
	CHECK(q, r == 0, "r=%ld%ld", r, 0);
	d = opendir(M "/many");
	for (i = 0; i < 20; i++) readdir(d);
	pos = telldir(d);
	for (i = 0; i < 5; i++) { e = readdir(d); strcpy(n1[i], e ? e->d_name : "?"); }
	seekdir(d, pos);
	for (i = 0; i < 5; i++) { e = readdir(d); strcpy(n2[i], e ? e->d_name : "?"); }
	closedir(d);
	r = 0; for (i = 0; i < 5; i++) if (strcmp(n1[i], n2[i])) r = 1;
	sprintf(q, "%s telldir/seekdir", tag);
	CHECK(q, r == 0, "%ld%ld", r, 0);
}

/* Phase 2: change files that pfsshell wrote */
static void modify_phase(void)
{
	int fd, r, i;
	unsigned char buf[10000];

	fd = open(M "/sizes/f_65537", O_RDWR);
	lseek(fd, 20000, SEEK_SET);
	memset(buf, 0xAB, 9000);
	r = write(fd, buf, 9000);
	close(fd);
	CHECK("write: overwrite middle", r == 9000 && fsize(M "/sizes/f_65537") == 65537, "r=%ld size=%ld", r, fsize(M "/sizes/f_65537"));
	fd = open(M "/sizes/f_1", O_WRONLY | O_APPEND);
	for (i = 0; i < 10000; i++) buf[i] = (unsigned char)(i * 13);
	r = write(fd, buf, 10000);
	close(fd);
	CHECK("write: append", r == 10000 && fsize(M "/sizes/f_1") == 10001, "r=%ld size=%ld", r, fsize(M "/sizes/f_1"));
	fd = open(M "/sizes/f_0", O_WRONLY);
	r = write(fd, "hello from linux", 16);
	close(fd);
	CHECK("write: empty file", r == 16 && fsize(M "/sizes/f_0") == 16, "r=%ld size=%ld", r, fsize(M "/sizes/f_0"));
	r = truncate(M "/sizes/f_5242880", 3000000);
	CHECK("truncate shrink", r == 0 && fsize(M "/sizes/f_5242880") == 3000000, "r=%ld size=%ld", r, fsize(M "/sizes/f_5242880"));
	r = truncate(M "/sizes/f_8193", 20000);
	CHECK("truncate extend", r == 0 && fsize(M "/sizes/f_8193") == 20000 && zero_range(M "/sizes/f_8193", 8193, 20000),
	      "r=%ld size=%ld", r, fsize(M "/sizes/f_8193"));

	/* remove every even entry of the 300 and add 100 new ones */
	r = 0;
	for (i = 2; i <= 300; i += 2) {
		sprintf((char *)buf, M "/many/" MANY, i);
		if (unlink((char *)buf)) r = i;
	}
	CHECK("unlink 150 pfsshell files", r == 0 && count_dir(M "/many") == 150, "bad=%ld count=%ld", r, count_dir(M "/many"));
	r = 0;
	for (i = 1; i <= 100; i++) {
		sprintf((char *)buf, M "/many/new_%03d", i);
		if (make_file((char *)buf, i * 100, i + 100)) r = i;
	}
	CHECK("create 100 into freed slots", r == 0 && count_dir(M "/many") == 250, "bad=%ld count=%ld", r, count_dir(M "/many"));
}

/* Phase 2: new files and directories */
static void create_phase(void)
{
	char p[1024], name[1024];
	int fd, fa, fb, r, i, badat;
	struct stat st, st2;
	struct utimbuf ut;

	r = mkdir(M "/lx", 0755);
	CHECK("mkdir", r == 0 && stat(M "/lx", &st) == 0 && S_ISDIR(st.st_mode), "r=%ld%ld", r, 0);
	r = mkdir(M "/lx", 0755);
	CHECK("mkdir existing", r < 0 && errno == EEXIST, "r=%ld errno=%ld", r, errno);
	r = mkdir(M "/lx/sub", 0700) | mkdir(M "/lx/d1", 0755) | mkdir(M "/lx/d1/d2", 0755)
	    | mkdir(M "/lx/d1/d2/d3", 0755) | mkdir(M "/lx/empty", 0755) | mkdir(M "/lx/empty2", 0755);
	CHECK("mkdir nested", r == 0 && count_dir(M "/lx/d1/d2/d3") == 0, "r=%ld%ld", r, 0);

	fd = open(M "/lx/hello.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
	r = fd >= 0 ? write(fd, "hello", 5) : -1;
	if (fd >= 0) close(fd);
	CHECK("create file", r == 5 && fsize(M "/lx/hello.txt") == 5, "r=%ld size=%ld", r, fsize(M "/lx/hello.txt"));
	fd = open(M "/lx/hello.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
	CHECK("create O_EXCL existing", fd < 0 && errno == EEXIST, "fd=%ld errno=%ld", fd, errno);
	r = make_file(M "/lx/zero", 0, 0);
	CHECK("create empty file", r == 0 && fsize(M "/lx/zero") == 0, "r=%ld size=%ld", r, fsize(M "/lx/zero"));

	memset(name, 'x', 256); name[256] = 0;
	sprintf(p, M "/lx/%s", name);
	fd = open(p, O_WRONLY | O_CREAT, 0644);
	CHECK("256-byte name refused", fd < 0 && errno == ENAMETOOLONG, "fd=%ld errno=%ld", fd, errno);
	name[255] = 0;
	sprintf(p, M "/lx/%s", name);
	r = make_file(p, 1000, 3);
	CHECK("255-byte name", r == 0 && check_pattern(p, 1000, 3) == 0, "r=%ld%ld", r, 0);
	fd = open(M "/lx/.a", O_WRONLY | O_CREAT, 0644);
	CHECK("2-byte dot name refused", fd < 0 && errno == EINVAL, "fd=%ld errno=%ld", fd, errno);

	/* a directory over several zones */
	badat = 0;
	r = mkdir(M "/lx/many2", 0755);
	for (i = 1; i <= 400 && !r; i++) {
		sprintf(p, MANY2, i);
		if (make_file(p, (i * 37) % 20000, i)) badat = i;
	}
	CHECK("400 files", r == 0 && badat == 0 && count_dir(M "/lx/many2") == 400, "badat=%ld count=%ld", badat, count_dir(M "/lx/many2"));

	/* two files growing in turn: one-zone extents, chained SEGIs */
	fa = open(M "/lx/fragA", O_WRONLY | O_CREAT, 0644);
	fb = open(M "/lx/fragB", O_WRONLY | O_CREAT, 0644);
	badat = fa < 0 || fb < 0;
	for (i = 0; i < NFRAG && !badat; i++)
		badat = put_pattern(fa, (long long)i * ZONE, ZONE, 21) || put_pattern(fb, (long long)i * ZONE, ZONE, 22);
	if (fa >= 0) close(fa);
	if (fb >= 0) close(fb);
	CHECK("fragmented files", !badat && check_pattern(M "/lx/fragA", NFRAG * ZONE, 21) == 0
	      && check_pattern(M "/lx/fragB", NFRAG * ZONE, 22) == 0, "badat=%ld%ld", badat, 0);
	r = truncate(M "/lx/fragA", 200 * ZONE + 100);
	CHECK("truncate drops a SEGI", r == 0 && check_pattern(M "/lx/fragA", 200 * ZONE + 100, 21) == 0, "r=%ld%ld", r, 0);
	r = truncate(M "/lx/fragB", 50 * ZONE + 1);
	CHECK("truncate drops all SEGIs", r == 0 && check_pattern(M "/lx/fragB", 50 * ZONE + 1, 22) == 0, "r=%ld%ld", r, 0);
	fb = open(M "/lx/fragB", O_WRONLY);
	fa = open(M "/lx/fragC", O_WRONLY | O_CREAT, 0644);
	badat = fa < 0 || fb < 0 || put_pattern(fb, 50 * ZONE + 1, ZONE - 1, 22);
	for (i = 51; i < 200 && !badat; i++)
		badat = put_pattern(fb, (long long)i * ZONE, ZONE, 22) || put_pattern(fa, (long long)(i - 51) * ZONE, ZONE, 23);
	if (fa >= 0) close(fa);
	if (fb >= 0) close(fb);
	CHECK("regrow after truncate", !badat && check_pattern(M "/lx/fragB", 200 * ZONE, 22) == 0
	      && check_pattern(M "/lx/fragC", 149 * ZONE, 23) == 0, "badat=%ld%ld", badat, 0);

	r = make_file(M "/lx/big", 3 * 1024 * 1024 + 77, 31);
	CHECK("3 MiB file", r == 0 && check_pattern(M "/lx/big", 3 * 1024 * 1024 + 77, 31) == 0, "r=%ld%ld", r, 0);

	fd = open(M "/lx/sparse", O_WRONLY | O_CREAT, 0644);
	r = fd >= 0 && lseek(fd, 100000, SEEK_SET) == 100000 ? write(fd, "end", 3) : -1;
	if (fd >= 0) close(fd);
	CHECK("sparse write", r == 3 && fsize(M "/lx/sparse") == 100003 && zero_range(M "/lx/sparse", 0, 100000),
	      "r=%ld size=%ld", r, fsize(M "/lx/sparse"));
	r = truncate(M "/lx/hello.txt", 30000);
	CHECK("truncate extend new file", r == 0 && fsize(M "/lx/hello.txt") == 30000 && zero_range(M "/lx/hello.txt", 5, 30000),
	      "r=%ld size=%ld", r, fsize(M "/lx/hello.txt"));

	r = symlink("hello.txt", M "/lx/link");
	i = readlink(M "/lx/link", p, sizeof(p));
	CHECK("symlink", r == 0 && i == 9 && !memcmp(p, "hello.txt", 9) && lstat(M "/lx/link", &st) == 0
	      && S_ISLNK(st.st_mode) && fsize(M "/lx/link") == 30000, "r=%ld len=%ld", r, i);
	memset(name, 's', 903); name[903] = 0;
	r = symlink(name, M "/lx/longlink");
	i = readlink(M "/lx/longlink", p, sizeof(p));
	CHECK("903-byte symlink", r == 0 && i == 903, "r=%ld len=%ld", r, i);
	name[903] = 's'; name[904] = 0;
	r = symlink(name, M "/lx/toolong");
	CHECK("904-byte symlink refused", r < 0 && errno == ENAMETOOLONG, "r=%ld errno=%ld", r, errno);

	/* renames */
	r = rename(M "/lx/hello.txt", M "/lx/hello2.txt");
	CHECK("rename in dir", r == 0 && fsize(M "/lx/hello2.txt") == 30000 && fsize(M "/lx/hello.txt") < 0, "r=%ld%ld", r, 0);
	r = rename(M "/lx/hello2.txt", M "/lx/sub/hello3.txt");
	CHECK("rename across dirs", r == 0 && fsize(M "/lx/sub/hello3.txt") == 30000, "r=%ld%ld", r, 0);
	r = rename(M "/lx/d1", M "/lx/sub/d1moved");
	CHECK("rename dir across dirs", r == 0 && stat(M "/lx/sub/d1moved/d2/d3", &st) == 0, "r=%ld%ld", r, 0);
	make_file(M "/lx/victim", 50000, 9);
	sprintf(p, MANY2, 1);
	r = rename(p, M "/lx/victim");
	CHECK("rename over file", r == 0 && check_pattern(M "/lx/victim", 37, 1) == 0 && count_dir(M "/lx/many2") == 399,
	      "r=%ld%ld", r, 0);
	r = rename(M "/lx/empty2", M "/lx/sub/d1moved");
	CHECK("rename onto non-empty dir", r < 0 && errno == ENOTEMPTY, "r=%ld errno=%ld", r, errno);
	r = mkdir(M "/lx/sub/target", 0755) | rename(M "/lx/empty2", M "/lx/sub/target");
	CHECK("rename onto empty dir", r == 0 && stat(M "/lx/empty2", &st) < 0, "r=%ld%ld", r, 0);
	r = rename(M "/lx/sub", M "/lx/sub/d1moved/inside");
	CHECK("rename into own subtree", r < 0 && errno == EINVAL, "r=%ld errno=%ld", r, errno);

	/* removal */
	r = rmdir(M "/lx/sub");
	CHECK("rmdir non-empty", r < 0 && errno == ENOTEMPTY, "r=%ld errno=%ld", r, errno);
	r = rmdir(M "/lx/empty");
	CHECK("rmdir", r == 0 && stat(M "/lx/empty", &st) < 0, "r=%ld%ld", r, 0);
	r = unlink(M "/lx/sub");
	CHECK("unlink dir refused", r < 0 && (errno == EISDIR || errno == EPERM), "r=%ld errno=%ld", r, errno);
	fd = open(M "/lx/tmpopen", O_RDWR | O_CREAT, 0644);
	put_pattern(fd, 0, 100000, 4);
	r = unlink(M "/lx/tmpopen");
	i = put_pattern(fd, 100000, 50000, 4);
	CHECK("unlink open file", r == 0 && i == 0 && fsize(M "/lx/tmpopen") < 0 && lseek(fd, 0, SEEK_END) == 150000, "r=%ld%ld", r, i);
	close(fd);

	/* attributes */
	r = chmod(M "/lx/sub/hello3.txt", 0600);
	CHECK("chmod", r == 0 && stat(M "/lx/sub/hello3.txt", &st) == 0 && (st.st_mode & 0200) && !(st.st_mode & 0022),
	      "r=%ld mode=%lo", r, st.st_mode);
	ut.actime = ut.modtime = 1000000000;
	r = utime(M "/lx/sub/hello3.txt", &ut);
	CHECK("utime", r == 0 && stat(M "/lx/sub/hello3.txt", &st) == 0 && st.st_mtime == 1000000000, "r=%ld mtime=%ld", r, st.st_mtime);
	r = chown(M "/lx/zero", 1, 1);
	CHECK("chown refused", r < 0 && errno == EPERM, "r=%ld errno=%ld", r, errno);
	stat(M "/lx", &st);
	stat(M "/lx/sub/..", &st2);
	CHECK("dotdot", st.st_ino == st2.st_ino, "%ld %ld", st.st_ino, st2.st_ino);
}

/* Files whose contents pfsshell must read identically */
static void print_cksums(void)
{
	char p[600], q[600];
	int i;
	print_cksum(M "/sizes/f_65537", "sizes/f_65537");
	print_cksum(M "/sizes/f_1", "sizes/f_1");
	print_cksum(M "/sizes/f_0", "sizes/f_0");
	print_cksum(M "/sizes/f_5242880", "sizes/f_5242880");
	print_cksum(M "/sizes/f_8193", "sizes/f_8193");
	print_cksum(M "/many/new_050", "many/new_050");
	print_cksum(M "/lx/fragA", "lx/fragA");
	print_cksum(M "/lx/fragB", "lx/fragB");
	print_cksum(M "/lx/fragC", "lx/fragC");
	print_cksum(M "/lx/big", "lx/big");
	print_cksum(M "/lx/sparse", "lx/sparse");
	print_cksum(M "/lx/victim", "lx/victim");
	print_cksum(M "/lx/sub/hello3.txt", "lx/sub/hello3.txt");
	for (i = 2; i <= 400; i += 97) {
		sprintf(p, MANY2, i);
		strcpy(q, p + strlen(M "/"));
		print_cksum(p, q);
	}
}

static void print_listings(void)
{
	print_ls("");
	print_ls("/many");
	print_ls("/lx");
	print_ls("/lx/sub");
	print_ls("/lx/sub/d1moved");
	print_ls("/lx/many2");
}

/* Phase 3 (read-only, after a reboot): everything from phase 2 */
static void verify_phase(void)
{
	char p[600];
	int i, badat = 0;
	struct stat st;

	read_checks("after-write", 1);
	CHECK("persist fragA", check_pattern(M "/lx/fragA", 200 * ZONE + 100, 21) == 0, "%ld%ld", 0, 0);
	CHECK("persist fragB", check_pattern(M "/lx/fragB", 200 * ZONE, 22) == 0, "%ld%ld", 0, 0);
	CHECK("persist fragC", check_pattern(M "/lx/fragC", 149 * ZONE, 23) == 0, "%ld%ld", 0, 0);
	CHECK("persist big", check_pattern(M "/lx/big", 3 * 1024 * 1024 + 77, 31) == 0, "%ld%ld", 0, 0);
	CHECK("persist sparse", fsize(M "/lx/sparse") == 100003 && zero_range(M "/lx/sparse", 0, 100000), "%ld%ld", 0, 0);
	for (i = 2; i <= 400; i++) {
		sprintf(p, MANY2, i);
		if (check_pattern(p, (i * 37) % 20000, i)) badat = i;
	}
	CHECK("persist 399 files", badat == 0 && count_dir(M "/lx/many2") == 399, "badat=%ld%ld", badat, 0);
	i = readlink(M "/lx/link", p, sizeof(p));
	CHECK("persist symlink", i == 9 && !memcmp(p, "hello.txt", 9), "len=%ld%ld", i, 0);
	CHECK("persist moved tree", stat(M "/lx/sub/d1moved/d2/d3", &st) == 0 && S_ISDIR(st.st_mode), "%ld%ld", 0, 0);
	stat(M "/lx/sub/hello3.txt", &st);
	CHECK("persist mtime", st.st_mtime == 1000000000 && (st.st_mode & 0222) == 0200, "mtime=%ld mode=%lo", st.st_mtime, st.st_mode);
	CHECK("persist victim", check_pattern(M "/lx/victim", 37, 1) == 0, "%ld%ld", 0, 0);
	CHECK("unlinked open file gone", fsize(M "/lx/tmpopen") < 0 && fsize(M "/lx/empty") < 0, "%ld%ld", 0, 0);
	i = open(M "/lx/new", O_WRONLY | O_CREAT, 0644);
	CHECK("read-only mount refuses create", i < 0 && errno == EROFS, "fd=%ld errno=%ld", i, errno);
}

/* Phase 4: after pfsshell changed the volume, write more, fill it up and
 * remove everything Linux made */
static void final_phase(void)
{
	char p[600];
	int fd, r, i;
	long long total = 0;
	struct stat st;
	struct statfs sf, sf2;
	unsigned char buf[65536];

	CHECK("pfsshell put readable", check_pattern(M "/lx/pfs_put.bin", 70000, 99) == 0, "%ld%ld", 0, 0);
	CHECK("pfsshell mkdir", stat(M "/lx/pdir", &st) == 0 && S_ISDIR(st.st_mode), "%ld%ld", 0, 0);
	CHECK("pfsshell rm", fsize(M "/lx/victim") < 0, "%ld%ld", 0, 0);
	CHECK("pfsshell rename", check_pattern(M "/lx/fragB_r", 200 * ZONE, 22) == 0, "%ld%ld", 0, 0);
	r = make_file(M "/lx/pdir/inside", 20000, 7);
	CHECK("create in pfsshell dir", r == 0 && check_pattern(M "/lx/pdir/inside", 20000, 7) == 0, "r=%ld%ld", r, 0);

	statfs(M, &sf);
	fd = open(M "/fill", O_WRONLY | O_CREAT, 0644);
	memset(buf, 0x5A, sizeof(buf));
	while ((r = write(fd, buf, sizeof(buf))) > 0) total += r;
	i = errno;
	close(fd);
	statfs(M, &sf2);
	CHECK("fill to ENOSPC", r < 0 && i == ENOSPC && sf2.f_bfree < 3 && total > 100000000,
	      "total=%ld free=%ld", (long)total, sf2.f_bfree);
	r = mkdir(M "/fulldir", 0755);
	CHECK("mkdir on full volume", r < 0 && errno == ENOSPC, "r=%ld errno=%ld", r, errno);
	r = unlink(M "/fill");
	sync();
	statfs(M, &sf2);
	CHECK("space back after unlink", r == 0 && sf2.f_bfree == sf.f_bfree, "free=%ld before=%ld", sf2.f_bfree, sf.f_bfree);

	r = rm_rf(M "/lx");
	CHECK("rm -r", r == 0 && fsize(M "/lx") < 0, "r=%ld%ld", r, 0);
	r = 0;
	for (i = 1; i <= 100; i++) {
		sprintf(p, M "/many/new_%03d", i);
		if (unlink(p)) r = i;
	}
	CHECK("unlink new files", r == 0 && count_dir(M "/many") == 150, "badat=%ld%ld", r, 0);
}

int main(int argc, char **argv)
{
	const char *phase = getenv("phase");
	const char *opts = "partition=PP.TEST,unsafe_write";
	int cfd, fd, ph = phase ? atoi(phase) : 0;

	cfd = open("/dev/console", O_RDWR);
	if (cfd >= 0) { dup2(cfd, 0); dup2(cfd, 1); dup2(cfd, 2); if (cfd > 2) close(cfd); }
	setvbuf(stdout, NULL, _IONBF, 0);
	crc_init();
	mknod("/dev/hdb", S_IFBLK | 0600, makedev(3, 64));
	printf("ps2fstest phase %d\n", ph);
	if (ph == 1 || ph == 3)
		opts = "partition=PP.TEST";
	if (mount("/dev/hdb", M, "ps2fs", 0, opts) != 0) {
		printf("FAIL mount: %s\n", strerror(errno));
		nfail++;
	} else {
		ok("mount");
		switch (ph) {
		case 1:
			read_checks("ro", 0);
			fd = open(M "/sizes/f_1", O_WRONLY);
			CHECK("read-only by default", fd < 0 && errno == EROFS, "fd=%ld errno=%ld", fd, errno);
			print_ls("");
			print_ls("/many");
			break;
		case 2:
			modify_phase();
			create_phase();
			printf("STEP sync\n");
			sync();
			printf("STEP cksums\n");
			print_cksums();
			print_listings();
			break;
		case 3:
			verify_phase();
			print_cksums();
			print_listings();
			break;
		case 4:
			final_phase();
			print_ls("");
			print_ls("/many");
			break;
		}
		sync();
		CHECK("umount", umount(M) == 0, "%ld%ld", 0, 0);
	}
	printf("RESULT pass=%d fail=%d\n", npass, nfail);
	sync();
	reboot(RB_AUTOBOOT);
	return 0;
}
