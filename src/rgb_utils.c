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

size_t kp_rgb_resolve_targets(const uint32_t *keys, size_t keys_len,
                              const uint32_t *leds, size_t leds_len, size_t *out,
                              size_t out_max) {
  size_t n = 0;

  for (size_t i = 0; i < keys_len && n < out_max; i++) {
    size_t led = kp_rgb_led_for_position(keys[i]);
    if (led != SIZE_MAX) {
      out[n++] = led;
    }
  }

  for (size_t i = 0; i < leds_len && n < out_max; i++) {
    if (leds[i] < KP_LED_COUNT) {
      out[n++] = leds[i];
    }
  }

  /* Neither spec given means the whole half; an explicit spec that resolves to
   * nothing stays empty rather than silently lighting everything. */
  if (keys_len == 0 && leds_len == 0) {
    for (size_t i = 0; i < KP_LED_COUNT && n < out_max; i++) {
      out[n++] = i;
    }
  }

  return n;
}

void kp_rgb_indicator_paint(struct kp_rgb_frame *frame, const size_t *leds,
                            size_t led_count, struct led_rgb color, uint8_t strength) {
  struct led_rgb painted = kp_rgb_rgb_scale(color, kp_rgb_brightness_pct(frame));

  for (size_t i = 0; i < led_count; i++) {
    if (leds[i] < frame->count) {
      frame->pixels[leds[i]] = kp_rgb_rgb_mix(frame->pixels[leds[i]], painted, strength);
    }
  }
}

