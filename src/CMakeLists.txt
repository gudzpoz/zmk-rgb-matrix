# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# ZMK app headers
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_MATRIX rgb_matrix.c rgb_utils.c)

# Indicators are gated by their own DT-presence-derived Kconfig, so a device
# always exists for every node that references one.
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_INDICATOR_CAPS_LOCK indicators/caps_lock.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_INDICATOR_LAYER indicators/layer.c)

zephyr_library_sources_ifdef(CONFIG_KEYPAW_BEHAVIOR_RGB_MATRIX
  # controller
  behavior_rgb_matrix.c
  # built-in effects
  effects/solid.c
  effects/breathe.c
  effects/spectrum.c
  effects/swirl.c
  effects/reactive.c
  effects/ripple.c
)
