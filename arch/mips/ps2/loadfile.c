/*
 *  PlayStation 2 IOP module loader
 *
 *  Loads IRX images into the IOP at run time through the LOADFILE RPC
 *  service (rom0:LOADFILE), the same way SifExecModuleBuffer() does in
 *  ps2sdk: the image is copied to the IOP heap with SIF DMA and loaded
 *  from there.
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
#define LF_F_MOD_BUF_LOAD	6
#define LF_PATH_MAX		252
#define LF_ARG_MAX		252

/* struct _lf_module_buffer_load_arg of ps2sdk */
struct lf_module_buffer_load_arg {
	union {
		u32	ptr;
		int	result;
	} p;
	union {
		int	arg_len;
		int	modres;
	} q;
	char	unused[LF_PATH_MAX];
	char	args[LF_ARG_MAX];
};

static ps2sif_clientdata_t lf_cd;
static int lf_bound;
static DECLARE_MUTEX(lf_sem);
static struct lf_module_buffer_load_arg lf_arg __attribute__((aligned(64)));

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

int ps2_load_module_buffer(const void *buf, int size,
			   const char *args, int arglen, int *modres)
{
	struct completion compl;
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
	if (!lf_bound) {
		rv = lf_bind();
		if (rv) {
			printk(KERN_ERR "ps2loadfile: LOADFILE RPC service "
			       "not available (%d)\n", rv);
			goto out;
		}
		lf_bound = 1;
	}

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
	init_completion(&compl);
	do {
		rv = ps2sif_callrpc(&lf_cd, LF_F_MOD_BUF_LOAD, SIF_RPCM_NOWAIT,
				    &lf_arg, sizeof(lf_arg), &lf_arg, 8,
				    lf_rpcend, &compl);
	} while (rv == -E_SIF_PKT_ALLOC || rv == -SIF_RPCE_SENDP);
	if (rv == 0) {
		wait_for_completion(&compl);
		rv = lf_arg.p.result;
		if (modres)
			*modres = lf_arg.q.modres;
	} else
		rv = -EIO;
	/* the loader has relocated the module; the copy is no longer used */
	ps2sif_freeiopheap(iop_addr);
out:
	up(&lf_sem);
	free_pages((unsigned long)image, order);
	return rv;
}

EXPORT_SYMBOL(ps2_load_module_buffer);
