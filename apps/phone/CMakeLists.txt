cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(n900_phone)

# Camera sensor register lists are generated from a local Linux tree at build time
set(LINUX_DIR $ENV{HOME}/linux CACHE PATH "Linux source tree with the et8ek8 driver")
set(ET8EK8_MODES ${LINUX_DIR}/drivers/media/i2c/et8ek8/et8ek8_mode.c)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(
  OUTPUT ${GEN_DIR}/et8ek8_regs.h
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_regs.py
          ${ET8EK8_MODES} ${GEN_DIR}/et8ek8_regs.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_regs.py ${ET8EK8_MODES}
)
add_custom_target(et8ek8_regs DEPENDS ${GEN_DIR}/et8ek8_regs.h)
add_dependencies(app et8ek8_regs)
target_include_directories(app PRIVATE ${GEN_DIR})
# Zephyr kite images from the documentation artwork
set(KITE_PNG $ENV{ZEPHYR_BASE}/doc/_static/images/kite.png)
add_custom_command(
  OUTPUT ${GEN_DIR}/kites.c ${GEN_DIR}/kites.h
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_kites.py
          ${KITE_PNG} ${GEN_DIR}/kites.c ${GEN_DIR}/kites.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_kites.py ${KITE_PNG}
)
add_custom_target(kites DEPENDS ${GEN_DIR}/kites.c ${GEN_DIR}/kites.h)
add_dependencies(app kites)
target_sources(app PRIVATE src/hw.c src/ui.c src/camera.c src/sound.c ${GEN_DIR}/kites.c)
# The terminal: the shell backend, its character grid and its font (see gen_font.py)
target_sources(app PRIVATE src/shell_term.c src/term.c src/font_adwaita_mono_14.c)
