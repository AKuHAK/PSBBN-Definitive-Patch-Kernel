#
# Makefile for the MMCE (SD2PSX, MemCard PRO2) SD card filesystem.
#

O_TARGET := mmcefs.o

obj-y := super.o namei.o file.o fxio.o mmcehd.o
obj-m := $(O_TARGET)

include $(TOPDIR)/Rules.make

# mmcehd.c carries the IOP modules as C arrays (the PS2 binutils have no
# .incbin)
IRX_BLOBS := irx/iomanX.irx irx/fileXio.irx irx/mmceman.irx

mmcehd.o: irx_blobs.h

irx_blobs.h: $(IRX_BLOBS) irx2c.sh
	$(CONFIG_SHELL) irx2c.sh $(IRX_BLOBS) > $@.tmp && mv $@.tmp $@
