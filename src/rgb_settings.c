/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Per-behavior RGB matrix settings persistence. New records are namespaced by
 * the behavior device name; the old single-context record is read only when it
 * is unambiguous.
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
#include <zmk/workqueue.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_SETTINGS)

#define KP_RGB_PERSIST_VERSION 1
#define KP_RGB_PERSIST_SUBTREE "keypaw/rgb_matrix"
#define KP_RGB_PERSIST_PATH_MAX 64

struct kp_rgb_persist_effect {
  uint16_t duration_ms;
  uint16_t h;
  uint8_t s;
  uint8_t b;
};

struct kp_rgb_persist_blob {
  uint16_t selected_index;
  uint8_t version;
  uint8_t user_on;
  uint8_t effect_count;
  uint8_t reserved;
  struct kp_rgb_persist_effect effects[KP_RGB_PERSIST_MAX_EFFECTS];
};

BUILD_ASSERT(sizeof(struct kp_rgb_persist_effect) == 6,
             "the persisted effect gained padding");
BUILD_ASSERT(sizeof(struct kp_rgb_persist_blob) ==
                 6 + KP_RGB_PERSIST_MAX_EFFECTS *
                         sizeof(struct kp_rgb_persist_effect),
             "the persisted blob gained padding");

static void kp_rgb_encode(struct kp_rgb_behavior_context *ctx,
                          struct kp_rgb_persist_blob *blob) {
  memset(blob, 0, sizeof(*blob));
  kp_rgb_matrix_lock();
  blob->version = KP_RGB_PERSIST_VERSION;
  blob->user_on = ctx->state.user_on;
  size_t count =
      MIN(kp_rgb_effect_count(ctx), (size_t)KP_RGB_PERSIST_MAX_EFFECTS);
  blob->effect_count = (uint8_t)count;
  blob->selected_index = (uint16_t)kp_rgb_selected_effect(ctx);
  for (size_t i = 0; i < count; i++) {
    const struct device *dev = kp_rgb_effect_at(ctx, i);
    if (dev == NULL)
      continue;
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
  struct kp_rgb_behavior_context *ctx =
      CONTAINER_OF(work, struct kp_rgb_behavior_context, save_work.work);
  struct kp_rgb_persist_blob blob;
  char path[KP_RGB_PERSIST_PATH_MAX];
  kp_rgb_encode(ctx, &blob);
  snprintk(path, sizeof(path), KP_RGB_PERSIST_SUBTREE "/%s/state",
           ctx->dev->name);
  int err = settings_save_one(path, &blob, sizeof(blob));
  if (err < 0) {
    LOG_WRN("Failed to persist RGB state for %s (err %d)", ctx->dev->name, err);
  }
}

static struct kp_rgb_behavior_context *kp_rgb_context_named(const char *name,
                                                            size_t len) {
  for (size_t i = 0; i < kp_rgb_behavior_count(); i++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(i);
    if (ctx != NULL && strlen(ctx->dev->name) == len &&
        !memcmp(ctx->dev->name, name, len)) {
      return ctx;
    }
  }
  return NULL;
}

static int kp_rgb_load_context(struct kp_rgb_behavior_context *ctx, size_t len,
                               settings_read_cb read_cb, void *cb_arg) {
  if (len != sizeof(struct kp_rgb_persist_blob)) {
    LOG_INF("Discarding RGB state for %s of unexpected size %u", ctx->dev->name,
            (uint32_t)len);
    return -EINVAL;
  }
  struct kp_rgb_persist_blob blob;
  int rc = read_cb(cb_arg, &blob, sizeof(blob));
  if (rc < 0)
    return rc;
  if (blob.version != KP_RGB_PERSIST_VERSION) {
    LOG_WRN("Discarding RGB state for %s of version %u", ctx->dev->name,
            blob.version);
    return -EINVAL;
  }
  size_t count =
      MIN((size_t)blob.effect_count, (size_t)KP_RGB_PERSIST_MAX_EFFECTS);
  count = MIN(count, kp_rgb_effect_count(ctx));
  kp_rgb_matrix_lock();
  for (size_t i = 0; i < count; i++) {
    const struct device *dev = kp_rgb_effect_at(ctx, i);
    if (dev == NULL)
      continue;
    const struct kp_rgb_persist_effect *stored = &blob.effects[i];
    if (stored->h > KP_RGB_HUE_MAX || stored->s > KP_RGB_SAT_MAX ||
        stored->b > KP_RGB_BRT_MAX) {
      LOG_WRN("Skipping out-of-range persisted effect %u for %s", (uint32_t)i,
              ctx->dev->name);
      continue;
    }
    struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(dev);
    data->color =
        (struct kp_rgb_hsb){.h = stored->h, .s = stored->s, .b = stored->b};
    data->duration_ms = (uint16_t)CLAMP(
        (int32_t)stored->duration_ms, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
        CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
  }
  kp_rgb_matrix_unlock();
  if (kp_rgb_select_effect(ctx, blob.selected_index) < 0) {
    LOG_WRN("Persisted effect %u is unavailable for %s",
            (uint32_t)blob.selected_index, ctx->dev->name);
    kp_rgb_resolve_active(ctx);
  }
  kp_rgb_matrix_lock();
  ctx->state.user_on = blob.user_on;
  kp_rgb_matrix_unlock();
#if IS_ENABLED(CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE)
  bool active = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;
#else
  bool active = true;
#endif
  if (blob.user_on && active) {
    zmk_rgb_matrix_on(ctx->dev);
  } else {
    zmk_rgb_matrix_off(ctx->dev);
  }
  return 0;
}

static int kp_rgb_load_cb(const char *name, size_t len,
                          settings_read_cb read_cb, void *cb_arg) {
  const char *slash = strchr(name, '/');
  struct kp_rgb_behavior_context *ctx;
  if (slash == NULL) {
    const char *next;
    if (!(settings_name_steq(name, "state", &next) && !next) ||
        kp_rgb_behavior_count() != 1) {
      return -ENOENT;
    }
    ctx = kp_rgb_behavior_at(0);
  } else {
    if (strcmp(slash + 1, "state") != 0)
      return -ENOENT;
    ctx = kp_rgb_context_named(name, (size_t)(slash - name));
    if (ctx == NULL)
      return -ENOENT;
  }
  return kp_rgb_load_context(ctx, len, read_cb, cb_arg);
}

SETTINGS_STATIC_HANDLER_DEFINE(kp_rgb_matrix, KP_RGB_PERSIST_SUBTREE, NULL,
                               kp_rgb_load_cb, NULL, NULL);

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

int kp_rgb_save_state(struct kp_rgb_behavior_context *ctx) {
  if (ctx == NULL)
    return -EINVAL;
#if IS_ENABLED(CONFIG_SETTINGS)
  int ret = k_work_reschedule(&ctx->save_work,
                              K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
  return MIN(ret, 0);
#else
  return 0;
#endif
}

/* --- reset ---------------------------------------------------------------- */

static void kp_rgb_restore_all(void) {
  for (size_t i = 0; i < kp_rgb_behavior_count(); i++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(i);
    if (ctx == NULL) {
      continue;
    }
    (void)kp_rgb_apply_defaults(ctx);
    if (ctx->state.on) {
      zmk_rgb_matrix_on(ctx->dev);
    } else {
      zmk_rgb_matrix_off(ctx->dev);
    }
  }
  zmk_rgb_matrix_flush();
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void kp_rgb_reset_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  struct k_work_sync sync;
  char path[KP_RGB_PERSIST_PATH_MAX];
  /* Settle any in-flight debounced save first, or it would recreate the keys
   * deleted below. No kp_rgb_matrix_lock here: the save handler takes it, so a
   * sync cancel under the lock would deadlock. */
  for (size_t i = 0; i < kp_rgb_behavior_count(); i++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(i);
    if (ctx == NULL) {
      continue;
    }
    k_work_cancel_delayable_sync(&ctx->save_work, &sync);
    snprintk(path, sizeof(path), KP_RGB_PERSIST_SUBTREE "/%s/state",
             ctx->dev->name);
    int err = settings_delete(path);
    if (err < 0 && err != -ENOENT) {
      LOG_WRN("Failed to clear RGB state for %s (err %d)", ctx->dev->name, err);
    }
  }
  /* The pre-multi-context record. */
  int err = settings_delete(KP_RGB_PERSIST_SUBTREE "/state");
  if (err < 0 && err != -ENOENT) {
    LOG_WRN("Failed to clear the legacy RGB state (err %d)", err);
  }
  kp_rgb_restore_all();
}
K_WORK_DEFINE(kp_rgb_reset_work, kp_rgb_reset_work_handler);
#endif /* IS_ENABLED(CONFIG_SETTINGS) */

void kp_rgb_reset_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  /* settings_delete blocks on flash I/O, so keep it off the behavior thread. */
  k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &kp_rgb_reset_work);
#else
  kp_rgb_restore_all();
#endif
}

static int kp_rgb_settings_init(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  for (size_t i = 0; i < kp_rgb_behavior_count(); i++) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(i);
    if (ctx != NULL) {
      k_work_init_delayable(&ctx->save_work, kp_rgb_save_work_handler);
    }
  }
#endif
  return 0;
}
SYS_INIT(kp_rgb_settings_init, POST_KERNEL,
         CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
