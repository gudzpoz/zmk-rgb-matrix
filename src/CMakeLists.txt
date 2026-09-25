# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

# ZMK app headers
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_MATRIX rgb_matrix.c rgb_utils.c)

# The generic overlay compositor is gated by its own DT-presence-derived
# Kconfig, so a device always exists for every node that references one.
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_OVERLAY overlays/overlay.c)

# Conditions are stateless predicates shared by overlays and triggers; each is
# likewise gated by its own DT-presence-derived Kconfig.
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_CONDITION_LAYER conditions/layer.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_CONDITION_CAPS_LOCK conditions/caps_lock.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_CONDITION_ALWAYS conditions/always.c)

zephyr_library_sources_ifdef(CONFIG_KEYPAW_BEHAVIOR_RGB_MATRIX
  # controller and effect persistence
  behavior_rgb_matrix.c
  rgb_settings.c
  # global oscillators and static fields
  effects/solid.c
  effects/breathe.c
  effects/spectrum.c
  effects/static.c
  # reactive fields
  effects/reactive.c
  effects/ripple.c
  # moving spatial fields
  effects/rainbow.c
  effects/band.c
  # stochastic fields
  effects/rain.c
  effects/starlight.c
  effects/heatmap.c
  effects/digital_rain.c
)

# Trigger table: central-only, because a trigger's condition may read keymap
# state, which does not exist on a peripheral.
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_TRIGGERS
  rgb_triggers.c
  triggers/trigger.c
)

# Split state sync: central-only, because it needs the transport's connected
# source list, which only exists on the central.
zephyr_library_sources_ifdef(CONFIG_KEYPAW_RGB_SPLIT_SYNC
  rgb_split_sync.c
)
