/*
 * Copyright The Zephyr Project Contributors
 * Copyright (c) 2026 Silicon Laboratories Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_TI_OMAP3_SOC_H_
#define ZEPHYR_SOC_TI_OMAP3_SOC_H_

#ifndef _ASMLANGUAGE

#include <stdbool.h>
#include <stdint.h>

/* Required by the CMSIS Cortex-A core header */
#define __CORTEX_A 8U

/* Address the image was loaded at, recorded by the entry stub */
extern uint32_t omap3_load_addr;

/* SCTLR as left by the bootloader, recorded by soc_reset_hook() */
extern uint32_t omap3_boot_sctlr;

void omap3_dss_console_init(void);

/**
 * @brief Call a service of the secure world
 *
 * Only high-security devices have these services. The secure side is not
 * re-entered after an interrupt, so interrupts must be locked, and it reads
 * the parameters from memory, so they must not be in the data cache only.
 *
 * @param service Service number
 * @param process Process ID, 0 for the services of the ROM code
 * @param flags Flags of the call
 * @param params Physical address of the parameter list of the service
 *
 * @return Result of the service, 0 on success
 */
uint32_t omap3_secure_call(uint32_t service, uint32_t process, uint32_t flags, uint32_t params);

void omap3_boot_stage(uint32_t stage, uint32_t rgb);
void omap3_boot_stage_late(uint32_t rgb);

#if defined(CONFIG_SOC_OMAP3_BOOT_STAGES)
#define OMAP3_BOOT_STAGE(stage, rgb) omap3_boot_stage(stage, rgb)
#define OMAP3_BOOT_STAGE_LATE(rgb)   omap3_boot_stage_late(rgb)
#else
#define OMAP3_BOOT_STAGE(stage, rgb) do { } while (false)
#define OMAP3_BOOT_STAGE_LATE(rgb)   do { } while (false)
#endif

#endif /* !_ASMLANGUAGE */

#endif /* ZEPHYR_SOC_TI_OMAP3_SOC_H_ */
