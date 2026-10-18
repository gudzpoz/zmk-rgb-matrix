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
zephyr_library_sources(kp_rgb_overlay_pair.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_COMPOSITE_TEST kp_rgb_composite_test.c)
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_SELECTOR_TEST kp_rgb_selector_test.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_SELECTOR_TEST kp_rgb_selector_condition_test.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_SETTINGS_TEST kp_rgb_settings_test.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_SPLIT_TEST kp_rgb_split_test.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_FIXED_SCENE_TEST kp_rgb_fixed_scene_test.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_GEOMETRY_TEST kp_rgb_geometry_test.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_CONTRACTS native_contract.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_ENGINE_CONTRACTS kp_rgb_engine_contracts.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_CONDITION_CONTRACTS kp_rgb_condition_contracts.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_CONTROL_CONTRACTS kp_rgb_control_contracts.c)
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_EFFECT_CONTRACTS kp_rgb_effect_contracts.c)
if(CONFIG_KEYPAW_SIM_ENGINE_CONTRACTS)
  zephyr_ld_options(-Wl,--wrap=k_work_reschedule_for_queue)
  zephyr_ld_options(-Wl,--wrap=k_work_submit_to_queue)
  zephyr_ld_options(-Wl,--wrap=kp_rgb_conditions_request_refresh)
endif()
if(CONFIG_KEYPAW_SIM_SINGLETON_SPLIT_TEST)
  zephyr_ld_options(-Wl,--wrap=zmk_behavior_invoke_binding)
endif()
