# SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_property(ALLOW_EMPTY TRUE)
zephyr_library_sources_ifdef(CONFIG_BT_NOKIA_H4P hci_nokia_h4p.c)

if(CONFIG_BT_NOKIA_H4P AND NOT CONFIG_BT_NOKIA_H4P_FIRMWARE STREQUAL "")
  generate_inc_file_for_target(
    ${ZEPHYR_CURRENT_LIBRARY}
    ${CONFIG_BT_NOKIA_H4P_FIRMWARE}
    ${ZEPHYR_BINARY_DIR}/include/generated/nokia_h4p_firmware.inc
  )
  zephyr_library_compile_definitions(NOKIA_H4P_FIRMWARE)
endif()
