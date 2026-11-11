# Copyright The Zephyr Project Contributors
# Copyright (c) 2026 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_AUDIO_CODEC audio)
add_subdirectory_ifdef(CONFIG_BT_HCI bluetooth)
add_subdirectory_ifdef(CONFIG_CHARGER charger)
add_subdirectory_ifdef(CONFIG_DISPLAY display)
add_subdirectory_ifdef(CONFIG_FUEL_GAUGE fuel_gauge)
add_subdirectory_ifdef(CONFIG_GPIO gpio)
add_subdirectory_ifdef(CONFIG_HAPTICS haptics)
add_subdirectory_ifdef(CONFIG_HWINFO hwinfo)
add_subdirectory_ifdef(CONFIG_I2C i2c)
add_subdirectory_ifdef(CONFIG_I2S i2s)
add_subdirectory_ifdef(CONFIG_INPUT input)
add_subdirectory(interrupt_controller)
add_subdirectory_ifdef(CONFIG_LED led)
add_subdirectory_ifdef(CONFIG_MFD mfd)
add_subdirectory_ifdef(CONFIG_PINCTRL pinctrl)
add_subdirectory_ifdef(CONFIG_SENSOR sensor)
add_subdirectory(timer)
add_subdirectory_ifdef(CONFIG_UDC_DRIVER usb/udc)
add_subdirectory_ifdef(CONFIG_WIFI wifi)
