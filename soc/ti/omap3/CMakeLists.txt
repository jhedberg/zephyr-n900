# Copyright The Zephyr Project Contributors
# Copyright (c) 2026 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)

zephyr_sources(soc.c entry.S secure_call.S)
zephyr_sources_ifdef(CONFIG_SOC_OMAP3_DSS_CONSOLE dss_console.c)
zephyr_sources_ifdef(CONFIG_SOC_OMAP3_BOOT_STAGES boot_stage.c)

zephyr_linker_sources(ROM_START SORT_KEY 0x00entry entry.ld)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_a_r/scripts/linker.ld CACHE INTERNAL "")
