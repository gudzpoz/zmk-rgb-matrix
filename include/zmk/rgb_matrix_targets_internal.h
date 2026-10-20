/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <dt-bindings/keypaw/rgb_matrix.h>

#define KP_RGB_TARGET_MAP_NAME_(cfg, key, led) cfg##_target_key_##key##_at_##led
#define KP_RGB_TARGET_MAP_NAME(cfg, key, led) KP_RGB_TARGET_MAP_NAME_(cfg, key, led)
#define KP_RGB_TARGET_KEY_NAME_(cfg, key) cfg##_target_key_##key
#define KP_RGB_TARGET_KEY_NAME(cfg, key) KP_RGB_TARGET_KEY_NAME_(cfg, key)
#define KP_RGB_TARGET_BEFORE_(cfg, prop, idx) cfg##_target_##prop##_##idx##_before
#define KP_RGB_TARGET_BEFORE(cfg, prop, idx) KP_RGB_TARGET_BEFORE_(cfg, prop, idx)
#define KP_RGB_TARGET_AFTER_(cfg, prop, idx) cfg##_target_##prop##_##idx##_after
#define KP_RGB_TARGET_AFTER(cfg, prop, idx) KP_RGB_TARGET_AFTER_(cfg, prop, idx)

#define KP_RGB_TARGET_KEYS(node, fn, cfg)                                      \
  COND_CODE_1(DT_NODE_HAS_PROP(node, keys),                                    \
              (DT_FOREACH_PROP_ELEM_SEP_VARGS(node, keys, fn, (), cfg)), ())
#define KP_RGB_TARGET_LEDS(node, fn, cfg)                                      \
  COND_CODE_1(DT_NODE_HAS_PROP(node, leds),                                    \
              (DT_FOREACH_PROP_ELEM_SEP_VARGS(node, leds, fn, (), cfg)), ())

#define KP_RGB_TARGET_MAP_STEP(node, prop, led, cfg, owner, key)               \
  KP_RGB_TARGET_MAP_NAME(cfg, key, led) =                                      \
      (!(DT_PROP_BY_IDX(node, prop, led) & KP_RGB_NO_KEY_XY_FLAG) &&           \
       (uint32_t)DT_PROP_BY_IDX(node, prop, led) ==                            \
           (uint32_t)DT_PROP_BY_IDX(owner, keys, key))                         \
          ? led                                                                \
          : COND_CODE_1(IS_EQ(led, 0), (-1),                                   \
                        (KP_RGB_TARGET_MAP_NAME(cfg, key, UTIL_DEC(led)))),

#define KP_RGB_TARGET_KEY_ENUM(node, prop, key, cfg)                           \
  enum {                                                                       \
    DT_FOREACH_PROP_ELEM_VARGS(KP_RGB_NODE, mapping, KP_RGB_TARGET_MAP_STEP,   \
                               cfg, node, key)                                 \
        KP_RGB_TARGET_KEY_NAME(cfg, key) = COND_CODE_1(                        \
            UTIL_BOOL(KP_LED_COUNT),                                           \
            (KP_RGB_TARGET_MAP_NAME(cfg, key, UTIL_DEC(KP_LED_COUNT))), (-1))  \
  };

/* Each implicit successor starts at the preceding valid-prefix count. */
#define KP_RGB_TARGET_KEY_PREFIX(node, prop, key, cfg)                         \
  KP_RGB_TARGET_BEFORE(cfg, prop, key),                                        \
      KP_RGB_TARGET_AFTER(cfg, prop, key) =                                    \
          KP_RGB_TARGET_BEFORE(cfg, prop, key) +                               \
          (KP_RGB_TARGET_KEY_NAME(cfg, key) >= 0) - 1,
#define KP_RGB_TARGET_LED_PREFIX(node, prop, led, cfg)                         \
  KP_RGB_TARGET_BEFORE(cfg, prop, led),                                        \
      KP_RGB_TARGET_AFTER(cfg, prop, led) =                                    \
          KP_RGB_TARGET_BEFORE(cfg, prop, led) +                               \
          ((uint32_t)DT_PROP_BY_IDX(node, prop, led) < KP_LED_COUNT) - 1,

/* Valid entries advance the prefix; invalid entries fill the tail backwards. */
#define KP_RGB_TARGET_DEST(cfg, pos, before, valid)                            \
  ((valid) ? (before) : cfg##_target_candidates - 1 - ((pos) - (before)))
#define KP_RGB_TARGET_KEY_ENTRY(node, prop, key, cfg)                          \
  [KP_RGB_TARGET_DEST(cfg, key, KP_RGB_TARGET_BEFORE(cfg, prop, key),          \
                      KP_RGB_TARGET_KEY_NAME(cfg, key) >= 0)] =                \
      KP_RGB_TARGET_KEY_NAME(cfg, key) >= 0 ? KP_RGB_TARGET_KEY_NAME(cfg, key) \
                                            : 0,
#define KP_RGB_TARGET_LED_ENTRY(node, prop, led, cfg)                          \
  [KP_RGB_TARGET_DEST(cfg, DT_PROP_LEN_OR(node, keys, 0) + led,                \
                      KP_RGB_TARGET_BEFORE(cfg, prop, led),                    \
                      (uint32_t)DT_PROP_BY_IDX(node, prop, led) <              \
                          KP_LED_COUNT)] =                                     \
      (uint32_t)DT_PROP_BY_IDX(node, prop, led) < KP_LED_COUNT                 \
          ? (uint32_t)DT_PROP_BY_IDX(node, prop, led)                          \
          : 0,

#define KP_RGB_OVERLAY_STATIC_TARGETS(node, cfg)                               \
  KP_RGB_TARGET_KEYS(node, KP_RGB_TARGET_KEY_ENUM, cfg)                        \
  enum {                                                                       \
    cfg##_target_prefix_initial = -1,                                          \
    KP_RGB_TARGET_KEYS(node, KP_RGB_TARGET_KEY_PREFIX, cfg)                    \
        KP_RGB_TARGET_LEDS(node, KP_RGB_TARGET_LED_PREFIX, cfg)                \
            cfg##_target_count,                                                \
    cfg##_target_candidates = DT_PROP_LEN_OR(node, keys, 0) +                  \
        DT_PROP_LEN_OR(node, leds, 0),                                         \
  };                                                                           \
  static const size_t cfg##_targets[MAX(1, cfg##_target_candidates)] = {       \
      KP_RGB_TARGET_KEYS(node, KP_RGB_TARGET_KEY_ENTRY, cfg)                   \
          KP_RGB_TARGET_LEDS(node, KP_RGB_TARGET_LED_ENTRY, cfg)}
