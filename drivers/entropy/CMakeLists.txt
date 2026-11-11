# SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_ENTROPY_OMAP3_ROM entropy_omap3_rom.c)
