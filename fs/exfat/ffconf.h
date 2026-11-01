/*---------------------------------------------------------------------------/
/  Configurations of FatFs Module for the Linux 2.4 exFAT driver
/
/  Based on ffconf.h of FatFs R0.16. Only the options used by fs/exfat are
/  documented here; see the FatFs documentation (elm-chan.org/fsw/ff/) for
/  the meaning of each option.
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80386	/* Revision ID (must match FatFs R0.16) */

/*---------------------------------------------------------------------------/
/ Integer types (Linux kernel port)
/
/ ff.h only knows how to pick the integer types from <stdint.h> on C99
/ compilers.  gcc 2.95 is a C89 compiler and the kernel cannot include libc
/ headers, so the types are defined here and ff.h is told to use them.
/---------------------------------------------------------------------------*/

#define FF_INTDEF	2
typedef unsigned int		UINT;	/* int must be 16-bit or 32-bit */
typedef unsigned char		BYTE;	/* char must be 8-bit */
typedef unsigned short		WORD;	/* 16-bit unsigned */
typedef unsigned int		DWORD;	/* 32-bit unsigned */
typedef unsigned long long	QWORD;	/* 64-bit unsigned */
typedef WORD			WCHAR;	/* UTF-16 code unit */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0	/* Read/write */
#define FF_FS_MINIMIZE	0	/* Full API */
#define FF_USE_FIND	0
#define FF_USE_MKFS	0	/* Formatting is left to mkfs.exfat */
#define FF_USE_FASTSEEK	1	/* Cluster link map for fast backward seeks */
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	1	/* f_chmod() and f_utime() */
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437	/* Only used for FAT short names; exFAT has none */
#define FF_USE_LFN	3	/* LFN working buffers come from ff_memalloc() */
#define FF_MAX_LFN	255
#define FF_LFN_UNICODE	1	/* UTF-16 API; the driver converts to iocharset */
#define FF_LFN_BUF	255	/* Holds any exFAT name (max 255 UTF-16 units) */
#define FF_SFN_BUF	12
#define FF_FS_RPATH	0	/* The driver always passes absolute paths */
#define FF_PATH_DEPTH	10

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES	10	/* Max number of exFAT volumes mounted at once */
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS	"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS	512	/* Only 512-byte sector media is supported */
#define FF_MAX_SS	512
#define FF_LBA64	0	/* 2^32 sectors (2 TiB), the 2.4 block layer limit */
#define FF_MIN_GPT	0x10000000
#define FF_USE_TRIM	0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY	0	/* Each open file has its own sector buffer */
#define FF_FS_EXFAT	1
#define FF_FS_NORTC	0	/* get_fattime() is provided by the driver */
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2025
#define FF_FS_CRTIME	0
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK	0	/* Open file sharing is managed by the driver */
#define FF_FS_REENTRANT	0	/* The driver serialises access per volume */
#define FF_FS_TIMEOUT	1000

/*--- End of configuration options ---*/
