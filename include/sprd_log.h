/*
 * Copyright (C) 2013 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __SPRD_LOG_H__
#define __SPRD_LOG_H__
#ifdef CONFIG_LOG_2_SD
#include "../common/loader/sysdump.h"
#endif

#define LOG_BUFFER_START_ADDR   ALIGN(LOG_BUFFER_ADDR + sizeof(LOG_BUFFER), 8)
#define LOG_BUFFER_SIZE         0x040000 /* 256KB */

#define LOG_FOLDER_NUM		3
#define LOG_FOLDER_NAME		"ylog/ap/uboot"
#define LOG_AUTO_TEST		"ylog/uboot/uboot_log_auto_test.txt"

#define UBOOT_LOG_PARTITION	"uboot_log"
#define LOG_HEAD_MAGIC	        0x0000abcd
#define LOG_VERSION		        1
#define LOG_HEADER_SIZE	        512
#define LOG_BODY_NUM_MAX	12
/* log body num for panic */
#define LOG_PANIC_BODY_NUM	7
/* log body num for cboot */
#define LOG_CBOOT_BODY_NUM	4
#define LOG_FASTBOOT_BODY_NUM	2
#define LOG_DOWNLD_BODY_NUM	2

#define LOG_BODY_SIZE		LOG_BUFFER_SIZE		/* 256KB */

#define SUCESS		1
#define FAILED		0

#define LAST_LOG_PARTITION_OFFSET		(0)

/*
 * Where sprd_log_flush() writes.
 *
 * Defaults to the "last log" slot at offset 0, which is also where LK writes
 * its own log body at the end of every normal boot. On a chainloaded U-Boot
 * that is fatal to debugging: the boot that follows a failed attempt destroys
 * our log before it can be read, so an absent banner proves nothing. Boards
 * that are chainloaded should point this at a slot LK leaves alone.
 */
#ifndef SPRD_UBOOT_LOG_OFFSET
#define SPRD_UBOOT_LOG_OFFSET			LAST_LOG_PARTITION_OFFSET
#endif

/*
 * Boot-stage markers in DRAM, for a board with no UART pad.
 *
 * Hardware-established: DRAM contents survive a software reboot (an Android
 * "Restart" left 0x5a5a1234 intact at four separate addresses) but NOT the
 * force-reboot key combo or a power cycle, both of which leave 0xffffffff.
 * So this channel is readable after a watchdog/panic reset, which is how a
 * failed chainload ends, but not after a manual force-reboot.
 *
 * 0xb1000000 is unreserved in the board DTS -- between logobuffer (ends
 * 0xb09e4000) and pre_log_buffer (0xb4000000) -- and is confirmed writable
 * from LK's hook point. It is execute-never, which does not matter here.
 */
#define SPRD_BOOT_MARK_ADDR			0xb1000000
/* Deliberately NOT 0x5a5a1234: that is what the inline dramprobe test writes
 * to this same address, and it is also tuboot_s in arch/arm/lib/board.c. A
 * stale probe value must not be readable as a U-Boot stage marker. */
#define SPRD_BOOT_MARK_MAGIC			0xb007b007
/*
 * Stage numbers must increase along the execution path: the marker is a plain
 * store, so the value left behind is simply the last one written, and a lower
 * number later would read as going backwards. start.S owns 0x10-0x1f, the C
 * path 0x20+.
 */
/* Framebuffer progress bands, matching scripts/chainload_slotb_ums9620.py.
 * Bands 0-1 belong to the trampoline, 2-5 to start.S, 6+ to the C stages. */
#define SPRD_FB_BAND_BASE			0xb0100000
/* 0x4380 = 17280 = 720*24 bytes: a whole number of rows at BOTH 24bpp (8
 * rows) and 32bpp (6 rows). 0x8000 was neither, so every band started at a
 * different x and they rendered as a staggered diagonal instead of bars.
 * The panel is in fact 32bpp, so these are 6-row bands starting at row 364;
 * about 59 fit. See the depth note in common/sprd_log.c. */
#define SPRD_FB_BAND_SIZE			0x4380

#define SPRD_MARK_RESET				0x10	/* first instruction at the load base */
#define SPRD_MARK_VBAR				0x11	/* switch_el done, vectors installed */
#define SPRD_MARK_ERRATA			0x12	/* apply_core_errata returned */
#define SPRD_MARK_LOWLEVEL			0x13	/* lowlevel_init returned */
#define SPRD_MARK_MAIN				0x20	/* _main, before board_init_f */
#define SPRD_MARK_BOARD_INIT_F			0x21
#define SPRD_MARK_BOARD_INIT_R			0x22
#define SPRD_MARK_MAIN_LOOP			0x23	/* init done, UFS up */
/*
 * 0x30 + N = about to call init_sequence[N] in board_init_f. That table is
 * where a chainloaded U-Boot currently stops, and it is entirely silent:
 * serial_init and console_init_f are themselves entries in it, so nothing
 * before them can print. Index order is the table in arch/arm/lib/board.c.
 */
#define SPRD_MARK_INIT_FN			0x30
/*
 * First band of the FDT verdict block (see sprd_fdt_verdict).
 *
 * Band 8 is free: bands 8 and 9 belong to board_init_r and main_loop, which a
 * payload dying in board_init_f never reaches, so nothing else paints them.
 *
 * (An earlier revision claimed band 14 was off screen and that the "~29 bands
 * fit" estimate was wrong. Both claims were mistaken -- the estimate is right,
 * bands 0..28 are all visible, and band 14 simply was not being painted yet
 * because the payload died before reaching it.)
 */
#define SPRD_FB_VERDICT_BAND			8
/*
 * First band of the per-init_sequence-entry markers, and the highest offset
 * from it that stays on screen. ~29 bands are visible (see sprd_boot_mark),
 * so 12 + 16 = 28 is the last one that can be read.
 */
#define SPRD_FB_INITFN_BAND			12
#define SPRD_FB_INITFN_MAX			16
/*
 * 0xd00dfeed as it sits in memory, read back as a little-endian word. Avoids
 * pulling in the fdt byte-swap helpers this early.
 */
#define SPRD_FDT_MAGIC_LE			0xedfe0dd0
/* log position for panic (0x40000) */
#define PANIC_LOG_PARTITION_OFFSET		(LAST_LOG_PARTITION_OFFSET + LOG_BUFFER_SIZE)
/* log position for cboot (0x200000) */
#define START_LOG_PARTITION_OFFSET		(PANIC_LOG_PARTITION_OFFSET + LOG_BUFFER_SIZE * LOG_PANIC_BODY_NUM)
/* log position for fastboot (0x300000) */
#define FASTBOOT_LOG_PARTITION_OFFSET		(START_LOG_PARTITION_OFFSET + LOG_BUFFER_SIZE * LOG_CBOOT_BODY_NUM)
/* log position for download (0x380000) */
#define DOWNLD_LOG_PARTITION_OFFSET		(FASTBOOT_LOG_PARTITION_OFFSET + LOG_BUFFER_SIZE * 2)

typedef enum {
	LR_NORMAL,
	LR_ABNORMAL,
	LR_LONG_PRESS,
	LR_UNKNOWN,
} LOG_REBOOT_TYPE_T;

typedef enum {
	PANIC_LOG_TYPE = 0,
	START_LOG_TYPE,
	FASTBOOT_LOG_TYPE,
	DOWNLD_LOG_TYPE,
	MAX_LOG_TYPE
} LOG_TYPE_T;

typedef struct _LOG_BUFFER {
	uint64_t magic;		/* 53 50 52 44 75 6c 6f 67 */
	uchar* addr;
	uchar* pointer;
	uint32_t size;
	uint32_t used;
	uint32_t spare;
	uint32_t status;
	uchar* log2pc_start;
} LOG_BUFFER;

typedef struct _LOG_BODY {
	uint32_t p_offset;	/* offset in uboot log partiton */
	uint32_t b_offset;	/* offset in log body, it also indicates the log size in emmc */
	uint32_t size;
} LOG_BODY;

typedef struct _LOG_PARTITION_HEADER {
	uint32_t magic;
	uint32_t len;
	uint32_t boot_count;
	uint32_t body_num;
	uint32_t type;
	uint32_t next_body;
	LOG_BODY body[LOG_BODY_NUM_MAX];
} LOG_PARTITION_HEADER;

typedef struct _LOG_STRUCT {
	int32_t flag;
	uint64_t log_type_p_offset[MAX_LOG_TYPE];
	LOG_PARTITION_HEADER log_par_hdr[MAX_LOG_TYPE];
} LOG_STRUCT;

unsigned long get_uboot_log_addr(void);
uint32_t get_uboot_log_len(void);
/* Flush the captured console buffer to the uboot_log partition. */
void sprd_log_flush(void);
/* Record that this boot stage was reached, in DRAM that survives a reset. */
void sprd_boot_mark(uint32_t stage);
void sprd_fdt_verdict(const void *blob, const void *expected);
void sprd_fb_probe(unsigned int band, unsigned char shade);
void sprd_fb_step(unsigned int band);
/* Framebuffer text console -- renders into LK's live buffer, touches no HW. */
void sprd_fb_text_init(void);
void sprd_fb_putc(char c);
void sprd_fb_puts(const char *s);
void sprd_fb_printf(const char *fmt, ...);
/* Register fbcon as a stdio device and make it stdout. */
int sprd_fbcon_init(void);
void init_log_struct(void);
int init_log_partition_hdr(void);
void flush_log_buffer(void);
void write_uboot_last_log(void);

#ifdef CONFIG_SPRD_LOG
#ifdef DEBUG
# if defined(CONFIG_LOG_2_EMMC) || defined(CONFIG_LOG_2_UFS)
#define init_write_log() do {		\
	init_log_struct();		\
	init_log_partition_hdr();	\
} while(0)

#define reinit_write_log() do {		\
	init_log_partition_hdr();	\
	flush_log_buffer();		\
} while(0)

#define write_log() flush_log_buffer()
#define write_log_last() do {		\
	flush_log_buffer();		\
	write_uboot_last_log();		\
} while(0)

# elif defined(CONFIG_LOG_2_SD)
#define write_log_last() do {		\
	extern int sd_fs_type;		\
	puts("SD:   ");			\
	if (init_mmc_fat(&sd_fs_type)) {	\
		debugf("ERROR: init_mmc_fat,sd_fs_type=%d.\n", sd_fs_type);	\
	}				\
	flush_log_buffer();		\
} while(0)

# endif

#else

#define init_write_log()
#define reinit_write_log()
#define write_log()

# if defined(CONFIG_LOG_2_EMMC) || defined(CONFIG_LOG_2_UFS)
#define write_log_last() do {		\
	init_log_struct();		\
	init_log_partition_hdr();	\
	flush_log_buffer();		\
	write_uboot_last_log();		\
} while(0)

# else
#define write_log_last()
# endif

#endif
#else
#define init_write_log()
#define reinit_write_log()
#define write_log()
#define write_log_last()
#endif

extern LOG_BUFFER *p_log_buffer;
#ifdef CONFIG_DTS_MEM_LAYOUT
extern uint32_t log_buffer_enabled_flag;
#endif
extern LOG_REBOOT_TYPE_T lr_cause;
#endif //__SPRD_LOG_H__
