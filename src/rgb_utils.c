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

void kp_rgb_overlay_paint_pixels(const struct kp_rgb_frame *frame, const size_t *leds,
                                 size_t led_count, const struct led_rgb *src,
                                 uint8_t strength) {
  for (size_t i = 0; i < led_count; i++) {
    frame->pixels[leds[i]] =
        kp_rgb_rgb_mix(frame->pixels[leds[i]], src[leds[i]], strength);
  }
}
