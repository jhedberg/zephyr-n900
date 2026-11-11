# SPDX-FileCopyrightText: Copyright (c) 2026 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(n900_phone_term)

# The phone demo's terminal grid, on its own
set(PHONE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../apps/phone/src)
target_include_directories(app PRIVATE ${PHONE_SRC})
target_sources(app PRIVATE src/main.c ${PHONE_SRC}/term.c)
