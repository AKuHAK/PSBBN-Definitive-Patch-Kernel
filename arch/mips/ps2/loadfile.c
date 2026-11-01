/*
 *  PlayStation 2 IOP module loader
 *
 *  Loads IRX images into the IOP at run time through the LOADFILE RPC
 *  service (rom0:LOADFILE): from EE memory (the image is copied to the
 *  IOP heap with SIF DMA, function 6), or from a path the IOP can open
 *  such as "mc0:/..." (function 0).  The LOADFILE server of PSBBN's IOP
 *  only has function 0: it never answers function 6, which is detected.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/completion.h>
#include <asm/io.h>
#include <asm/semaphore.h>
#include <asm/ps2/sifdefs.h>
#include <asm/ps2/loadfile.h>

#define LOADFILE_RPC_ID		0x80000006
#define LF_F_MOD_LOAD		0
#define LF_F_MOD_BUF_LOAD	6
#define LF_PATH_MAX		252
#define LF_ARG_MAX		252

/* Argument of functions 0 and 6: {path or IOP address, argument length}
 * in, {result, module result} out; the path is unused by function 6 */
struct lf_module_load_arg {
	union {
		u32	ptr;
		int	result;
	} p;
	union {
		int	arg_len;
		int	modres;
	} q;
	char	path[LF_PATH_MAX];
	char	args[LF_ARG_MAX];
};

static ps2sif_clientdata_t lf_cd;
static int lf_bound;
static DECLARE_MUTEX(lf_sem);
static struct lf_module_load_arg lf_arg __attribute__((aligned(64)));

static void lf_rpcend(void *arg)
{
	complete((struct completion *)arg);
}

static int lf_bind(void)
{
	struct completion compl;
	int i, rv;

	for (i = 0; i < 50; i++) {
		init_completion(&compl);
		memset(&lf_cd, 0, sizeof(lf_cd));
		rv = ps2sif_bindrpc(&lf_cd, LOADFILE_RPC_ID, SIF_RPCM_NOWAIT,
				    lf_rpcend, &compl);
		if (rv < 0)
			return -EIO;
		wait_for_completion(&compl);
		if (lf_cd.serve != 0)
			return 0;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 50);
	}
	return -ENODEV;
}

/* Call LOADFILE function fno with lf_arg; lf_sem held, server bound */
static int lf_call(int fno)
{
	struct completion compl;
	int rv;

	dma_cache_wback_inv((unsigned long)&lf_arg, sizeof(lf_arg));
	init_completion(&compl);
	do {
		rv = ps2sif_callrpc(&lf_cd, fno, SIF_RPCM_NOWAIT,
				    &lf_arg, sizeof(lf_arg), &lf_arg, 8,
				    lf_rpcend, &compl);
	} while (rv == -E_SIF_PKT_ALLOC || rv == -SIF_RPCE_SENDP);
	if (rv)
		return -EIO;
	wait_for_completion(&compl);
	dma_cache_inv((unsigned long)&lf_arg, sizeof(lf_arg));
	return 0;
}

static int lf_bind_once(void)
{
	int rv;

	if (lf_bound)
		return 0;
	rv = lf_bind();
	if (rv) {
		printk(KERN_ERR "ps2loadfile: LOADFILE RPC service "
		       "not available (%d)\n", rv);
		return rv;
	}
	lf_bound = 1;
	return 0;
}

int ps2_load_module(const char *path, const char *args, int arglen,
		    int *modres)
{
	int rv;

	if (strlen(path) >= LF_PATH_MAX || arglen < 0 || arglen > LF_ARG_MAX)
		return -EINVAL;
	down(&lf_sem);
	if ((rv = lf_bind_once()))
		goto out;
	memset(&lf_arg, 0, sizeof(lf_arg));
	lf_arg.q.arg_len = arglen;
	strcpy(lf_arg.path, path);
	if (arglen)
		memcpy(lf_arg.args, args, arglen);
	if ((rv = lf_call(LF_F_MOD_LOAD)))
		goto out;
	rv = lf_arg.p.result;
	if (modres)
		*modres = lf_arg.q.modres;
out:
	up(&lf_sem);
	return rv;
}

EXPORT_SYMBOL(ps2_load_module);

int ps2_load_module_buffer(const void *buf, int size,
			   const char *args, int arglen, int *modres)
{
	ps2sif_dmadata_t sdd;
	unsigned int dmaid;
	void *iop_addr;
	char *image;
	int asize, order, rv;

	if (size <= 0 || arglen < 0 || arglen > LF_ARG_MAX)
		return -EINVAL;
	/* SIF DMA moves whole quadwords; keep the copy cache-line aligned */
	asize = (size + 63) & ~63;
	order = get_order(asize);
	image = (char *)__get_free_pages(GFP_KERNEL, order);
	if (!image)
		return -ENOMEM;
	memcpy(image, buf, size);
	memset(image + size, 0, asize - size);

	down(&lf_sem);
	if ((rv = lf_bind_once()))
		goto out;

	iop_addr = ps2sif_allociopheap(asize);
	if (!iop_addr) {
		rv = -ENOMEM;
		goto out;
	}
	dma_cache_wback_inv((unsigned long)image, asize);
	sdd.data = (unsigned int)image;
	sdd.addr = (unsigned int)iop_addr;
	sdd.size = asize;
	sdd.mode = 0;
	dmaid = ps2sif_setdma_wait(&sdd, 1);
	ps2sif_dmastat_wait(dmaid);

	memset(&lf_arg, 0, sizeof(lf_arg));
	lf_arg.p.ptr = (u32)iop_addr;
	lf_arg.q.arg_len = arglen;
	if (arglen)
		memcpy(lf_arg.args, args, arglen);
	rv = lf_call(LF_F_MOD_BUF_LOAD);
	if (rv == 0 && lf_arg.p.ptr == (u32)iop_addr) {
		/* no reply written: the server lacks function 6 (PSBBN) */
		rv = -ENOSYS;
	} else if (rv == 0) {
		rv = lf_arg.p.result;
		if (modres)
			*modres = lf_arg.q.modres;
	}
	/* the loader has relocated the module; the copy is no longer used */
	ps2sif_freeiopheap(iop_addr);
out:
	up(&lf_sem);
	free_pages((unsigned long)image, order);
	return rv;
}

EXPORT_SYMBOL(ps2_load_module_buffer);
