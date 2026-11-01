/*
 *  PlayStation 2 IOP module loader
 *
 *  This file is subject to the terms and conditions of the GNU General
 *  Public License Version 2. See the file "COPYING" in the main
 *  directory of this archive for more details.
 */
#ifndef __ASM_PS2_LOADFILE_H
#define __ASM_PS2_LOADFILE_H

/*
 * Load and start an IOP module (IRX image) from EE memory, using the
 * LOADFILE RPC service of the IOP.  Returns the IOP module id (> 0) or a
 * negative error (-ENOSYS: the IOP cannot load from memory); *modres
 * receives the module's start result (0: resident, 1: no resident end,
 * i.e. it failed to start, 2: removable).
 */
extern int ps2_load_module_buffer(const void *buf, int size,
				  const char *args, int arglen, int *modres);

/*
 * As above, for an IRX file the IOP opens itself ("mc0:/dir/x.irx",
 * "rom0:NAME").  Servers that cannot load from memory (PSBBN's IOP:
 * ps2_load_module_buffer() returns -ENOSYS) still have this one.
 */
extern int ps2_load_module(const char *path, const char *args, int arglen,
			   int *modres);

#endif /* __ASM_PS2_LOADFILE_H */
