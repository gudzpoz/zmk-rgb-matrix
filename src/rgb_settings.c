/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Settings persistence for the keypaw RGB matrix, modelled on
 * zmk/app/src/rgb_underglow.c and zmk/app/src/backlight.c: one versioned blob
 * under "keypaw/rgb_matrix/state", written by a debounced work item and read
 * back through a STATIC settings handler.
 *
 * What is stored is the *user intent* (not the momentary lit state), the
 * selected effect, and every effect's colour and period -- the module keeps
 * those per effect, so a single active-effect colour would not round-trip.
 */

#define DT_DRV_COMPAT keypaw_behavior_rgb_matrix

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/activity.h>
#include <zmk/rgb_matrix.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_SETTINGS)

#define KP_RGB_PERSIST_VERSION 1
#define KP_RGB_PERSIST_SUBTREE "keypaw/rgb_matrix"

/* Deliberately NOT __packed: the fields are ordered and padded so every member
 * is naturally aligned, which keeps the code free of unaligned accesses and of
 * -Waddress-of-packed-member. The two asserts below pin the exact layout the
 * length check in kp_rgb_load_cb() relies on. */
struct kp_rgb_persist_effect {
  uint16_t duration_ms; /* <= CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS */
  uint16_t h;           /* 0..KP_RGB_HUE_MAX */
  uint8_t s;            /* 0..KP_RGB_SAT_MAX */
  uint8_t b;            /* 0..KP_RGB_BRT_MAX */
};

struct kp_rgb_persist_blob {
  uint16_t selected_index;
  uint8_t version;
  uint8_t user_on;
  uint8_t effect_count;
  uint8_t reserved; /* keeps `effects` 2-byte aligned; always written as 0 */
  struct kp_rgb_persist_effect effects[KP_RGB_PERSIST_MAX_EFFECTS];
};

BUILD_ASSERT(sizeof(struct kp_rgb_persist_effect) == 6, "the persisted effect gained padding");
BUILD_ASSERT(sizeof(struct kp_rgb_persist_blob) ==
                 6 + KP_RGB_PERSIST_MAX_EFFECTS * sizeof(struct kp_rgb_persist_effect),
             "the persisted blob gained padding; the length check would reject it");

/* Snapshot the live state. The lock is held only for the copy: settings_save_one()
 * blocks on a flash erase and must not be reached while the render waits on
 * kp_rgb_matrix_lock(). */
static void kp_rgb_encode(struct kp_rgb_persist_blob *blob) {
  memset(blob, 0, sizeof(*blob));

  kp_rgb_matrix_lock();
  blob->version = KP_RGB_PERSIST_VERSION;
  blob->user_on = kp_rgb_state.user_on;

  size_t count = MIN(kp_rgb_effect_count(), (size_t)KP_RGB_PERSIST_MAX_EFFECTS);
  blob->effect_count = (uint8_t)count;
  blob->selected_index = (uint16_t)kp_rgb_selected_effect();

  for (size_t i = 0; i < count; i++) {
    const struct device *dev = kp_rgb_effect_at(i);
    if (dev == NULL) {
      continue; /* disabled child: leave the entry zeroed */
    }
    const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(dev);
    blob->effects[i] = (struct kp_rgb_persist_effect){
        .duration_ms = data->duration_ms,
        .h = data->color.h,
        .s = data->color.s,
        .b = data->color.b,
    };
  }
  kp_rgb_matrix_unlock();
}

static void kp_rgb_save_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  struct kp_rgb_persist_blob blob;
  kp_rgb_encode(&blob);

  int err = settings_save_one(KP_RGB_PERSIST_SUBTREE "/state", &blob, sizeof(blob));
  if (err < 0) {
    LOG_WRN("Failed to persist RGB matrix state (err %d)", err);
  }
}

static struct k_work_delayable kp_rgb_save_work;

static int kp_rgb_load_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
  const char *next;

  if (!(settings_name_steq(name, "state", &next) && !next)) {
    return -ENOENT;
  }

  /* A different length is a different layout: keeping the devicetree defaults
   * that kp_effects_init() already seeded is the safest response. */
  if (len != sizeof(struct kp_rgb_persist_blob)) {
    LOG_INF("Discarding RGB matrix state of unexpected size %u", (uint32_t)len);
    return -EINVAL;
  }

  struct kp_rgb_persist_blob blob;
  int rc = read_cb(cb_arg, &blob, sizeof(blob));
  if (rc < 0) {
    return rc;
  }

  if (blob.version != KP_RGB_PERSIST_VERSION) {
    LOG_WRN("Discarding RGB matrix state of version %u", blob.version);
    return -EINVAL;
  }

  /* The stored index is only pinned to an effect by the registry's ordering, so
   * it is validated like any other untrusted input. An invalid entry keeps its
   * devicetree seed rather than being rejected wholesale. */
  size_t count = MIN((size_t)blob.effect_count, (size_t)KP_RGB_PERSIST_MAX_EFFECTS);
  count = MIN(count, kp_rgb_effect_count());

  kp_rgb_matrix_lock();
  for (size_t i = 0; i < count; i++) {
    const struct device *dev = kp_rgb_effect_at(i);
    if (dev == NULL) {
      continue;
    }

    const struct kp_rgb_persist_effect *stored = &blob.effects[i];
    if (stored->h > KP_RGB_HUE_MAX || stored->s > KP_RGB_SAT_MAX ||
        stored->b > KP_RGB_BRT_MAX) {
      LOG_WRN("Skipping out-of-range persisted effect %u", (uint32_t)i);
      continue;
    }

    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(dev);
    data->color = (struct kp_rgb_hsb){.h = stored->h, .s = stored->s, .b = stored->b};
    data->duration_ms = (uint16_t)CLAMP((int32_t)stored->duration_ms,
                                        CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
                                        CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  }
  kp_rgb_matrix_unlock();

  if (zmk_rgb_matrix_select_effect(blob.selected_index) < 0) {
    LOG_WRN("Persisted effect %u is unavailable; keeping the default",
            (uint32_t)blob.selected_index);
    kp_rgb_resolve_active();
  }

  /* Restore the intent, then make the lit state follow it. A load that happens
   * while the keyboard is already idle leaves the strip dark but keeps the
   * intent, so the wake edge relights it as usual. */
  kp_rgb_state.user_on = blob.user_on;

#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
  bool active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
#else
  bool active = true;
#endif

  if (blob.user_on && active) {
    zmk_rgb_matrix_on();
  } else {
    zmk_rgb_matrix_off();
  }

  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(kp_rgb_matrix, KP_RGB_PERSIST_SUBTREE, NULL, kp_rgb_load_cb, NULL,
                               NULL);

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/* Defined even when settings are off, so the behavior's command path needs no
 * conditional of its own. */
int kp_rgb_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  int ret = k_work_reschedule(&kp_rgb_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
  return MIN(ret, 0);
#else
  return 0;
#endif
}

/* Runs before the APPLICATION level, where the trigger table evaluates its boot
 * winner and can already call kp_rgb_save_state(). */
static int kp_rgb_settings_init(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  k_work_init_delayable(&kp_rgb_save_work, kp_rgb_save_work_handler);
#endif
  return 0;
}

SYS_INIT(kp_rgb_settings_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
