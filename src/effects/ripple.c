/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Built-in "ripple" effect: a keypress spawns an expanding, hue-shifting ring
 * centred on that key's LED. This effect owns its trigger pool and its clock,
 * and asks the engine which LED sits under the pressed key.
 */

#define DT_DRV_COMPAT keypaw_rgb_matrix_ripple

#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>
#include <zmk/rgb_matrix_math.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KP_RIPPLE_TRIGGERS 8
/* Ring half-thickness, relative to the ripple radius, in percent. */
#define KP_RIPPLE_WIDTH 60
/* Share of the ripple's life spent fading out at the end, in percent. Without
 * it the ring is culled at full brightness the moment it reaches the board. */
#define KP_RIPPLE_FADE_PCT 25

struct kp_eff_ripple_config {
  struct kp_rgb_effect_common_config common;
  /* Unlit-LED brightness relative to the effect colour, in percent. Effect
   * local on purpose: it is not the keyboard-wide idle brightness. */
  uint8_t background_brightness;
};

struct kp_rgb_ripple_trigger {
  bool used;
  int64_t start_ms;
  int32_t x;
  int32_t y;
};

struct kp_eff_ripple_data {
  struct kp_rgb_effect_common_data common;
  struct kp_rgb_ripple_trigger triggers[KP_RIPPLE_TRIGGERS];
};

static void kp_eff_ripple_add(struct led_rgb *acc, struct led_rgb add) {
  acc->r = (uint8_t)MIN(255u, (uint32_t)acc->r + add.r);
  acc->g = (uint8_t)MIN(255u, (uint32_t)acc->g + add.g);
  acc->b = (uint8_t)MIN(255u, (uint32_t)acc->b + add.b);
}

static bool kp_eff_ripple_render(const struct device *dev, const struct kp_rgb_frame *f) {
  struct kp_eff_ripple_data *data = dev->data;
  const struct kp_eff_ripple_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  struct kp_rgb_hsb base = data->common.color;

  bool evolving = false;

  struct kp_rgb_hsb bg = base;
  bg.b = KP_RGB_SCALE(base.b, cfg->background_brightness);
  struct led_rgb bg_rgb = kp_rgb_hsb_to_rgb(bg);
  for (size_t t = 0; t < f->target_count; t++) {
    size_t i = f->targets[t];
    f->pixels[i] = bg_rgb;
  }

  for (size_t t = 0; t < KP_RIPPLE_TRIGGERS; t++) {
    struct kp_rgb_ripple_trigger *trigger = &data->triggers[t];
    if (!trigger->used) {
      continue;
    }

    uint64_t age = f->now_ms > trigger->start_ms
                       ? (uint64_t)f->now_ms - (uint64_t)trigger->start_ms
                       : 0;
    if (age >= period) {
      trigger->used = false;
      continue;
    }

    evolving = true;
    int32_t radius = (int32_t)((uint64_t)f->board_length * age / period);
    if (radius <= 0) {
      continue;
    }
    int32_t half_band = MAX(radius * KP_RIPPLE_WIDTH / 100, 1);

  /* Ease the ring out so it dims away as the trigger expires instead of being
   * cut off. */
    uint32_t fade_window = MAX(period * KP_RIPPLE_FADE_PCT / 100u, 1u);
    uint8_t fade = age + fade_window < period
                       ? 255u
                       : (uint8_t)(255u * (period - age) / fade_window);

    for (size_t target = 0; target < f->target_count; target++) {
      size_t i = f->targets[target];
      int32_t dx = (int32_t)f->coords[i].x - trigger->x;
      int32_t dy = (int32_t)f->coords[i].y - trigger->y;
      /* Linear distance, as every other spatial effect uses: comparing a squared
       * distance against the linear `half_band` collapses the ring to a sliver
       * narrower than the LED pitch. */
      int32_t delta = (int32_t)kp_rgb_isqrt((uint32_t)(dx * dx + dy * dy)) - radius;
      if (delta < 0) {
        delta = -delta;
      }
      if (delta > half_band) {
        continue;
      }

      uint8_t amp = (uint8_t)(255u - (uint32_t)delta * 255u / (uint32_t)half_band);
      amp = (uint8_t)((uint32_t)amp * fade / 255u);
      struct kp_rgb_hsb hsb = base;
      hsb.h = (uint16_t)((hsb.h + (uint32_t)radius * KP_RGB_HUE_MAX / f->board_length) %
                         KP_RGB_HUE_MAX);
      hsb.b = (uint8_t)((uint32_t)base.b * amp / 255u);
      kp_eff_ripple_add(&f->pixels[i], kp_rgb_hsb_to_rgb(hsb));
    }
  }
  return evolving;
}

static bool kp_eff_ripple_event(const struct device *dev,
                                const struct kp_rgb_key_event *ev) {
  if (!ev->pressed) {
    return false;
  }

  size_t led = kp_rgb_led_for_position(ev->position);
  const struct kp_rgb_coord *origin = kp_rgb_led_coord(led);
  if (origin == NULL) {
    return false;
  }

  struct kp_eff_ripple_data *data = dev->data;
  for (size_t t = 0; t < KP_RIPPLE_TRIGGERS; t++) {
    if (data->triggers[t].used) {
      continue;
    }
    data->triggers[t] = (struct kp_rgb_ripple_trigger){
        .used = true,
        .start_ms = ev->timestamp_ms,
        .x = origin->x,
        .y = origin->y,
    };
    return true;
  }
  return false;
}

static void kp_eff_ripple_reset(const struct device *dev, int64_t now_ms) {
  ARG_UNUSED(now_ms);
  struct kp_eff_ripple_data *data = dev->data;
  struct kp_rgb_effect_common_data common = data->common;
  *data = (struct kp_eff_ripple_data){.common = common};
}

static void kp_eff_ripple_set_active(const struct device *dev, bool active, int64_t now_ms) {
  if (!active) {
    kp_eff_ripple_reset(dev, now_ms);
  }
}

static const struct kp_rgb_effect_callbacks kp_eff_ripple_callbacks = {
    .render = kp_eff_ripple_render,
    .on_event = kp_eff_ripple_event,
    .set_active = kp_eff_ripple_set_active,
    .reset = kp_eff_ripple_reset,
};

#define KP_EFF_RIPPLE_DEFINE(inst)                                             \
  static const struct kp_eff_ripple_config kp_eff_ripple_##inst##_cfg = {      \
      .common = {.index = KP_RGB_EFFECT_INDEX(inst)},                          \
      .background_brightness = (uint8_t)CLAMP(                                 \
          DT_PROP_OR(DT_DRV_INST(inst), background_brightness, 10), 0, 100),   \
  };                                                                           \
  static struct kp_eff_ripple_data kp_eff_ripple_##inst##_data = {             \
      .common =                                                                \
          {                                                                    \
              .color = KP_RGB_HSB_FROM_HEX(                                    \
                  DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF)),             \
              .duration_ms = MIN(                                              \
                  DT_PROP_OR(DT_DRV_INST(inst), duration, 0), UINT16_MAX),     \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), &kp_eff_ripple_callbacks,            \
                       kp_eff_ripple_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_RIPPLE_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
