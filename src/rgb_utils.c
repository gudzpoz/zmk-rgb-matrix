#include "rgb_matrix_internal.h"

#if KP_LED_COUNT > 0
#define KP_RGB_IDENTITY_INDEX(i, _) i
const size_t kp_rgb_all_targets[KP_LED_COUNT] = {
    LISTIFY(KP_LED_COUNT, KP_RGB_IDENTITY_INDEX, (,), _)};
#else
const size_t kp_rgb_all_targets[1] = {0};
#endif

void kp_rgb_overlay_targets_init(void) {
  const struct device *const *overlays = kp_rgb_overlay_list();
  for (size_t i = 0; i < KP_RGB_OVERLAY_COUNT; i++) {
    const struct kp_rgb_overlay_common_config *cfg = overlays[i]->config;
    struct kp_rgb_overlay_common_data *data = overlays[i]->data;
    data->led_count = 0;
    if (cfg->all_leds) continue;
    for (size_t j = 0; j < cfg->key_count; j++) {
      size_t led = kp_rgb_led_for_position(cfg->keys[j]);
      if (led != SIZE_MAX) data->leds[data->led_count++] = led;
    }
    for (size_t j = 0; j < cfg->led_count; j++) {
      if (cfg->leds[j] < KP_LED_COUNT)
        data->leds[data->led_count++] = cfg->leds[j];
    }
  }
}

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
