/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/rgb_color.h>

/* One behavior's persisted RGB state. Deliberately free of ZMK and devicetree
 * dependencies so a host build can unit-test the record codec; see
 * tests/unit. The layout is the on-flash contract: reordering or resizing a
 * field needs a KP_RGB_PERSIST_VERSION bump. */
#define KP_RGB_PERSIST_VERSION 1

/* The blob holds one fixed entry per effect, so this is also the largest effect
 * registry the engine accepts when persistence is compiled in. */
#define KP_RGB_PERSIST_MAX_EFFECTS 16

struct kp_rgb_persist_effect {
  uint16_t duration_ms;
  uint16_t h;
  uint8_t s;
  uint8_t b;
};

struct kp_rgb_persist_blob {
  uint16_t selected_index;
  uint8_t version;
  uint8_t user_on;
  uint8_t effect_count;
  uint8_t reserved;
  struct kp_rgb_persist_effect effects[KP_RGB_PERSIST_MAX_EFFECTS];
};

/* A stored record of any other size or version must be discarded. */
bool kp_rgb_persist_size_ok(size_t len);
bool kp_rgb_persist_version_ok(uint8_t version);

/* True when a stored colour is within the engine's ranges; out-of-range entries
 * are skipped rather than rejecting the whole record. */
bool kp_rgb_persist_effect_valid(const struct kp_rgb_persist_effect *effect);

/* Clamp a stored period into the configured range. */
uint16_t kp_rgb_persist_clamp_duration(uint16_t duration_ms, uint16_t min_ms,
                                       uint16_t max_ms);

/* Stamp a complete record from up to KP_RGB_PERSIST_MAX_EFFECTS presets. */
void kp_rgb_persist_pack(uint16_t selected_index, bool user_on,
                         const struct kp_rgb_persist_effect *effects,
                         size_t count, struct kp_rgb_persist_blob *out);
