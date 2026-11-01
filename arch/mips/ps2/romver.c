/*
 *  PlayStation 2 ROM files and MECHACON region parameters
 *
 *  Small files of rom0: and rom1: (versions, regions) as the IOP's ROM
 *  driver would return them: the EE reads both ROMs directly.  The
 *  MECHACON of slim consoles (6.0 and later) keeps region parameters that
 *  the OSD uses to pick per-region ROM files; they are read with
 *  S-command 0x36 through the S-command RPC service of CDVDFSV.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/completion.h>
#include <asm/addrspace.h>
#include <asm/io.h>
#include <asm/semaphore.h>
#include <asm/ps2/sifdefs.h>
#include <asm/ps2/siflock.h>
#include <asm/ps2/romver.h>

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

int ps2_rom_read_version(int rom, const char *name, char *buf, int len)
{
	if (rom == 0)
		return rom_read_version(ROM0_BASE, name, buf, len);
	if (*(volatile u32 *)KSEG1ADDR(SSBUS_DEV1_ADDR) != ROM1_BASE)
		return -ENODEV;
	return rom_read_version(ROM1_BASE, name, buf, len);
}

EXPORT_SYMBOL(ps2_rom_read_version);

int ps2_region_params(u8 *params)
{
	static DECLARE_MUTEX(sem);
	static u8 cache[PS2_REGION_PARAMS];
	static int status = 1;		/* 1: not read yet */
	static const u8 mv_sub = 0;
	ps2sif_lock_t *lock;
	struct scmd_arg *arg;
	unsigned long page;
	u8 *out;
	int rv;

	down(&sem);
	if (status != 1)
		goto out;
	status = -ENODEV;
	if ((lock = ps2sif_getlock(PS2LOCK_CDVD)) == NULL)
		goto out;
	if ((page = __get_free_page(GFP_KERNEL)) == 0) {
		status = 1;		/* try again later */
		goto out;
	}
	arg = (struct scmd_arg *)page;
	out = (u8 *)page + 64;

	ps2sif_lock(lock, "romver");
	if (scmd_bind() == 0 &&
	    scmd_apply_retry(arg, out, SCMD_MV, &mv_sub, 1) == 0 &&
	    out[MV_MAJOR] >= 6 &&
	    scmd_apply_retry(arg, out, SCMD_REGION, NULL, 0) == 0) {
		memcpy(cache, out + 1, sizeof(cache));
		status = 0;
	}
	ps2sif_unlock(lock);
	free_page(page);
 out:
	if ((rv = status) == 0)
		memcpy(params, cache, sizeof(cache));
	up(&sem);
	return rv == 1 ? -ENOMEM : rv;
}

EXPORT_SYMBOL(ps2_region_params);
