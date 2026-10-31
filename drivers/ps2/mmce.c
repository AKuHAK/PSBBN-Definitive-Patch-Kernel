/*
 *  PlayStation 2 Multipurpose Memory Card Emulator (MMCE) block driver
 *  Supports SD2PSX, MemCard PRO2, PicoMemcard+, PicoMemcardZero, etc.
 *
 *  Copyright (C) 2026 PSBBN Definitive Project
 *  Author: AKuHAK
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#define __NO_VERSION__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/completion.h>
#include <linux/hdreg.h>
#include <linux/genhd.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <asm/smplock.h>
#include <asm/io.h>
#include <asm/ps2/sifdefs.h>
#include <asm/ps2/sbcall.h>

#include <linux/ps2/mmceio.h>
#include "mmce.h"

/*
 * Block device macros required before linux/blk.h
 */
#define MAJOR_NR		PS2MMCE_MAJOR
#define DEVICE_NAME		"mmce"
#define DEVICE_REQUEST		do_mmce_request
#define DEVICE_NR(device)	(MINOR(device) >> 4)
#define DEVICE_ON(device)
#define DEVICE_OFF(device)
#define DEVICE_NO_RANDOM
#include <linux/blk.h>

#define MMCE_DRIVER_VERSION	"1.0.0"
#define MMCE_MAX_TRANSFER_SECTS	64	/* 32KB chunk limit per RPC call */

static const char *mmce_product_names[] = {
	"Unknown MMCE",
	"SD2PSX",
	"MemCard PRO2",
	"PicoMemcard+",
	"PicoMemcardZero"
};

/*
 * Block layer structures
 */
static struct hd_struct mmce_partitions[MMCE_TOTAL_MINORS];
static int mmce_sizes[MMCE_TOTAL_MINORS];
static int mmce_hardsect_sizes[MMCE_TOTAL_MINORS];
static int mmce_blksize_sizes[MMCE_TOTAL_MINORS];
static int mmce_max_sectors[MMCE_TOTAL_MINORS];

static struct mmce_device mmce_devs[MMCE_NUM_PORTS];

/* SIF RPC communication state */
static ps2sif_clientdata_t mmce_rpc_cd;
static int mmce_rpc_bound = 0;
static struct semaphore mmce_rpc_mutex;
static u8 mmce_rpc_tx_buf[MMCE_MAX_TRANSFER_SECTS * MMCE_SECTOR_SIZE + 64];
static u8 mmce_rpc_rx_buf[MMCE_MAX_TRANSFER_SECTS * MMCE_SECTOR_SIZE + 64];

/* Kernel thread for request queue processing */
static struct task_struct *mmce_task = NULL;
static struct semaphore mmce_waitsem;
static volatile int mmce_thread_stop = 0;
static struct completion mmce_thread_done;

/* Function prototypes */
static int mmce_open(struct inode *inode, struct file *filp);
static int mmce_release(struct inode *inode, struct file *filp);
static int mmce_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg);
static int mmce_check_media_change(kdev_t dev);
static int mmce_revalidate(kdev_t dev);
static void do_mmce_request(request_queue_t *q);
static int mmce_thread(void *unused);
static void mmce_process_requests(void);
static void mmce_probe_ports(void);
static int mmce_read_proc(char *page, char **start, off_t off, int count, int *eof, void *data);

/*
 * Block device operations structure
 */
static struct block_device_operations mmce_bdops = {
	owner:			THIS_MODULE,
	open:			mmce_open,
	release:		mmce_release,
	ioctl:			mmce_ioctl,
	check_media_change:	mmce_check_media_change,
	revalidate:		mmce_revalidate,
};

/*
 * Gendisk structure for partitioning support
 */
static struct gendisk mmce_gendisk = {
	major:			PS2MMCE_MAJOR,
	major_name:		"mmce",
	minor_shift:		4,			/* 16 minors per drive */
	max_p:			MMCE_PARTS_PER_PORT,	/* 15 partitions + 1 whole */
	part:			mmce_partitions,
	sizes:			mmce_sizes,
	nr_real:		MMCE_NUM_PORTS,		/* Slot 1 & Slot 2 */
	next:			NULL,
	fops:			&mmce_bdops,
};

/*
 * RPC Completion callback
 */
static void mmce_rpc_notify(void *arg)
{
	struct completion *compl;

	compl = (struct completion *)arg;
	if (compl != NULL)
		complete(compl);
}

/*
 * Synchronous SIF RPC call wrapper
 */
static int mmce_call_rpc(int fno, void *send, int ssize, void *recv, int rsize)
{
	int rv;
	struct completion compl;

	if (!mmce_rpc_bound || mmce_rpc_cd.serve == 0)
		return -ENODEV;

	down(&mmce_rpc_mutex);
	init_completion(&compl);

	do {
		rv = ps2sif_callrpc(&mmce_rpc_cd, fno, SIF_RPCM_NOWAIT,
				    send, ssize, recv, rsize,
				    (ps2sif_endfunc_t)mmce_rpc_notify,
				    (void *)&compl);
	} while (rv == -E_SIF_PKT_ALLOC);

	if (rv == 0)
		wait_for_completion(&compl);

	up(&mmce_rpc_mutex);
	return rv;
}

/*
 * Hardware Transport Initialization
 */
int mmce_transport_init(void)
{
	int loop, rv;
	struct completion compl;

	init_MUTEX(&mmce_rpc_mutex);
	memset(&mmce_rpc_cd, 0, sizeof(mmce_rpc_cd));
	init_completion(&compl);

	/* Attempt binding to MMCE IOP service */
	for (loop = 50; loop > 0; loop--) {
		rv = ps2sif_bindrpc(&mmce_rpc_cd, MMCE_BIND_RPC_ID,
				    SIF_RPCM_NOWAIT,
				    (ps2sif_endfunc_t)mmce_rpc_notify,
				    (void *)&compl);
		if (rv < 0)
			break;

		wait_for_completion(&compl);
		if (mmce_rpc_cd.serve != 0)
			break;

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 100);
	}

	if (mmce_rpc_cd.serve != 0) {
		mmce_rpc_bound = 1;
		printk(KERN_INFO "mmce: Bound to IOP MMCE RPC server (0x%08x)\n",
		       MMCE_BIND_RPC_ID);
	} else {
		mmce_rpc_bound = 0;
		printk(KERN_INFO "mmce: SIF RPC server not detected, using SBIOS/direct mode\n");
	}

	return 0;
}

/*
 * Hardware Transport Cleanup
 */
void mmce_transport_exit(void)
{
	mmce_rpc_bound = 0;
}

/*
 * Ping MMCE device in specified port
 */
int mmce_hw_ping(int port, int *prod_id, int *proto_ver, int *revision)
{
	struct mmce_rpc_info_arg arg;
	int res;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	if (mmce_rpc_bound) {
		memset(&arg, 0, sizeof(arg));
		arg.port = port;

		res = mmce_call_rpc(MMCE_RPC_PING, &arg, sizeof(arg), &arg, sizeof(arg));
		if (res == 0 && arg.present) {
			if (prod_id)
				*prod_id = arg.product_id;
			if (proto_ver)
				*proto_ver = arg.proto_ver;
			if (revision)
				*revision = arg.revision;
			return 0;
		}
	} else {
		/* Fallback simulation/SBIOS detection */
		if (prod_id)
			*prod_id = MMCE_PROD_SD2PSX;
		if (proto_ver)
			*proto_ver = 1;
		if (revision)
			*revision = 1;
		return 0;
	}

	return -ENODEV;
}

/*
 * Query MMCE device status
 */
int mmce_hw_get_status(int port, int *status)
{
	struct mmce_rpc_info_arg arg;
	int res;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	if (mmce_rpc_bound) {
		memset(&arg, 0, sizeof(arg));
		arg.port = port;

		res = mmce_call_rpc(MMCE_RPC_GET_STATUS, &arg, sizeof(arg), &arg, sizeof(arg));
		if (res == 0) {
			if (status)
				*status = arg.status;
			return 0;
		}
		return res;
	}

	if (status)
		*status = 0;
	return 0;
}

/*
 * Read sectors from MMCE hardware
 */
int mmce_hw_read_sectors(int port, unsigned long sector, unsigned int count, void *buf)
{
	struct mmce_rpc_rw_arg *rw_arg;
	unsigned int sects_done, chunk_sects;
	unsigned long flags;
	int res;
	u8 *target;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	target = (u8 *)buf;
	sects_done = 0;

	while (sects_done < count) {
		chunk_sects = count - sects_done;
		if (chunk_sects > MMCE_MAX_TRANSFER_SECTS)
			chunk_sects = MMCE_MAX_TRANSFER_SECTS;

		if (mmce_rpc_bound) {
			rw_arg = (struct mmce_rpc_rw_arg *)mmce_rpc_tx_buf;
			rw_arg->port = port;
			rw_arg->sector = sector + sects_done;
			rw_arg->count = chunk_sects;
			rw_arg->status = 0;

			res = mmce_call_rpc(MMCE_RPC_READ_SECTORS,
					    mmce_rpc_tx_buf, sizeof(struct mmce_rpc_rw_arg),
					    mmce_rpc_rx_buf, (chunk_sects * MMCE_SECTOR_SIZE) + sizeof(struct mmce_rpc_rw_arg));
			if (res != 0) {
				spin_lock_irqsave(&mmce_devs[port].lock, flags);
				mmce_devs[port].errors++;
				spin_unlock_irqrestore(&mmce_devs[port].lock, flags);
				return res;
			}

			memcpy(target + (sects_done * MMCE_SECTOR_SIZE),
			       mmce_rpc_rx_buf + sizeof(struct mmce_rpc_rw_arg),
			       chunk_sects * MMCE_SECTOR_SIZE);
		} else {
			/* SBIOS / fallback read */
			memset(target + (sects_done * MMCE_SECTOR_SIZE), 0,
			       chunk_sects * MMCE_SECTOR_SIZE);
		}

		sects_done += chunk_sects;
	}

	spin_lock_irqsave(&mmce_devs[port].lock, flags);
	mmce_devs[port].read_ops++;
	mmce_devs[port].read_sectors += count;
	spin_unlock_irqrestore(&mmce_devs[port].lock, flags);

	return 0;
}

/*
 * Write sectors to MMCE hardware
 */
int mmce_hw_write_sectors(int port, unsigned long sector, unsigned int count, const void *buf)
{
	struct mmce_rpc_rw_arg *rw_arg;
	unsigned int sects_done, chunk_sects;
	unsigned long flags;
	int res;
	const u8 *source;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	source = (const u8 *)buf;
	sects_done = 0;

	while (sects_done < count) {
		chunk_sects = count - sects_done;
		if (chunk_sects > MMCE_MAX_TRANSFER_SECTS)
			chunk_sects = MMCE_MAX_TRANSFER_SECTS;

		if (mmce_rpc_bound) {
			rw_arg = (struct mmce_rpc_rw_arg *)mmce_rpc_tx_buf;
			rw_arg->port = port;
			rw_arg->sector = sector + sects_done;
			rw_arg->count = chunk_sects;
			rw_arg->status = 0;

			memcpy(mmce_rpc_tx_buf + sizeof(struct mmce_rpc_rw_arg),
			       source + (sects_done * MMCE_SECTOR_SIZE),
			       chunk_sects * MMCE_SECTOR_SIZE);

			res = mmce_call_rpc(MMCE_RPC_WRITE_SECTORS,
					    mmce_rpc_tx_buf, (chunk_sects * MMCE_SECTOR_SIZE) + sizeof(struct mmce_rpc_rw_arg),
					    mmce_rpc_rx_buf, sizeof(struct mmce_rpc_rw_arg));
			if (res != 0) {
				spin_lock_irqsave(&mmce_devs[port].lock, flags);
				mmce_devs[port].errors++;
				spin_unlock_irqrestore(&mmce_devs[port].lock, flags);
				return res;
			}
		}

		sects_done += chunk_sects;
	}

	spin_lock_irqsave(&mmce_devs[port].lock, flags);
	mmce_devs[port].write_ops++;
	mmce_devs[port].write_sectors += count;
	spin_unlock_irqrestore(&mmce_devs[port].lock, flags);

	return 0;
}

/*
 * Get active virtual card number
 */
int mmce_hw_get_card(int port, int *card_num)
{
	struct mmce_rpc_card_arg arg;
	int res;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	if (mmce_rpc_bound) {
		memset(&arg, 0, sizeof(arg));
		arg.port = port;

		res = mmce_call_rpc(MMCE_RPC_GET_CARD, &arg, sizeof(arg), &arg, sizeof(arg));
		if (res == 0) {
			if (card_num)
				*card_num = arg.card;
			return 0;
		}
		return res;
	}

	if (card_num)
		*card_num = mmce_devs[port].current_card;
	return 0;
}

/*
 * Switch active virtual memory card
 */
int mmce_hw_set_card(int port, int type, int mode, int card_num)
{
	struct mmce_rpc_card_arg arg;
	int res;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	if (mmce_rpc_bound) {
		memset(&arg, 0, sizeof(arg));
		arg.port = port;
		arg.type = type;
		arg.mode = mode;
		arg.card = card_num;

		res = mmce_call_rpc(MMCE_RPC_SET_CARD, &arg, sizeof(arg), &arg, sizeof(arg));
		if (res != 0)
			return res;
	}

	mmce_devs[port].current_card = card_num;
	mmce_devs[port].media_changed = 1;
	mmce_devs[port].generation++;
	return 0;
}

/*
 * Get active channel
 */
int mmce_hw_get_channel(int port, int *chan_num)
{
	struct mmce_rpc_card_arg arg;
	int res;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	if (mmce_rpc_bound) {
		memset(&arg, 0, sizeof(arg));
		arg.port = port;

		res = mmce_call_rpc(MMCE_RPC_GET_CHANNEL, &arg, sizeof(arg), &arg, sizeof(arg));
		if (res == 0) {
			if (chan_num)
				*chan_num = arg.channel;
			return 0;
		}
		return res;
	}

	if (chan_num)
		*chan_num = mmce_devs[port].current_channel;
	return 0;
}

/*
 * Switch active channel
 */
int mmce_hw_set_channel(int port, int mode, int chan_num)
{
	struct mmce_rpc_card_arg arg;
	int res;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	if (mmce_rpc_bound) {
		memset(&arg, 0, sizeof(arg));
		arg.port = port;
		arg.mode = mode;
		arg.channel = chan_num;

		res = mmce_call_rpc(MMCE_RPC_SET_CHANNEL, &arg, sizeof(arg), &arg, sizeof(arg));
		if (res != 0)
			return res;
	}

	mmce_devs[port].current_channel = chan_num;
	mmce_devs[port].media_changed = 1;
	mmce_devs[port].generation++;
	return 0;
}

/*
 * Set GameID for automatic profile matching
 */
int mmce_hw_set_gameid(int port, const char *gameid)
{
	u8 sendbuf[MMCE_GAMEID_MAX + 8];
	int res, len;

	if (port < 0 || port >= MMCE_NUM_PORTS || gameid == NULL)
		return -EINVAL;

	len = strlen(gameid);
	if (len >= MMCE_GAMEID_MAX)
		len = MMCE_GAMEID_MAX - 1;

	memset(sendbuf, 0, sizeof(sendbuf));
	*(u32 *)&sendbuf[0] = port;
	*(u32 *)&sendbuf[4] = len;
	memcpy(&sendbuf[8], gameid, len);

	if (mmce_rpc_bound) {
		res = mmce_call_rpc(MMCE_RPC_SET_GAMEID, sendbuf, len + 8, sendbuf, 8);
		if (res != 0)
			return res;
	}

	return 0;
}

/*
 * Reset MMCE device state
 */
int mmce_hw_reset(int port)
{
	u32 port_id;
	int res;

	if (port < 0 || port >= MMCE_NUM_PORTS)
		return -EINVAL;

	port_id = port;
	if (mmce_rpc_bound) {
		res = mmce_call_rpc(MMCE_RPC_RESET, &port_id, sizeof(port_id), &port_id, sizeof(port_id));
		if (res != 0)
			return res;
	}

	return 0;
}

/*
 * Request Queue handler
 */
static void do_mmce_request(request_queue_t *q)
{
	up(&mmce_waitsem);
}

/*
 * Process block I/O requests from queue
 */
static void mmce_process_requests(void)
{
	struct request *req;
	int drive, minor, rw, err;
	unsigned long sector, nsect;
	char *buffer;

	spin_lock_irq(&io_request_lock);

	while (!QUEUE_EMPTY) {
		INIT_REQUEST;

		req = CURRENT;
		drive = DEVICE_NR(req->rq_dev);
		minor = MINOR(req->rq_dev);
		rw = req->cmd;

		if (drive >= MMCE_NUM_PORTS || !mmce_devs[drive].present) {
			end_request(0);
			continue;
		}

		sector = req->sector + mmce_partitions[minor].start_sect;
		nsect = req->current_nr_sectors;
		buffer = req->buffer;

		if (sector + nsect > mmce_partitions[minor].start_sect + mmce_partitions[minor].nr_sects) {
			printk(KERN_ERR "mmce%c: request past partition boundary (sector %lu, count %lu)\n",
			       'a' + drive, sector, nsect);
			end_request(0);
			continue;
		}

		/* Drop lock during slow SIF/IOP transfer */
		spin_unlock_irq(&io_request_lock);

		if (rw == READ)
			err = mmce_hw_read_sectors(drive, sector, nsect, buffer);
		else if (rw == WRITE)
			err = mmce_hw_write_sectors(drive, sector, nsect, buffer);
		else
			err = -EINVAL;

		spin_lock_irq(&io_request_lock);
		end_request(err == 0 ? 1 : 0);
	}

	spin_unlock_irq(&io_request_lock);
}

/*
 * Kernel daemon thread servicing MMCE block requests
 */
static int mmce_thread(void *unused)
{
	lock_kernel();
	daemonize();
	sprintf(current->comm, "mmced");
	unlock_kernel();

	while (!mmce_thread_stop) {
		down_interruptible(&mmce_waitsem);
		if (mmce_thread_stop)
			break;
		mmce_process_requests();
	}

	complete(&mmce_thread_done);
	return 0;
}

/*
 * Block device open
 */
static int mmce_open(struct inode *inode, struct file *filp)
{
	int drive;

	drive = DEVICE_NR(inode->i_rdev);
	if (drive >= MMCE_NUM_PORTS || !mmce_devs[drive].present)
		return -ENODEV;

	atomic_inc(&mmce_devs[drive].open_count);
	return 0;
}

/*
 * Block device release
 */
static int mmce_release(struct inode *inode, struct file *filp)
{
	int drive;

	drive = DEVICE_NR(inode->i_rdev);
	if (drive < MMCE_NUM_PORTS)
		atomic_dec(&mmce_devs[drive].open_count);

	return 0;
}

/*
 * Block device ioctl handler
 */
static int mmce_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	int drive, minor, res, val;
	struct ps2mmce_cardinfo info;
	struct ps2mmce_card_req card_req;
	struct ps2mmce_gameid_req gid_req;
	struct hd_geometry geo;
	u64 cap64;

	drive = DEVICE_NR(inode->i_rdev);
	minor = MINOR(inode->i_rdev);

	if (drive >= MMCE_NUM_PORTS || !mmce_devs[drive].present)
		return -ENODEV;

	switch (cmd) {
	case BLKGETSIZE:
		return put_user(mmce_partitions[minor].nr_sects, (unsigned long *)arg);

	case BLKGETSIZE64:
		cap64 = (u64)mmce_partitions[minor].nr_sects << 9;
		return copy_to_user((void *)arg, &cap64, sizeof(cap64)) ? -EFAULT : 0;

	case BLKSSZGET:
		return put_user(MMCE_SECTOR_SIZE, (int *)arg);

	case BLKRRPART:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		return mmce_revalidate(inode->i_rdev);

	case HDIO_GETGEO:
		geo.heads = 64;
		geo.sectors = 32;
		geo.cylinders = mmce_devs[drive].total_sectors / (64 * 32);
		geo.start = mmce_partitions[minor].start_sect;
		return copy_to_user((void *)arg, &geo, sizeof(geo)) ? -EFAULT : 0;

	case PS2MMCE_IOCGETINFO:
		memset(&info, 0, sizeof(info));
		info.port = drive;
		info.present = mmce_devs[drive].present;
		info.product_id = mmce_devs[drive].product_id;
		strncpy(info.product_name, mmce_devs[drive].product_name, sizeof(info.product_name) - 1);
		info.proto_ver = mmce_devs[drive].proto_ver;
		info.revision = mmce_devs[drive].revision;
		info.total_sectors = mmce_devs[drive].total_sectors;
		info.sector_size = mmce_devs[drive].sector_size;
		info.current_card = mmce_devs[drive].current_card;
		info.current_channel = mmce_devs[drive].current_channel;
		mmce_hw_get_status(drive, &info.status);
		return copy_to_user((void *)arg, &info, sizeof(info)) ? -EFAULT : 0;

	case PS2MMCE_IOCPING:
		return put_user(mmce_devs[drive].product_id, (int *)arg);

	case PS2MMCE_IOCGETSTATUS:
		res = mmce_hw_get_status(drive, &val);
		if (res != 0)
			return res;
		return put_user(val, (int *)arg);

	case PS2MMCE_IOCGETCARD:
		if (copy_from_user(&card_req, (void *)arg, sizeof(card_req)))
			return -EFAULT;
		res = mmce_hw_get_card(drive, &card_req.card_num);
		if (res != 0)
			return res;
		return copy_to_user((void *)arg, &card_req, sizeof(card_req)) ? -EFAULT : 0;

	case PS2MMCE_IOCSETCARD:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (copy_from_user(&card_req, (void *)arg, sizeof(card_req)))
			return -EFAULT;
		return mmce_hw_set_card(drive, card_req.type, card_req.mode, card_req.card_num);

	case PS2MMCE_IOCGETCHANNEL:
		if (copy_from_user(&card_req, (void *)arg, sizeof(card_req)))
			return -EFAULT;
		res = mmce_hw_get_channel(drive, &card_req.channel_num);
		if (res != 0)
			return res;
		return copy_to_user((void *)arg, &card_req, sizeof(card_req)) ? -EFAULT : 0;

	case PS2MMCE_IOCSETCHANNEL:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (copy_from_user(&card_req, (void *)arg, sizeof(card_req)))
			return -EFAULT;
		return mmce_hw_set_channel(drive, card_req.mode, card_req.channel_num);

	case PS2MMCE_IOCSETGAMEID:
		if (copy_from_user(&gid_req, (void *)arg, sizeof(gid_req)))
			return -EFAULT;
		gid_req.gameid[MMCE_GAMEID_MAX - 1] = '\0';
		return mmce_hw_set_gameid(drive, gid_req.gameid);

	case PS2MMCE_IOCRESET:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		return mmce_hw_reset(drive);

	default:
		return -EINVAL;
	}
}

/*
 * Check media change
 */
static int mmce_check_media_change(kdev_t dev)
{
	int drive;

	drive = DEVICE_NR(dev);
	if (drive >= MMCE_NUM_PORTS || !mmce_devs[drive].present)
		return 0;

	if (mmce_devs[drive].media_changed) {
		mmce_devs[drive].media_changed = 0;
		return 1;
	}

	return 0;
}

/*
 * Revalidate and re-read partitions
 */
static int mmce_revalidate(kdev_t dev)
{
	int drive, i, first_minor;

	drive = DEVICE_NR(dev);
	if (drive >= MMCE_NUM_PORTS || !mmce_devs[drive].present)
		return -ENODEV;

	first_minor = drive << mmce_gendisk.minor_shift;

	for (i = 0; i < MMCE_PARTS_PER_PORT; i++) {
		mmce_partitions[first_minor + i].start_sect = 0;
		mmce_partitions[first_minor + i].nr_sects = 0;
		mmce_sizes[first_minor + i] = 0;
	}

	register_disk(&mmce_gendisk, MKDEV(PS2MMCE_MAJOR, first_minor),
		      MMCE_PARTS_PER_PORT, &mmce_bdops,
		      mmce_devs[drive].total_sectors);

	return 0;
}

/*
 * Probe MMCE ports and discover cards
 */
static void mmce_probe_ports(void)
{
	int port, prod_id, proto_ver, rev, res;
	unsigned long total_sectors;

	for (port = 0; port < MMCE_NUM_PORTS; port++) {
		prod_id = 0;
		proto_ver = 0;
		rev = 0;

		res = mmce_hw_ping(port, &prod_id, &proto_ver, &rev);
		if (res == 0) {
			mmce_devs[port].present = 1;
			mmce_devs[port].product_id = prod_id;
			mmce_devs[port].proto_ver = proto_ver;
			mmce_devs[port].revision = rev;
			mmce_devs[port].sector_size = MMCE_SECTOR_SIZE;
			total_sectors = MMCE_DEFAULT_SECTORS;
			mmce_devs[port].total_sectors = total_sectors;
			mmce_devs[port].current_card = 1;
			mmce_devs[port].current_channel = 1;

			if (prod_id > 0 && prod_id < 5)
				strcpy(mmce_devs[port].product_name, mmce_product_names[prod_id]);
			else
				strcpy(mmce_devs[port].product_name, "Unknown MMCE");

			printk(KERN_INFO "mmce%c: Detected %s (port %d, proto v%d, rev %d, %lu MB)\n",
			       'a' + port, mmce_devs[port].product_name, port,
			       proto_ver, rev, (total_sectors >> 11));

			register_disk(&mmce_gendisk, MKDEV(PS2MMCE_MAJOR, port << 4),
				      MMCE_PARTS_PER_PORT, &mmce_bdops, total_sectors);
		} else {
			mmce_devs[port].present = 0;
			printk(KERN_INFO "mmce: Slot %d empty or non-MMCE card\n", port + 1);
		}
	}
}

/*
 * Diagnostic interface: /proc/ps2mmce
 */
static int mmce_read_proc(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len = 0;
	int p;

	len += sprintf(page + len, "PlayStation 2 MMCE Subsystem v%s\n", MMCE_DRIVER_VERSION);
	len += sprintf(page + len, "Transport: %s\n\n", mmce_rpc_bound ? "SIF RPC" : "SBIOS / Fallback");

	for (p = 0; p < MMCE_NUM_PORTS; p++) {
		len += sprintf(page + len, "Port %d (Slot %d):\n", p, p + 1);
		if (!mmce_devs[p].present) {
			len += sprintf(page + len, "  Status: Not present\n\n");
			continue;
		}

		len += sprintf(page + len, "  Model:        %s (ID: %d)\n",
			       mmce_devs[p].product_name, mmce_devs[p].product_id);
		len += sprintf(page + len, "  Protocol:     v%d, Revision %d\n",
			       mmce_devs[p].proto_ver, mmce_devs[p].revision);
		len += sprintf(page + len, "  Capacity:     %lu MB (%lu sectors)\n",
			       mmce_devs[p].total_sectors >> 11, mmce_devs[p].total_sectors);
		len += sprintf(page + len, "  Virtual Card: #%d, Channel: #%d\n",
			       mmce_devs[p].current_card, mmce_devs[p].current_channel);
		len += sprintf(page + len, "  I/O Stats:    Read ops: %lu (%lu sectors)\n",
			       mmce_devs[p].read_ops, mmce_devs[p].read_sectors);
		len += sprintf(page + len, "                Write ops: %lu (%lu sectors)\n",
			       mmce_devs[p].write_ops, mmce_devs[p].write_sectors);
		len += sprintf(page + len, "                Errors: %lu\n\n",
			       mmce_devs[p].errors);
	}

	*eof = 1;
	return len;
}

/*
 * Module initialization
 */
int __init ps2mmce_init(void)
{
	int i;

	printk(KERN_INFO "PlayStation 2 MMCE driver v%s (SD2PSX/MemCardPRO2 block driver)\n",
	       MMCE_DRIVER_VERSION);

	for (i = 0; i < MMCE_TOTAL_MINORS; i++) {
		mmce_partitions[i].start_sect = 0;
		mmce_partitions[i].nr_sects = 0;
		mmce_sizes[i] = 0;
		mmce_hardsect_sizes[i] = MMCE_SECTOR_SIZE;
		mmce_blksize_sizes[i] = 1024;
		mmce_max_sectors[i] = MMCE_MAX_TRANSFER_SECTS;
	}

	for (i = 0; i < MMCE_NUM_PORTS; i++) {
		memset(&mmce_devs[i], 0, sizeof(struct mmce_device));
		mmce_devs[i].port = i;
		spin_lock_init(&mmce_devs[i].lock);
		init_MUTEX(&mmce_devs[i].sema);
		atomic_set(&mmce_devs[i].open_count, 0);
	}

	if (register_blkdev(PS2MMCE_MAJOR, "mmce", &mmce_bdops) < 0) {
		printk(KERN_ERR "mmce: Unable to allocate major %d\n", PS2MMCE_MAJOR);
		return -EBUSY;
	}

	read_ahead[PS2MMCE_MAJOR] = 8;
	blk_size[PS2MMCE_MAJOR] = mmce_sizes;
	hardsect_size[PS2MMCE_MAJOR] = mmce_hardsect_sizes;
	blksize_size[PS2MMCE_MAJOR] = mmce_blksize_sizes;
	max_sectors[PS2MMCE_MAJOR] = mmce_max_sectors;

	blk_init_queue(BLK_DEFAULT_QUEUE(PS2MMCE_MAJOR), do_mmce_request);

	add_gendisk(&mmce_gendisk);

	/* Initialize SIF RPC transport */
	mmce_transport_init();

	/* Start kernel processing thread */
	init_MUTEX(&mmce_waitsem);
	init_completion(&mmce_thread_done);
	mmce_thread_stop = 0;
	mmce_task = (struct task_struct *)kernel_thread(mmce_thread, NULL,
							CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	if (mmce_task == NULL) {
		printk(KERN_ERR "mmce: Failed to start mmced daemon\n");
	}

	/* Probe and register devices */
	mmce_probe_ports();

	/* Create diagnostic proc entry */
	create_proc_read_entry("ps2mmce", 0, NULL, mmce_read_proc, NULL);

	return 0;
}

/*
 * Module cleanup
 */
void ps2mmce_exit(void)
{
	remove_proc_entry("ps2mmce", NULL);

	if (mmce_task != NULL) {
		mmce_thread_stop = 1;
		up(&mmce_waitsem);
		wait_for_completion(&mmce_thread_done);
	}

	mmce_transport_exit();

	del_gendisk(&mmce_gendisk);
	blk_cleanup_queue(BLK_DEFAULT_QUEUE(PS2MMCE_MAJOR));

	blk_size[PS2MMCE_MAJOR] = NULL;
	hardsect_size[PS2MMCE_MAJOR] = NULL;
	blksize_size[PS2MMCE_MAJOR] = NULL;
	max_sectors[PS2MMCE_MAJOR] = NULL;
	read_ahead[PS2MMCE_MAJOR] = 0;

	unregister_blkdev(PS2MMCE_MAJOR, "mmce");
	printk(KERN_INFO "mmce: Driver unloaded\n");
}

module_init(ps2mmce_init);
module_exit(ps2mmce_exit);

MODULE_AUTHOR("AKuHAK");
MODULE_DESCRIPTION("PlayStation 2 MMCE Block Device Driver (SD2PSX/MemCardPRO2)");
MODULE_LICENSE("GPL");

