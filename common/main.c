/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/* #define	DEBUG	*/

#include <common.h>
#include <autoboot.h>
#include <cli.h>
#include <version.h>
#ifdef CONFIG_SPRD_LOG
#include <sprd_log.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

/*
 * Board-specific Platform code can reimplement show_boot_progress () if needed
 */
__weak void show_boot_progress(int val) {}

static void modem_init(void)
{
#ifdef CONFIG_MODEM_SUPPORT
	debug("DEBUG: main_loop:   gd->do_mdm_init=%lu\n", gd->do_mdm_init);
	if (gd->do_mdm_init) {
		char *str = getenv("mdm_cmd");

		setenv("preboot", str);  /* set or delete definition */
		mdm_init(); /* wait for modem connection */
	}
#endif  /* CONFIG_MODEM_SUPPORT */
}

static void run_preboot_environment_command(void)
{
#ifdef CONFIG_PREBOOT
	char *p;

	p = getenv("preboot");
	if (p != NULL) {
# ifdef CONFIG_AUTOBOOT_KEYED
		int prev = disable_ctrlc(1);	/* disable Control C checking */
# endif

		run_command_list(p, -1, 0);

# ifdef CONFIG_AUTOBOOT_KEYED
		disable_ctrlc(prev);	/* restore Control C checking */
# endif
	}
#endif /* CONFIG_PREBOOT */
}

/* We come here after U-Boot is initialised and ready to process commands */
void main_loop(void)
{
	const char *s;

	bootstage_mark_name(BOOTSTAGE_ID_MAIN_LOOP, "main_loop");

#ifdef CONFIG_SPRD_LOG
	/*
	 * Reaching main_loop() means init completed, so flush what we have to the
	 * uboot_log partition now rather than only on the way out via bootm/pxe.
	 * On a board with no UART pad this is the first point at which anything
	 * we printed becomes readable at all, and it is what distinguishes "our
	 * U-Boot ran and got this far" from "it never started" -- the two look
	 * identical on a blank screen otherwise.
	 *
	 * The banner is deliberately distinctive: the same partition also holds
	 * the stock LK log, so the reader needs to be able to tell whose output
	 * this is at a glance.
	 */
	sprd_boot_mark(SPRD_MARK_MAIN_LOOP);
	printf("\n=== U-BOOT MAIN_LOOP REACHED (%s) ===\n", U_BOOT_VERSION);
	sprd_log_flush();
#endif

#ifndef CONFIG_SYS_GENERIC_BOARD
	debug("Warning: Your board does not use generic board. Please read\n");
	debug("doc/README.generic-board and take action. Boards not\n");
	debug("upgraded by the late 2014 may break or be removed.\n");
#endif

	modem_init();
#ifdef CONFIG_VERSION_VARIABLE
	setenv("ver", version_string);  /* set version variable */
#endif /* CONFIG_VERSION_VARIABLE */

	cli_init();

	run_preboot_environment_command();

#if defined(CONFIG_UPDATE_TFTP)
	update_tftp(0UL);
#endif /* CONFIG_UPDATE_TFTP */

	s = bootdelay_process();
	if (cli_process_fdt(&s))
		cli_secure_boot_cmd(s);

	autoboot_command(s);

	cli_loop();
}
