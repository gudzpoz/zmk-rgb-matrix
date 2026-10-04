#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>

#include "rgb_matrix_internal.h"

/* Appends `led` unless it is already present, so an LED named twice (or by both
 * the key and raw-LED lists) is painted once. */
static void kp_rgb_target_add(size_t *out, size_t *n, size_t out_max,
                              size_t led) {
  if (*n >= out_max) {
    return;
  }

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
      kp_rgb_target_add(out, &n, out_max, led);
    }
  }

  for (size_t i = 0; i < leds_len && n < out_max; i++) {
    if (leds[i] < KP_LED_COUNT) {
      kp_rgb_target_add(out, &n, out_max, leds[i]);
    }
  }

  return n;
}

void kp_rgb_overlay_paint(struct kp_rgb_frame *frame, const size_t *leds,
                          size_t led_count, struct led_rgb color, uint8_t strength) {
  struct led_rgb painted = kp_rgb_rgb_scale(color, kp_rgb_brightness_pct(frame));

  for (size_t i = 0; i < led_count; i++) {
    if (leds[i] < frame->count) {
      frame->pixels[leds[i]] = kp_rgb_rgb_mix(frame->pixels[leds[i]], painted, strength);
    }
  }
}

void kp_rgb_overlay_paint_pixels(struct kp_rgb_frame *frame, const size_t *leds,
                                 size_t led_count, const struct led_rgb *src,
                                 uint8_t strength) {
  /* The source is already at frame scale: a composited effect dims itself by
   * the frame brightness. */
  for (size_t i = 0; i < led_count; i++) {
    if (leds[i] < frame->count) {
      frame->pixels[leds[i]] =
          kp_rgb_rgb_mix(frame->pixels[leds[i]], src[leds[i]], strength);
    }
  }
}

/* State words are written by the central control worker or received split commands.
 * Local overlay gates are published by the local control worker.
 */
#define KP_OVERLAY_SLOTS MAX(1, KP_RGB_OVERLAY_COUNT)

static const struct device *kp_overlay_registry[KP_OVERLAY_SLOTS];
static volatile uint16_t kp_overlay_state[KP_RGB_OVERLAY_WORDS];

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* What every peripheral was last told; dispatch() sends only differing words. A
 * mirror rather than a changed-word list means a change also survives a tick
 * that skipped its dispatch: `state != sent` stays true. Central only. */
static uint16_t kp_overlay_sent[KP_RGB_OVERLAY_WORDS];

#endif

void kp_rgb_overlay_register(const struct device *dev) {
  const struct kp_rgb_overlay_common_data *data = dev->data;
  if (data == NULL || data->index >= KP_OVERLAY_SLOTS) {
    return;
  }
  kp_overlay_registry[data->index] = dev;
}

/* Every registered overlay, in container order; slots not yet registered are
 * NULL and skipped by the callers. */
const struct device *const *kp_rgb_overlay_list(void) {
  return kp_overlay_registry;
}

size_t kp_rgb_overlay_count(void) { return KP_RGB_OVERLAY_COUNT; }

uint16_t kp_rgb_overlay_word_count(void) { return KP_RGB_OVERLAY_WORDS; }

bool kp_rgb_overlay_set_word(uint16_t word, uint16_t value) {
  if (word >= KP_RGB_OVERLAY_WORDS || kp_overlay_state[word] == value) {
    return false;
  }
  kp_overlay_state[word] = value;
  return true;
}

uint16_t kp_rgb_overlay_get_word(uint16_t word) {
  return word < KP_RGB_OVERLAY_WORDS ? kp_overlay_state[word] : 0;
}

bool kp_rgb_overlay_covers_all(const struct device *dev) {
  const struct kp_rgb_overlay_common_config *cfg = dev->config;
  /* `all-leds` at `opacity` 100 replaces every pixel, so the engine may skip
   * what is painted below. */
  return cfg->all_leds && cfg->opacity >= 100;
}

bool kp_rgb_overlay_gate(const struct device *dev) {
  const struct kp_rgb_overlay_common_data *data = dev->data;

  if (data->local) {
    return data->gate;
  }

  /* Central-evaluated: the bit was filled by refresh() on the central or by the
   * RGB_OVL_STATE_CMD handler on a peripheral. */
  return (kp_overlay_state[data->index / 16] >> (data->index % 16)) & 1u;
}

void kp_rgb_overlay_conditions_init(void) {
  for (size_t i = 0; i < ARRAY_SIZE(kp_overlay_registry); i++) {
    const struct device *dev = kp_overlay_registry[i];
    if (!dev) continue;
    const struct kp_rgb_overlay_api *api = dev->api;
    const struct kp_rgb_overlay_common_data *data = dev->data;
    if (data->local || !IS_ENABLED(CONFIG_ZMK_SPLIT) ||
        IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) {
      kp_rgb_condition_require(api->condition);
    }
  }
}

bool kp_rgb_overlay_refresh(void) {
  bool changed = false;
  uint16_t next[KP_RGB_OVERLAY_WORDS] = {0};

  for (size_t i = 0; i < ARRAY_SIZE(kp_overlay_registry); i++) {
    const struct device *dev = kp_overlay_registry[i];
    if (!dev) continue;
    const struct kp_rgb_overlay_api *api = dev->api;
    struct kp_rgb_overlay_common_data *data = dev->data;
    if (!data->local && IS_ENABLED(CONFIG_ZMK_SPLIT) &&
        !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) continue;

    bool active = kp_rgb_condition_valid(api->condition) &&
                  (!api->condition || kp_rgb_condition_value(api->condition));
    changed |= data->gate != active;
    data->gate = active;
    if (!data->local && active) {
      next[data->index / 16] |= (uint16_t)BIT(data->index % 16);
    }
  }

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  for (uint16_t w = 0; w < KP_RGB_OVERLAY_WORDS; w++) {
    changed |= kp_overlay_state[w] != next[w];
    kp_overlay_state[w] = next[w];
  }
#else
  ARG_UNUSED(next);
#endif

  return changed;
}

void kp_rgb_overlay_dispatch(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};
  for (uint16_t w = 0; w < KP_RGB_OVERLAY_WORDS; w++) {
    uint16_t bits = kp_rgb_overlay_get_word(w);
    if (bits == kp_overlay_sent[w]) {
      continue;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = kp_rgb_controller.dev->name,
        .param1 = RGB_OVL_STATE_CMD,
        .param2 = RGB_OVL_STATE_VAL(w, bits),
    };
    zmk_behavior_invoke_binding(&binding, event, true);
    /* GLOBAL locality reports no per-peripheral result, so a split drop is
     * invisible here: this records the local attempt, and the connect-time sync
     * is what repairs a peripheral that missed it. */
    kp_overlay_sent[w] = bits;
  }
#endif
}
