/*
 * pfsfsck: ps2sdk's PFS checker (iop/hdd/fsck) built for the host, in
 * read-only check mode, as an oracle for test images.
 *
 * Usage: pfsfsck <disk image> <partition>
 * Exit status: 0 clean, 1 errors found, 2 could not check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include "fsck.c"

/*
 * The check runs synchronously in main(), so the IOP thread and event flag
 * of the module are never used; these only satisfy fsck_start().
 */
int fsckCreateEventFlag(void) { return 1; }
int fsckCreateThread(void (*function)(void *arg), int StackSize) { (void)function; (void)StackSize; return 1; }
int SetEventFlag(int ef, unsigned int bits) { (void)ef; (void)bits; return 0; }
int WaitEventFlag(int ef, unsigned int bits, int mode, unsigned int *resbits) { (void)ef; (void)bits; (void)mode; (void)resbits; return 0; }
int PollEventFlag(int ef, unsigned int bits, int mode, unsigned int *resbits) { (void)ef; (void)bits; (void)mode; (void)resbits; return 0; }
int StartThread(int thid, void *arg) { (void)thid; (void)arg; return 0; }
/* only used for fsck's time estimate */
int GetSystemTime(iop_sys_clock_t *c) { memset(c, 0, sizeof(*c)); return 0; }
void SysClock2USec(iop_sys_clock_t *c, u32 *sec, u32 *usec) { (void)c; *sec = 0; *usec = 0; }

extern void set_atad_device_path(const char *path);
extern int _init_apa(int argc, char *argv[]);

int main(int argc, char **argv)
{
	static char *apa_args[] = { "ps2hdd.irx", "-o", "4", NULL };
	iomanX_iop_file_t file;
	char name[64];
	int result;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <disk image> <partition>\n", argv[0]);
		return 2;
	}
	set_atad_device_path(argv[1]);
	if (_init_apa(3, apa_args) < 0 || pfsCacheInit(0x7E, 1024) < 0 ||
	    pfsBitmapInit() < 0) {
		fprintf(stderr, "pfsfsck: initialisation failed\n");
		return 2;
	}
	snprintf(name, sizeof(name), "hdd0:%s", argv[2]);
	memset(&file, 0, sizeof(file));
	result = FsckOpen(&file, name, 0, FSCK_MODE_VERBOSITY(1));
	if (result < 0) {
		fprintf(stderr, "pfsfsck: cannot open %s (%d)\n", name, result);
		return 2;
	}
	FsckThread(&MainPFSMount);
	/* bitmap mismatches only set hasError, not errorCount */
	printf("pfsfsck %s: %u files, %u directories, %u zones used, %u errors%s\n",
	       argv[2], fsckRuntimeData.status.files,
	       fsckRuntimeData.status.directories,
	       fsckRuntimeData.status.zoneUsed,
	       fsckRuntimeData.status.errorCount,
	       fsckRuntimeData.hasError ? ", NOT CLEAN" : ", clean");
	return (fsckRuntimeData.hasError || fsckRuntimeData.status.errorCount) ? 1 : 0;
}
