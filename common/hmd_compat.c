#include <common.h>
#include <stdbool.h>

/*
 * This tree references a few HMD-specific hooks, but their vendor
 * implementation is not present in this BSP drop. Provide conservative
 * defaults so we can build and keep normal boot behavior predictable.
 *
 * These live in common/ rather than in a board directory because the callers
 * are all shared code -- common/cmd_cboot.c, common/loader/loader_nvm.c and
 * drivers/power/battery/sgm41512.c. While this file sat in
 * board/spreadtrum/ums512_1h10/ it only satisfied the linker for that one
 * board, and every other target in the tree (including all six ums9620_*
 * targets) failed to link with undefined references to these three symbols.
 */
int s_is6000F;

bool get_hmd_configs(void)
{
	s_is6000F = 0;
	return false;
}

bool get_diag_flag(void)
{
	return true;
}
