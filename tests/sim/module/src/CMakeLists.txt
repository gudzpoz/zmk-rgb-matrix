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
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
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
zephyr_library_sources_ifdef(CONFIG_KEYPAW_SIM_STATIC_METADATA_TEST
  kp_rgb_static_metadata_test.c kp_rgb_static_metadata_condition.c)
if(CONFIG_KEYPAW_SIM_ENGINE_CONTRACTS)
  zephyr_ld_options(-Wl,--wrap=k_work_reschedule_for_queue)
  zephyr_ld_options(-Wl,--wrap=z_impl_k_mutex_lock)
endif()
if(CONFIG_KEYPAW_SIM_SINGLETON_SPLIT_TEST)
  zephyr_ld_options(-Wl,--wrap=zmk_behavior_invoke_binding)
endif()

if(CONFIG_KEYPAW_SIM_SOURCE_CONTRACTS)
  zephyr_library_sources(kp_rgb_source_contracts.c)
  # Native_sim has no USB controller or battery: double only those external services.
  set(rgb_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
  set_property(SOURCE ${rgb_source_dir}/conditions/endpoint.c
    DIRECTORY ${rgb_source_dir} APPEND PROPERTY COMPILE_DEFINITIONS
    CONFIG_ZMK_BLE=1 CONFIG_USB_DEVICE_STACK=1)
  if(CONFIG_KEYPAW_SIM_SOURCE_USB_HID)
    set_property(SOURCE ${rgb_source_dir}/conditions/endpoint.c
      DIRECTORY ${rgb_source_dir} APPEND PROPERTY COMPILE_DEFINITIONS CONFIG_ZMK_USB=1)
  endif()
  set_property(SOURCE ${rgb_source_dir}/conditions/battery.c
    DIRECTORY ${rgb_source_dir} APPEND PROPERTY COMPILE_DEFINITIONS CONFIG_USB_DEVICE_STACK=1)
  if(CONFIG_KEYPAW_SIM_SOURCE_SOC)
    set_property(SOURCE ${rgb_source_dir}/conditions/battery.c
      DIRECTORY ${rgb_source_dir} APPEND PROPERTY COMPILE_DEFINITIONS CONFIG_ZMK_BATTERY_REPORTING=1)
  endif()
  zephyr_ld_options(-Wl,--wrap=zmk_endpoint_get_preferred_transport
    -Wl,--wrap=zmk_endpoint_get_selected -Wl,--wrap=zmk_endpoint_is_connected
    -Wl,--wrap=zmk_ble_active_profile_index -Wl,--wrap=zmk_ble_active_profile_is_connected
    -Wl,--wrap=zmk_ble_active_profile_is_open)
endif()
