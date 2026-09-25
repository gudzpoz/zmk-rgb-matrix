/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * "heatmap" effect: each key's colour reflects how recently (and how often) it was
 * pressed, decaying over time. Port of QMK's TYPING_HEATMAP.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_heatmap

#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zmk/events/position_state_changed.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_matrix_math.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_heatmap_config {
  struct kp_rgb_effect_common_config common;
  uint16_t decrease_delay_ms; /* heat lost per this many ms */
  uint16_t spread;            /* layout-unit radius for neighbour heating */
  uint8_t area_limit;         /* cap on neighbour heat contribution */
  uint8_t increase_step;      /* shades added per keypress */
  bool slim;                  /* skip neighbour spread */
};

struct kp_eff_heatmap_data {
  struct kp_rgb_effect_common_data common;
  uint8_t temp[KP_LED_COUNT]; /* 0..255 heat per LED */
};


static void kp_eff_heatmap_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_heatmap_data *data = dev->data;
  const struct kp_eff_heatmap_config *cfg = dev->config;
  uint8_t pct = kp_rgb_brightness_pct(f);
  struct kp_rgb_hsb base = data->common.color;

  /* Heat decay: lose `elapsed / decrease_delay` shades this tick. */
  uint32_t loss = f->elapsed / MAX(cfg->decrease_delay_ms, 1u);
  for (size_t i = 0; i < f->count; i++) {
    data->temp[i] = loss >= data->temp[i] ? 0 : (uint8_t)(data->temp[i] - loss);
  }

  for (size_t i = 0; i < f->count; i++) {
    uint8_t t = data->temp[i];
    if (t == 0) {
      f->pixels[i] = (struct led_rgb){0, 0, 0};
      continue;
    }
    /* Cold (t=0) -> blue (h=240), hot (t=255) -> red (h=0). */
    struct kp_rgb_hsb hsb = {
        .h = (uint16_t)((255u - t) * 240u / 255u),
        .s = KP_RGB_SAT_MAX,
        .b = (uint8_t)((uint32_t)base.b * t / 255u),
    };
    f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));
  }
}

static void kp_eff_heatmap_event(const struct device *dev,
                                 const struct zmk_position_state_changed *ev) {
  if (!ev->state) {
    return; /* only a press adds heat */
  }

  struct kp_eff_heatmap_data *data = dev->data;
  const struct kp_eff_heatmap_config *cfg = dev->config;
  size_t led = kp_rgb_led_for_position(ev->position);
  const struct kp_rgb_coord *origin = kp_rgb_led_coord(led);
  if (origin == NULL) {
    return;
  }

  data->temp[led] = MIN(255u, (uint32_t)data->temp[led] + cfg->increase_step);
  if (cfg->slim || cfg->spread == 0) {
    return;
  }
  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    if (i == led) {
      continue;
    }
    const struct kp_rgb_coord *c = kp_rgb_led_coord(i);
    if (c == NULL) {
      continue;
    }
    int32_t dx = (int32_t)c->x - origin->x;
    int32_t dy = (int32_t)c->y - origin->y;
    uint32_t dist = kp_rgb_isqrt((uint32_t)(dx * dx + dy * dy));
    if (dist > cfg->spread) {
      continue;
    }
    uint32_t contrib =
        (uint32_t)cfg->increase_step * (cfg->spread - dist) / cfg->spread;
    contrib = MIN(contrib, cfg->area_limit);
    data->temp[i] = MIN(255u, (uint32_t)data->temp[i] + contrib);
  }
}

#define KP_EFF_HEATMAP_DEFINE(inst)                                            \
  BUILD_ASSERT(DT_PROP_OR(DT_DRV_INST(inst), decrease_delay_ms, 25) >= 0 &&    \
                   DT_PROP_OR(DT_DRV_INST(inst), decrease_delay_ms, 25) <=     \
                       UINT16_MAX,                                             \
               "heatmap decrease-delay-ms must fit a nonnegative uint16_t");   \
  BUILD_ASSERT(DT_PROP_OR(DT_DRV_INST(inst), spread, 40) >= 0 &&               \
                   DT_PROP_OR(DT_DRV_INST(inst), spread, 40) <= UINT16_MAX,    \
               "heatmap spread must fit a nonnegative uint16_t");              \
  BUILD_ASSERT(DT_PROP_OR(DT_DRV_INST(inst), area_limit, 16) >= 0 &&           \
                   DT_PROP_OR(DT_DRV_INST(inst), area_limit, 16) <= UINT8_MAX, \
               "heatmap area-limit must fit a nonnegative uint8_t");           \
  BUILD_ASSERT(DT_PROP_OR(DT_DRV_INST(inst), increase_step, 32) >= 0 &&        \
                   DT_PROP_OR(DT_DRV_INST(inst), increase_step, 32) <=         \
                       UINT8_MAX,                                              \
               "heatmap increase-step must fit a nonnegative uint8_t");        \
  static const struct kp_eff_heatmap_config kp_eff_heatmap_##inst##_cfg = {    \
      .common = {.index = KP_RGB_EFFECT_INDEX(inst)},                          \
      .decrease_delay_ms =                                                     \
          DT_PROP_OR(DT_DRV_INST(inst), decrease_delay_ms, 25),                \
      .spread = DT_PROP_OR(DT_DRV_INST(inst), spread, 40),                     \
      .area_limit = DT_PROP_OR(DT_DRV_INST(inst), area_limit, 16),             \
      .increase_step = DT_PROP_OR(DT_DRV_INST(inst), increase_step, 32),       \
      .slim = DT_PROP_OR(DT_DRV_INST(inst), slim, 0),                          \
  };                                                                           \
  static struct kp_eff_heatmap_data kp_eff_heatmap_##inst##_data = {           \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),             \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_heatmap_render,               \
                       kp_eff_heatmap_event, kp_eff_heatmap_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_HEATMAP_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
