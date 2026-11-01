/*
 *  linux/drivers/ps2/sysvers.c
 *
 *  ROM versions of /proc/ps2sysvers that PSBBN's boot loader cannot read
 *
 *  The boot loader reads rom0:PS1VER, rom1:DVDID and rom1:DVDVER.  Slim
 *  consoles (SCPH-70000 and later) have one of each per region instead,
 *  named with a region letter: rom0:PS1VERx for the region of
 *  rom0:ROMVER, rom1:DVDIDx and rom1:DVDVERx for the DVD player region,
 *  which the MECHACON keeps with its other region parameters.  The loader
 *  then passes "na" and empty strings; look the files up as the OSD of
 *  those consoles does.
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/ps2/romver.h>

#include "sysvers.h"

void ps2sysvers_fixup(const char **ps1drv_rom, const char **dvd_id,
		      const char **dvd_rom)
{
	static char ps1ver[16], dvdid[2], dvdver[16];
	u8 params[PS2_REGION_PARAMS];
	char name[16], buf[16];
	const char *p;
	int region;

	if (*ps1drv_rom == NULL || **ps1drv_rom == '\0') {
		if (!ps2_rom_read_version(0, "ROMVER", buf, sizeof(buf)) &&
		    strlen(buf) > 4) {
			sprintf(name, "PS1VER%c", buf[4]);
			if (!ps2_rom_read_version(0, name, ps1ver,
						  sizeof(ps1ver)))
				*ps1drv_rom = ps1ver;
		}
	}

	if (*dvd_id != NULL && **dvd_id != '\0' && strcmp(*dvd_id, "na"))
		return;
	if (ps2_region_params(params))
		return;
	region = params[PS2_REGION_DVD];
	if (region < 'A' || region > 'Z')
		return;
	sprintf(name, "DVDID%c", region);
	if (ps2_rom_read_version(1, name, buf, sizeof(buf)))
		return;
	/* "3.10R": the loader passes the letter after the version */
	for (p = buf; *p >= '0' && *p <= '9'; p++)
		;
	if (*p != '.')
		return;
	for (p++; *p >= '0' && *p <= '9'; p++)
		;
	if (*p == '\0')
		return;
	dvdid[0] = *p;
	*dvd_id = dvdid;
	sprintf(name, "DVDVER%c", region);
	if (!ps2_rom_read_version(1, name, dvdver, sizeof(dvdver)))
		*dvd_rom = dvdver;
}
