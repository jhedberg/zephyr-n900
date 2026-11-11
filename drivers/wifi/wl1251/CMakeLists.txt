# SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
  wl1251.c
  wl1251_boot.c
  wl1251_io.c
)
zephyr_library_sources_ifdef(CONFIG_WIFI_NM_WPA_SUPPLICANT wl1251_supp.c)
zephyr_library_link_libraries_ifdef(CONFIG_WIFI_NM_WPA_SUPPLICANT hostap)

# The firmware and the calibration data that are loaded into the chip at
# power-up. Without them the driver builds, but cannot start the chip.
if(NOT CONFIG_WIFI_WL1251_FIRMWARE STREQUAL "" AND NOT CONFIG_WIFI_WL1251_NVS STREQUAL "")
  generate_inc_file_for_target(
    ${ZEPHYR_CURRENT_LIBRARY}
    ${CONFIG_WIFI_WL1251_FIRMWARE}
    ${ZEPHYR_BINARY_DIR}/include/generated/wl1251_fw.inc
  )
  generate_inc_file_for_target(
    ${ZEPHYR_CURRENT_LIBRARY}
    ${CONFIG_WIFI_WL1251_NVS}
    ${ZEPHYR_BINARY_DIR}/include/generated/wl1251_nvs.inc
  )
  zephyr_library_compile_definitions(WL1251_FIRMWARE=1)
else()
  message(WARNING "CONFIG_WIFI_WL1251_FIRMWARE and CONFIG_WIFI_WL1251_NVS are not set: "
                  "the WL1251 will not start")
endif()
