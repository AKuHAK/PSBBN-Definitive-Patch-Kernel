/*
 *  linux/fs/mmcefs/mmcehd.c
 *
 *  A disk image on the SD card of an MMCE (SD2PSX, MemCard PRO2) as the
 *  PS2 hard disk: block major 3, so the image becomes /dev/hda and its APA
 *  partitions /dev/hdaN, exactly as on the internal HDD.  Everything above
 *  the block layer (APA parsing, ps2fs, ext2/ext3, swap, pfsd, bn) is
 *  unchanged.
 *
 *  Only with the boot parameter "mmce" (or "mmce=<IOP path>"); without it
 *  the kernel uses the internal HDD as before.  The image is a plain file
 *  (default mmce0:/PSBBN/psbbn.img, then mmce1:) reached through fileXio
 *  (fxio.c) and mmceman, the same path as mmcefs, so /mnt/mmce keeps
 *  working next to it.  The IOP modules are built into the kernel, since
 *  /lib/modules lies inside the image.
 *
 *  blk_dev_init() calls mmcehd_init() right before ide_init(): the request
 *  cache exists by then, and if the image is usable major 3 is taken first
 *  and the IDE driver leaves the internal disk alone ("UNABLE TO GET MAJOR
 *  NUMBER 3").  If the image is missing or broken, nothing is registered
 *  and the kernel boots from the HDD.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/major.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/smp_lock.h>
#include <linux/blk.h>		/* no MAJOR_NR: 3 is also the old hd.c */
#include <asm/uaccess.h>
#include <asm/byteorder.h>
#include <asm/semaphore.h>
#include <asm/ps2/loadfile.h>

#include "mmcefs.h"

#ifndef MODULE		/* the root disk cannot come from a module */

#include "irx_blobs.h"	/* irx_iomanx[], irx_filexio[], irx_mmceman[] */

#define MAJOR_NR	IDE0_MAJOR	/* /dev/hda */
#define MMCEHD_SHIFT	6
#define MMCEHD_MINORS	(1 << MMCEHD_SHIFT)
#define MMCEHD_MAX_SECT	64	/* sectors per request (32 KiB) */
#define MMCEHD_SEEK_SET	0
#define MMCEHD_SEEK_END	2
#define APA_MAGIC	0x00415041	/* "APA\0" */
#define APA_MAX_PARTS	256

static int mmcehd_wanted;
static char mmcehd_param[128];
static char mmcehd_path[128];

static int mmcehd_fd = -1;
static unsigned long mmcehd_capacity;	/* sectors */
static u8 *mmcehd_stage;		/* FX_IOBUF bytes */

static struct hd_struct mmcehd_part[MMCEHD_MINORS];
static int mmcehd_sizes[MMCEHD_MINORS];
static int mmcehd_blksizes[MMCEHD_MINORS];
static int mmcehd_hardsect[MMCEHD_MINORS];
static int mmcehd_maxsect[MMCEHD_MINORS];
static int mmcehd_usage[MMCEHD_MINORS];
static DECLARE_MUTEX(mmcehd_part_sem);
static DECLARE_MUTEX(mmcehd_reval_sem);
static struct gendisk mmcehd_gendisk;

static DECLARE_WAIT_QUEUE_HEAD(mmcehd_wait);

static unsigned long st_reads, st_writes, st_rsect, st_wsect, st_errors;
static unsigned long st_busy;		/* jiffies spent in transfers */
static int st_unknown_ioctls;

/*
 * "mmce" uses the default image paths, "mmce=<path>" one IOP path such as
 * mmce1:/PSBBN/psbbn.img.  From Sony's boot loader use its --root/-rt
 * argument "/dev/mmce" instead (see mmcehd_root_wanted()).
 */
static int __init mmcehd_setup(char *s)
{
	if (*s == '=') {
		strncpy(mmcehd_param, s + 1, sizeof(mmcehd_param) - 1);
		mmcehd_wanted = 1;
		return 1;
	}
	if (*s == 0) {
		mmcehd_wanted = 1;
		return 1;
	}
	return 0;		/* "mmcefoo": not ours */
}
__setup("mmce", mmcehd_setup);

/* --- IOP side ------------------------------------------------------ */

static int __init mmcehd_load_irx(const char *name, const u8 *image,
				  int size)
{
	int rv, modres = -1;

	rv = ps2_load_module_buffer(image, size, NULL, 0, &modres);
	if (rv < 0)
		printk(KERN_ERR "mmcehd: loading %s failed (%d)\n", name, rv);
	else if (modres == 1)
		printk(KERN_WARNING "mmcehd: %s did not stay resident\n",
		       name);
	else
		printk(KERN_INFO "mmcehd: loaded built-in %s (IOP module "
		       "%d)\n", name, rv);
	return rv < 0 ? rv : 0;
}

/* Bind fileXio, loading iomanX and fileXio if it is not serving yet */
static int __init mmcehd_prepare_fileio(void)
{
	int rv;

	fx_lock();
	rv = fx_bind();
	fx_unlock();
	if (rv == -ENODEV) {
		mmcehd_load_irx("iomanX.irx", irx_iomanx, sizeof(irx_iomanx));
		mmcehd_load_irx("fileXio.irx", irx_filexio,
				sizeof(irx_filexio));
		fx_lock();
		rv = fx_bind();
		fx_unlock();
	}
	if (rv)
		printk(KERN_ERR "mmcehd: fileXio RPC service not available "
		       "(%d)\n", rv);
	return rv;
}

static int __init mmcehd_open(const char *path)
{
	int fd;

	fx_lock();
	fd = fx_open(path, FX_O_RDWR, 0);
	fx_unlock();
	return fd;
}

/* Read @count sectors at @lba into @buf; called with fx_lock() held */
static int mmcehd_read_locked(unsigned long lba, void *buf, int count)
{
	loff_t pos = (loff_t)lba << 9;
	int done = 0, n, want = count << 9;

	if (fx_lseek64(mmcehd_fd, pos, MMCEHD_SEEK_SET) != pos)
		return -EIO;
	while (done < want) {
		n = fx_read(mmcehd_fd, (u8 *)buf + done, want - done);
		if (n <= 0)
			return n < 0 ? n : -EIO;
		done += n;
	}
	return 0;
}

/*
 * The image must start with an intact APA chain that ends inside it.
 * Reading every header also proves that 64-bit seeks reach the end of a
 * large image.
 */
static int __init mmcehd_check_apa(void)
{
	u32 *h = (u32 *)mmcehd_stage;
	unsigned long lba = 0;
	u32 sum;
	int i, n, rv;

	for (n = 0; n < APA_MAX_PARTS; n++) {
		fx_lock();
		rv = mmcehd_read_locked(lba, h, 2);
		fx_unlock();
		if (rv) {
			printk(KERN_ERR "mmcehd: cannot read the APA header "
			       "at sector %lu (%d)\n", lba, rv);
			return -EIO;
		}
		for (sum = 0, i = 1; i < 256; i++)
			sum += le32_to_cpu(h[i]);
		if (le32_to_cpu(h[1]) != APA_MAGIC ||
		    le32_to_cpu(h[0]) != sum) {
			printk(KERN_ERR "mmcehd: no valid APA header at sector "
			       "%lu\n", lba);
			return -EINVAL;
		}
		if (le32_to_cpu(h[16]) != lba || !le32_to_cpu(h[17]) ||
		    le32_to_cpu(h[17]) > mmcehd_capacity - lba) {
			printk(KERN_ERR "mmcehd: APA partition at sector %lu "
			       "does not fit the image\n", lba);
			return -EINVAL;
		}
		lba = le32_to_cpu(h[2]);	/* next */
		if (lba == 0)
			return n + 1;
		if (lba >= mmcehd_capacity) {
			printk(KERN_ERR "mmcehd: APA chain leaves the image "
			       "(sector %lu)\n", lba);
			return -EINVAL;
		}
	}
	printk(KERN_ERR "mmcehd: APA chain does not end\n");
	return -EINVAL;
}

/* Find, open and check the image; leaves it open in mmcehd_fd */
static int __init mmcehd_find_image(void)
{
	static const char *defaults[] = {
		"mmce0:/PSBBN/psbbn.img", "mmce1:/PSBBN/psbbn.img", NULL
	};
	const char *one[2];
	const char **paths = defaults;
	loff_t size;
	int i, fd = -ENOENT, loaded = 0, parts;

	if (mmcehd_param[0]) {
		one[0] = mmcehd_param;
		one[1] = NULL;
		paths = one;
	}
	for (i = 0; paths[i]; i++) {
		fd = mmcehd_open(paths[i]);
		if (fd == -ENODEV && !loaded) {
			/* no mmceN: device yet */
			mmcehd_load_irx("mmceman.irx", irx_mmceman,
					sizeof(irx_mmceman));
			loaded = 1;
			fd = mmcehd_open(paths[i]);
		}
		if (fd >= 0) {
			strcpy(mmcehd_path, paths[i]);
			break;
		}
		printk(KERN_INFO "mmcehd: %s: cannot open (%d)\n",
		       paths[i], fd);
	}
	if (fd < 0)
		return fd;
	mmcehd_fd = fd;

	fx_lock();
	size = fx_lseek64(fd, 0, MMCEHD_SEEK_END);
	fx_unlock();
	if (size < 0 || size < 4 * 1024 * 1024) {
		printk(KERN_ERR "mmcehd: %s: bad size %Ld\n", mmcehd_path,
		       (long long)size);
		goto bad;
	}
	if ((size >> 9) > 0xffffffffULL) {
		printk(KERN_WARNING "mmcehd: using the first 2 TiB of %s\n",
		       mmcehd_path);
		size = 0xffffffffULL << 9;
	}
	mmcehd_capacity = (unsigned long)(size >> 9);
	parts = mmcehd_check_apa();
	if (parts < 0)
		goto bad;
	printk(KERN_INFO "mmcehd: %s: %lu sectors (%lu MiB), %d APA "
	       "headers\n", mmcehd_path, mmcehd_capacity,
	       mmcehd_capacity >> 11, parts);
	return 0;
bad:
	fx_lock();
	fx_close(fd);
	fx_unlock();
	mmcehd_fd = -1;
	return -EINVAL;
}

/* --- requests -------------------------------------------------------- */

/* Copy @n bytes between the stage and the buffer heads at (*bhp, *offp) */
static void mmcehd_copy(struct buffer_head **bhp, unsigned int *offp,
			u8 *stage, unsigned int n, int to_stage)
{
	struct buffer_head *bh = *bhp;
	unsigned int off = *offp, len;

	while (n && bh) {
		len = bh->b_size - off;
		if (len > n)
			len = n;
		if (to_stage)
			memcpy(stage, bh->b_data + off, len);
		else
			memcpy(bh->b_data + off, stage, len);
		stage += len;
		n -= len;
		off += len;
		if (off == bh->b_size) {
			bh = bh->b_reqnext;
			off = 0;
		}
	}
	*bhp = bh;
	*offp = off;
}

static int mmcehd_transfer(struct request *req)
{
	struct buffer_head *bh = req->bh;
	unsigned int off = 0, n;
	unsigned long left = req->nr_sectors << 9;
	loff_t pos = (loff_t)req->sector << 9;
	int write = req->cmd == WRITE, rv = 0, done;

	if (req->sector + req->nr_sectors > mmcehd_capacity ||
	    req->sector + req->nr_sectors < req->sector)
		return -EIO;
	fx_lock();
	if (mmcehd_fd < 0) {
		rv = -EIO;
		goto out;
	}
	if (fx_lseek64(mmcehd_fd, pos, MMCEHD_SEEK_SET) != pos) {
		rv = -EIO;
		goto out;
	}
	while (left) {
		n = left > FX_IOBUF ? FX_IOBUF : left;
		if (write) {
			mmcehd_copy(&bh, &off, mmcehd_stage, n, 1);
			rv = fx_write(mmcehd_fd, mmcehd_stage, n);
			if (rv != n) {
				rv = rv < 0 ? rv : -EIO;
				goto out;
			}
		} else {
			for (done = 0; done < n; done += rv) {
				rv = fx_read(mmcehd_fd, mmcehd_stage + done,
					     n - done);
				if (rv <= 0) {
					rv = rv < 0 ? rv : -EIO;
					goto out;
				}
			}
			mmcehd_copy(&bh, &off, mmcehd_stage, n, 0);
		}
		left -= n;
	}
	rv = 0;
out:
	fx_unlock();
	return rv;
}

/* Called with io_request_lock held: the thread takes the requests */
static void mmcehd_request(request_queue_t *q)
{
	wake_up(&mmcehd_wait);
}

static int mmcehd_thread(void *unused)
{
	request_queue_t *q = BLK_DEFAULT_QUEUE(MAJOR_NR);
	struct request *req;
	struct buffer_head *bh;
	unsigned long t;
	int rv;

	daemonize();
	exit_files(current);
	strcpy(current->comm, "mmcehd");
	spin_lock_irq(&current->sigmask_lock);
	sigfillset(&current->blocked);
	flush_signals(current);
	spin_unlock_irq(&current->sigmask_lock);
	current->flags |= PF_NOIO;

	for (;;) {
		spin_lock_irq(&io_request_lock);
		while (list_empty(&q->queue_head)) {
			spin_unlock_irq(&io_request_lock);
			wait_event(mmcehd_wait, !list_empty(&q->queue_head));
			spin_lock_irq(&io_request_lock);
		}
		req = blkdev_entry_next_request(&q->queue_head);
		blkdev_dequeue_request(req);
		spin_unlock_irq(&io_request_lock);

		t = jiffies;
		rv = mmcehd_transfer(req);
		st_busy += jiffies - t;
		if (req->cmd == WRITE) {
			st_writes++;
			st_wsect += req->nr_sectors;
		} else {
			st_reads++;
			st_rsect += req->nr_sectors;
		}
		if (rv) {
			st_errors++;
			if (st_errors <= 20)
				printk(KERN_ERR "mmcehd: %s of %lu sectors at "
				       "%lu failed (%d)\n",
				       req->cmd == WRITE ? "write" : "read",
				       req->nr_sectors, req->sector, rv);
		}
		while ((bh = req->bh) != NULL) {
			req->bh = bh->b_reqnext;
			bh->b_reqnext = NULL;
			bh->b_end_io(bh, rv == 0);
		}
		spin_lock_irq(&io_request_lock);
		end_that_request_last(req);
		spin_unlock_irq(&io_request_lock);
	}
	return 0;
}

/* --- block device ---------------------------------------------------- */

static int mmcehd_bdopen(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);

	if (minor >= MMCEHD_MINORS)
		return -ENXIO;
	if (minor && !mmcehd_part[minor].nr_sects)
		return -ENXIO;
	mmcehd_usage[minor]++;
	return 0;
}

static int mmcehd_bdrelease(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);

	if (minor < MMCEHD_MINORS && mmcehd_usage[minor] > 0)
		mmcehd_usage[minor]--;
	return 0;
}

/*
 * BLKRRPART, as ide_revalidate_disk() does it: partitions in use keep
 * working (the APA parser gives them the same numbers again) and only
 * the unused ones are invalidated.  bn and pfsd call it after creating
 * an APA partition.
 */
static int mmcehd_revalidate(void)
{
	unsigned long old_start[MMCEHD_MINORS], old_nr[MMCEHD_MINORS];
	int p;

	if (down_trylock(&mmcehd_reval_sem))
		return -EBUSY;
	down(&mmcehd_part_sem);
	for (p = 1; p < MMCEHD_MINORS; p++) {
		old_start[p] = old_nr[p] = 0;
		if (mmcehd_part[p].nr_sects) {
			if (mmcehd_usage[p]) {
				old_start[p] = mmcehd_part[p].start_sect;
				old_nr[p] = mmcehd_part[p].nr_sects;
			} else
				invalidate_device(MKDEV(MAJOR_NR, p), 1);
		}
		mmcehd_part[p].start_sect = 0;
		mmcehd_part[p].nr_sects = 0;
		mmcehd_part[p].nr_segs = 1;
		if (mmcehd_part[p].seg)
			kfree(mmcehd_part[p].seg);
		mmcehd_part[p].seg = NULL;
		mmcehd_part[p].hash_unit = 0;
		if (mmcehd_part[p].hash)
			kfree(mmcehd_part[p].hash);
		mmcehd_part[p].hash = NULL;
	}
	grok_partitions(&mmcehd_gendisk, 0, MMCEHD_MINORS, mmcehd_capacity);
	up(&mmcehd_part_sem);
	for (p = 1; p < MMCEHD_MINORS; p++)
		if (old_nr[p] && (mmcehd_part[p].start_sect != old_start[p] ||
				  mmcehd_part[p].nr_sects != old_nr[p]))
			printk(KERN_WARNING "hda%d: Device is in use! Data in "
			       "the partition may be corrupt.\n", p);
	up(&mmcehd_reval_sem);
	return 0;
}

static int mmcehd_ioctl(struct inode *inode, struct file *file,
			unsigned int cmd, unsigned long arg)
{
	int minor = MINOR(inode->i_rdev);
	struct hd_geometry geo;
	unsigned long nr;

	if (minor >= MMCEHD_MINORS)
		return -ENXIO;
	nr = mmcehd_part[minor].nr_sects;
	switch (cmd) {
	case BLKGETSIZE:
		return put_user(nr, (unsigned long *)arg);
	case BLKGETSIZE64:
		return put_user((u64)nr << 9, (u64 *)arg);
	case HDIO_GETGEO:
		if (!arg)
			return -EINVAL;
		geo.heads = 16;
		geo.sectors = 63;
		geo.cylinders = mmcehd_capacity / (16 * 63);
		geo.start = mmcehd_part[minor].start_sect;
		return copy_to_user((void *)arg, &geo, sizeof(geo)) ?
			-EFAULT : 0;
	case BLKRRPART:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		return mmcehd_revalidate();
	case HDIO_DRIVE_CMD:
		/* hdparm -y/-S/-f and the like: nothing to spin down */
		if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SYS_RAWIO))
			return -EACCES;
		return 0;
	case BLKFLSBUF:
	case BLKROSET:
	case BLKROGET:
	case BLKRASET:
	case BLKRAGET:
	case BLKSSZGET:
	case BLKBSZGET:
	case BLKBSZSET:
	case BLKPG:
	case BLKELVGET:
	case BLKELVSET:
		return blk_ioctl(inode->i_rdev, cmd, arg);
	}
	if (st_unknown_ioctls++ < 16)
		printk(KERN_INFO "mmcehd: unsupported ioctl 0x%x on hda%d "
		       "from %s\n", cmd, minor, current->comm);
	return -EINVAL;
}

static struct block_device_operations mmcehd_fops = {
	open:		mmcehd_bdopen,
	release:	mmcehd_bdrelease,
	ioctl:		mmcehd_ioctl,
};

static int mmcehd_proc(char *page, char **start, off_t off, int count,
		       int *eof, void *data)
{
	int len;

	len = sprintf(page,
		"image\t%s\nsectors\t%lu\nreads\t%lu\nread_sectors\t%lu\n"
		"writes\t%lu\nwrite_sectors\t%lu\nerrors\t%lu\n"
		"busy_ms\t%lu\nopen\t%s\n",
		mmcehd_path, mmcehd_capacity, st_reads, st_rsect, st_writes,
		st_wsect, st_errors, st_busy * 1000 / HZ,
		mmcehd_fd >= 0 ? "yes" : "no (closed for reboot)");
	if (off >= len) {
		*eof = 1;
		return 0;
	}
	*start = page + off;
	len -= off;
	if (len > count)
		len = count;
	else
		*eof = 1;
	return len;
}

/*
 * Close the image before a reboot, power-off or native launch, so the
 * MMCE firmware writes back everything it holds for the file.  Dirty
 * buffers go first; a write after this point fails.
 */
static int mmcehd_reboot(struct notifier_block *nb, unsigned long code,
			 void *unused)
{
	if (code != SYS_RESTART && code != SYS_HALT && code != SYS_POWER_OFF)
		return NOTIFY_DONE;
	fsync_dev(0);		/* all devices, waits for the I/O */
	fx_lock();
	if (mmcehd_fd >= 0) {
		fx_close(mmcehd_fd);
		mmcehd_fd = -1;
		printk(KERN_INFO "mmcehd: %s closed\n", mmcehd_path);
	}
	fx_unlock();
	return NOTIFY_DONE;
}

static struct notifier_block mmcehd_notifier = {
	notifier_call:	mmcehd_reboot,
};

/*
 * "root=/dev/mmce" also asks for the image.  Sony's loader rejects a root
 * string that starts with "/dev/hd" and is longer than 10 characters
 * (BB.ELF 0x1000ca8), so "/dev/hda1 mmce" cannot be passed that way; a
 * root outside /dev/hd* is not checked at all.  The root is then
 * /dev/hda1, from the image or, without one, from the internal HDD.
 */
static int __init mmcehd_root_wanted(void)
{
	extern char saved_command_line[];
	extern char root_device_name[];
	char *p = saved_command_line;

	while ((p = strstr(p, "root=/dev/mmce")) != NULL) {
		if (p == saved_command_line || p[-1] == ' ') {
			ROOT_DEV = MKDEV(MAJOR_NR, 1);
			strcpy(root_device_name, "hda1");
			return 1;
		}
		p++;
	}
	return 0;
}

/* Called by blk_dev_init() before ide_init() */
int __init mmcehd_init(void)
{
	request_queue_t *q;
	int i, rv;

	if (mmcehd_root_wanted())
		mmcehd_wanted = 1;
	if (!mmcehd_wanted)
		return 0;
	printk(KERN_INFO "mmcehd: looking for the disk image on the MMCE\n");
	rv = fx_init();		/* normally done by init_mmcefs() */
	if (rv)
		return rv;
	mmcehd_stage = kmalloc(FX_IOBUF, GFP_KERNEL);
	if (!mmcehd_stage)
		return -ENOMEM;
	if ((rv = mmcehd_prepare_fileio()) || (rv = mmcehd_find_image())) {
		printk(KERN_ERR "mmcehd: no usable disk image, using the "
		       "internal HDD\n");
		goto fail;
	}

	rv = register_blkdev(MAJOR_NR, "mmcehd", &mmcehd_fops);
	if (rv) {
		printk(KERN_ERR "mmcehd: major %d is taken (%d)\n", MAJOR_NR,
		       rv);
		goto fail_close;
	}
	for (i = 0; i < MMCEHD_MINORS; i++) {
		mmcehd_blksizes[i] = BLOCK_SIZE;
		mmcehd_hardsect[i] = 512;
		mmcehd_maxsect[i] = MMCEHD_MAX_SECT;
		mmcehd_part[i].nr_segs = 1;
	}
	blksize_size[MAJOR_NR] = mmcehd_blksizes;
	hardsect_size[MAJOR_NR] = mmcehd_hardsect;
	max_sectors[MAJOR_NR] = mmcehd_maxsect;
	read_ahead[MAJOR_NR] = 8;
	q = BLK_DEFAULT_QUEUE(MAJOR_NR);
	blk_init_queue(q, mmcehd_request);
	blk_queue_headactive(q, 0);

	mmcehd_gendisk.major = MAJOR_NR;
	mmcehd_gendisk.major_name = "hd";
	mmcehd_gendisk.minor_shift = MMCEHD_SHIFT;
	mmcehd_gendisk.max_p = MMCEHD_MINORS;
	mmcehd_gendisk.part = mmcehd_part;
	mmcehd_gendisk.sizes = mmcehd_sizes;
	mmcehd_gendisk.nr_real = 1;
	mmcehd_gendisk.fops = &mmcehd_fops;
	mmcehd_gendisk.part_sem = &mmcehd_part_sem;
	add_gendisk(&mmcehd_gendisk);
	blk_dev[MAJOR_NR].gd = &mmcehd_gendisk;

	rv = kernel_thread(mmcehd_thread, NULL, CLONE_FS | CLONE_FILES |
			   CLONE_SIGHAND);
	if (rv < 0) {
		printk(KERN_ERR "mmcehd: no I/O thread (%d)\n", rv);
		del_gendisk(&mmcehd_gendisk);
		blk_dev[MAJOR_NR].gd = NULL;
		blk_cleanup_queue(q);
		blksize_size[MAJOR_NR] = NULL;
		hardsect_size[MAJOR_NR] = NULL;
		max_sectors[MAJOR_NR] = NULL;
		unregister_blkdev(MAJOR_NR, "mmcehd");
		goto fail_close;
	}
	register_disk(&mmcehd_gendisk, MKDEV(MAJOR_NR, 0), MMCEHD_MINORS,
		      &mmcehd_fops, mmcehd_capacity);
	register_reboot_notifier(&mmcehd_notifier);
	create_proc_read_entry("mmcehd", 0, NULL, mmcehd_proc, NULL);
	printk(KERN_INFO "mmcehd: %s is /dev/hda; the internal HDD is not "
	       "used\n", mmcehd_path);
	return 0;

fail_close:
	fx_lock();
	fx_close(mmcehd_fd);
	fx_unlock();
	mmcehd_fd = -1;
fail:
	kfree(mmcehd_stage);
	mmcehd_stage = NULL;
	return rv;
}

#endif /* !MODULE */
