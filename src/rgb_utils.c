#include <zmk/rgb_matrix.h>

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

uint32_t kp_rgb_hsb_to_hex(struct kp_rgb_hsb color) {
  struct led_rgb rgb = kp_rgb_hsb_to_rgb(color);

  return ((uint32_t)rgb.r << 16) | ((uint32_t)rgb.g << 8) | rgb.b;
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

struct kp_rgb_hsb kp_rgb_hex_to_hsb(uint32_t hex) {
  uint32_t r = (hex >> 16) & 0xFF;
  uint32_t g = (hex >> 8) & 0xFF;
  uint32_t b = hex & 0xFF;

  uint32_t max = MAX(r, MAX(g, b));
  uint32_t min = MIN(r, MIN(g, b));
  uint32_t d = max - min;

  struct kp_rgb_hsb hsb = {
    .h = 0,
    .s = max == 0 ? 0 : (uint8_t)(d * KP_RGB_SAT_MAX / max),
    .b = (uint8_t)(max * KP_RGB_BRT_MAX / 255),
  };

  if (d == 0) {
    return hsb;
  }

  int32_t hh;

  if (max == r) {
    hh = 60 * ((int32_t)g - (int32_t)b) / (int32_t)d;
  } else if (max == g) {
    /* Multiply by 60 before dividing: |b - r| < d so the inner division
     * alone would truncate to 0 and collapse every green-dominant colour to
     * exactly 120 degrees. */
    hh = 120 + 60 * ((int32_t)b - (int32_t)r) / (int32_t)d;
  } else {
    hh = 240 + 60 * ((int32_t)r - (int32_t)g) / (int32_t)d;
  }

  if (hh < 0) {
    hh += KP_RGB_HUE_MAX;
  }

  hsb.h = (uint16_t)(hh % KP_RGB_HUE_MAX);

  return hsb;
}
