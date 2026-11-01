/*
 *  linux/drivers/ps2/sysvers.h
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */

/*
 * Replace the PS1 driver and DVD player versions that the boot loader
 * could not read (empty, or "na" for the DVD region) with the region's
 * files of a slim console's ROMs.  The strings stay valid until unload.
 */
extern void ps2sysvers_fixup(const char **ps1drv_rom, const char **dvd_id,
			     const char **dvd_rom);
