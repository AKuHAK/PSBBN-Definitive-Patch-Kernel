/*
 *  PlayStation 2 Multipurpose Memory Card Emulator (MMCE) driver
 *  (SD2PSX, MemCard PRO2, PicoMemcard+, etc.)
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */
#ifndef _PS2MMCEIO_H_
#define _PS2MMCEIO_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define MMCE_NAME_MAX		31
#define MMCE_GAMEID_MAX		256

/* Product IDs returned by MMCE hardware */
#define MMCE_PROD_UNKNOWN	0
#define MMCE_PROD_SD2PSX	1
#define MMCE_PROD_MEMCARDPRO2	2
#define MMCE_PROD_PICOMEMCARD_P	3
#define MMCE_PROD_PICOMEMCARD_Z	4

/* Hardware protocol constants */
#define MMCE_ID			0x8B
#define MMCE_RESERVED		0xFF
#define MMCE_REPLY_CONST	0xAA

/* Hardware command opcodes */
#define MMCE_CMD_PING		0x01
#define MMCE_CMD_GET_STATUS	0x02
#define MMCE_CMD_GET_CARD	0x03
#define MMCE_CMD_SET_CARD	0x04
#define MMCE_CMD_GET_CHANNEL	0x05
#define MMCE_CMD_SET_CHANNEL	0x06
#define MMCE_CMD_GET_GAMEID	0x07
#define MMCE_CMD_SET_GAMEID	0x08
#define MMCE_CMD_RESET		0x09
#define MMCE_CMD_SET_CARD_CHAN	0x0A

#define MMCE_CMD_FS_OPEN	0x40
#define MMCE_CMD_FS_CLOSE	0x41
#define MMCE_CMD_FS_READ	0x42
#define MMCE_CMD_FS_WRITE	0x43
#define MMCE_CMD_FS_LSEEK	0x44
#define MMCE_CMD_FS_REMOVE	0x46
#define MMCE_CMD_FS_MKDIR	0x47
#define MMCE_CMD_FS_RMDIR	0x48
#define MMCE_CMD_FS_DOPEN	0x49
#define MMCE_CMD_FS_DCLOSE	0x4A
#define MMCE_CMD_FS_DREAD	0x4B
#define MMCE_CMD_FS_GETSTAT	0x4C
#define MMCE_CMD_FS_LSEEK64	0x53
#define MMCE_CMD_FS_READ_SECTOR	0x58

/* Card switching modes */
#define MMCE_MODE_NUM		0x0
#define MMCE_MODE_NEXT		0x1
#define MMCE_MODE_PREV		0x2

/* Card types */
#define MMCE_CARD_REGULAR	0x0
#define MMCE_CARD_BOOT		0x1

/* SIF RPC Binding ID for MMCE IOP service */
#define MMCE_BIND_RPC_ID	0x0815e200

/* RPC function codes */
#define MMCE_RPC_PING		1
#define MMCE_RPC_READ_SECTORS	2
#define MMCE_RPC_WRITE_SECTORS	3
#define MMCE_RPC_GET_INFO	4
#define MMCE_RPC_GET_STATUS	5
#define MMCE_RPC_SET_CARD	6
#define MMCE_RPC_GET_CARD	7
#define MMCE_RPC_SET_CHANNEL	8
#define MMCE_RPC_GET_CHANNEL	9
#define MMCE_RPC_SET_GAMEID	10
#define MMCE_RPC_RESET		11

/* Status flags */
#define MMCE_STAT_BUSY		(1 << 0)
#define MMCE_STAT_CARD_CHANGED	(1 << 1)
#define MMCE_STAT_WRITE_PROTECT	(1 << 2)

/* Userspace structures */
struct ps2mmce_cardinfo {
	int port;			/* 0 = Slot 1, 1 = Slot 2 */
	int present;			/* 1 if card detected */
	int product_id;			/* MMCE_PROD_* */
	char product_name[32];		/* "SD2PSX", "MemCard PRO2", etc. */
	int proto_ver;			/* MMCE Protocol version */
	int revision;			/* Hardware revision */
	unsigned long total_sectors;	/* Total 512-byte sectors */
	unsigned int sector_size;	/* Sector size in bytes (512) */
	int current_card;		/* Active virtual memory card */
	int current_channel;		/* Active channel */
	int status;			/* MMCE_STAT_* */
};

struct ps2mmce_card_req {
	int port;
	int type;			/* MMCE_CARD_REGULAR / MMCE_CARD_BOOT */
	int mode;			/* MMCE_MODE_NUM / NEXT / PREV */
	int card_num;			/* Card number */
	int channel_num;		/* Channel number */
};

struct ps2mmce_gameid_req {
	int port;
	char gameid[MMCE_GAMEID_MAX];
};

/* Ioctl definitions */
#define PS2MMCE_IOCTL			'M'
#define PS2MMCE_IOCGETINFO		_IOR(PS2MMCE_IOCTL, 1, struct ps2mmce_cardinfo)
#define PS2MMCE_IOCPING			_IOR(PS2MMCE_IOCTL, 2, int)
#define PS2MMCE_IOCGETSTATUS		_IOR(PS2MMCE_IOCTL, 3, int)
#define PS2MMCE_IOCGETCARD		_IOR(PS2MMCE_IOCTL, 4, struct ps2mmce_card_req)
#define PS2MMCE_IOCSETCARD		_IOW(PS2MMCE_IOCTL, 5, struct ps2mmce_card_req)
#define PS2MMCE_IOCGETCHANNEL		_IOR(PS2MMCE_IOCTL, 6, struct ps2mmce_card_req)
#define PS2MMCE_IOCSETCHANNEL		_IOW(PS2MMCE_IOCTL, 7, struct ps2mmce_card_req)
#define PS2MMCE_IOCSETGAMEID		_IOW(PS2MMCE_IOCTL, 8, struct ps2mmce_gameid_req)
#define PS2MMCE_IOCGETGAMEID		_IOR(PS2MMCE_IOCTL, 9, struct ps2mmce_gameid_req)
#define PS2MMCE_IOCRESET		_IOW(PS2MMCE_IOCTL, 10, int)

#endif /* _PS2MMCEIO_H_ */
