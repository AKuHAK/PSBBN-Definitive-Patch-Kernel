/*
 *  linux/fs/mmcefs/fxio.c
 *
 *  Client for the fileXio RPC service (fileXio.irx, ps2sdk).  Requests
 *  carry EE physical addresses wherever the IOP transfers data by DMA:
 *  read data and its unaligned head/tail ("rests"), stat and dirent
 *  structures, and the source of write data.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/completion.h>
#include <linux/mm.h>
#include <asm/io.h>
#include <asm/semaphore.h>
#include <asm/ps2/sifdefs.h>

#include "mmcefs.h"

#define FILEXIO_RPC_ID	0x0b0b0b00

enum {
	FILEXIO_DOPEN = 1, FILEXIO_DREAD, FILEXIO_DCLOSE,
	FILEXIO_OPEN = 9, FILEXIO_CLOSE, FILEXIO_READ, FILEXIO_WRITE,
	FILEXIO_RMDIR = 15, FILEXIO_GETSTAT,
	FILEXIO_RENAME = 21,
	FILEXIO_LSEEK64 = 28, FILEXIO_MKDIR, FILEXIO_REMOVE
};

/* rests_pkt: unaligned head and tail of a read, sent back by the IOP */
struct fx_rests {
	int	ssize;
	int	esize;
	u32	sbuf;
	u32	ebuf;
	u8	sbuffer[64];
	u8	ebuffer[64];
};

struct fx_path_pkt {		/* dopen, rmdir, remove */
	char	path[512];
};

struct fx_mkdir_pkt {
	char	path[512];
	int	mode;
};

struct fx_rename_pkt {
	char	from[512];
	char	to[512];
};

struct fx_open_pkt {
	char	path[512];
	int	flags;
	int	mode;
};

struct fx_fd_pkt {		/* close, dclose */
	int	fd;
};

struct fx_read_pkt {
	int	fd;
	u32	buffer;
	int	size;
	u32	intr_data;
};

struct fx_write_pkt {
	int	fd;
	u32	buffer;
	int	size;
	u32	unaligned_len;
	u8	unaligned[64];
};

struct fx_lseek64_pkt {
	int	fd;
	u32	offset_lo;
	u32	offset_hi;
	int	whence;
};

struct fx_getstat_pkt {
	char	path[512];
	u32	stat;
};

struct fx_dread_pkt {
	int	fd;
	u32	dirent;
};

static DECLARE_MUTEX(fx_sem);
static int fx_bound;

/*
 * Request/reply and DMA buffers.  The IOP reaches them by physical
 * address, and a module's static data lives in vmalloc space, so they
 * come from the page allocator; every member is 64-byte aligned.
 */
struct fx_bufs {
	u32		sbuf[1024 / 4];
	u32		rbuf[64 / 4];
	struct fx_rests	rests;
	u8		pad[256 - sizeof(struct fx_rests)];
	struct fx_stat	stat;
	struct fx_dirent dirent;
	u8		pad2[384 - sizeof(struct fx_dirent)];
	u8		iobuf[FX_IOBUF];
	ps2sif_clientdata_t cd;	/* updated by the SBIOS */
};

static struct fx_bufs *fx;
static int fx_order;

#define fx_cd		(fx->cd)
#define fx_sbuf		(fx->sbuf)
#define fx_rbuf		(fx->rbuf)
#define fx_iobuf	(fx->iobuf)
#define fx_rests	(fx->rests)
#define fx_statbuf	(fx->stat)
#define fx_direntbuf	(fx->dirent)

int fx_init(void)
{
	fx_order = get_order(sizeof(struct fx_bufs));
	fx = (struct fx_bufs *)__get_free_pages(GFP_KERNEL, fx_order);
	if (!fx)
		return -ENOMEM;
	memset(fx, 0, sizeof(*fx));
	return 0;
}

void fx_exit(void)
{
	free_pages((unsigned long)fx, fx_order);
	fx = NULL;
}

void fx_lock(void)
{
	down(&fx_sem);
}

void fx_unlock(void)
{
	up(&fx_sem);
}

static void fx_rpcend(void *arg)
{
	complete((struct completion *)arg);
}

/* Is the fileXio service running?  Binds to it on first success. */
int fx_bind(void)
{
	struct completion compl;
	int i, rv;

	if (fx_bound)
		return 0;
	for (i = 0; i < 25; i++) {
		init_completion(&compl);
		memset(&fx_cd, 0, sizeof(fx_cd));
		rv = ps2sif_bindrpc(&fx_cd, FILEXIO_RPC_ID, SIF_RPCM_NOWAIT,
				    fx_rpcend, &compl);
		if (rv < 0)
			return -EIO;
		wait_for_completion(&compl);
		if (fx_cd.serve != 0) {
			fx_bound = 1;
			return 0;
		}
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 50);
	}
	return -ENODEV;
}

/*
 * iomanX returns newlib errno values; the common ones match Linux, the
 * others are translated here.
 */
static int fx_errno(int e)
{
	if (e >= 0)
		return e;
	switch (-e) {
	case 90:			/* newlib ENOTEMPTY */
		return -ENOTEMPTY;
	case 91:			/* newlib ENAMETOOLONG */
		return -ENAMETOOLONG;
	case 88:			/* newlib ENOSYS */
	case 95:			/* newlib EOPNOTSUPP */
	case 134:			/* newlib ENOTSUP */
		return -EOPNOTSUPP;
	}
	if (-e <= 34)			/* ENOENT, EIO, EBADF, EEXIST, ENOSPC... */
		return e;
	return -EIO;
}

/* Send fx_sbuf as function fno; the reply lands in fx_rbuf */
static int fx_call(int fno, int ssize, int rsize)
{
	struct completion compl;
	int rv;

	if (!fx_bound)
		return -ENODEV;
	init_completion(&compl);
	do {
		rv = ps2sif_callrpc(&fx_cd, fno, SIF_RPCM_NOWAIT,
				    fx_sbuf, ssize, fx_rbuf, rsize,
				    fx_rpcend, &compl);
	} while (rv == -E_SIF_PKT_ALLOC || rv == -SIF_RPCE_SENDP);
	if (rv != 0)
		return -EIO;
	wait_for_completion(&compl);
	return 0;
}

static int fx_path_call(int fno, const char *path)
{
	struct fx_path_pkt *p = (struct fx_path_pkt *)fx_sbuf;
	int rv;

	if (strlen(path) >= sizeof(p->path))
		return -ENAMETOOLONG;
	strcpy(p->path, path);
	rv = fx_call(fno, sizeof(*p), 4);
	return rv ? rv : fx_errno((int)fx_rbuf[0]);
}

int fx_open(const char *path, int flags, int mode)
{
	struct fx_open_pkt *p = (struct fx_open_pkt *)fx_sbuf;
	int rv;

	if (strlen(path) >= sizeof(p->path))
		return -ENAMETOOLONG;
	strcpy(p->path, path);
	p->flags = flags;
	p->mode = mode;
	rv = fx_call(FILEXIO_OPEN, sizeof(*p), 4);
	return rv ? rv : fx_errno((int)fx_rbuf[0]);
}

static int fx_fd_call(int fno, int fd)
{
	struct fx_fd_pkt *p = (struct fx_fd_pkt *)fx_sbuf;
	int rv;

	p->fd = fd;
	rv = fx_call(fno, sizeof(*p), 4);
	return rv ? rv : fx_errno((int)fx_rbuf[0]);
}

int fx_close(int fd)
{
	return fx_fd_call(FILEXIO_CLOSE, fd);
}

int fx_read(int fd, void *buf, int size)
{
	struct fx_read_pkt *p = (struct fx_read_pkt *)fx_sbuf;
	unsigned long phys = virt_to_phys(fx_iobuf);
	int rv, total;

	if (size <= 0)
		return 0;
	if (size > FX_IOBUF)
		size = FX_IOBUF;
	/* no dirty lines may be written back over the DMA'd data later */
	dma_cache_wback_inv((unsigned long)fx_iobuf, size);
	dma_cache_wback_inv((unsigned long)&fx_rests, sizeof(fx_rests));
	p->fd = fd;
	p->buffer = phys;
	p->size = size;
	p->intr_data = virt_to_phys(&fx_rests);
	rv = fx_call(FILEXIO_READ, sizeof(*p), 4);
	if (rv)
		return rv;
	total = fx_errno((int)fx_rbuf[0]);
	dma_cache_inv((unsigned long)fx_iobuf, size);
	dma_cache_inv((unsigned long)&fx_rests, sizeof(fx_rests));
	if (total <= 0)
		return total;
	if (total > size)
		total = size;
	/* the aligned middle arrived by DMA; head and tail came in rests */
	if (fx_rests.ssize > 0 && fx_rests.ssize <= 64)
		memcpy(fx_iobuf, fx_rests.sbuffer, fx_rests.ssize);
	if (fx_rests.esize > 0 && fx_rests.esize <= 64 &&
	    fx_rests.ebuf >= phys &&
	    fx_rests.ebuf - phys + fx_rests.esize <= (u32)size)
		memcpy(fx_iobuf + (fx_rests.ebuf - phys), fx_rests.ebuffer,
		       fx_rests.esize);
	memcpy(buf, fx_iobuf, total);
	return total;
}

int fx_write(int fd, const void *buf, int size)
{
	struct fx_write_pkt *p = (struct fx_write_pkt *)fx_sbuf;
	int rv;

	if (size <= 0)
		return 0;
	if (size > FX_IOBUF)
		size = FX_IOBUF;
	memcpy(fx_iobuf, buf, size);
	/* the IOP pulls the data from memory */
	dma_cache_wback_inv((unsigned long)fx_iobuf, size);
	p->fd = fd;
	p->buffer = virt_to_phys(fx_iobuf);
	p->size = size;
	p->unaligned_len = 0;	/* fx_iobuf is 64-byte aligned */
	rv = fx_call(FILEXIO_WRITE, sizeof(*p), 4);
	return rv ? rv : fx_errno((int)fx_rbuf[0]);
}

loff_t fx_lseek64(int fd, loff_t offset, int whence)
{
	struct fx_lseek64_pkt *p = (struct fx_lseek64_pkt *)fx_sbuf;
	loff_t pos;
	int rv;

	p->fd = fd;
	p->offset_lo = (u32)offset;
	p->offset_hi = (u32)(offset >> 32);
	p->whence = whence;
	rv = fx_call(FILEXIO_LSEEK64, sizeof(*p), 8);
	if (rv)
		return rv;
	pos = ((loff_t)fx_rbuf[1] << 32) | fx_rbuf[0];
	if (pos < 0 && pos >= -4096)
		return fx_errno((int)pos);
	return pos;
}

int fx_getstat(const char *path, struct fx_stat *st)
{
	struct fx_getstat_pkt *p = (struct fx_getstat_pkt *)fx_sbuf;
	int rv;

	if (strlen(path) >= sizeof(p->path))
		return -ENAMETOOLONG;
	strcpy(p->path, path);
	p->stat = virt_to_phys(&fx_statbuf);
	dma_cache_wback_inv((unsigned long)&fx_statbuf, sizeof(fx_statbuf));
	rv = fx_call(FILEXIO_GETSTAT, sizeof(*p), 4);
	if (rv)
		return rv;
	rv = fx_errno((int)fx_rbuf[0]);
	if (rv < 0)
		return rv;
	dma_cache_inv((unsigned long)&fx_statbuf, sizeof(fx_statbuf));
	memcpy(st, &fx_statbuf, sizeof(*st));
	return 0;
}

int fx_dopen(const char *path)
{
	return fx_path_call(FILEXIO_DOPEN, path);
}

/* Returns > 0 with an entry in *de, 0 at the end of the directory */
int fx_dread(int fd, struct fx_dirent *de)
{
	struct fx_dread_pkt *p = (struct fx_dread_pkt *)fx_sbuf;
	int rv;

	p->fd = fd;
	p->dirent = virt_to_phys(&fx_direntbuf);
	dma_cache_wback_inv((unsigned long)&fx_direntbuf,
			    sizeof(fx_direntbuf));
	rv = fx_call(FILEXIO_DREAD, sizeof(*p), 4);
	if (rv)
		return rv;
	rv = fx_errno((int)fx_rbuf[0]);
	if (rv <= 0)
		return rv;
	dma_cache_inv((unsigned long)&fx_direntbuf, sizeof(fx_direntbuf));
	memcpy(de, &fx_direntbuf, sizeof(*de));
	de->name[sizeof(de->name) - 1] = 0;
	return rv;
}

int fx_dclose(int fd)
{
	return fx_fd_call(FILEXIO_DCLOSE, fd);
}

int fx_mkdir(const char *path, int mode)
{
	struct fx_mkdir_pkt *p = (struct fx_mkdir_pkt *)fx_sbuf;
	int rv;

	if (strlen(path) >= sizeof(p->path))
		return -ENAMETOOLONG;
	strcpy(p->path, path);
	p->mode = mode;
	rv = fx_call(FILEXIO_MKDIR, sizeof(*p), 4);
	return rv ? rv : fx_errno((int)fx_rbuf[0]);
}

int fx_rmdir(const char *path)
{
	return fx_path_call(FILEXIO_RMDIR, path);
}

int fx_remove(const char *path)
{
	return fx_path_call(FILEXIO_REMOVE, path);
}

int fx_rename(const char *from, const char *to)
{
	struct fx_rename_pkt *p = (struct fx_rename_pkt *)fx_sbuf;
	int rv;

	if (strlen(from) >= sizeof(p->from) || strlen(to) >= sizeof(p->to))
		return -ENAMETOOLONG;
	strcpy(p->from, from);
	strcpy(p->to, to);
	rv = fx_call(FILEXIO_RENAME, sizeof(*p), 4);
	return rv ? rv : fx_errno((int)fx_rbuf[0]);
}
