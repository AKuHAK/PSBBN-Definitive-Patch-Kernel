/*
 * ps2fs/misc.c
 *
 * Copyright (c) 2002 Andrew Church <achurch@achurch.org>
 *
 * Error routines shamelessly borrowed from ext2/super.c
 */

#include <linux/config.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/time.h>

#include "ps2fs_fs.h"

/*************************************************************************/
/*************************************************************************/

static char error_buf[1024];

/*************************************************************************/

/* Report a warning on the given filesystem. */

void ps2fs_warning(struct super_block *sb, const char *function,
		   const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsprintf(error_buf, fmt, args);
    va_end(args);
    printk(KERN_WARNING "ps2fs WARNING (device %s): %s: %s\n",
	   bdevname(sb->s_dev), function, error_buf);
}

/*************************************************************************/

/* Report an error on the given filesystem. */

void ps2fs_error(struct super_block *sb, const char *function,
		 const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsprintf(error_buf, fmt, args);
    va_end(args);
    printk(KERN_ERR "ps2fs ERROR (device %s): %s: %s\n",
	   bdevname(sb->s_dev), function, error_buf);
}

/*************************************************************************/

/* Times on disk are local time of the console clock, which by PS2
 * convention runs on Japan Standard Time; tzoffset is in seconds east of
 * UTC. */

time_t from_ps2time(const struct ps2fs_datetime *dt, __s32 tzoffset)
{
    unsigned int year = le16_to_cpu(dt->year);

    if (year < 1970 || year > 2105 || dt->month < 1 || dt->month > 12
	|| dt->day < 1 || dt->day > 31)
	return 0;
    return mktime(year, dt->month, dt->day, dt->hour, dt->min, dt->sec)
	- tzoffset;
}

/*************************************************************************/

void to_ps2time(struct ps2fs_datetime *dt, time_t time, __s32 tzoffset)
{
    unsigned long t, days, era, doe, yoe, doy, mp, year, month;

    t = time + tzoffset > 0 ? time + tzoffset : 0;
    days = t / 86400;
    t %= 86400;
    /* civil date from day number (days since 0000-03-01 shifted) */
    days += 719468;
    era = days / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = yoe + era * 400 + (month <= 2);
    dt->unused = 0;
    dt->sec = t % 60;
    dt->min = t / 60 % 60;
    dt->hour = t / 3600;
    dt->day = doy - (153 * mp + 2) / 5 + 1;
    dt->month = month;
    dt->year = cpu_to_le16(year);
}

/*************************************************************************/
/*************************************************************************/

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)

/* simple_strtol() for kernel 2.2.  Strangely enough, this is declared in
 * <linux/kernel.h> but isn't exported (at least in kernel 2.2.1), so we
 * have to define it ourselves.
 */

long simple_strtol(const char *str, char **ret, unsigned int base)
{
    int negative;
    unsigned long val;

    if (str && *str == '-') {
	negative = 1;
	str++;
    } else {
	negative = 0;
    }
    val = simple_strtoul(str, ret, base);
    if (val > 0x7FFFFFFF) {
	if (negative)
	    return -0x80000000;
	else
	    return 0x7FFFFFFF;
    } else {
	if (negative)
	    return -val;
	else
	    return val;
    }
}

#endif  /* kernel 2.2 */

/*************************************************************************/
