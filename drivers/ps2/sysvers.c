/*
 *  linux/drivers/ps2/sysvers.c
 *
 *  ROM versions of /proc/ps2sysvers that PSBBN's boot loader cannot read
 *
 *  The boot loader reads rom0:PS1VER, rom1:DVDID and rom1:DVDVER.  Slim
 *  consoles (SCPH-70000 and later) have one of each per region instead,
 *  named with a region letter: rom0:PS1VERx for the region of
 *  rom0:ROMVER, rom1:DVDIDx and rom1:DVDVERx for the DVD player region,
 *  which the MECHACON keeps with its other region parameters.  The loader
 *  then passes "na" and empty strings; look the files up as the OSD of
 *  those consoles does.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/completion.h>
#include <asm/addrspace.h>
#include <asm/io.h>
#include <asm/ps2/sifdefs.h>
#include <asm/ps2/siflock.h>

#include "sysvers.h"

#define ROM0_BASE	0x1fc00000
#define ROM1_BASE	0x1e000000
#define ROM_SIZE	0x400000
#define ROMDIR_SCAN	0x4000
#define SSBUS_DEV1_ADDR	0x1f801400	/* where the IOP bus maps ROM1 */

#define SCMD_RPC_ID	0x80000593	/* CDVDFSV S-command service */
#define SCMD_APPLY	11		/* sceCdApplySCmd() */
#define SCMD_MV		0x03		/* subcommand 0: MECHACON version */
#define MV_MAJOR	1		/* reply: status, major, minor, ... */
#define SCMD_REGION	0x36		/* region parameters, MECHACON 6.0+ */
#define SCMD_STAT_ERR	0x80
#define SCMD_RETRY	5

/* Reply of SCMD_REGION: status, then the parameters; byte 8 of those is
 * the DVD player region */
#define REGION_DVD	(1 + 8)

struct scmd_arg {
	u8	cmd;
	u8	pad;
	u16	insize;
	u8	in[16];
};

struct romdir_ent {
	char	name[10];
	u16	extinfo;
	u32	size;
};

static ps2sif_clientdata_t scmd_cd;

/* Copy len bytes at ROM offset off, reading the ROM by 32-bit words */
static void rom_read(unsigned long base, u32 off, void *buf, int len)
{
	volatile u32 *p = (volatile u32 *)KSEG1ADDR(base + (off & ~3));
	u8 *d = buf;
	int i = 0, k = off & 3;
	u32 w;

	while (i < len) {
		w = *p++;
		for (; k < 4 && i < len; k++)
			d[i++] = (w >> (8 * k)) & 0xff;
		k = 0;
	}
}

/*
 * Find a ROM file.  The directory starts with the entry "RESET", whose
 * size is the offset of the directory; files follow in directory order,
 * each aligned to 16 bytes.
 */
static int rom_find(unsigned long base, const char *name, u32 *offp,
		    u32 *sizep)
{
	struct romdir_ent ent;
	u32 dir, off, i;

	for (dir = 0; dir < ROMDIR_SCAN; dir += 16) {
		rom_read(base, dir, &ent, sizeof(ent));
		if (!memcmp(ent.name, "RESET\0\0\0\0", 10) && ent.size == dir)
			break;
	}
	if (dir >= ROMDIR_SCAN)
		return -ENOENT;
	off = 0;
	for (i = 0; i < ROMDIR_SCAN / 16; i++) {
		rom_read(base, dir + i * 16, &ent, sizeof(ent));
		if (ent.name[0] == '\0' || off + ent.size > ROM_SIZE)
			break;
		if (!strncmp(ent.name, name, sizeof(ent.name))) {
			*offp = off;
			*sizep = ent.size;
			return 0;
		}
		off += (ent.size + 15) & ~15;
	}
	return -ENOENT;
}

/* Read a version file up to its first control character, as the loader */
static int rom_read_version(unsigned long base, const char *name, char *buf,
			    int len)
{
	u32 off, size;
	int i, n;

	if (rom_find(base, name, &off, &size))
		return -ENOENT;
	n = size < len - 1 ? size : len - 1;
	rom_read(base, off, buf, n);
	for (i = 0; i < n; i++)
		if (buf[i] < 0x20 || buf[i] > 0x7e)
			break;
	buf[i] = '\0';
	return i ? 0 : -ENOENT;
}

static void scmd_rpcend(void *arg)
{
	complete((struct completion *)arg);
}

static int scmd_bind(void)
{
	struct completion compl;
	int i, rv;

	for (i = 0; i < 50; i++) {
		init_completion(&compl);
		memset(&scmd_cd, 0, sizeof(scmd_cd));
		rv = ps2sif_bindrpc(&scmd_cd, SCMD_RPC_ID, SIF_RPCM_NOWAIT,
				    scmd_rpcend, &compl);
		if (rv < 0)
			return -EIO;
		wait_for_completion(&compl);
		if (scmd_cd.serve != 0)
			return 0;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 50);
	}
	return -ENODEV;
}

/*
 * Send S-command cmd to the MECHACON; out receives its 16-byte reply.
 * arg and out are in the page allocator's memory, which the IOP reaches
 * by DMA.  CDVD lock held, service bound.
 */
static int scmd_apply(struct scmd_arg *arg, u8 *out, int cmd, const void *in,
		      int insize)
{
	struct completion compl;
	int rv;

	memset(arg, 0, sizeof(*arg));
	arg->cmd = cmd;
	arg->insize = insize;
	if (insize)
		memcpy(arg->in, in, insize);
	dma_cache_wback_inv((unsigned long)arg, sizeof(*arg));
	dma_cache_wback_inv((unsigned long)out, 16);
	init_completion(&compl);
	do {
		rv = ps2sif_callrpc(&scmd_cd, SCMD_APPLY, SIF_RPCM_NOWAIT,
				    arg, sizeof(*arg), out, 16,
				    scmd_rpcend, &compl);
	} while (rv == -E_SIF_PKT_ALLOC || rv == -SIF_RPCE_SENDP);
	if (rv)
		return -EIO;
	wait_for_completion(&compl);
	dma_cache_inv((unsigned long)out, 16);
	return 0;
}

static int scmd_apply_retry(struct scmd_arg *arg, u8 *out, int cmd,
			    const void *in, int insize)
{
	int i;

	for (i = 0; i < SCMD_RETRY; i++) {
		if (scmd_apply(arg, out, cmd, in, insize))
			return -EIO;
		if (!(out[0] & SCMD_STAT_ERR))
			return 0;
	}
	return -EIO;
}

/* The DVD player region letter from the MECHACON, or 0 */
static int dvd_region(void)
{
	static const u8 mv_sub = 0;
	ps2sif_lock_t *lock;
	struct scmd_arg *arg;
	unsigned long page;
	u8 *out;
	int region = 0;

	if ((lock = ps2sif_getlock(PS2LOCK_CDVD)) == NULL)
		return 0;
	if ((page = __get_free_page(GFP_KERNEL)) == 0)
		return 0;
	arg = (struct scmd_arg *)page;
	out = (u8 *)page + 64;

	ps2sif_lock(lock, "sysvers");
	if (scmd_bind())
		goto out;
	/* only MECHACON 6.0 and later (slim consoles) know SCMD_REGION */
	if (scmd_apply_retry(arg, out, SCMD_MV, &mv_sub, 1) || out[MV_MAJOR] < 6)
		goto out;
	if (scmd_apply_retry(arg, out, SCMD_REGION, NULL, 0))
		goto out;
	if (out[REGION_DVD] >= 'A' && out[REGION_DVD] <= 'Z')
		region = out[REGION_DVD];
 out:
	ps2sif_unlock(lock);
	free_page(page);
	return region;
}

void ps2sysvers_fixup(const char **ps1drv_rom, const char **dvd_id,
		      const char **dvd_rom)
{
	static char ps1ver[16], dvdid[2], dvdver[16];
	char name[16], buf[16];
	const char *p;
	int region;

	if (*ps1drv_rom == NULL || **ps1drv_rom == '\0') {
		if (!rom_read_version(ROM0_BASE, "ROMVER", buf, sizeof(buf)) &&
		    strlen(buf) > 4) {
			sprintf(name, "PS1VER%c", buf[4]);
			if (!rom_read_version(ROM0_BASE, name, ps1ver,
					      sizeof(ps1ver)))
				*ps1drv_rom = ps1ver;
		}
	}

	if (*dvd_id != NULL && **dvd_id != '\0' && strcmp(*dvd_id, "na"))
		return;
	if (*(volatile u32 *)KSEG1ADDR(SSBUS_DEV1_ADDR) != ROM1_BASE)
		return;
	if ((region = dvd_region()) == 0)
		return;
	sprintf(name, "DVDID%c", region);
	if (rom_read_version(ROM1_BASE, name, buf, sizeof(buf)))
		return;
	/* "3.10R": the loader passes the letter after the version */
	for (p = buf; *p >= '0' && *p <= '9'; p++)
		;
	if (*p != '.')
		return;
	for (p++; *p >= '0' && *p <= '9'; p++)
		;
	if (*p == '\0')
		return;
	dvdid[0] = *p;
	*dvd_id = dvdid;
	sprintf(name, "DVDVER%c", region);
	if (!rom_read_version(ROM1_BASE, name, dvdver, sizeof(dvdver)))
		*dvd_rom = dvdver;
}
