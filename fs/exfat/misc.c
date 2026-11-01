/*
 *  linux/fs/exfat/misc.c
 *
 *  Glue between FatFs and the kernel: disk I/O, memory, time, name
 *  conversion and FatFs path construction.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/blkdev.h>
#include <linux/dcache.h>
#include <linux/errno.h>

#include "exfat_fs.h"
#include "diskio.h"

extern struct timezone sys_tz;

/* FatFs logical drive number -> mounted volume */
static struct exfat_sb_info *exfat_vols[FF_VOLUMES];
static DECLARE_MUTEX(exfat_vols_sem);

int exfat_vol_attach(struct exfat_sb_info *sbi)
{
	int i, err = -EBUSY;

	down(&exfat_vols_sem);
	for (i = 0; i < FF_VOLUMES; i++) {
		if (!exfat_vols[i]) {
			exfat_vols[i] = sbi;
			sbi->vol = i;
			err = 0;
			break;
		}
	}
	up(&exfat_vols_sem);
	return err;
}

void exfat_vol_detach(struct exfat_sb_info *sbi)
{
	down(&exfat_vols_sem);
	if (exfat_vols[sbi->vol] == sbi)
		exfat_vols[sbi->vol] = NULL;
	up(&exfat_vols_sem);
}

/* "N:" - the FatFs path of the volume itself */
void exfat_vol_path(struct exfat_sb_info *sbi, WCHAR *path)
{
	path[0] = '0' + sbi->vol;
	path[1] = ':';
	path[2] = 0;
}

static struct exfat_sb_info *exfat_drv(BYTE pdrv)
{
	return pdrv < FF_VOLUMES ? exfat_vols[pdrv] : NULL;
}

/*
 * FatFs media access interface.  These are only called by FatFs, i.e. with
 * sbi->lock held, and transfer 512-byte sectors through the buffer cache.
 */

#define EXFAT_IO_BATCH	16	/* sectors submitted to the block layer at once */

DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv);
}

DSTATUS disk_status(BYTE pdrv)
{
	struct exfat_sb_info *sbi = exfat_drv(pdrv);

	if (!sbi)
		return STA_NOINIT;
	if ((sbi->sb->s_flags & MS_RDONLY) || is_read_only(sbi->dev))
		return STA_PROTECT;
	return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	struct exfat_sb_info *sbi = exfat_drv(pdrv);
	struct buffer_head *bhs[EXFAT_IO_BATCH];
	DRESULT res = RES_OK;
	UINT i, n;

	if (!sbi)
		return RES_NOTRDY;
	while (count && res == RES_OK) {
		n = count < EXFAT_IO_BATCH ? count : EXFAT_IO_BATCH;
		for (i = 0; i < n; i++)
			bhs[i] = getblk(sbi->dev, sector + i, EXFAT_SECTOR_SIZE);
		ll_rw_block(READ, n, bhs);
		for (i = 0; i < n; i++) {
			wait_on_buffer(bhs[i]);
			if (buffer_uptodate(bhs[i]))
				memcpy(buff + (i << EXFAT_SECTOR_BITS),
				       bhs[i]->b_data, EXFAT_SECTOR_SIZE);
			else
				res = RES_ERROR;
			brelse(bhs[i]);
		}
		buff += n << EXFAT_SECTOR_BITS;
		sector += n;
		count -= n;
	}
	return res;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	struct exfat_sb_info *sbi = exfat_drv(pdrv);
	struct buffer_head *bhs[EXFAT_IO_BATCH];
	DRESULT res = RES_OK;
	UINT i, n;
	int sync;

	if (!sbi)
		return RES_NOTRDY;
	sync = (sbi->sb->s_flags & MS_SYNCHRONOUS) != 0;
	while (count && res == RES_OK) {
		n = count < EXFAT_IO_BATCH ? count : EXFAT_IO_BATCH;
		for (i = 0; i < n; i++) {
			bhs[i] = getblk(sbi->dev, sector + i, EXFAT_SECTOR_SIZE);
			/* do not race with a read of the old contents */
			wait_on_buffer(bhs[i]);
			memcpy(bhs[i]->b_data, buff + (i << EXFAT_SECTOR_BITS),
			       EXFAT_SECTOR_SIZE);
			mark_buffer_uptodate(bhs[i], 1);
			mark_buffer_dirty(bhs[i]);
		}
		if (sync) {
			ll_rw_block(WRITE, n, bhs);
			for (i = 0; i < n; i++) {
				wait_on_buffer(bhs[i]);
				if (!buffer_uptodate(bhs[i]))
					res = RES_ERROR;
			}
		}
		for (i = 0; i < n; i++)
			brelse(bhs[i]);
		buff += n << EXFAT_SECTOR_BITS;
		sector += n;
		count -= n;
	}
	return res;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	struct exfat_sb_info *sbi = exfat_drv(pdrv);

	if (!sbi)
		return RES_NOTRDY;
	switch (cmd) {
	case CTRL_SYNC:
		/*
		 * Dirty sectors are written back by bdflush, by sync(2) and
		 * by fsync(2); only "-o sync" mounts wait here.  This only
		 * touches the buffer cache, so it is safe under sbi->lock.
		 */
		if (sbi->sb->s_flags & MS_SYNCHRONOUS)
			return fsync_no_super(sbi->dev) ? RES_ERROR : RES_OK;
		return RES_OK;
	case GET_SECTOR_SIZE:
		*(WORD *)buff = EXFAT_SECTOR_SIZE;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *)buff = 1;
		return RES_OK;
	}
	return RES_PARERR;
}

/* Current local time in FAT format, for timestamps written by FatFs */
DWORD get_fattime(void)
{
	WORD fdate, ftime;

	exfat_time_unix2fat(CURRENT_TIME, &fdate, &ftime);
	return ((DWORD)fdate << 16) | ftime;
}

/* LFN working buffers; FatFs calls these with sbi->lock held */
void *ff_memalloc(UINT msize)
{
	return kmalloc(msize, GFP_NOFS);
}

void ff_memfree(void *mblock)
{
	kfree(mblock);
}

int exfat_errno(FRESULT res)
{
	switch (res) {
	case FR_OK:
		return 0;
	case FR_NO_FILE:
	case FR_NO_PATH:
		return -ENOENT;
	case FR_INVALID_NAME:
	case FR_INVALID_PARAMETER:
	case FR_NO_FILESYSTEM:
		return -EINVAL;
	case FR_DENIED:
		return -EACCES;
	case FR_EXIST:
		return -EEXIST;
	case FR_WRITE_PROTECTED:
		return -EROFS;
	case FR_NOT_ENOUGH_CORE:
		return -ENOMEM;
	case FR_TOO_MANY_OPEN_FILES:
		return -EMFILE;
	case FR_LOCKED:
	case FR_TIMEOUT:
		return -EBUSY;
	default:
		return -EIO;
	}
}

/*
 * Time conversion.  FAT timestamps are local time with 2 second resolution
 * covering 1980..2107; sys_tz is the kernel's idea of the local zone, as
 * in the FAT driver.  Calendar arithmetic follows H. Hinnant's
 * days_from_civil/civil_from_days algorithms (proleptic Gregorian).
 */

static long exfat_days_from_civil(int y, int m, int d)
{
	long era, yoe, doy, doe;

	y -= m <= 2;
	era = y / 400;
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void exfat_civil_from_days(long z, int *y, int *m, int *d)
{
	long era, doe, yoe, doy, mp;

	z += 719468;
	era = z / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = yoe + era * 400 + (*m <= 2);
}

time_t exfat_time_fat2unix(WORD fdate, WORD ftime)
{
	int year = 1980 + (fdate >> 9);
	int month = (fdate >> 5) & 15;
	int day = fdate & 31;
	long t;

	if (month < 1 || month > 12)
		month = 1;
	if (day < 1)
		day = 1;
	if (year >= 2038)	/* beyond a 32-bit time_t */
		return 0x7fffffff;
	t = exfat_days_from_civil(year, month, day) * 86400 +
		(ftime >> 11) * 3600 + ((ftime >> 5) & 63) * 60 +
		(ftime & 31) * 2;
	return t + sys_tz.tz_minuteswest * 60;
}

void exfat_time_unix2fat(time_t t, WORD *fdate, WORD *ftime)
{
	long lt = t - sys_tz.tz_minuteswest * 60;
	long secs;
	int y, m, d;

	if (lt < 315532800)		/* 1980-01-01 00:00:00 */
		lt = 315532800;
	exfat_civil_from_days(lt / 86400, &y, &m, &d);
	secs = lt % 86400;
	*fdate = ((y - 1980) << 9) | (m << 5) | d;
	*ftime = ((secs / 3600) << 11) | (((secs / 60) % 60) << 5) |
		((secs % 60) / 2);
}

/*
 * Character set handling.  Names are UTF-8 unless the iocharset option
 * selected an NLS table.  FatFs itself works in UTF-16.
 */

static int exfat_utf8_decode(const unsigned char *s, int len, DWORD *pc)
{
	DWORD c = s[0];
	int n, i;

	if (c < 0x80) {
		*pc = c;
		return 1;
	}
	if ((c & 0xe0) == 0xc0) {
		n = 2;
		c &= 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		n = 3;
		c &= 0x0f;
	} else if ((c & 0xf8) == 0xf0) {
		n = 4;
		c &= 0x07;
	} else
		return -1;
	if (len < n)
		return -1;
	for (i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return -1;
		c = (c << 6) | (s[i] & 0x3f);
	}
	/* no overlong forms, UTF-16 surrogates or values beyond Unicode */
	if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) ||
	    (n == 4 && c < 0x10000) || c > 0x10ffff ||
	    (c >= 0xd800 && c <= 0xdfff))
		return -1;
	*pc = c;
	return n;
}

static int exfat_utf8_encode(DWORD c, unsigned char *s, int max)
{
	if (c < 0x80) {
		if (max < 1)
			return -1;
		s[0] = c;
		return 1;
	}
	if (c < 0x800) {
		if (max < 2)
			return -1;
		s[0] = 0xc0 | (c >> 6);
		s[1] = 0x80 | (c & 0x3f);
		return 2;
	}
	if (c < 0x10000) {
		if (max < 3)
			return -1;
		s[0] = 0xe0 | (c >> 12);
		s[1] = 0x80 | ((c >> 6) & 0x3f);
		s[2] = 0x80 | (c & 0x3f);
		return 3;
	}
	if (max < 4)
		return -1;
	s[0] = 0xf0 | (c >> 18);
	s[1] = 0x80 | ((c >> 12) & 0x3f);
	s[2] = 0x80 | ((c >> 6) & 0x3f);
	s[3] = 0x80 | (c & 0x3f);
	return 4;
}

/*
 * Decode one character of a name in the iocharset.  Returns the number of
 * bytes used, or a negative value for an invalid byte sequence.
 */
int exfat_next_char(struct exfat_sb_info *sbi, const unsigned char *name,
		    int len, DWORD *c)
{
	wchar_t wc;
	int n;

	if (!sbi->nls)
		return exfat_utf8_decode(name, len, c);
	n = sbi->nls->char2uni(name, len, &wc);
	if (n <= 0 || (wc >= 0xd800 && wc <= 0xdfff))
		return -1;
	*c = wc;
	return n;
}

/*
 * Convert a name to UTF-16.  Returns the number of units stored (without
 * terminator), -EINVAL for characters exFAT cannot store, or -ENAMETOOLONG.
 * Control characters and '\' must never reach FatFs: it would take them as
 * the end of the path or as a separator and act on a different object.
 */
int exfat_name_to_uni(struct exfat_sb_info *sbi, const unsigned char *name,
		      int len, WCHAR *uni, int max)
{
	DWORD c;
	int n = 0, l;

	while (len > 0) {
		l = exfat_next_char(sbi, name, len, &c);
		if (l <= 0)
			return -EINVAL;
		name += l;
		len -= l;
		if (c < 0x20 || c == 0x7f ||
		    (c < 0x80 && strchr("\"*/:<>?\\|", (int)c)))
			return -EINVAL;
		if (c >= 0x10000) {
			if (n + 2 > max)
				return -ENAMETOOLONG;
			c -= 0x10000;
			uni[n++] = 0xd800 | (c >> 10);
			uni[n++] = 0xdc00 | (c & 0x3ff);
		} else {
			if (n + 1 > max)
				return -ENAMETOOLONG;
			uni[n++] = c;
		}
	}
	return n;
}

/*
 * Convert a terminated UTF-16 name from FatFs to the iocharset.  Returns
 * the length; characters the iocharset cannot represent become '?'.
 */
int exfat_uni_to_name(struct exfat_sb_info *sbi, const WCHAR *uni,
		      unsigned char *name, int max)
{
	DWORD c;
	int n = 0, l;

	while (*uni) {
		c = *uni++;
		if (c >= 0xd800 && c <= 0xdbff && *uni >= 0xdc00 &&
		    *uni <= 0xdfff)
			c = 0x10000 + ((c - 0xd800) << 10) + (*uni++ - 0xdc00);
		if (sbi->nls)
			l = c < 0x10000 ?
				sbi->nls->uni2char(c, name + n, max - n) : -1;
		else
			l = (c >= 0xd800 && c <= 0xdfff) ? -1 :
				exfat_utf8_encode(c, name + n, max - n);
		if (l <= 0) {
			if (n >= max)
				break;
			name[n] = '?';
			l = 1;
		}
		n += l;
	}
	return n;
}

/*
 * Build the FatFs path "N:/dir/.../name" of @dentry, which may be negative.
 * Returns a kmalloc'ed string or an ERR_PTR.  Must be called without
 * sbi->lock held: the buffer is allocated with GFP_KERNEL.
 */
WCHAR *exfat_build_path(struct dentry *dentry)
{
	struct exfat_sb_info *sbi = EXFAT_SB(dentry->d_sb);
	struct dentry *d;
	WCHAR *buf, *p, *tmp;
	int n, err = 0;

	buf = kmalloc(EXFAT_PATH_MAX * sizeof(WCHAR), GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);
	p = buf + EXFAT_PATH_MAX;
	*--p = 0;
	spin_lock(&dcache_lock);
	for (d = dentry; !IS_ROOT(d); d = d->d_parent) {
		/* convert in front of p, then slide the name up against p */
		tmp = p - (EXFAT_NAME_MAX + 1);
		if (tmp < buf + 3) {
			err = -ENAMETOOLONG;
			break;
		}
		n = exfat_name_to_uni(sbi, d->d_name.name, d->d_name.len,
				      tmp, EXFAT_NAME_MAX);
		if (n <= 0) {
			err = n ? n : -EINVAL;
			break;
		}
		memmove(p - n, tmp, n * sizeof(WCHAR));
		p -= n;
		*--p = '/';
	}
	spin_unlock(&dcache_lock);
	if (err) {
		kfree(buf);
		return ERR_PTR(err);
	}
	if (!*p)
		*--p = '/';		/* the root directory */
	*--p = ':';
	*--p = '0' + sbi->vol;
	memmove(buf, p, (buf + EXFAT_PATH_MAX - p) * sizeof(WCHAR));
	return buf;
}

void exfat_put_path(WCHAR *path)
{
	kfree(path);
}
