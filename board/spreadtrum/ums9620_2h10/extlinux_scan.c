/*
 * extlinux_scan -- boot mainline off the SD card via extlinux.conf.
 *
 * This is the payoff of the whole slot-B chainloader: a fast dev-boot loop
 * where the OS lives on a microSD and U-Boot finds and boots it. It is wired as
 * CONFIG_BOOTCOMMAND, because this device has no stdin in our chainloaded
 * U-Boot (sprd_fbcon_init assigns stdout only), so nothing interactive works --
 * the boot has to run itself from autoboot.
 *
 * Adapted from board/spreadtrum/ums512_1h10/extlinux_diag.c, but deliberately
 * stripped: no charge screen, no audio-DSP setup, no lcd_banner. Those are
 * ums512/sharkl5pro board specifics. In particular the DSP shared-memory init
 * that version does before sysboot is not replicated here -- LK runs a full
 * normal boot up to the point we hijack it with the late slot-B jump, so the
 * DSP (like UFS, the ADI/PMIC and the panel) is already live. If that ever
 * proves wrong, the hook is marked below.
 *
 * The infrastructure (sysboot, the extlinux parser, ext4+FAT reads) is already
 * built in via CONFIG_CMD_PXE and CONFIG_CMD_FAT; all this adds is the scan and
 * the SD bring-up in front of it.
 */
#include <common.h>
#include <command.h>
#include <mmc.h>
#include <fs.h>
#include <sprd_log.h>

/* 1: replicate the ums512 audio-DSP share-memory init before sysboot. Left off
 * on the assumption LK already did it (see file header). Flip to 1 to test if
 * audio/DSP misbehaves on a booted kernel. */
#define EXTLINUX_INIT_DSP	0

#if EXTLINUX_INIT_DSP
extern void memset_dsp_share_memory(void);
#endif

/*
 * The SD is device 1 (EMMC=0, SD=1 in sdio_cfg.c). A dev card's boot files sit
 * on the first FAT/ext partition; scan the first two to be forgiving of layout,
 * and try both the bare and /boot-prefixed conf paths.
 */
#define EXTLINUX_SD_DEV		1
static const int	part_list[] = { 1, 2 };
static const char * const conf_paths[] = {
	"/extlinux/extlinux.conf",
	"/boot/extlinux/extlinux.conf",
};

static void try_sysboot(int part, const char *path)
{
	char cmd[160];

	printf("[extlinux] try mmc %d:%d %s\n", EXTLINUX_SD_DEV, part, path);

	/*
	 * Flush the captured log before sysboot: a successful boot never
	 * returns, so this is the last chance to persist it. sprd_log_flush()
	 * is a no-op unless a flush target is configured, which is fine.
	 */
	sprd_log_flush();

	/*
	 * "fat", not "any": the generic "any" loader (do_get_any) needs
	 * CONFIG_CMD_FS_GENERIC, which regresses this build to an early hang
	 * (see the note in ums9620_rg_cube.h). A FAT boot partition is enough
	 * for now.
	 */
	snprintf(cmd, sizeof(cmd),
		 "sysboot mmc %d:%d fat ${pxefile_addr_r} %s",
		 EXTLINUX_SD_DEV, part, path);
	run_command(cmd, 0);

	/*
	 * Do not trust sysboot's return value: it returns 0 after handling the
	 * menu even when the kernel load failed. A *successful* boot jumps to
	 * the kernel and never returns, so simply reaching this line means the
	 * boot did not launch. Treat it as a miss and let the caller carry on
	 * to the next candidate (and ultimately halt) rather than falling
	 * through to an interactive prompt this device has no stdin for.
	 */
	printf("[extlinux] boot did not launch: mmc %d:%d %s\n",
	       EXTLINUX_SD_DEV, part, path);
}

static int do_extlinux_scan(cmd_tbl_t *cmdtp, int flag, int argc,
			    char * const argv[])
{
	char part_spec[8];
	unsigned int pi, ci;

	printf("[extlinux] SD extlinux scan\n");

	if (!board_sd_init()) {
		printf("[extlinux] no sd card\n");
		goto halt;
	}
	printf("[extlinux] sd card up\n");

	for (pi = 0; pi < ARRAY_SIZE(part_list); pi++) {
		for (ci = 0; ci < ARRAY_SIZE(conf_paths); ci++) {
			snprintf(part_spec, sizeof(part_spec), "%d:%d",
				 EXTLINUX_SD_DEV, part_list[pi]);

			if (!file_exists("mmc", part_spec, conf_paths[ci],
					 FS_TYPE_ANY))
				continue;

			printf("[extlinux] found %s on mmc %s\n",
			       conf_paths[ci], part_spec);

#if EXTLINUX_INIT_DSP
			memset_dsp_share_memory();
#endif
			/*
			 * Returns only on failure (a real boot never comes
			 * back), so just continue to the next candidate.
			 */
			try_sysboot(part_list[pi], conf_paths[ci]);
		}
	}

	printf("[extlinux] no extlinux.conf found\n");

halt:
	/*
	 * This U-Boot is a chainloaded guest that boots mainline directly;
	 * there is no stock LK context to hand back to (cboot cannot reconstruct
	 * LK's exact handoff state from here). So stop, visibly, rather than
	 * masking a failed or missing boot.
	 */
	printf("[extlinux] no mainline boot; halting\n");
	sprd_log_flush();
	for (;;)
		;

	return 1;
}

U_BOOT_CMD(
	extlinux_scan, 1, 0, do_extlinux_scan,
	"scan the SD card for extlinux.conf and boot it",
	""
);
