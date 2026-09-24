# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# <zmk/rgb_matrix.h> pulls in ZMK's own headers (zmk/behavior.h, zmk/keymap.h
# and friends), which the ZMK app exposes only as an app-private include dir.
# The parent module's include/ is already global via zephyr_include_directories,
# so <zmk/rgb_matrix.h> itself resolves without help; this is only for the ones
# it includes.
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_CAPTURE kp_rgb_sim.c)
