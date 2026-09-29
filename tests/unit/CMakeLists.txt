# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
#
# Host unit tests for the module's pure helpers (colour math). Built for
# native_sim: rgb_color.h exposes Zephyr's `struct led_rgb`, so the minimal
# `unit_testing` board (which has no syscall/devicetree headers) cannot compile
# it. ztest supplies main(); only rgb_color.c is linked, so the ZMK app is not
# involved.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(keypaw_rgb_unit)

target_sources(app PRIVATE
    src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/rgb_color.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/rgb_persist.c)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
