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
#include <rtc.h>
#include <boot_mode.h>
#include <common.h>
#include <linux/input.h>
#include <linux/mtd/mtd.h>
#include <linux/sizes.h>
#include <asm/arch/check_reboot.h>
#include <mmc.h>
#include <fat.h>
#include <exfat.h>
#include <chipram_env.h>
#include <sprd_common_rw.h>
#include <stdio_dev.h>
#include <video_font.h>
#include <stdarg.h>
#include <vsprintf.h>

#ifdef CONFIG_SPRD_LOG
extern LOG_BUFFER *p_log_buffer;
#endif

LOG_REBOOT_TYPE_T lr_cause = LR_NORMAL;

unsigned long get_uboot_log_addr(void)
{
	if (NULL != p_log_buffer)
		return p_log_buffer->addr;
	return 0;
}

uint32_t get_uboot_log_len(void)
{
	if (NULL != p_log_buffer)
		return p_log_buffer->size;
	return 0;
}

/*
 * Flush the captured console buffer to the uboot_log partition.
 *
 * This is the only way to get any output off a board with no broken-out UART
 * pad, which is every T820 unit checked so far. CONFIG_SPRD_LOG captures
 * console output into a DRAM buffer (see sprd_log_capture() in common/console.c)
 * but nothing writes that buffer out on its own.
 *
 * Deliberately placed here rather than in a board directory: it has no
 * board- or SoC-specific dependencies, and identical copies were previously
 * inlined in cmd_bootm.c and cmd_pxe.c with a third in the ums512_1h10 board
 * code, so no target outside that one board could flush a log at all.
 *
 * Writes ->used rather than ->size so the image is the actual log, not 256KB
 * mostly-zero. common_raw_write() resolves "ufs" or "mmc" at runtime, so this
 * works on the UFS-based boards despite the CONFIG_LOG_2_EMMC name.
 */
/*
 * Record that a boot stage was reached, in DRAM that outlives the reset.
 *
 * This is the only diagnostic available before UFS is up: sprd_log_flush()
 * cannot run until driver model and the block device exist, which is most of
 * the way to main_loop(). Between the chainload jump and that point there is
 * no console, no display (LK's panel thread never ran) and no storage, so a
 * failure anywhere in it is indistinguishable from never having started.
 *
 * Written monotonically, so the value left behind is the furthest stage
 * reached. Read it back from the LK side with the inline harness -- see
 * tools/diag-payload/, TEST_DRAMREAD.
 *
 * flush_dcache_range() because later stages run with caches on, and a line
 * still dirty in cache does not survive the reset that makes this readable.
 */
/*
 * Fill nbands consecutive framebuffer bands with a repeated byte.
 *
 * A repeated byte reads as a flat grey at any depth, which sidesteps the
 * pixel-format question entirely: logo_bpix says 24, but the buffer is sized
 * for 32bpp page flipping and nothing here has established which the display
 * controller is actually scanning out.
 */
static void sprd_fb_fill(unsigned int band, unsigned int nbands,
			 unsigned char shade)
{
	unsigned long fb = SPRD_FB_BAND_BASE + band * SPRD_FB_BAND_SIZE;
	unsigned long len = (unsigned long)nbands * SPRD_FB_BAND_SIZE;
	volatile unsigned long *p = (volatile unsigned long *)fb;
	unsigned long pat = 0x0101010101010101UL * shade;
	unsigned long i;

	for (i = 0; i < len / 8; i++)
		p[i] = pat;
	__asm__ __volatile__("dsb sy" ::: "memory");
	flush_dcache_range(fb, fb + len);
}

/*
 * Paint one band directly, for bisecting a stretch of straight-line code that
 * is too early for any other channel.
 *
 * Bands 6 and 9 are free: band 6 is computed for SPRD_MARK_MAIN, which
 * nothing ever passes to sprd_boot_mark, and band 9 belongs to main_loop,
 * which a payload dying in board_init_f never reaches. Both sit between bands
 * that are known visible on hardware.
 */
void sprd_fb_probe(unsigned int band, unsigned char shade)
{
	sprd_fb_fill(band, 1, shade);
}

/*
 * Paint a progress band with the same repeating white/mid/dark cycle the
 * init_sequence markers use, keyed off the band number so consecutive steps
 * stay countable. A run of identical white bands reads as one solid block and
 * cannot be counted off a photographed panel -- this is the fix for that, and
 * it matters more than it sounds: miscounting a run is how you end up
 * debugging the wrong function.
 */
void sprd_fb_step(unsigned int band)
{
	static const unsigned char cycle[3] = { 0xff, 0x90, 0x40 };

	sprd_fb_fill(band, 1, cycle[band % 3]);
}

/*
 * A minimal text console rendered straight into the framebuffer LK left
 * running.
 *
 * Why not U-Boot's lcd_console: it models depth as NBITS(vl_bpix) = 1 <<
 * vl_bpix, so it can express 16bpp or 32bpp but not the 24 this panel uses,
 * and getting there means drv_lcd_init() -> sprdfb_probe(), which re-runs
 * panel bring-up against hardware LK already has powered. That was tried and
 * visibly damaged the display -- it faded, inverted and vignetted.
 *
 * This touches nothing but memory. 32bpp, 4 bytes per pixel.
 *
 * The depth was established by getting it wrong: rendering at 24bpp (stride
 * 2160) into a 32bpp buffer (stride 2880) chopped the text into exactly four
 * interleaved sections. That is the signature -- written row y lands on
 * display row 0.75y at x offset (y*2160 mod 2880)/4, which cycles 0, 540, 360,
 * 180 and repeats every 4 rows. Four offsets, four sections.
 *
 * Note this contradicts the earlier inference from band geometry (that 8-row
 * bands and ~29 visible bands implied 24bpp). That estimate came from pacing
 * out a photograph and was not precise enough to separate 24 from 32; at 32bpp
 * the bands are 6 rows starting at row 364 and ~59 of them fit. The rendering
 * artifact is the authoritative measurement -- trust it over the pacing.
 *
 * The text console owns the WHOLE panel. It used to be confined to the top
 * 0x100000 (rows 0..363) so the progress bands could stay readable in the
 * bottom half -- but that coexistence only mattered before this console
 * existed. The bands (sprd_boot_mark, sprd_fb_step, sprd_fdt_verdict) are the
 * sole channel for start.S .. board_init_r; once board_init_r calls
 * sprd_fb_text_init() they have done their job, so it clears the entire buffer
 * and text takes all 720 rows (45 lines at 16px, vs the old 22). The bands are
 * transient scaffolding that gets painted over exactly when printf replaces it.
 */
#define SPRD_FB_TEXT_BASE	0xb0000000
#define SPRD_FB_WIDTH		720
#define SPRD_FB_HEIGHT		720
#define SPRD_FB_BYTESPP	4
#define SPRD_FB_STRIDE		(SPRD_FB_WIDTH * SPRD_FB_BYTESPP)
#define SPRD_FB_SIZE		(SPRD_FB_STRIDE * SPRD_FB_HEIGHT)
#define SPRD_FB_TEXT_ROWS	SPRD_FB_HEIGHT
#define SPRD_FB_TEXT_COLS	(SPRD_FB_WIDTH / VIDEO_FONT_WIDTH)

static unsigned int fb_text_col;
static unsigned int fb_text_row;
/*
 * Set once the text console owns the whole panel (sprd_fb_text_init). After
 * this, sprd_boot_mark() must not paint progress bands: they are the pre-console
 * channel, and a late marker -- e.g. SPRD_MARK_MAIN_LOOP at the top of
 * main_loop() -- would otherwise stamp a stray bar over live text. The DRAM
 * marker write stays unconditional.
 */
static int fb_text_active;

static void sprd_fb_drawc(unsigned char c)
{
	const unsigned char *g = &video_fontdata[c * VIDEO_FONT_HEIGHT];
	unsigned int x0 = fb_text_col * VIDEO_FONT_WIDTH;
	unsigned int y0 = fb_text_row * VIDEO_FONT_HEIGHT;
	unsigned int y, x;

	for (y = 0; y < VIDEO_FONT_HEIGHT; y++) {
		volatile unsigned char *p = (volatile unsigned char *)
			(SPRD_FB_TEXT_BASE + (y0 + y) * SPRD_FB_STRIDE
			 + x0 * SPRD_FB_BYTESPP);
		unsigned char bits = g[y];

		for (x = 0; x < VIDEO_FONT_WIDTH; x++) {
			unsigned char v = (bits & (0x80 >> x)) ? 0xff : 0x00;

			p[0] = v;
			p[1] = v;
			p[2] = v;
			p[3] = v;	/* bands write all 4 bytes too */
			p += SPRD_FB_BYTESPP;
		}
	}
}

/*
 * Move the whole text region up by one font row and clear the newly exposed
 * bottom line. Caches are off in this build (CONFIG_SYS_DCACHE_OFF), so this is
 * a plain uncached framebuffer copy -- slow, but it only runs once the panel is
 * full, and it lands the cursor on the newest output, which is what matters
 * when reading a boot log for the next hang.
 */
static void sprd_fb_scroll(void)
{
	unsigned long keep = (unsigned long)(SPRD_FB_TEXT_ROWS
			- VIDEO_FONT_HEIGHT) * SPRD_FB_STRIDE;
	unsigned long line = (unsigned long)VIDEO_FONT_HEIGHT * SPRD_FB_STRIDE;
	volatile unsigned long *dst = (volatile unsigned long *)SPRD_FB_TEXT_BASE;
	volatile unsigned long *src = (volatile unsigned long *)
		(SPRD_FB_TEXT_BASE + line);
	unsigned long i;

	for (i = 0; i < keep / 8; i++)
		dst[i] = src[i];

	dst = (volatile unsigned long *)(SPRD_FB_TEXT_BASE + keep);
	for (i = 0; i < line / 8; i++)
		dst[i] = 0;

	__asm__ __volatile__("dsb sy" ::: "memory");
	flush_dcache_range(SPRD_FB_TEXT_BASE, SPRD_FB_TEXT_BASE + SPRD_FB_SIZE);
}

static void sprd_fb_newline(void)
{
	fb_text_col = 0;
	if ((fb_text_row + 1) * VIDEO_FONT_HEIGHT + VIDEO_FONT_HEIGHT
	    > SPRD_FB_TEXT_ROWS)
		sprd_fb_scroll();	/* full: stay on the last line */
	else
		fb_text_row++;
}

void sprd_fb_putc(char c)
{
	if (c == '\n') {
		sprd_fb_newline();
		return;
	}
	if (c == '\r')
		return;
	if (c < 0x20)
		return;

	sprd_fb_drawc((unsigned char)c);
	if (++fb_text_col >= SPRD_FB_TEXT_COLS)
		sprd_fb_newline();
}

void sprd_fb_puts(const char *s)
{
	while (*s)
		sprd_fb_putc(*s++);
	__asm__ __volatile__("dsb sy" ::: "memory");
	flush_dcache_range(SPRD_FB_TEXT_BASE,
			   SPRD_FB_TEXT_BASE + SPRD_FB_SIZE);
}

void sprd_fb_printf(const char *fmt, ...)
{
	char buf[256];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	sprd_fb_puts(buf);
}

/* Clear the text region and home the cursor. Leaves the bands alone. */
void sprd_fb_text_init(void)
{
	volatile unsigned long *p = (volatile unsigned long *)SPRD_FB_TEXT_BASE;
	unsigned long i;

	for (i = 0; i < SPRD_FB_SIZE / 8; i++)
		p[i] = 0;
	__asm__ __volatile__("dsb sy" ::: "memory");
	flush_dcache_range(SPRD_FB_TEXT_BASE, SPRD_FB_TEXT_BASE + SPRD_FB_SIZE);
	fb_text_col = 0;
	fb_text_row = 0;
	fb_text_active = 1;	/* text owns the panel: stop band painting */
}

/*
 * Register the framebuffer text renderer as a stdio device and make it stdout.
 *
 * After this, ordinary printf() lands on the panel. That is the whole point:
 * every remaining problem in this bring-up (the mmu_setup hang, the ufs_init
 * double-init, sizing CONFIG_SYS_MEM_TOP_HIDE from something better than a
 * guess) has so far had to be diagnosed by counting coloured stripes off a
 * photograph. With this they become ordinary printf debugging.
 *
 * Named "fbcon" rather than "lcd" so it cannot be confused with U-Boot's own
 * LCD console, which is unusable here on two counts -- it needs
 * drv_lcd_init() -> sprdfb_probe(), which damages a panel LK already has
 * powered, and its NBITS(vl_bpix) depth model cannot express this buffer.
 */
static void sprd_fbcon_putc(struct stdio_dev *dev, const char c)
{
	sprd_fb_putc(c);
}

static void sprd_fbcon_puts(struct stdio_dev *dev, const char *s)
{
	sprd_fb_puts(s);
}

int sprd_fbcon_init(void)
{
	struct stdio_dev dev;

	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "fbcon");
	dev.flags = DEV_FLAGS_OUTPUT;
	dev.putc  = sprd_fbcon_putc;
	dev.puts  = sprd_fbcon_puts;

	if (stdio_register(&dev))
		return -1;

	return console_assign(stdout, "fbcon");
}

/*
 * Replay the captured console buffer onto the panel.
 *
 * CONFIG_SPRD_LOG has been capturing every puts() into a DRAM ring
 * (sprd_log_capture in common/console.c) since early board_init_f, long before
 * fbcon exists. Once the panel console is up this walks that buffer through it,
 * so the entire boot -- everything printed before the console handoff included
 * -- is readable at once instead of only what fbcon caught live.
 *
 * Reads ->used once, so the live lines printed after this call (which land in
 * the same ring) are not double-rendered.
 */
void sprd_log_dump_to_fb(void)
{
#ifdef CONFIG_SPRD_LOG
	const unsigned char *p;
	uint32_t i, used;

	if (!p_log_buffer || !p_log_buffer->addr)
		return;

	used = p_log_buffer->used;
	p = (const unsigned char *)p_log_buffer->addr;
	for (i = 0; i < used; i++)
		sprd_fb_putc((char)p[i]);
#endif
}

void sprd_boot_mark(uint32_t stage)
{
	volatile uint32_t *mark = (volatile uint32_t *)SPRD_BOOT_MARK_ADDR;

	mark[0] = SPRD_BOOT_MARK_MAGIC;
	mark[1] = stage;
	__asm__ __volatile__("dsb sy" ::: "memory");
	flush_dcache_range(SPRD_BOOT_MARK_ADDR, SPRD_BOOT_MARK_ADDR + 64);

	/*
	 * Paint a framebuffer band too, matching what start.S does for the
	 * earlier stages. Chainloaded from LK's late hook the panel is already
	 * up and unrepainted, so this is visible immediately -- no reboot, no
	 * log, no working console. Bands 6+ are the C stages.
	 */
	if (stage >= SPRD_MARK_MAIN && !fb_text_active) {
		unsigned int band;
		unsigned char shade;
		unsigned long fb;

		if (stage >= SPRD_MARK_INIT_FN) {
			unsigned int n = stage - SPRD_MARK_INIT_FN;

			/*
			 * One band PER init_sequence entry, from band 12 up.
			 *
			 * The old scheme shared band 11 and stepped the shade
			 * per entry, on the belief that a band each would run
			 * off the bottom of the screen. It does not: the band
			 * base (0xb0100000) is 0x100000 into a framebuffer at
			 * 0xb0000000, which at 24bpp on a 720-wide panel is row
			 * ~485 of 720, and a band is 8 rows -- so ~29 bands are
			 * visible, and entries 0..16 all fit.
			 *
			 * Telling 0xd0 from 0xe0 by eye on a photographed panel
			 * is not reliable; counting bands is. Band 11 keeps the
			 * old shade ramp as a "walk started" marker, and the
			 * number of bands below it is N + 1.
			 *
			 * The shades cycle white/mid/dark rather than all being
			 * one value: a dozen identical bands read as a single
			 * solid block, whereas a repeating 3-cycle gives a
			 * white anchor every third band and stays countable.
			 */
			static const unsigned char cycle[3] = {
				0xff, 0x90, 0x40
			};

			sprd_fb_fill(11, 1, 0x30 + n * 0x10);

			if (n > SPRD_FB_INITFN_MAX)
				n = SPRD_FB_INITFN_MAX;
			band = SPRD_FB_INITFN_BAND + n;
			shade = cycle[n % 3];
		} else {
			band = 6 + (stage - SPRD_MARK_MAIN);
			shade = 0xa0 + (stage - SPRD_MARK_MAIN) * 0x18;
		}
		sprd_fb_fill(band, 1, shade);
	}
}

/*
 * Paint a verdict on the FDT that board_init_f is about to check.
 *
 * The payload stops at init_sequence[2] (fdtdec_check_fdt), and the only way
 * to hang in there is the FDT-invalid branch of fdtdec_prepare_fdt: it calls
 * puts() four entries before serial_init runs, so it spins on an
 * uninitialised UART. That makes the failure silent, and the DRAM marker can
 * only say *where* we stopped, not *why* -- and reading it back costs a
 * hardware watchdog timeout plus a reboot.
 *
 * So report the cause on the panel instead, immediately. Three outcomes, told
 * apart by shade, with height as a secondary cue -- the diagonal speckle
 * leaves the bands legible underneath, but may shift their colour:
 *
 * All in band 8, one band each, told apart by shade:
 *
 *   0x60 mid-dark -- entered but did not finish. Should never be the final
 *                    value; if it is, we hang inside this function.
 *   0xff white    -- pointer correct, magic correct. The FDT is intact and
 *                    the hang is something else in the puts() path.
 *   0xb0 light    -- pointer correct, magic wrong. The DTB did not arrive
 *                    intact: it occupies the last ~13KB of the copied image,
 *                    so this is the truncation signature.
 *   0x20 v. dark  -- pointer is not &_end. Something overwrote fdt_blob
 *                    between board_init_f setting it and here.
 *
 * Deliberately no DRAM marker write: that channel does not survive the
 * watchdog reset this failure ends in, so it reports the wrong thing. The
 * panel is the only trustworthy readout here -- LK blanks and repaints it
 * every boot, so whatever is on it was drawn by the run you just made.
 */
void sprd_fdt_verdict(const void *blob, const void *expected)
{
	unsigned char shade;

	/*
	 * Stamp entry into our own band first, then overwrite it with the
	 * answer. Costs no extra band, and separates "never called" from
	 * "called, hung inside" -- which is exactly where we are: bands 6 and 9
	 * bracket this call and both paint, but band 8 stays background.
	 *
	 * Everything here is one band. The earlier two-band encoding spilled
	 * into band 9 and fought with the probe that lives there.
	 */
	sprd_fb_fill(SPRD_FB_VERDICT_BAND, 1, 0x60);

	/*
	 * Dereference `expected`, never `blob`.
	 *
	 * `expected` is a PC-relative &_end from the caller, so it is a real
	 * address whatever state the image is in -- reading it can return
	 * rubbish but cannot wander off. `blob` came out of the GOT, which is
	 * exactly the memory under suspicion, so dereferencing it is what hung
	 * the previous build: the entry stamp painted and the answer never did.
	 */
	if (*(const uint32_t *)expected != SPRD_FDT_MAGIC_LE)
		shade = 0x20;		/* very dark : image tail is not there */
	else if (blob != expected)
		shade = 0xb0;		/* light     : tail fine, pointer wrong */
	else
		shade = 0xff;		/* white     : both correct             */

	sprd_fb_fill(SPRD_FB_VERDICT_BAND, 1, shade);
}

void sprd_log_flush(void)
{
#if defined(CONFIG_SPRD_LOG) && defined(CONFIG_LOG_2_EMMC)
	if (!p_log_buffer || !p_log_buffer->addr || !p_log_buffer->used)
		return;

	if (common_raw_write(UBOOT_LOG_PARTITION,
			     (uint64_t)p_log_buffer->used, (uint64_t)0,
			     (uint64_t)SPRD_UBOOT_LOG_OFFSET,
			     (char *)p_log_buffer->addr))
		printf("[uboot] uboot_log dump failed\n");
#endif
}

#if defined(CONFIG_LOG_2_SD)
int sd_fs_type;  /* file system type on sd card */

void write_log_to_mmc(char *filename, int fs_type,
	void *memaddr, unsigned long memsize)
{
	int ret = 0;
	unsigned long actwrite = 0;

	debugf("writing 0x%lx bytes to sd file %s\n",
		memsize, filename);

	if (fs_type==FS_FAT32)
		ret = file_fat_write(filename, memaddr, 0, memsize, &actwrite);
	else if (fs_type==FS_EXFAT)
		ret = file_exfat_write(filename, memaddr, memsize);
	else
		ret = -1;

	if (ret != 0) {
		debugf("write file %s to SD Card error, and ret = %d !!!\n", filename, ret);
	}

	return;
}


/*
 * Flush log buffer to sd/emmc/nand etc
 */
void flush_log_buffer(void)
{
	struct rtc_time tm;
	dir_entry *dentptr = NULL;
	char fnbuf[72] = {0}, fnbuf_rename[72] = {0};
	int i, j, ret;
	int auto_test_flag = 0;

	if (sd_fs_type == FS_FAT32) {
		int mod, key_code;
		unsigned long max_size = 0;

		debugf("FileSystem is FAT32 !!!\n");
		max_size = p_log_buffer->size;

		mod = fat_checksys(max_size);
		if (mod &(FSFATAL | FSNOSPACE)) {
			int nospace_flag = 1;
			if (mod & FSFATAL) {
				debugf("\nHello Baby: SD Card is demaged !!!");
			}
			else if (mod & FSNOSPACE) {
				for (i = 1; i <= LOG_FOLDER_NUM; i++) {
					sprintf(fnbuf, LOG_FOLDER_NAME"/%d", i);
					fnbuf[strlen(fnbuf)] = '\0';
					dentptr = check_folder_flag(fnbuf);
					if(dentptr == NULL)
						break;
				}
				if (i != 1) {
					for(j = i; j != 1;) {
						sprintf(fnbuf, LOG_FOLDER_NAME"/%d", --j);
						fnbuf[strlen(fnbuf)] = '\0';
						ret = delete_folder(fnbuf);
						if(ret == -1) {
							debugf("ERROR: delete files or folder failed !!!\n");
							goto FINISH;
						}
						mod = fat_checksys(max_size);
						if (!(mod & FSNOSPACE)) {
							nospace_flag = 0;
							break;
						}
					}
				}
				if (nospace_flag) {
					debugf("\nHello Baby: SD Card have not enough space !!!");
				}
			}
			if (nospace_flag) {
				debugf("need to format SD Card !!!\n");
				goto FINISH;

			}
		}

		if(do_new_folder(LOG_FOLDER_NAME)) {
			debugf("ERROR: creat %s folder failed !!!\n", LOG_FOLDER_NAME);
			goto FINISH;
		}

		if(check_folder_flag(LOG_AUTO_TEST)) {
			auto_test_flag = 1;
			debugf("Now has existed uboot_log_auto_test.txt!!!\n");
		}

		for (i = 1; i <= LOG_FOLDER_NUM; i++) {
			sprintf(fnbuf, LOG_FOLDER_NAME"/%d", i);
			fnbuf[strlen(fnbuf)] = '\0';
			dentptr = check_folder_flag(fnbuf);
			if(dentptr == NULL)
				break;
		}

		if (i > LOG_FOLDER_NUM) {
			debugf("there existed %d history log !!!\n", LOG_FOLDER_NUM);
			i --;
			sprintf(fnbuf, LOG_FOLDER_NAME"/%d", i);
			fnbuf[strlen(fnbuf)] = '\0';
			ret = delete_folder(fnbuf);
			if(ret == -1) {
				debugf("ERROR: delete files or folder failed !!!\n");
				goto FINISH;
			}
		}

		for(j = i - 1; j > 0 && i != 1; j = j - 2) {
				sprintf(fnbuf, LOG_FOLDER_NAME"/%d", j++);
				fnbuf[strlen(fnbuf)] = '\0';
				sprintf(fnbuf_rename, LOG_FOLDER_NAME"/%d", j);
				fnbuf_rename[strlen(fnbuf_rename)] ='\0';
				debugf("rename folder from %s to %s !!!\n", fnbuf, fnbuf_rename);
				ret = rename_folder(fnbuf, fnbuf_rename);
				if (ret == -1) {
					debugf("ERROR: rename folder failed !!!\n");
					break;
				}
			}

		sprintf(fnbuf, LOG_FOLDER_NAME"/%d", 1);

		if(do_new_folder(fnbuf)) {
			debugf("ERROR: creat %s folder failed !!!\n", fnbuf);
			goto FINISH;
		}
	}
	else if (sd_fs_type == FS_EXFAT) {
		int mod, key_code;
		unsigned long max_size;

		debugf("FileSystem is exFAT !!!\n");
		max_size = p_log_buffer->size;
		debugf("ExFAT max space size is %lx\n",max_size);

		mod = exfat_checksys(max_size);
		if (mod & FSSMSIZE) {
			debugf("SD card volume size is smaller then dumped size. Skip write !!!\n");
			goto FINISH;
		}

		if (mod &(FSFATAL | FSNOSPACE)) {
			if (mod & FSFATAL)
				debugf("\nHello Baby: SD Card is demaged !!!");
			else if (mod & FSNOSPACE)
				debugf("\nHello Baby: SD Card has not enough space !!!");

			debugf("\npress volumedown to format SD Card !!!\n");
		}
	}
	else {
		debugf("Invalid file system... Write will be skipped !!!\n");
		goto FINISH;
	}

	sprintf(fnbuf, LOG_FOLDER_NAME"/1/uboot_log_%04d_%02d_%02d_%02d_%02d_%02d.txt", \
			tm.tm_year, tm.tm_mon, tm.tm_mday, \
			tm.tm_hour, tm.tm_min, tm.tm_sec);

	debugf("time is %04d.%02d.%02d_%02d:%02d:%02d\n", tm.tm_year, tm.tm_mon, \
			tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

	write_log_to_mmc(fnbuf, sd_fs_type,
			(uchar *)p_log_buffer->addr, p_log_buffer->used);

FINISH:
	debugf ("Write  finish or happen abnormally!!!\n");
#if (defined CONFIG_X86) && (defined CONFIG_MOBILEVISOR)
		reset_to_normal(CMD_NORMAL_MODE);
#endif

	return;
}

#elif defined(CONFIG_LOG_2_EMMC) || defined(CONFIG_LOG_2_UFS)
extern int panic_cnt;
LOG_STRUCT log_st;

void init_log_struct(void)
{
	memset(&log_st, 0, sizeof(LOG_STRUCT));
}

int init_log_partition_hdr(void)
{
	int i;
	int cur_body; /* current log body */
	uint8_t hdr_buf[LOG_HEADER_SIZE];
	LOG_PARTITION_HEADER *p_hdr = hdr_buf;
	const uint64_t log_p_offset[MAX_LOG_TYPE] = {
		PANIC_LOG_PARTITION_OFFSET,
		START_LOG_PARTITION_OFFSET,
		FASTBOOT_LOG_PARTITION_OFFSET,
		DOWNLD_LOG_PARTITION_OFFSET,
	};
	const int log_body_count[MAX_LOG_TYPE] = {
		LOG_PANIC_BODY_NUM,
		LOG_CBOOT_BODY_NUM,
		LOG_FASTBOOT_BODY_NUM,
		LOG_DOWNLD_BODY_NUM,
	};
	LOG_TYPE_T t;

	for (t = PANIC_LOG_TYPE; t < MAX_LOG_TYPE; t++) {
		debugf("init log type %d\n", t);

		memset(hdr_buf, 0, sizeof(hdr_buf));
		if (0 != common_raw_read(UBOOT_LOG_PARTITION, (uint64_t)LOG_HEADER_SIZE, log_p_offset[t], hdr_buf)) {
			errorf("read hdr error\n");
			log_st.flag = FAILED;
			return -1;
		}

		if (p_hdr->magic != LOG_HEAD_MAGIC/* || p_hdr->next_body > log_body_count[t]*/) {
			debugf("reset log partition header\n");
			p_hdr->magic = LOG_HEAD_MAGIC;
			p_hdr->len = LOG_HEADER_SIZE;
			p_hdr->boot_count = 1;
			p_hdr->body_num = log_body_count[t];
			p_hdr->type = t;
			p_hdr->next_body = 1;

			for (i = 0; i < p_hdr->body_num; i++) {
				if (i == 0) {
					p_hdr->body[i].p_offset = LOG_HEADER_SIZE;
					p_hdr->body[i].size = LOG_BODY_SIZE - LOG_HEADER_SIZE;	//the same as buffer size
				} else {
					p_hdr->body[i].p_offset = i * LOG_BODY_SIZE;
					p_hdr->body[i].size = LOG_BODY_SIZE;
				}
				p_hdr->body[i].b_offset = 0;
			}

			if (0 != common_raw_write(UBOOT_LOG_PARTITION, (uint64_t)LOG_HEADER_SIZE, (uint64_t)0,
									  log_p_offset[t], hdr_buf)) {
				errorf("write hdr error\n");
				log_st.flag = FAILED;
				return -1;
			}
		} else {
			p_hdr->boot_count += 1;
			p_hdr->next_body += 1;
			if (p_hdr->next_body > p_hdr->body_num)
				p_hdr->next_body = 1;

			/* reset the body offset  */
			p_hdr->body[p_hdr->next_body - 1].b_offset = 0;
		}

		log_st.log_type_p_offset[t] = log_p_offset[t];
		memcpy(&log_st.log_par_hdr[t], p_hdr, sizeof(LOG_PARTITION_HEADER));
		if (t == START_LOG_TYPE)
			printf("bootup init log success on %d times\n", p_hdr->boot_count);
		else if (t == PANIC_LOG_TYPE)
			printf("boot count %d\n", p_hdr->boot_count);
	}

	log_st.flag = SUCESS; /* set sucess flag */
	return 0;
}

void flush_log_buffer(void)
{
	int cur_body; /* current log body */
	int64_t buf_size;
	uint64_t p_offset;
	uint8_t hdr_buf[LOG_HEADER_SIZE];
	uint8_t *buf;
	LOG_BUFFER *p_log;
	LOG_PARTITION_HEADER *p_hdr;
	LOG_BODY *body;
	boot_mode_t mode;
	char *boot_mode;

	if (log_st.flag != SUCESS) {
		return;
	}

	/* choose the right log partition */
	if (panic_cnt || lr_cause == LR_ABNORMAL || lr_cause == LR_LONG_PRESS
			|| lr_cause == LR_UNKNOWN)
		p_hdr = &log_st.log_par_hdr[PANIC_LOG_TYPE];
	else {
#ifdef CONFIG_ZEBU
		mode = BOOTLOADER_MODE_LOAD;
#else
		mode = get_boot_role();
#endif
		if (mode == BOOTLOADER_MODE_DOWNLOAD)
			p_hdr = &log_st.log_par_hdr[DOWNLD_LOG_TYPE];
		else {
			boot_mode = getenv("bootmode");
			if (boot_mode && !strcmp(boot_mode, "fastboot"))
				p_hdr = &log_st.log_par_hdr[FASTBOOT_LOG_TYPE];
			else
				p_hdr = &log_st.log_par_hdr[START_LOG_TYPE];
		}
	}

	if (p_hdr->type >= MAX_LOG_TYPE) {
		errorf("error log type %d\n", p_hdr->type);
	}

	cur_body = ((p_hdr->next_body > p_hdr->body_num) || !p_hdr->next_body) ? 0 : p_hdr->next_body - 1;
	body = &p_hdr->body[cur_body];
	p_log = p_log_buffer;

	buf = p_log->addr + body->b_offset;
	buf_size = p_log->used - body->b_offset;
	if (buf_size > body->size - body->b_offset) /* FIXME */
		buf_size = body->size - body->b_offset;

	if (buf_size <= 0) /* there is no more log in buf or no more space on flash */
		return;

	p_offset = body->p_offset + body->b_offset;

	if (body->b_offset == 0) {
		if (0 != common_raw_erase(UBOOT_LOG_PARTITION, (uint64_t)body->size,
								  log_st.log_type_p_offset[p_hdr->type] + body->p_offset)) {
			errorf("erase %llx size %x error\n", log_st.log_type_p_offset[p_hdr->type] + body->p_offset,
				   body->size);
			return;
		}
	}

	if (NULL != getenv("bootmode") && !strcmp(getenv("bootmode"), "fastboot"))
		body->b_offset = 0;
	else
		body->b_offset = p_log->used; /* must place this sentence before any printf until wirte to emmc */

	/* write log */
	if (0 != common_raw_write(UBOOT_LOG_PARTITION, (uint64_t)buf_size, (uint64_t)0,
							  log_st.log_type_p_offset[p_hdr->type] + p_offset, buf)) {
		errorf("write log 2 emmc failed\n");
		return;
	}

	memset((void *)hdr_buf, 0, LOG_HEADER_SIZE);
	memcpy((void *)hdr_buf, p_hdr, sizeof(LOG_PARTITION_HEADER));
	/* write back to header */
	if (0 != common_raw_write(UBOOT_LOG_PARTITION, (uint64_t)LOG_HEADER_SIZE, (uint64_t)0,
							  log_st.log_type_p_offset[p_hdr->type], hdr_buf)) {
		errorf("write header 2 emmc failed\n");
		return;
	}
}

void write_uboot_last_log(void)
{
	boot_mode_t mode;

	mode = get_boot_role();
	if (mode == BOOTLOADER_MODE_DOWNLOAD)
		return;

	/* write current log to last log location */
	if (0 != common_raw_write(UBOOT_LOG_PARTITION, (uint64_t)p_log_buffer->size, (uint64_t)0,
						LAST_LOG_PARTITION_OFFSET, p_log_buffer->addr)) {
		errorf("save current log as last log failed\n");
		return;
	}
}

#endif

