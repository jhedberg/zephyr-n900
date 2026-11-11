/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Entry into the secure world of a high-security OMAP3, where the ROM code
 * and the protected application installed by the bootloader provide the
 * services that the normal world has no hardware access for.
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>

_ASM_FILE_PROLOGUE

GTEXT(omap3_secure_call)

	.arch_extension sec

/*
 * uint32_t omap3_secure_call(uint32_t service, uint32_t process,
 *			      uint32_t flags, uint32_t params)
 *
 * The secure monitor takes the service in r12 and the other arguments
 * where they already are, and returns the result in r0. It does not keep
 * r4 to r11.
 */
SECTION_FUNC(TEXT, omap3_secure_call)
	push	{r4-r11, lr}
	mov	r12, r0
	mov	r6, #0xff	/* a new call, not one that was interrupted */
	dsb
	smc	#1
	pop	{r4-r11, pc}
