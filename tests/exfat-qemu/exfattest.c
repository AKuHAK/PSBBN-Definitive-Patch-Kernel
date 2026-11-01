/* In-guest test suite for the 2.4 exFAT driver; runs as /sbin/init.
 * Kernel command line "phase=1|2|3" selects the phase. */
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
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/vfs.h>
#include <sys/reboot.h>
#include <sys/sysmacros.h>

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

static int write_pattern(const char *path, long long size, int seed, int chunk)
{
	unsigned char *buf = malloc(chunk);
	long long off = 0;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644), n, i;
	if (fd < 0) { free(buf); return -1; }
	while (off < size) {
		n = size - off < chunk ? (int)(size - off) : chunk;
		for (i = 0; i < n; i++) buf[i] = pat(off + i, seed);
		if (write(fd, buf, n) != n) { close(fd); free(buf); return -2; }
		off += n;
	}
	free(buf);
	return close(fd);
}

/* returns 0 if file equals pattern, else offset+1 of first mismatch or -1 */
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

static long fsize(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long)st.st_size; }
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

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

static const long long sizes[] = { 0, 1, 511, 512, 513, 4095, 4096, 4097, 65537, 300000, 1048576 + 3 };
#define NSIZES (sizeof(sizes) / sizeof(sizes[0]))
static const char *uni_names[] = { "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82_\xd0\xbc\xd0\xb8\xd1\x80.txt",
	"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.dat", "\xf0\x9f\x98\x80.bin", "caf\xc3\xa9" };

static void phase1(void)
{
	char p[600], q[600];
	int i, fd, r;
	long long m;
	struct stat st;
	struct statfs sf1, sf2;
	unsigned char *map, buf[8192];
	struct utimbuf ut;
	DIR *d;
	struct dirent *e;
	long pos;
	char names1[5][300], names2[5][300];

	statfs(M, &sf1);
	/* 1: sizes with odd chunking */
	for (i = 0; i < (int)NSIZES; i++) {
		sprintf(p, M "/size_%lld", sizes[i]);
		r = write_pattern(p, sizes[i], i, 1000 + i * 37);
		m = check_pattern(p, sizes[i], i);
		sprintf(q, "size %lld", sizes[i]);
		CHECK(q, r == 0 && m == 0, "write=%ld check=%ld", r, m);
	}
	statfs(M, &sf2);
	CHECK("statfs free decreases", sf2.f_bfree < sf1.f_bfree, "before=%ld after=%ld", sf1.f_bfree, sf2.f_bfree);
	/* 2: overwrite in the middle */
	write_pattern(M "/mid", 50000, 3, 4096);
	fd = open(M "/mid", O_RDWR);
	lseek(fd, 12345, SEEK_SET);
	memset(buf, 0xAB, 7000);
	write(fd, buf, 7000);
	close(fd);
	fd = open(M "/mid", O_RDONLY);
	r = 0;
	for (m = 0; m < 50000; m++) {
		unsigned char c;
		read(fd, &c, 1);
		if (c != ((m >= 12345 && m < 19345) ? 0xAB : pat(m, 3))) { r = 1; break; }
	}
	close(fd);
	CHECK("overwrite middle", r == 0 && fsize(M "/mid") == 50000, "bad at %ld size %ld", (long)m, fsize(M "/mid"));
	/* 3: hole */
	fd = open(M "/hole", O_RDWR | O_CREAT, 0644);
	lseek(fd, 100000, SEEK_SET);
	write(fd, "X", 1);
	close(fd);
	fd = open(M "/hole", O_RDONLY);
	r = 0;
	while ((i = read(fd, buf, sizeof(buf))) > 0) {
		int k; for (k = 0; k < i; k++) if (buf[k] != 0 && buf[k] != 'X') r = 1;
	}
	close(fd);
	CHECK("hole reads zeroes", r == 0 && fsize(M "/hole") == 100001, "r=%ld size=%ld", r, fsize(M "/hole"));
	/* 4: truncate shrink / extend */
	write_pattern(M "/trunc", 70000, 4, 3000);
	r = truncate(M "/trunc", 5000);
	m = check_pattern(M "/trunc", 5000, 4);
	CHECK("truncate shrink", r == 0 && m == 0, "r=%ld m=%ld", r, m);
	r = truncate(M "/trunc", 20000);
	fd = open(M "/trunc", O_RDONLY);
	read(fd, buf, 5000);
	i = read(fd, buf, 8192);
	close(fd);
	{ int k, z = 1; for (k = 0; k < i; k++) if (buf[k]) z = 0;
	  CHECK("truncate extend zeroes", r == 0 && i == 8192 && z && fsize(M "/trunc") == 20000, "r=%ld size=%ld", r, fsize(M "/trunc")); }
	fd = open(M "/trunc", O_WRONLY | O_TRUNC);
	close(fd);
	CHECK("O_TRUNC", fsize(M "/trunc") == 0, "size=%ld%ld", fsize(M "/trunc"), 0);
	/* 5: append */
	write_pattern(M "/app", 1000, 5, 1000);
	fd = open(M "/app", O_WRONLY | O_APPEND);
	write(fd, "appended", 8);
	close(fd);
	CHECK("O_APPEND", fsize(M "/app") == 1008, "size=%ld%ld", fsize(M "/app"), 0);
	/* 6: shared mmap write */
	write_pattern(M "/mm", 3 * 4096 + 100, 6, 4096);
	fd = open(M "/mm", O_RDWR);
	map = mmap(0, 3 * 4096 + 100, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	r = map == MAP_FAILED;
	if (!r) {
		for (i = 0; i < 3 * 4096 + 100; i++) if (map[i] != pat(i, 6)) r = 2;
		memset(map + 4000, 'M', 300);
		msync(map, 3 * 4096 + 100, MS_SYNC);
		munmap(map, 3 * 4096 + 100);
	}
	close(fd);
	fd = open(M "/mm", O_RDONLY);
	read(fd, buf, 8192);
	close(fd);
	CHECK("mmap read+shared write", r == 0 && buf[3999] == pat(3999, 6) && buf[4000] == 'M' && buf[4299] == 'M' && buf[4300] == pat(4300, 6), "r=%ld c=%ld", r, buf[4000]);
	/* 7: directories and renames */
	mkdir(M "/a", 0755); mkdir(M "/a/b", 0755); mkdir(M "/a/b/c", 0755);
	write_pattern(M "/a/b/c/deep", 9999, 7, 999);
	r = rename(M "/a/b", M "/a/x");
	CHECK("rename dir", r == 0 && check_pattern(M "/a/x/c/deep", 9999, 7) == 0 && !exists(M "/a/b"), "r=%ld%ld", r, 0);
	r = rmdir(M "/a/x");
	CHECK("rmdir nonempty", r < 0 && errno == ENOTEMPTY, "r=%ld errno=%ld", r, errno);
	mkdir(M "/a/empty", 0755);
	CHECK("rmdir empty", rmdir(M "/a/empty") == 0 && !exists(M "/a/empty"), "%ld%ld", 0, 0);
	write_pattern(M "/src", 7777, 8, 777);
	write_pattern(M "/dst", 3333, 9, 333);
	r = rename(M "/src", M "/dst");
	CHECK("rename over file", r == 0 && check_pattern(M "/dst", 7777, 8) == 0 && !exists(M "/src"), "r=%ld%ld", r, 0);
	r = rename(M "/dst", M "/a/x/c/moved");
	CHECK("rename across dirs", r == 0 && check_pattern(M "/a/x/c/moved", 7777, 8) == 0, "r=%ld%ld", r, 0);
	/* rename while open, then keep writing */
	fd = open(M "/openren", O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(fd, "hello", 5);
	r = rename(M "/openren", M "/openren2");
	write(fd, " world", 6);
	close(fd);
	fd = open(M "/openren2", O_RDONLY); i = read(fd, buf, 100); close(fd);
	CHECK("rename open file", r == 0 && i == 11 && !memcmp(buf, "hello world", 11), "r=%ld len=%ld", r, i);
	/* unlink open file, recreate same name */
	fd = open(M "/gone", O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(fd, "old", 3);
	r = unlink(M "/gone");
	write(fd, "more", 4);	/* may fail; must not corrupt */
	close(fd);
	write_pattern(M "/gone", 1234, 10, 100);
	CHECK("unlink open + recreate", r == 0 && check_pattern(M "/gone", 1234, 10) == 0, "r=%ld%ld", r, 0);
	/* 8: case insensitivity */
	write_pattern(M "/CaseFile.txt", 4242, 11, 500);
	CHECK("case-insensitive stat", fsize(M "/casefile.TXT") == 4242, "size=%ld%ld", fsize(M "/casefile.TXT"), 0);
	fd = open(M "/CASEFILE.TXT", O_WRONLY | O_CREAT | O_EXCL, 0644);
	CHECK("case-insensitive O_EXCL", fd < 0 && errno == EEXIST, "fd=%ld errno=%ld", fd, errno);
	CHECK("case preserved in readdir", dir_has(M, "CaseFile.txt") && !dir_has(M, "casefile.TXT"), "%ld%ld", 0, 0);
	stat(M "/nocase", &st);	/* negative dentry with other case */
	fd = open(M "/NoCase", O_WRONLY | O_CREAT, 0644); close(fd);
	CHECK("create uses typed case", dir_has(M, "NoCase"), "%ld%ld", 0, 0);
	/* 9: names */
	fd = open(M "/bad\\name", O_WRONLY | O_CREAT, 0644);
	CHECK("reject backslash", fd < 0 && errno == EINVAL, "fd=%ld errno=%ld", fd, errno);
	fd = open(M "/bad:name", O_WRONLY | O_CREAT, 0644);
	CHECK("reject colon", fd < 0 && errno == EINVAL, "fd=%ld errno=%ld", fd, errno);
	fd = open(M "/bad\nname", O_WRONLY | O_CREAT, 0644);
	CHECK("reject control char", fd < 0 && errno == EINVAL && !exists(M "/bad"), "fd=%ld errno=%ld", fd, errno);
	memset(q, 'L', 255); q[255] = 0; sprintf(p, M "/%s", q);
	fd = open(p, O_WRONLY | O_CREAT, 0644); close(fd);
	CHECK("255-char name", fd >= 0 && dir_has(M, q), "fd=%ld%ld", fd, 0);
	strcat(p, "L");
	fd = open(p, O_WRONLY | O_CREAT, 0644);
	CHECK("256-char name", fd < 0 && errno == ENAMETOOLONG, "fd=%ld errno=%ld", fd, errno);
	for (i = 0; i < 4; i++) {
		sprintf(p, M "/%s", uni_names[i]);
		write_pattern(p, 100 + i, 12 + i, 50);
		CHECK(uni_names[i], dir_has(M, uni_names[i]) && check_pattern(p, 100 + i, 12 + i) == 0, "%ld%ld", 0, 0);
	}
	/* 10: many entries, delete during readdir */
	mkdir(M "/many", 0755);
	for (i = 0; i < 600; i++) {
		sprintf(p, M "/many/file_%04d_with_a_fairly_long_name_to_use_several_entries", i);
		fd = open(p, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd);
	}
	CHECK("600 entries", count_dir(M "/many") == 600, "count=%ld%ld", count_dir(M "/many"), 0);
	d = opendir(M "/many"); i = 0;
	while ((e = readdir(d))) {
		int n;
		if (sscanf(e->d_name, "file_%d", &n) == 1 && (n & 1)) { sprintf(p, M "/many/%s", e->d_name); if (!unlink(p)) i++; }
	}
	closedir(d);
	CHECK("unlink during readdir", i == 300 && count_dir(M "/many") == 300, "deleted=%ld count=%ld", i, count_dir(M "/many"));
	/* rm -r style: unlink every entry, seeking back to the current
	 * position each time, as glibc does when its buffer overflows */
	mkdir(M "/many2", 0755);
	for (i = 0; i < 300; i++) {
		sprintf(p, M "/many2/f%03d", i);
		fd = open(p, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd);
	}
	d = opendir(M "/many2"); i = 0;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		sprintf(p, M "/many2/%s", e->d_name);
		if (!unlink(p)) i++;
		seekdir(d, telldir(d));
	}
	closedir(d);
	CHECK("unlink all, seeking back", i == 300 && count_dir(M "/many2") == 0 && rmdir(M "/many2") == 0,
	      "deleted=%ld left=%ld", i, count_dir(M "/many2"));
	d = opendir(M "/many");
	for (i = 0; i < 10; i++) readdir(d);
	pos = telldir(d);
	for (i = 0; i < 5; i++) { e = readdir(d); strcpy(names1[i], e ? e->d_name : "?"); }
	seekdir(d, pos);
	for (i = 0; i < 5; i++) { e = readdir(d); strcpy(names2[i], e ? e->d_name : "?"); }
	closedir(d);
	r = 0; for (i = 0; i < 5; i++) if (strcmp(names1[i], names2[i])) r = 1;
	CHECK("telldir/seekdir", r == 0, "%ld%ld", r, 0);
	/* 11: attributes and times */
	write_pattern(M "/ro", 10, 20, 10);
	chmod(M "/ro", 0444);
	stat(M "/ro", &st);
	fd = open(M "/ro", O_WRONLY);
	CHECK("chmod read-only", !(st.st_mode & 0222) && fd < 0, "mode=%lo fd=%ld", st.st_mode, fd);
	if (fd >= 0) close(fd);
	chmod(M "/ro", 0644);
	fd = open(M "/ro", O_WRONLY);
	CHECK("chmod writable", fd >= 0, "fd=%ld%ld", fd, 0);
	if (fd >= 0) close(fd);
	write_pattern(M "/ro2", 10, 21, 10);
	chmod(M "/ro2", 0444);
	CHECK("unlink read-only", unlink(M "/ro2") == 0 && !exists(M "/ro2"), "%ld%ld", 0, 0);
	ut.actime = ut.modtime = 981173106;	/* 2001-02-03 04:05:06 UTC */
	write_pattern(M "/timed", 10, 22, 10);
	r = utime(M "/timed", &ut);
	stat(M "/timed", &st);
	CHECK("utime", r == 0 && st.st_mtime == 981173106, "r=%ld mtime=%ld", r, st.st_mtime);
	/* 12: exec from exFAT */
	{
		int in = open("/sbin/init", O_RDONLY), out = open(M "/prog", O_WRONLY | O_CREAT | O_TRUNC, 0755), n, status = -1;
		pid_t pid;
		while ((n = read(in, buf, sizeof(buf))) > 0) write(out, buf, n);
		close(in); close(out);
		pid = fork();
		if (!pid) { execl(M "/prog", "prog", "--exit42", (char *)0); _exit(1); }
		waitpid(pid, &status, 0);
		CHECK("exec from exfat", WIFEXITED(status) && WEXITSTATUS(status) == 42, "status=%lx%ld", status, 0);
	}
	/* 13: concurrency */
	{
		int c, status, allok = 1;
		mkdir(M "/conc", 0755);
		for (c = 0; c < 4; c++) {
			if (!fork()) {
				int it, bad = 0;
				char f[100];
				for (it = 0; it < 15; it++) {
					sprintf(f, M "/conc/w%d_%d", c, it % 3);
					if (write_pattern(f, 20000 + it * 1111 + c, c * 100 + it, 777)) bad = 1;
					if (check_pattern(f, 20000 + it * 1111 + c, c * 100 + it)) bad = 1;
					if (it % 3 == 2) { sprintf(f, M "/conc/w%d_0", c); unlink(f); }
					count_dir(M "/conc");
				}
				_exit(bad);
			}
		}
		for (c = 0; c < 4; c++) { wait(&status); if (!WIFEXITED(status) || WEXITSTATUS(status)) allok = 0; }
		CHECK("4 concurrent writers", allok, "%ld%ld", 0, 0);
	}
	/* 14: big file under memory pressure */
	r = write_pattern(M "/big", 12 * 1048576 + 17, 30, 65536);
	m = check_pattern(M "/big", 12 * 1048576 + 17, 30);
	CHECK("12 MB file", r == 0 && m == 0, "r=%ld m=%ld", r, m);
	/* random backward reads (fast seek map) */
	fd = open(M "/big", O_RDONLY); r = 0;
	for (i = 40; i >= 0; i--) {
		long long off = (long long)i * 300007;
		unsigned char c;
		lseek(fd, off, SEEK_SET); read(fd, &c, 1);
		if (c != pat(off, 30)) r = 1;
	}
	close(fd);
	CHECK("backward seeks", r == 0, "%ld%ld", r, 0);
	/* 15: fill the volume */
	{
		long long tot = 0; int n;
		memset(buf, 0x5A, sizeof(buf));
		fd = open(M "/fill", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		while ((n = write(fd, buf, sizeof(buf))) > 0) tot += n;
		i = errno;
		close(fd);
		CHECK("ENOSPC on full volume", n < 0 && i == ENOSPC && fsize(M "/fill") == tot, "errno=%ld size=%ld", i, fsize(M "/fill"));
		r = unlink(M "/fill");
		statfs(M, &sf1);
		CHECK("space returns after unlink", r == 0 && sf1.f_bfree > 100, "r=%ld free=%ld", r, sf1.f_bfree);
	}
}

static void phase2(void)
{
	char p[600], q[600];
	int i;
	struct stat st;
	long long m;

	for (i = 0; i < (int)NSIZES; i++) {
		sprintf(p, M "/size_%lld", sizes[i]);
		m = check_pattern(p, sizes[i], i);
		sprintf(q, "persist size %lld", sizes[i]);
		CHECK(q, m == 0, "m=%ld%ld", m, 0);
	}
	CHECK("persist deep", check_pattern(M "/a/x/c/deep", 9999, 7) == 0, "%ld%ld", 0, 0);
	CHECK("persist moved", check_pattern(M "/a/x/c/moved", 7777, 8) == 0, "%ld%ld", 0, 0);
	CHECK("persist recreated", check_pattern(M "/gone", 1234, 10) == 0, "%ld%ld", 0, 0);
	CHECK("persist big", check_pattern(M "/big", 12 * 1048576 + 17, 30) == 0, "%ld%ld", 0, 0);
	CHECK("persist many", count_dir(M "/many") == 300, "count=%ld%ld", count_dir(M "/many"), 0);
	CHECK("persist cold case lookup", fsize(M "/CASEFILE.txt") == 4242, "%ld%ld", fsize(M "/CASEFILE.txt"), 0);
	stat(M "/timed", &st);
	CHECK("persist mtime", st.st_mtime == 981173106, "mtime=%ld%ld", st.st_mtime, 0);
	CHECK("persist hole size", fsize(M "/hole") == 100001, "%ld%ld", fsize(M "/hole"), 0);
	for (i = 0; i < 4; i++) {
		sprintf(p, M "/%s", uni_names[i]);
		CHECK("persist unicode name", check_pattern(p, 100 + i, 12 + i) == 0, "i=%ld%ld", i, 0);
	}
	umount(M);
	/* iocharset: the Latin-1 name must appear converted */
	CHECK("mount iocharset=iso8859-1", mount("/dev/hdb", M, "exfat", 0, "iocharset=iso8859-1") == 0, "%ld%ld", 0, 0);
	CHECK("iso8859-1 name", exists(M "/caf\xe9") && dir_has(M, "caf\xe9"), "%ld%ld", 0, 0);
}

static int rm_r(const char *p)
{
	struct stat st; DIR *d; struct dirent *e; char c[700]; int r = 0;
	if (lstat(p, &st)) return -1;
	if (!S_ISDIR(st.st_mode)) return unlink(p);
	d = opendir(p);
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		sprintf(c, "%s/%s", p, e->d_name);
		if (rm_r(c)) r = -1;
	}
	closedir(d);
	return r ? r : rmdir(p);
}

static void phase3(void)
{
	DIR *d = opendir(M); struct dirent *e; char c[700]; int r = 0;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		sprintf(c, M "/%s", e->d_name);
		if (rm_r(c)) { r = 1; printf("cannot remove %s\n", c); }
	}
	closedir(d);
	CHECK("rm -r everything", r == 0 && count_dir(M) == 0, "r=%ld count=%ld", r, count_dir(M));
}

int main(int argc, char **argv)
{
	const char *phase = getenv("phase");
	int cfd;
	if (argc > 1 && !strcmp(argv[1], "--exit42")) return 42;
	mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
	cfd = open("/dev/console", O_RDWR);
	if (cfd >= 0) { dup2(cfd, 0); dup2(cfd, 1); dup2(cfd, 2); if (cfd > 2) close(cfd); }
	setvbuf(stdout, NULL, _IONBF, 0);
	mknod("/dev/hdb", S_IFBLK | 0600, makedev(3, 64));
	printf("exfattest phase %s\n", phase ? phase : "?");
	if (mount("/dev/hdb", M, "exfat", 0, "") != 0) {
		printf("FAIL mount: %s\n", strerror(errno));
		nfail++;
	} else {
		ok("mount");
		if (phase && *phase == '1') phase1();
		if (phase && *phase == '2') phase2();
		if (phase && *phase == '3') phase3();
		sync();
		CHECK("umount", umount(M) == 0, "%ld%ld", 0, 0);
	}
	printf("RESULT pass=%d fail=%d\n", npass, nfail);
	sync();
	reboot(RB_AUTOBOOT);
	return 0;
}
