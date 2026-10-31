/*
 *  PlayStation 2 Multipurpose Memory Card Emulator (MMCE) driver
 *  Internal header
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */
#ifndef _DRIVERS_PS2_MMCE_H_
#define _DRIVERS_PS2_MMCE_H_

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <asm/smplock.h>
#include <asm/io.h>
#include <asm/ps2/sifdefs.h>
#include <asm/ps2/sbcall.h>
#include <linux/ps2/mmceio.h>

#define MMCE_NUM_PORTS		2
#define MMCE_PARTS_PER_PORT	16
#define MMCE_TOTAL_MINORS	(MMCE_NUM_PORTS * MMCE_PARTS_PER_PORT)

#define MMCE_SECTOR_SIZE	512
#define MMCE_DEFAULT_SECTORS	(4UL * 1024 * 1024 * 2) /* 4GB in 512b sectors fallback */

/* RPC buffer structures for communication with IOP module */
struct mmce_rpc_rw_arg {
	u32 port;
	u32 sector;
	u32 count;
	u32 status;
};

struct mmce_rpc_info_arg {
	u32 port;
	u32 present;
	u32 product_id;
	u32 proto_ver;
	u32 revision;
	u32 total_sectors;
	u32 sector_size;
	u32 status;
};

struct mmce_rpc_card_arg {
	u32 port;
	u32 type;
	u32 mode;
	u32 card;
	u32 channel;
	u32 status;
};

/* Device state representation */
struct mmce_device {
	int port;
	int present;
	int product_id;
	char product_name[32];
	int proto_ver;
	int revision;
	unsigned long total_sectors;
	unsigned int sector_size;
	int current_card;
	int current_channel;
	int media_changed;
	int generation;
	atomic_t open_count;
	spinlock_t lock;
	struct semaphore sema;

	/* Statistics */
	unsigned long read_ops;
	unsigned long write_ops;
	unsigned long read_sectors;
	unsigned long write_sectors;
	unsigned long errors;
};

/* Prototypes */
int mmce_hw_ping(int port, int *prod_id, int *proto_ver, int *revision);
int mmce_hw_get_status(int port, int *status);
int mmce_hw_read_sectors(int port, unsigned long sector, unsigned int count, void *buf);
int mmce_hw_write_sectors(int port, unsigned long sector, unsigned int count, const void *buf);
int mmce_hw_get_card(int port, int *card_num);
int mmce_hw_set_card(int port, int type, int mode, int card_num);
int mmce_hw_get_channel(int port, int *chan_num);
int mmce_hw_set_channel(int port, int mode, int chan_num);
int mmce_hw_set_gameid(int port, const char *gameid);
int mmce_hw_reset(int port);

int mmce_transport_init(void);
void mmce_transport_exit(void);

#endif /* _DRIVERS_PS2_MMCE_H_ */
