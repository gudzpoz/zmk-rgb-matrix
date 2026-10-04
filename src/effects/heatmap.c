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
#if KP_LED_COUNT > 0
  struct kp_rgb_key_event pending[16];
  size_t pending_count;
  uint8_t temp[KP_LED_COUNT]; /* 0..255 heat per LED */
  uint32_t remainder[KP_LED_COUNT];
#endif
};


#if KP_LED_COUNT > 0
static void kp_eff_heatmap_apply(const struct device *dev,
                                 const struct kp_rgb_key_event *ev);

static void kp_eff_heatmap_advance(const struct device *dev,
                                   uint32_t elapsed_ms) {
  if (elapsed_ms == 0) {
    return;
  }
  struct kp_eff_heatmap_data *data = dev->data;
  const struct kp_eff_heatmap_config *cfg = dev->config;
  uint32_t delay = MAX(cfg->decrease_delay_ms, 1u);
  uint64_t elapsed = MIN(elapsed_ms, 255u * delay);
  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    uint64_t total = elapsed + data->remainder[i];
    uint64_t loss = total / delay;
    data->temp[i] = loss >= data->temp[i] ? 0 : data->temp[i] - loss;
    data->remainder[i] = data->temp[i] ? total % delay : 0;
  }
}
#endif

static bool kp_eff_heatmap_render(const struct device *dev,
                                  const struct kp_rgb_frame *f) {
#if KP_LED_COUNT == 0
  ARG_UNUSED(dev);
  ARG_UNUSED(f);
  return false;
#else
  struct kp_eff_heatmap_data *data = dev->data;
  uint8_t pct = kp_rgb_brightness_pct(f);
  struct kp_rgb_hsb base = data->common.color;

  int64_t end_ms = MAX(f->now_ms, 0);
  int64_t cursor = end_ms - (int64_t)MIN((uint64_t)end_ms, f->elapsed_ms);
  for (size_t i = 0; i < data->pending_count; i++) {
    const struct kp_rgb_key_event *ev = &data->pending[i];
    int64_t event_ms = CLAMP(ev->timestamp_ms, cursor, end_ms);
    kp_eff_heatmap_advance(dev, (uint32_t)(event_ms - cursor));
    kp_eff_heatmap_apply(dev, ev);
    cursor = event_ms;
  }
  kp_eff_heatmap_advance(dev, (uint32_t)(end_ms - cursor));
  data->pending_count = 0;
  bool evolving = false;

  for (size_t i = 0; i < f->count; i++) {
    uint8_t t = data->temp[i];
    evolving |= t != 0;
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
  return evolving;
#endif /* KP_LED_COUNT > 0 */
}

#if KP_LED_COUNT > 0
static void kp_eff_heatmap_apply(const struct device *dev,
                                 const struct kp_rgb_key_event *ev) {
  struct kp_eff_heatmap_data *data = dev->data;
  const struct kp_eff_heatmap_config *cfg = dev->config;
  if (cfg->increase_step == 0) {
    return;
  }
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
    if (contrib != 0) {
      data->temp[i] = MIN(255u, (uint32_t)data->temp[i] + contrib);
    }
  }
}
#endif

static bool kp_eff_heatmap_event(const struct device *dev, const struct kp_rgb_key_event *ev) {
#if KP_LED_COUNT > 0
  struct kp_eff_heatmap_data *data = dev->data;
  const struct kp_eff_heatmap_config *cfg = dev->config;
  if (!ev->pressed || cfg->increase_step == 0 ||
      kp_rgb_led_coord(kp_rgb_led_for_position(ev->position)) == NULL ||
      data->pending_count == 16) {
    return false;
  }
  data->pending[data->pending_count++] = *ev;
  return true;
#else
  ARG_UNUSED(dev);
  ARG_UNUSED(ev);
  return false;
#endif
}

static void kp_eff_heatmap_reset(const struct device *dev, int64_t now_ms) {
  ARG_UNUSED(now_ms);
  struct kp_eff_heatmap_data *data = dev->data;
  struct kp_rgb_effect_common_data common = data->common;
  *data = (struct kp_eff_heatmap_data){.common = common};
}

static void kp_eff_heatmap_set_active(const struct device *dev, bool active, int64_t now_ms) {
  if (!active) {
    kp_eff_heatmap_reset(dev, now_ms);
  }
}

static const struct kp_rgb_effect_callbacks kp_eff_heatmap_callbacks = {
    .render = kp_eff_heatmap_render,
    .on_event = kp_eff_heatmap_event,
    .set_active = kp_eff_heatmap_set_active,
    .reset = kp_eff_heatmap_reset,
};

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
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_heatmap_callbacks,           \
                       kp_eff_heatmap_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_HEATMAP_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
