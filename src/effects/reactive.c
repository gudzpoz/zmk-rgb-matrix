/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "reactive" effect: an LED lights when its key is pressed and fades
 * back to the effect's background brightness over one animation period.
 *
 * This effect owns its per-LED state (the fade levels) and receives key events
 * from the engine, asking it which LED sits under the pressed key.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_reactive

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>
#include <zmk/rgb_matrix_math.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* spread: point = key only, disc = filled circle, cross = row+column,
 * nexus = everything but the cross lines. */
/* palette: solid = user hue, gradient = position-based hue, complement =
 * pressed keys flash the opposite hue. */
DEFINE_DT_ENUM(kp_reactive_spread_t, spread, point, disc, cross, nexus);
DEFINE_DT_ENUM(kp_reactive_palette_t, palette, solid, gradient, complement);

struct kp_eff_reactive_config {
  struct kp_rgb_effect_common_config common;
  /* Unlit-LED brightness relative to the effect colour, in percent. Effect
   * local on purpose: it is not the keyboard-wide idle brightness. */
  uint8_t background_brightness;
  kp_reactive_spread_t spread;
  uint16_t radius; /* layout units, for disc/nexus falloff */
  bool multi;      /* accumulate several presses vs. just the latest */
  kp_reactive_palette_t palette;
};

struct kp_eff_reactive_data {
  struct kp_rgb_effect_common_data common;
#if KP_LED_COUNT > 0
  struct kp_rgb_key_event pending[16];
  size_t pending_count;
  uint8_t levels[KP_LED_COUNT];
  uint32_t remainder[KP_LED_COUNT];
#endif
};


#if KP_LED_COUNT > 0
/* Contribution (0..255) of a key hit at (px, py) onto an LED at (x, y). */
static uint8_t kp_reactive_shape(const struct kp_eff_reactive_config *cfg,
                                 uint16_t px, uint16_t py, uint16_t x, uint16_t y) {
  int32_t dx = (int32_t)x - px;
  int32_t dy = (int32_t)y - py;

  switch (cfg->spread) {
  case DT_ENUM_CONST(spread, disc): { /* disc: filled circle, brightness falls off to the radius */
    uint32_t dist = kp_rgb_isqrt((uint32_t)(dx * dx + dy * dy));
    int32_t r = cfg->radius;
    if (r <= 0 || (int32_t)dist >= r) {
      return 0;
    }
    return (uint8_t)(255u * (r - (int32_t)dist) / r);
  }
  case DT_ENUM_CONST(spread, cross): { /* cross: same column or row as the pressed key */
    uint16_t tol = 100; /* ~ one key width in layout units */
    return (abs(dx) < (int32_t)tol || abs(dy) < (int32_t)tol) ? 255 : 0;
  }
  case DT_ENUM_CONST(spread, nexus): { /* nexus: everything except the cross lines, fading outward from it */
    uint16_t tol = 100;
    int32_t ax = abs(dx);
    int32_t ay = abs(dy);
    if (ax < (int32_t)tol || ay < (int32_t)tol) {
      return 0;
    }
    /* Measured from the excluded cross, not the key: from the key, any radius
     * below the key pitch times sqrt(2) would silence the effect entirely. */
    int32_t reach = MIN(ax, ay) - (int32_t)tol;
    int32_t r = cfg->radius;
    if (r <= 0 || reach >= r) {
      return 0;
    }
    return (uint8_t)(255u * (r - reach) / r);
  }
  default: /* point */
    return (dx == 0 && dy == 0) ? 255 : 0;
  }
}
#endif /* KP_LED_COUNT > 0 */

#if KP_LED_COUNT > 0
static void kp_eff_reactive_apply(const struct device *dev, const struct kp_rgb_key_event *ev);

static void kp_eff_reactive_advance(const struct device *dev, uint32_t elapsed_ms) {
  if (elapsed_ms == 0) {
    return;
  }
  struct kp_eff_reactive_data *data = dev->data;
  uint32_t period = kp_rgb_effect_period(dev);
  uint64_t elapsed = MIN(elapsed_ms, period);
  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    uint64_t total = elapsed * 255u + data->remainder[i];
    uint64_t loss = total / period;
    data->levels[i] = loss >= data->levels[i] ? 0 : data->levels[i] - loss;
    data->remainder[i] = data->levels[i] ? total % period : 0;
  }
}
#endif

static bool kp_eff_reactive_render(const struct device *dev, const struct kp_rgb_frame *f) {
#if KP_LED_COUNT == 0
  ARG_UNUSED(dev);
  ARG_UNUSED(f);
  return false;
#else
  struct kp_eff_reactive_data *data = dev->data;
  const struct kp_eff_reactive_config *cfg = dev->config;
  int64_t end_ms = MAX(f->now_ms, 0);
  int64_t cursor = end_ms - (int64_t)MIN((uint64_t)end_ms, f->elapsed_ms);
  for (size_t i = 0; i < data->pending_count; i++) {
    const struct kp_rgb_key_event *ev = &data->pending[i];
    int64_t event_ms = CLAMP(ev->timestamp_ms, cursor, end_ms);
    kp_eff_reactive_advance(dev, (uint32_t)(event_ms - cursor));
    kp_eff_reactive_apply(dev, ev);
    cursor = event_ms;
  }
  kp_eff_reactive_advance(dev, (uint32_t)(end_ms - cursor));
  data->pending_count = 0;
  bool evolving = false;
  struct kp_rgb_hsb base = data->common.color;
  uint8_t floor_b = KP_RGB_SCALE(base.b, cfg->background_brightness);
  uint16_t span = MAX(f->board_length, 1u);

  for (size_t i = 0; i < f->count; i++) {
    evolving |= data->levels[i] != 0;
  }

  for (size_t t = 0; t < f->target_count; t++) {
    size_t i = f->targets[t];
    uint8_t level = data->levels[i];
    uint16_t hue = base.h;
    if (cfg->palette == DT_ENUM_CONST(palette, gradient)) {
      hue = (uint16_t)((base.h + (uint32_t)f->coords[i].x * KP_RGB_HUE_MAX / span) %
                       KP_RGB_HUE_MAX);
    } else if (cfg->palette == DT_ENUM_CONST(palette, complement) && level > 0) {
      /* COMPLEMENT: a lit key flashes the opposite hue; the unlit floor below
       * keeps the effect's own hue, so the flash reads against the preset. */
      hue = (uint16_t)((base.h + KP_RGB_HUE_MAX / 2) % KP_RGB_HUE_MAX);
    }
    struct kp_rgb_hsb hsb = {.h = hue, .s = base.s};
    hsb.b = MAX((uint8_t)((uint32_t)base.b * level / 255u), floor_b);
    f->pixels[i] = kp_rgb_hsb_to_rgb(hsb);
  }
  return evolving;
#endif /* KP_LED_COUNT > 0 */
}

#if KP_LED_COUNT > 0
static void kp_eff_reactive_apply(const struct device *dev,
                                  const struct kp_rgb_key_event *ev) {
  struct kp_eff_reactive_data *data = dev->data;
  const struct kp_eff_reactive_config *cfg = dev->config;
  size_t led = kp_rgb_led_for_position(ev->position);
  const struct kp_rgb_coord *origin = kp_rgb_led_coord(led);
  if (origin == NULL) {
    return;
  }

  /* Single-press modes reset every LED first so only the latest hit is live;
   * multi-press modes accumulate several hits additively. */
  if (!cfg->multi) {
    memset(data->levels, 0, sizeof(data->levels));
    memset(data->remainder, 0, sizeof(data->remainder));
  }
  for (size_t i = 0; i < KP_LED_COUNT; i++) {
    const struct kp_rgb_coord *c = kp_rgb_led_coord(i);
    if (c == NULL) {
      continue;
    }
    uint8_t contrib = kp_reactive_shape(cfg, origin->x, origin->y, c->x, c->y);
    if (contrib == 0) {
      continue;
    }
    if (cfg->multi) {
      data->levels[i] = MIN(255u, (uint32_t)data->levels[i] + contrib);
    } else {
      data->levels[i] = contrib;
    }
  }
}
#endif

static bool kp_eff_reactive_event(const struct device *dev, const struct kp_rgb_key_event *ev) {
#if KP_LED_COUNT > 0
  struct kp_eff_reactive_data *data = dev->data;
  if (!ev->pressed || kp_rgb_led_coord(kp_rgb_led_for_position(ev->position)) == NULL ||
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

static void kp_eff_reactive_reset(const struct device *dev, int64_t now_ms) {
  ARG_UNUSED(now_ms);
  struct kp_eff_reactive_data *data = dev->data;
  struct kp_rgb_effect_common_data common = data->common;
  *data = (struct kp_eff_reactive_data){.common = common};
}

static void kp_eff_reactive_set_active(const struct device *dev, bool active, int64_t now_ms) {
  if (!active) {
    kp_eff_reactive_reset(dev, now_ms);
  }
}

static const struct kp_rgb_effect_callbacks kp_eff_reactive_callbacks = {
    .render = kp_eff_reactive_render,
    .on_event = kp_eff_reactive_event,
    .set_active = kp_eff_reactive_set_active,
    .reset = kp_eff_reactive_reset,
};

#define KP_EFF_REACTIVE_DEFINE(inst)                                           \
  static const struct kp_eff_reactive_config kp_eff_reactive_##inst##_cfg = {  \
      KP_RGB_EFFECT_COMMON_CONFIG(inst),                                       \
      .background_brightness = (uint8_t)CLAMP(                                 \
          DT_PROP_OR(DT_DRV_INST(inst), background_brightness, 10), 0, 100),   \
      .spread = CONV_DT_ENUM(inst, spread),                                    \
      .radius = DT_PROP_OR(DT_DRV_INST(inst), spread_radius, 250),             \
      .multi = DT_PROP_OR(DT_DRV_INST(inst), multi, 0),                        \
      .palette = CONV_DT_ENUM(inst, palette),                                  \
  };                                                                           \
  static struct kp_eff_reactive_data kp_eff_reactive_##inst##_data = {         \
      KP_RGB_EFFECT_COMMON_DATA(inst, 0xFFFFFF, 0)};                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_reactive_callbacks,          \
                       kp_eff_reactive_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_REACTIVE_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
