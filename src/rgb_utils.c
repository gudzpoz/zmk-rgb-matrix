#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>

#include "rgb_matrix_internal.h"

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

/* -------------------------------------------------------------------------
 * Overlay state
 *
 * One on/off bit per overlay ordinal (its position under the
 * keypaw,rgb-overlays container), held in uint16_t words. The central fills it
 * by evaluating each remote kind's `active` once a tick and pushes the words
 * whose bits changed over the split link; a peripheral fills it from that
 * command. A locally determined kind is never in it -- its gate calls `active`
 * directly.
 *
 * Written on the split's system workqueue (the command handler), read on the RGB
 * matrix's low-priority workqueue (the render tick); each aligned uint16_t
 * access cannot tear, so the worst case is one stale ~32 ms frame.
 * ------------------------------------------------------------------------- */
#define KP_OVERLAY_SLOTS MAX(1, KP_RGB_OVERLAY_COUNT)
static const struct device *kp_overlay_registry[KP_OVERLAY_SLOTS];
static volatile uint16_t kp_overlay_state[KP_RGB_OVERLAY_WORDS];

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* What every peripheral was last told. dispatch() sends only the words that
 * differ from this. Keeping a mirror rather than a changed-word list means a
 * change also survives a tick that could not take the matrix lock and had to
 * skip its dispatch: `state != sent` stays true, so the next tick still has
 * something to push. Central only: a peripheral never dispatches. */
static uint16_t kp_overlay_sent[KP_RGB_OVERLAY_WORDS];
#endif

void kp_rgb_overlay_register(const struct device *dev) {
  const struct kp_rgb_overlay_common_data *data = dev->data;
  if (data == NULL || data->index >= KP_OVERLAY_SLOTS) {
    return;
  }
  kp_overlay_registry[data->index] = dev;
}

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

bool kp_rgb_overlay_gate(const struct device *dev,
                         const struct kp_rgb_overlay_api *api) {
  if (api->active == NULL) {
    return true; /* predicate-less kind renders every tick, as before */
  }
  const struct kp_rgb_overlay_common_data *data = dev->data;
  if (data->remote) {
    return (kp_overlay_state[data->index / 16] >> (data->index % 16)) & 1u;
  }
  return api->active(dev);
}

bool kp_rgb_overlay_refresh(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  uint16_t next[KP_RGB_OVERLAY_WORDS] = {0};
  for (size_t i = 0; i < ARRAY_SIZE(kp_overlay_registry); i++) {
    const struct device *dev = kp_overlay_registry[i];
    if (dev == NULL) {
      continue;
    }
    /* A remote kind's predicate reads a central-only symbol, so it is
     * deliberately never called on a peripheral (see the #else branch). */
    const struct kp_rgb_overlay_api *api = dev->api;
    const struct kp_rgb_overlay_common_data *data = dev->data;
    if (!data->remote) {
      continue;
    }
    if (api->active == NULL || api->active(dev)) {
      next[data->index / 16] |= (uint16_t)BIT(data->index % 16);
    }
  }
  bool pending = false;
  for (uint16_t w = 0; w < KP_RGB_OVERLAY_WORDS; w++) {
    if (next[w] != kp_overlay_state[w]) {
      kp_overlay_state[w] = next[w];
    }
    if (kp_overlay_state[w] != kp_overlay_sent[w]) {
      pending = true; /* not on the wire yet, or a previous send was skipped */
    }
  }
  return pending;
#else
  return false;
#endif
}

void kp_rgb_overlay_dispatch(void) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  /* Any behavior node works: the command reads no context state and is
   * BEHAVIOR_LOCALITY_GLOBAL, so zmk_behavior_invoke_binding() reaches every
   * peripheral (and re-invokes the local no-op). Only words that differ from
   * the last broadcast are sent, so a change costs one command per *changed*
   * word rather than one per word. */
  struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(0);
  if (ctx == NULL) {
    return;
  }
  struct zmk_behavior_binding_event event = {.timestamp = k_uptime_get()};
  for (uint16_t w = 0; w < KP_RGB_OVERLAY_WORDS; w++) {
    uint16_t bits = kp_rgb_overlay_get_word(w);
    if (bits == kp_overlay_sent[w]) {
      continue;
    }
    struct zmk_behavior_binding binding = {
        .behavior_dev = ctx->dev->name,
        .param1 = RGB_OVL_STATE_CMD,
        .param2 = RGB_OVL_STATE_VAL(w, bits),
    };
    zmk_behavior_invoke_binding(&binding, event, true);
    /* Best effort: a split drop is silent, so this records the attempt. A
     * reconnect re-sends every word (rgb_split_sync.c). */
    kp_overlay_sent[w] = bits;
  }
#endif
}
