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

#include <zephyr/sys/util.h>

#include <zmk/events/position_state_changed.h>
#include <zmk/rgb_matrix.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct kp_eff_reactive_config {
  /* Must embed the common config as its first member. */
  struct kp_rgb_effect_common_config common;
  /* Unlit-LED brightness relative to the effect colour, in percent. Effect
   * local on purpose: it is not the keyboard-wide idle brightness. */
  uint8_t background_brightness;
};

struct kp_eff_reactive_data {
  struct kp_rgb_effect_common_data common;
  uint8_t levels[KP_LED_COUNT];
};

static void kp_eff_reactive_render(const struct device *dev, struct kp_rgb_frame *f) {
  struct kp_eff_reactive_data *data = dev->data;
  const struct kp_eff_reactive_config *cfg = dev->config;
  uint32_t period = kp_rgb_effect_period(dev);
  uint32_t decay = MAX(255u * f->elapsed / period, 1u);
  uint8_t pct = kp_rgb_brightness_pct(f);
  struct kp_rgb_hsb base = kp_rgb_hex_to_hsb(data->common.color_hex);
  uint8_t floor_b = KP_RGB_SCALE(base.b, cfg->background_brightness);

  for (size_t i = 0; i < f->count; i++) {
    uint8_t level = data->levels[i];
    struct kp_rgb_hsb hsb = base;

    hsb.b = MAX((uint8_t)((uint32_t)base.b * level / 255u), floor_b);
    f->pixels[i] = kp_rgb_hsb_to_rgb(kp_rgb_hsb_scale(hsb, pct));

    /* Decay after rendering so a fresh hit shows at full brightness. */
    data->levels[i] = level > decay ? (uint8_t)(level - decay) : 0;
  }
}

static void kp_eff_reactive_event(const struct device *dev, const zmk_event_t *eh) {
  const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
  if (ev == NULL || !ev->state) {
    return;
  }

  size_t led = kp_rgb_led_for_position(ev->position);
  if (led != SIZE_MAX) {
    ((struct kp_eff_reactive_data *)dev->data)->levels[led] = 255;
  }
}

#define KP_EFF_REACTIVE_DEFINE(inst)                                           \
  static const struct kp_eff_reactive_config kp_eff_reactive_##inst##_cfg = {  \
      .common = {.index = DT_PROP(DT_DRV_INST(inst), index)},                  \
      .background_brightness = DT_PROP_OR(DT_DRV_INST(inst), background_brightness, 10), \
  };                                                                           \
  static struct kp_eff_reactive_data kp_eff_reactive_##inst##_data = {         \
      .common =                                                                \
          {                                                                    \
              .color_hex = DT_PROP_OR(DT_DRV_INST(inst), color, 0xFFFFFF),     \
              .duration_ms = DT_PROP_OR(DT_DRV_INST(inst), duration, 0),       \
          },                                                                   \
  };                                                                           \
  KP_RGB_EFFECT_DEFINE(DT_DRV_INST(inst), kp_eff_reactive_render,              \
                       kp_eff_reactive_event, kp_eff_reactive_##inst)

DT_INST_FOREACH_STATUS_OKAY(KP_EFF_REACTIVE_DEFINE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
