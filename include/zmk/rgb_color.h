/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <zephyr/drivers/led_strip.h>

/* HSB colour model and its conversion to the strip's RGB. Deliberately free of
 * ZMK and devicetree dependencies so a host build can unit-test it. */
#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define KP_RGB_BRT_MAX 100
struct kp_rgb_hsb {
  uint16_t h; /* 0..360 */
  uint8_t s;  /* 0..100 */
  uint8_t b;  /* 0..100 */
};

/* Compile-time 0xRRGGBB -> struct kp_rgb_hsb initializer, so an effect's preset
 * colour can stay a devicetree constant while the effect state holds HSB. */
#define KP_RGB_HEX_R(hex) ((int)(((uint32_t)(hex) >> 16) & 0xFFu))
#define KP_RGB_HEX_G(hex) ((int)(((uint32_t)(hex) >> 8) & 0xFFu))
#define KP_RGB_HEX_B(hex) ((int)((uint32_t)(hex) & 0xFFu))

#define KP_RGB_HEX_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define KP_RGB_HEX_MIN2(a, b) ((a) < (b) ? (a) : (b))
#define KP_RGB_HEX_MAX(hex)                                                    \
  KP_RGB_HEX_MAX2(KP_RGB_HEX_MAX2(KP_RGB_HEX_R(hex), KP_RGB_HEX_G(hex)),       \
                  KP_RGB_HEX_B(hex))
#define KP_RGB_HEX_MIN(hex)                                                    \
  KP_RGB_HEX_MIN2(KP_RGB_HEX_MIN2(KP_RGB_HEX_R(hex), KP_RGB_HEX_G(hex)),       \
                  KP_RGB_HEX_B(hex))
#define KP_RGB_HEX_DELTA(hex) (KP_RGB_HEX_MAX(hex) - KP_RGB_HEX_MIN(hex))

/* The +KP_RGB_HUE_MAX keeps the red-dominant term, the only one that can go
 * negative, in range before the modulo. */
#define KP_RGB_HEX_HUE(hex)                                                    \
  (KP_RGB_HEX_DELTA(hex) == 0 ? 0                                              \
   : KP_RGB_HEX_MAX(hex) == KP_RGB_HEX_R(hex)                                  \
       ? ((60 * (KP_RGB_HEX_G(hex) - KP_RGB_HEX_B(hex)) /                      \
           KP_RGB_HEX_DELTA(hex)) +                                            \
          KP_RGB_HUE_MAX) %                                                    \
             KP_RGB_HUE_MAX                                                    \
   : KP_RGB_HEX_MAX(hex) == KP_RGB_HEX_G(hex)                                  \
       ? (120 + 60 * (KP_RGB_HEX_B(hex) - KP_RGB_HEX_R(hex)) /                 \
                    KP_RGB_HEX_DELTA(hex))                                     \
       : (240 + 60 * (KP_RGB_HEX_R(hex) - KP_RGB_HEX_G(hex)) /                 \
                    KP_RGB_HEX_DELTA(hex)))

#define KP_RGB_HEX_SAT(hex)                                                    \
  (KP_RGB_HEX_MAX(hex) == 0                                                    \
       ? 0                                                                     \
       : KP_RGB_HEX_DELTA(hex) * KP_RGB_SAT_MAX / KP_RGB_HEX_MAX(hex))
#define KP_RGB_HEX_BRT(hex) (KP_RGB_HEX_MAX(hex) * KP_RGB_BRT_MAX / 255)

#define KP_RGB_HSB_FROM_HEX(hex)                                               \
  {.h = (uint16_t)KP_RGB_HEX_HUE(hex),                                         \
   .s = (uint8_t)KP_RGB_HEX_SAT(hex),                                          \
   .b = (uint8_t)KP_RGB_HEX_BRT(hex)}

struct led_rgb kp_rgb_hsb_to_rgb(struct kp_rgb_hsb color);

/* Brightness helpers. Effects render at full scale and then dim the result, so
 * that a single global brightness setting applies uniformly.
 */
#define KP_RGB_SCALE(v, pct) ((uint8_t)((uint32_t)(v) * (pct) / 100u))
struct kp_rgb_hsb kp_rgb_hsb_scale(struct kp_rgb_hsb color, uint8_t pct);
struct led_rgb kp_rgb_rgb_scale(struct led_rgb rgb, uint8_t pct);

/* Blend `over` into `base` by `pct` percent (0 keeps base, 100 replaces it).
 * Used by overlays to composite over whatever the effect rendered. */
struct led_rgb kp_rgb_rgb_mix(struct led_rgb base, struct led_rgb over,
                              uint8_t pct);

#define kp_hex_to_rgb(hex)                                                     \
  ((struct led_rgb){                                                           \
      .r = ((hex) >> 16) & 0xFF,                                               \
      .g = ((hex) >> 8) & 0xFF,                                                \
      .b = (hex) & 0xFF,                                                       \
  })
