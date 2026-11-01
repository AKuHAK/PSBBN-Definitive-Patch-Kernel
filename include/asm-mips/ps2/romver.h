/*
 *  PlayStation 2 ROM files and MECHACON region parameters
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */
#ifndef __ASM_PS2_ROMVER_H
#define __ASM_PS2_ROMVER_H

/*
 * Read the version file name ("ROMVER", "DVDIDR", ...) of rom0: (rom 0) or
 * rom1: (rom 1) into buf, up to its first control character.  Returns 0,
 * or -ENOENT (no such file) or -ENODEV (no ROM1).
 */
extern int ps2_rom_read_version(int rom, const char *name, char *buf, int len);

/*
 * Region parameters of the MECHACON, read once and kept.  Slim consoles
 * (MECHACON 6.0 and later) only; others get -ENODEV.  Takes the CDVD lock
 * and sleeps.
 */
#define PS2_REGION_PARAMS	13
#define PS2_REGION_OSD		3	/* OSD region letter of rom0:OSDVER */
#define PS2_REGION_OSDLANG	4	/* its default language, "eng" etc. */
#define PS2_REGION_DVD		8	/* DVD player region letter */
extern int ps2_region_params(u8 *params);

#endif /* __ASM_PS2_ROMVER_H */
