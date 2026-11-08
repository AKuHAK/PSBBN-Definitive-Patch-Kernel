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
#include <asm/ps2/sbcall.h>
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

/*
 * SBV (Subversive) runtime patches for IOP:
 *
 * 1. ps2_sbv_patch_disable_prefix_check():
 *    Disables the device prefix check in MODLOAD (which otherwise causes
 *    paths like "mc0:/..." to fail with error -201 / KE_ILLEGAL_OBJECT).
 *    Patches the check function to immediately return 0 (success).
 *
 * 2. ps2_sbv_patch_enable_lmb():
 *    Enables LoadModuleBuffer (function 6) on rom0:LOADFILE. Official Sony
 *    BIOSes only implement functions 0-5 and check "sltiu $v0, $a0, 6".
 *    This allocates a small trampoline on the IOP heap, patches the
 *    LOADFILE RPC jump table and boundary check ("sltiu $v0, $a0, 7").
 */

struct smod_mod_info {
	u32	next;
	u32	name;
	u16	version;
	u16	newflags;
	u16	id;
	u16	unused;
	u32	entry;
	u32	gp;
	u32	text_start;
	u32	text_size;
	u32	data_size;
	u32	bss_size;
	u32	unused1;
	u32	unused2;
};

struct slib_exp_lib {
	u32	prev;
	u32	caller;
	u16	version;
	u16	flags;
	char	name[8];
	u32	exports[0];
};

#define JAL_INSTR(addr)		(0x0c000000 | (0x03ffffff & ((addr) >> 2)))
#define HI16_INSTR(addr)	(0x3c110000 | (((addr) >> 16) & 0xffff))
#define LO16_INSTR(addr)	(0x36310000 | ((addr) & 0xffff))

static const u32 lmb_patch_template[32] = {
	0x27bdffd8,	/* addiu $sp, -40 */
	0xafb00018,	/* sw    $s0, 0x18($sp) */
	0xafbf0020,	/* sw    $ra, 0x20($sp) */
	0x00808021,	/* move  $s0, $a0 */
	0x8c840000,	/* lw    $a0, 0($a0) */
	0x0c000000,	/* jal   [LoadModuleBuffer] */
	0xafb1001c,	/* sw    $s1, 0x1c($sp) */
	0x3c110000,	/* lui   $s1, [HI16(result)] */
	0x04400008,	/* bltz  $v0, 1f */
	0x36310000,	/* ori   $s1, [LO16(result)] */
	0x00402021,	/* move  $a0, $v0 */
	0x26250008,	/* addiu $a1, $s1, 8 */
	0x8e060004,	/* lw    $a2, 4($s0) */
	0x26070104,	/* addiu $a3, $s0, 0x104 */
	0x26280004,	/* addiu $t0, $s1, 4 */
	0x0c000000,	/* jal   [StartModule] */
	0xafa80010,	/* sw    $t0, 0x10($sp) */
	0xae220000,	/* 1: sw $v0, 0($s1) */
	0x02201021,	/* move  $v0, $s1 */
	0x8fbf0020,	/* lw    $ra, 0x20($sp) */
	0x8fb1001c,	/* lw    $s1, 0x1c($sp) */
	0x8fb00018,	/* lw    $s0, 0x18($sp) */
	0x03e00008,	/* jr    $ra */
	0x27bd0028,	/* addiu $sp, 40 */
	0x00000000, 0x00000000,
	0x7962424c, 0x00004545,	/* "LBbyEE" */
	0x00000000, 0x00000000, 0x00000000, 0x00000000
};

static int smod_get_mod_by_name(const char *name, struct smod_mod_info *info)
{
	u32 next = 0x800;
	int len = strlen(name);

	while (next && (next & 0x001FFFFF) < 0x00200000) {
		struct smod_mod_info *cur;
		char *mod_name;

		cur = (struct smod_mod_info *)ps2sif_bustovirt(next & 0x001FFFFF);
		if (!cur->name)
			break;
		mod_name = (char *)ps2sif_bustovirt(cur->name & 0x001FFFFF);
		if (strncmp(mod_name, name, len) == 0 && (mod_name[len] == '\0' || len >= 8)) {
			*info = *cur;
			return cur->id;
		}
		next = cur->next;
	}
	return 0;
}

static struct slib_exp_lib *slib_get_exp_lib(const char *name)
{
	struct smod_mod_info loadcore_info;
	u32 core_end, cur;
	int i, len;
	u32 next_mod;

	/* In PS2 IOP, 0x800 is System_Memory_Manager; its next pointer
	 * points directly to Module_Manager (loadcore). */
	next_mod = *(volatile u32 *)ps2sif_bustovirt(0x800);
	if (!next_mod || (next_mod & 0x001FFFFF) >= 0x00200000) {
		printk(KERN_ERR "ps2loadfile: failed to locate loadcore head at 0x800\n");
		return NULL;
	}
	loadcore_info = *(struct smod_mod_info *)ps2sif_bustovirt(next_mod & 0x001FFFFF);

	core_end = loadcore_info.text_start + loadcore_info.text_size;

	/* Find LOADCORE's export library in the 512 bytes preceding text end */
	cur = 0;
	for (i = 0; i < 512; i += 4) {
		u32 addr = (core_end - 512 + i) & 0x001FFFFF;
		u32 *p = (u32 *)ps2sif_bustovirt(addr);
		if (*p == 0x830) {
			char *lib_name = (char *)p + 12;
			if (strncmp(lib_name, "loadcore", 8) == 0) {
				struct slib_exp_lib *core_exps = (struct slib_exp_lib *)p;
				u32 pfunc = core_exps->exports[3];
				u32 *func = (u32 *)ps2sif_bustovirt(pfunc & 0x001FFFFF);
				if ((func[0] & 0xffff0000) == 0x3c020000 &&
				    (func[1] & 0xffff0000) == 0x24420000) {
					u32 tbl_addr = ((func[0] & 0xffff) << 16) +
						       (short)(func[1] & 0xffff);
					u32 *tbl = (u32 *)ps2sif_bustovirt(tbl_addr & 0x001FFFFF);
					cur = tbl[0]; /* tail of export library list */
					break;
				}
			}
		}
	}

	if (!cur) {
		printk(KERN_ERR "ps2loadfile: loadcore export table not found\n");
		return NULL;
	}

	len = strlen(name);
	while (cur && (cur & 0x001FFFFF) < 0x00200000) {
		struct slib_exp_lib *lib = (struct slib_exp_lib *)ps2sif_bustovirt(cur & 0x001FFFFF);
		if (strncmp(lib->name, name, len) == 0)
			return lib;
		cur = lib->prev;
	}

	printk(KERN_ERR "ps2loadfile: export library '%s' not found\n", name);
	return NULL;
}

int ps2_sbv_patch_disable_prefix_check(void)
{
	struct slib_exp_lib *modload;
	u32 check_prefix_addr;
	volatile u32 *code;
	ps2sif_dmadata_t sdd;
	unsigned int dmaid;
	static u32 patch_buf[4] __attribute__((aligned(64)));

	modload = slib_get_exp_lib("modload");
	if (!modload) {
		printk(KERN_ERR "ps2loadfile: sbv prefix patch: modload not found\n");
		return -ENODEV;
	}

	check_prefix_addr = modload->exports[15];
	if (!check_prefix_addr) {
		printk(KERN_ERR "ps2loadfile: sbv prefix patch: check_prefix_addr is 0\n");
		return -EINVAL;
	}

	code = (volatile u32 *)ps2sif_bustovirt(check_prefix_addr & 0x001FFFFF);
	if (code[0] == 0x03e00008 && code[1] == 0x00001021)
		return 0; /* already patched */

	patch_buf[0] = 0x03e00008; /* jr $ra */
	patch_buf[1] = 0x00001021; /* move $v0, $zero (return 0 = allowed) */
	patch_buf[2] = 0;
	patch_buf[3] = 0;
	dma_cache_wback_inv((unsigned long)patch_buf, 16);

	sdd.data = (unsigned int)patch_buf;
	sdd.addr = check_prefix_addr;
	sdd.size = 16;
	sdd.mode = 0;
	dmaid = ps2sif_setdma_wait(&sdd, 1);
	ps2sif_dmastat_wait(dmaid);

	printk(KERN_INFO "ps2loadfile: MODLOAD prefix check disabled\n");
	return 0;
}

EXPORT_SYMBOL(ps2_sbv_patch_disable_prefix_check);

int ps2_sbv_patch_enable_lmb(void)
{
	struct slib_exp_lib *modload;
	struct smod_mod_info loadfile_info;
	u32 pStartModule, pLoadModuleBuffer;
	u32 scan_start, scan_end, off, match_off;
	u32 *dispatch_ptr;
	u32 jump_table, entry6_addr, result_addr;
	u16 hi;
	s16 lo;
	void *patch_addr;
	ps2sif_dmadata_t sdd;
	unsigned int dmaid;
	static u32 lmb_buf[32] __attribute__((aligned(64)));
	static u32 dma_word[4] __attribute__((aligned(64)));

	modload = slib_get_exp_lib("modload");
	if (!modload) {
		printk(KERN_ERR "ps2loadfile: sbv lmb patch: modload not found\n");
		return -ENODEV;
	}

	pStartModule = modload->exports[8];
	pLoadModuleBuffer = modload->exports[10];
	if (!pStartModule || !pLoadModuleBuffer) {
		printk(KERN_ERR "ps2loadfile: sbv lmb patch: StartModule/LoadModuleBuffer missing\n");
		return -EINVAL;
	}

	if (!smod_get_mod_by_name("LoadModuleByEE", &loadfile_info)) {
		printk(KERN_ERR "ps2loadfile: sbv lmb patch: LoadModuleByEE not found\n");
		return -ENODEV;
	}

	scan_start = loadfile_info.text_start + 0x400;
	scan_end = loadfile_info.text_start + 0x600;
	if (loadfile_info.text_size < 0x600)
		scan_end = loadfile_info.text_start + loadfile_info.text_size - 40;

	match_off = 0;
	for (off = scan_start; off < scan_end; off += 4) {
		u32 *p = (u32 *)ps2sif_bustovirt(off & 0x001FFFFF);
		if (p[0] == 0x27bdffe8 &&
		    (p[1] == 0x2c820006 || p[1] == 0x2c820007) &&
		    p[2] == 0x14400003 &&
		    p[3] == 0xafbf0010) {
			match_off = off;
			if (p[1] == 0x2c820007)
				return 0; /* already patched */
			break;
		}
	}

	if (!match_off) {
		printk(KERN_ERR "ps2loadfile: sbv lmb patch: dispatch function not found\n");
		return -ENOENT;
	}

	dispatch_ptr = (u32 *)ps2sif_bustovirt(match_off & 0x001FFFFF);
	hi = dispatch_ptr[7] & 0xffff;
	lo = dispatch_ptr[9] & 0xffff;
	jump_table = (hi << 16) + (short)lo;
	entry6_addr = jump_table + 0x18;

	patch_addr = ps2sif_allociopheap(128);
	if (!patch_addr) {
		printk(KERN_ERR "ps2loadfile: sbv lmb patch: out of IOP heap memory\n");
		return -ENOMEM;
	}

	result_addr = (u32)patch_addr + 96;

	memcpy(lmb_buf, lmb_patch_template, sizeof(lmb_patch_template));
	lmb_buf[5] = JAL_INSTR(pLoadModuleBuffer);
	lmb_buf[7] = HI16_INSTR(result_addr);
	lmb_buf[9] = LO16_INSTR(result_addr);
	lmb_buf[15] = JAL_INSTR(pStartModule);

	dma_cache_wback_inv((unsigned long)lmb_buf, 128);
	sdd.data = (unsigned int)lmb_buf;
	sdd.addr = (unsigned int)patch_addr;
	sdd.size = 128;
	sdd.mode = 0;
	dmaid = ps2sif_setdma_wait(&sdd, 1);
	ps2sif_dmastat_wait(dmaid);

	/* Patch jump table entry 6 */
	dma_word[0] = (u32)patch_addr;
	dma_cache_wback_inv((unsigned long)dma_word, 16);
	sdd.data = (unsigned int)dma_word;
	sdd.addr = entry6_addr;
	sdd.size = 16;
	sdd.mode = 0;
	dmaid = ps2sif_setdma_wait(&sdd, 1);
	ps2sif_dmastat_wait(dmaid);

	/* Patch boundary check: sltiu $v0, $a0, 7 */
	dma_word[0] = 0x2c820007;
	dma_cache_wback_inv((unsigned long)dma_word, 16);
	sdd.data = (unsigned int)dma_word;
	sdd.addr = match_off + 4;
	sdd.size = 16;
	sdd.mode = 0;
	dmaid = ps2sif_setdma_wait(&sdd, 1);
	ps2sif_dmastat_wait(dmaid);

	printk(KERN_INFO "ps2loadfile: LOADFILE LoadModuleBuffer (func 6) enabled\n");
	return 0;
}

EXPORT_SYMBOL(ps2_sbv_patch_enable_lmb);

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

	rv = ps2_sbv_patch_disable_prefix_check();
	if (rv)
		printk(KERN_WARNING "ps2loadfile: disable_prefix_check returned %d\n", rv);
	rv = ps2_sbv_patch_enable_lmb();
	if (rv)
		printk(KERN_WARNING "ps2loadfile: enable_lmb returned %d\n", rv);

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
