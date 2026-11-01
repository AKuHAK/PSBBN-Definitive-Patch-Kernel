#
# Makefile for the Linux exFAT filesystem routines.
#
# ff.c, ff.h, ffunicode.c and diskio.h are FatFs R0.16 (with the upstream
# patches 1 and 2) by ChaN; see README for the local changes.
#

O_TARGET := exfat.o

obj-y := super.o namei.o file.o misc.o ff.o ffunicode.o
obj-m := $(O_TARGET)

include $(TOPDIR)/Rules.make
