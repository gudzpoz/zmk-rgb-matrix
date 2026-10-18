#include "rgb_matrix_internal.h"

#if KP_LED_COUNT > 0
#define KP_RGB_IDENTITY_INDEX(i, _) i
const size_t kp_rgb_all_targets[KP_LED_COUNT] = {
    LISTIFY(KP_LED_COUNT, KP_RGB_IDENTITY_INDEX, (,), _)};
#else
const size_t kp_rgb_all_targets[1] = {0};
#endif

const size_t *kp_rgb_overlay_targets(const struct device *dev) {
  const struct kp_rgb_overlay_common_config *cfg = dev->config;
  const struct kp_rgb_overlay_common_data *data = dev->data;
  return cfg->all_leds ? kp_rgb_all_targets : data->leds;
}

size_t kp_rgb_overlay_target_count(const struct device *dev) {
  const struct kp_rgb_overlay_common_config *cfg = dev->config;
  const struct kp_rgb_overlay_common_data *data = dev->data;
  return cfg->all_leds ? KP_LED_COUNT : data->led_count;
}

/* Appends `led` unless it is already present, so an LED named twice (or by both
 * the key and raw-LED lists) is painted once. */
static void kp_rgb_target_add(size_t *out, size_t *n, size_t led) {
  for (size_t j = 0; j < *n; j++) {
    if (out[j] == led) {
      return;
    }
  }

  out[(*n)++] = led;
}

size_t kp_rgb_resolve_targets(const uint32_t *keys, size_t keys_len,
                              const uint32_t *leds, size_t leds_len, size_t *out,
                              size_t out_max) {
  size_t n = 0;

  for (size_t i = 0; i < keys_len && n < out_max; i++) {
    size_t led = kp_rgb_led_for_position(keys[i]);
    if (led != SIZE_MAX) {
      kp_rgb_target_add(out, &n, led);
    }
  }

  for (size_t i = 0; i < leds_len && n < out_max; i++) {
    if (leds[i] < KP_LED_COUNT) {
      kp_rgb_target_add(out, &n, leds[i]);
    }
  }

  return n;
}

void kp_rgb_overlay_paint(const struct kp_rgb_frame *frame, const size_t *leds,
                          size_t led_count, struct led_rgb color, uint8_t strength) {
  for (size_t i = 0; i < led_count; i++) {
    frame->pixels[leds[i]] = kp_rgb_rgb_mix(frame->pixels[leds[i]], color, strength);
  }
}

void kp_rgb_overlay_paint_pixels(const struct kp_rgb_frame *frame, const size_t *leds,
                                 size_t led_count, const struct led_rgb *src,
                                 uint8_t strength) {
  for (size_t i = 0; i < led_count; i++) {
    frame->pixels[leds[i]] =
        kp_rgb_rgb_mix(frame->pixels[leds[i]], src[leds[i]], strength);
  }
}
