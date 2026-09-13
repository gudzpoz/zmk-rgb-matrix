/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * "static" effect: paints each LED with a user-supplied colour from the
 * `led-colors` array, indexed by LED chain index. This is a strict superset of
 * QMK's ALPHAS_MODS (assign a different colour to alpha vs modifier keys) and any
 * other static per-key art.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_static

#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_static_config {
  struct kp_rgb_effect_common_config common;
  const uint32_t *colors; /* indexed by LED chain index */
  size_t colors_len;
};

struct kp_eff_static_data {
  struct kp_rgb_effect_common_data common;
};

#define KP_EFF_STATIC_COLOR(idx, node) DT_PROP_BY_IDX(node, led_colors, idx),

#define KP_EFF_STATIC_DEFINE(inst)                                             \
  COND_CODE_1(                                                                \
      DT_NODE_HAS_PROP(DT_DRV_INST(inst), led_colors),                        \
      (static const uint32_t kp_eff_static_##inst##_colors[] = {              \
        LISTIFY(DT_PROP_LEN(DT_DRV_INST(inst), led_colors), KP_EFF_STATIC_COLOR, \
                (), DT_DRV_INST(inst))                                         \
      };),                                                                    \
      (static const uint32_t kp_eff_static_##inst##_colors[] = {0};))         \
  BUILD_ASSERT(ARRAY_SIZE(kp_eff_static_##inst##_colors) <= KP_LED_COUNT,      \
               "led-colors has more entries than LEDs");                       \
  static const struct kp_eff_static_config kp_eff_static_##inst##_cfg = {      \
      .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                  \
      .colors = kp_eff_static_##inst##_colors,                                 \
      .colors_len = ARRAY_SIZE(kp_eff_static_##inst##_colors),                 \
  };                                                                           \
  static struct kp_eff_static_data kp_eff_static_##inst##_data = {             \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),             \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_static_render, NULL,          \
                       kp_eff_static_##inst)

static void kp_eff_static_render(const struct device *dev, struct kp_rgb_frame *f) {
  const struct kp_eff_static_config *cfg = dev->config;
  uint8_t pct = kp_rgb_brightness_pct(f);

  for (size_t i = 0; i < f->count; i++) {
    uint32_t hex = (i < cfg->colors_len) ? cfg->colors[i] : 0;
    struct led_rgb rgb = kp_hex_to_rgb(hex);
    f->pixels[i] = kp_rgb_rgb_scale(rgb, pct);
  }
}

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_STATIC_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
