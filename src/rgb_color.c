/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure HSB/RGB colour math, split out of rgb_utils.c so a host build can
 * unit-test it (tests/unit) without linking the ZMK app.
 */

#include <zephyr/sys/util.h>

#include <zmk/rgb_color.h>

struct led_rgb kp_rgb_hsb_to_rgb(struct kp_rgb_hsb color) {
  uint32_t h = color.h % KP_RGB_HUE_MAX;
  uint32_t v = (uint32_t)color.b * 255u / KP_RGB_BRT_MAX;
  uint32_t s = (uint32_t)color.s * 255u / KP_RGB_SAT_MAX;
  uint32_t i = h / 60u;
  uint32_t f = (h % 60u) * 255u / 60u;
  uint32_t p = v * (255u - s) / 255u;
  uint32_t q = v * (255u - (f * s) / 255u) / 255u;
  uint32_t t = v * (255u - ((255u - f) * s) / 255u) / 255u;

  uint32_t r, g, b;

  switch (i) {
  case 0:
    r = v;
    g = t;
    b = p;
    break;
  case 1:
    r = q;
    g = v;
    b = p;
    break;
  case 2:
    r = p;
    g = v;
    b = t;
    break;
  case 3:
    r = p;
    g = q;
    b = v;
    break;
  case 4:
    r = t;
    g = p;
    b = v;
    break;
  default:
    r = v;
    g = p;
    b = q;
    break;
  }

  return (struct led_rgb){.r = (uint8_t)r, .g = (uint8_t)g, .b = (uint8_t)b};
}

struct kp_rgb_hsb kp_rgb_hsb_scale(struct kp_rgb_hsb color, uint8_t pct) {
  color.b = KP_RGB_SCALE(color.b, MIN(pct, KP_RGB_BRT_MAX));
  return color;
}

struct led_rgb kp_rgb_rgb_scale(struct led_rgb rgb, uint8_t pct) {
  pct = MIN(pct, 100);
  return (struct led_rgb){
      .r = KP_RGB_SCALE(rgb.r, pct),
      .g = KP_RGB_SCALE(rgb.g, pct),
      .b = KP_RGB_SCALE(rgb.b, pct),
  };
}

struct led_rgb kp_rgb_rgb_mix(struct led_rgb base, struct led_rgb over, uint8_t pct) {
  pct = MIN(pct, 100);
  return (struct led_rgb){
      .r = (uint8_t)(base.r + ((int32_t)over.r - (int32_t)base.r) * pct / 100),
      .g = (uint8_t)(base.g + ((int32_t)over.g - (int32_t)base.g) * pct / 100),
      .b = (uint8_t)(base.b + ((int32_t)over.b - (int32_t)base.b) * pct / 100),
  };
}
