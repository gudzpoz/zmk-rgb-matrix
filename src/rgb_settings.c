/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_rgb_overlay_selector

#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <zmk/rgb_matrix.h>
#include <zmk/rgb_persist.h>

#include "rgb_matrix_internal.h"
#include "rgb_persist_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_SETTINGS)

#define KP_RGB_PERSIST_KEY KP_RGB_PERSIST_SUBTREE "/state"
#define KP_LEGACY_EFFECT_PREFIX "state/effects/"
#define KP_LEGACY_EFFECT_PATH KP_RGB_PERSIST_SUBTREE "/" KP_LEGACY_EFFECT_PREFIX
#define KP_FLAT_COUNT                                                          \
  MAX(1, DT_CHILD_NUM(DT_INST(0, keypaw_behavior_rgb_matrix)))

struct kp_parameter_load {
  struct kp_rgb_persist_effect scoped;
  struct kp_rgb_persist_effect legacy;
  bool scoped_pending;
  bool scoped_seen;
  bool legacy_pending;
};

struct kp_selector_load {
  struct kp_rgb_persist_selector_record scoped;
  struct kp_parameter_load *parameters;
  bool scoped_pending;
  bool scoped_seen;
  bool dirty;
  bool reset;
  uint32_t revision;
};

#define KP_SETTINGS_STORAGE(inst)                                              \
  static struct kp_parameter_load                                              \
      kp_parameters_##inst[DT_CHILD_NUM_STATUS_OKAY(DT_DRV_INST(inst))];
DT_INST_FOREACH_STATUS_OKAY(KP_SETTINGS_STORAGE)
#define KP_SETTINGS_SELECTOR(inst) {.parameters = kp_parameters_##inst},
static struct kp_selector_load
    kp_selectors[MAX(1, DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT))] = {
        DT_INST_FOREACH_STATUS_OKAY(KP_SETTINGS_SELECTOR)};
static struct kp_parameter_load kp_flat_parameters[KP_FLAT_COUNT];
static struct kp_rgb_persist_blob kp_legacy;
static struct kp_rgb_persist_global_state kp_global_load;
static bool kp_legacy_valid, kp_legacy_pending, kp_global_pending,
    kp_global_seen;
static bool kp_global_dirty, kp_global_reset;
static uint32_t kp_global_revision;

static void kp_rgb_save_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(kp_rgb_save_work, kp_rgb_save_work_handler);

static int kp_path(char *out, const char *selector, const char *effect) {
  int len = selector == NULL ? snprintk(out, SETTINGS_MAX_NAME_LEN + 1, "%s%s",
                                        KP_LEGACY_EFFECT_PATH, effect)
            : effect == NULL
                ? snprintk(out, SETTINGS_MAX_NAME_LEN + 1, "%s%s/state",
                           KP_RGB_PERSIST_SELECTOR_PREFIX, selector)
                : snprintk(out, SETTINGS_MAX_NAME_LEN + 1, "%s%s/effects/%s",
                           KP_RGB_PERSIST_SELECTOR_PREFIX, selector, effect);
  return len < 0 || len > SETTINGS_MAX_NAME_LEN ? -ENAMETOOLONG : 0;
}

static struct kp_rgb_persist_effect
kp_parameters_locked(const struct device *fx) {
  const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
  return (struct kp_rgb_persist_effect){
      .duration_ms = data->duration_ms, .h = data->color.h, .s = data->color.s};
}

static void
kp_restore_parameters_locked(const struct device *fx,
                             const struct kp_rgb_persist_effect *record) {
  struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
  data->color.h = record->h == KP_RGB_HUE_MAX ? 0 : record->h;
  data->color.s = record->s;
  data->duration_ms = kp_rgb_persist_clamp_duration(
      record->duration_ms, CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS,
      CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS);
}

static int kp_read(size_t len, size_t expected, settings_read_cb read_cb,
                   void *cb_arg, void *out) {
  if (len != expected)
    return -EINVAL;
  int rc = read_cb(cb_arg, out, expected);
  return rc < 0 ? rc : (rc == (int)expected ? 0 : -EINVAL);
}

static int kp_load_parameters(struct kp_parameter_load *load, bool scoped,
                              size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
  struct kp_rgb_persist_effect value;
  int err;
  if (scoped) {
    struct kp_rgb_persist_parameters_record bytes;
    err = kp_read(len, sizeof(bytes), read_cb, cb_arg, &bytes);
    if (err == 0)
      err = kp_rgb_persist_decode_parameters(&bytes, sizeof(bytes), &value);
  } else {
    struct kp_rgb_persist_effect bytes;
    err = kp_read(len, sizeof(bytes), read_cb, cb_arg, &bytes);
    if (err == 0)
      err = kp_rgb_persist_decode_legacy_effect(&bytes, sizeof(bytes), &value);
  }
  if (err < 0)
    return err;
  kp_rgb_matrix_lock();
  if (scoped) {
    load->scoped = value;
    load->scoped_pending = load->scoped_seen = true;
  } else {
    load->legacy = value;
    load->legacy_pending = true;
  }
  kp_rgb_matrix_unlock();
  return 0;
}

static int kp_rgb_load_cb(const char *name, size_t len,
                          settings_read_cb read_cb, void *cb_arg) {
  if (strcmp(name, "state") == 0) {
    if (len == sizeof(struct kp_rgb_persist_blob)) {
      struct kp_rgb_persist_blob bytes, value;
      int err = kp_read(len, sizeof(bytes), read_cb, cb_arg, &bytes);
      if (err == 0)
        err = kp_rgb_persist_decode_legacy(&bytes, sizeof(bytes), &value);
      if (err < 0)
        return err;
      kp_rgb_matrix_lock();
      kp_legacy = value;
      kp_legacy_valid = kp_legacy_pending = true;
      kp_rgb_matrix_unlock();
    } else {
      struct kp_rgb_persist_global_record bytes;
      struct kp_rgb_persist_global_state value;
      int err = kp_read(len, sizeof(bytes), read_cb, cb_arg, &bytes);
      if (err == 0)
        err = kp_rgb_persist_decode_global(&bytes, sizeof(bytes), &value);
      if (err < 0)
        return err;
      kp_rgb_matrix_lock();
      kp_global_load = value;
      kp_global_pending = kp_global_seen = true;
      kp_rgb_matrix_unlock();
    }
    return 0;
  }

  char path[SETTINGS_MAX_NAME_LEN + 1];
  bool legacy = strncmp(name, KP_LEGACY_EFFECT_PREFIX,
                        sizeof(KP_LEGACY_EFFECT_PREFIX) - 1) == 0;
  for (size_t i = 0; i < kp_rgb_persist_selector_count(); i++) {
    const struct device *owner;
    const struct kp_rgb_persist_selector_ids *ids =
        kp_rgb_persist_selector_at(i, &owner);
    kp_path(path, ids->id, NULL);
    if (strcmp(name, path + sizeof(KP_RGB_PERSIST_SUBTREE)) == 0) {
      struct kp_rgb_persist_selector_record bytes, value;
      int err = kp_read(len, sizeof(bytes), read_cb, cb_arg, &bytes);
      if (err == 0)
        err = kp_rgb_persist_decode_selector(&bytes, sizeof(bytes), &value);
      if (err < 0)
        return err;
      kp_rgb_matrix_lock();
      kp_selectors[i].scoped = value;
      kp_selectors[i].scoped_pending = kp_selectors[i].scoped_seen = true;
      kp_rgb_matrix_unlock();
      return 0;
    }
    for (size_t j = 0; j < ids->effect_count; j++) {
      if (!ids->effects[j].persist_parameters)
        continue;
      kp_path(path, legacy ? NULL : ids->id, ids->effects[j].id);
      if ((!legacy || owner == kp_rgb_selector_alias()) &&
          strcmp(name, path + sizeof(KP_RGB_PERSIST_SUBTREE)) == 0)
        return kp_load_parameters(&kp_selectors[i].parameters[j], !legacy, len,
                                  read_cb, cb_arg);
    }
  }
  /* The flat engine remains a compatibility bridge until Stage 7. */
  if (legacy && kp_rgb_persist_selector_count() == 0) {
    const char *id = name + sizeof(KP_LEGACY_EFFECT_PREFIX) - 1;
    for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
      const struct device *fx = kp_rgb_effect_at(i);
      if (fx != NULL && kp_rgb_effect_cfg(fx)->persist_parameters &&
          strcmp(id, kp_rgb_effect_cfg(fx)->persist_id) == 0)
        return kp_load_parameters(&kp_flat_parameters[i], false, len, read_cb,
                                  cb_arg);
    }
  }
  return 0;
}

static void kp_dirty_selector_locked(size_t index) {
  kp_selectors[index].dirty = true;
  kp_selectors[index].revision++;
}

static void kp_dirty_global_locked(void) {
  kp_global_dirty = true;
  kp_global_revision++;
}

static void kp_reset_load_locked(size_t index) {
  struct kp_selector_load *load = &kp_selectors[index];
  const struct kp_rgb_persist_selector_ids *ids =
      kp_rgb_persist_selector_at(index, NULL);
  load->scoped_pending = false;
  load->scoped_seen = true;
  memset(load->parameters, 0,
         ids->effect_count * sizeof(struct kp_parameter_load));
  for (size_t j = 0; j < ids->effect_count; j++) {
    const struct device *fx = kp_rgb_persist_member_at(index, j);
    load->parameters[j].scoped_seen = true;
    if (device_is_ready(fx))
      load->parameters[j].scoped = kp_parameters_locked(fx);
  }
}

int kp_rgb_settings_commit(void) {
  int err = kp_rgb_persist_ids_validate();
  if (err < 0)
    return err;
  bool save = false;
  kp_rgb_matrix_lock();
  /* Validate readiness before consuming any staged records. */
  for (size_t i = 0; i < kp_rgb_persist_selector_count(); i++) {
    const struct device *owner;
    const struct kp_rgb_persist_selector_ids *ids =
        kp_rgb_persist_selector_at(i, &owner);
    if (kp_rgb_selector_overlay_prepare_locked(owner) == NULL) {
      kp_rgb_matrix_unlock();
      return -ENODEV;
    }
    for (size_t j = 0; j < ids->effect_count; j++) {
      if (!device_is_ready(kp_rgb_persist_member_at(i, j))) {
        kp_rgb_matrix_unlock();
        return -ENODEV;
      }
    }
  }
  if (kp_global_pending || (kp_legacy_pending && !kp_global_seen)) {
    struct kp_rgb_persist_global_state defaults = {
        .user_on = kp_rgb_global.initial_on,
        .brightness =
            CLAMP(kp_rgb_global.initial_brightness, 0, KP_RGB_BRT_MAX)};
    struct kp_rgb_persist_global_state resolved;
    bool imported = kp_rgb_persist_resolve_global(
        &defaults, kp_legacy_valid ? &kp_legacy : NULL,
        kp_global_seen ? &kp_global_load : NULL, &resolved);
    kp_rgb_global.user_on = resolved.user_on;
    kp_rgb_global.brightness = resolved.brightness;
    if (imported) {
      kp_dirty_global_locked();
      save = true;
    }
    kp_rgb_reconcile_power_locked();
  }
  for (size_t i = 0; i < kp_rgb_persist_selector_count(); i++) {
    const struct device *owner;
    const struct kp_rgb_persist_selector_ids *ids =
        kp_rgb_persist_selector_at(i, &owner);
    struct kp_rgb_selector *selector =
        kp_rgb_selector_overlay_prepare_locked(owner);
    struct kp_selector_load *load = &kp_selectors[i];
    bool alias = owner == kp_rgb_selector_alias();
    bool imported = false;
    if (load->scoped_pending ||
        (alias && kp_legacy_pending && !load->scoped_seen)) {
      size_t slot = selector->initial_effect;
      if (kp_rgb_selector_at(selector, slot) == NULL)
        slot = kp_rgb_selector_calc_index(selector, 0, 0);
      const struct device *default_fx = kp_rgb_selector_at(selector, slot);
      struct kp_rgb_persist_selector_record resolved;
      imported = kp_rgb_persist_resolve_selector(
          kp_rgb_effect_cfg(default_fx)->persist_id,
          kp_legacy_valid ? &kp_legacy : NULL, alias,
          load->scoped_seen ? &load->scoped : NULL, ids->effects,
          ids->effect_count, &resolved);
      for (size_t j = 0; j < ids->effect_count; j++) {
        if (strcmp(resolved.selected_id, ids->effects[j].id) == 0) {
          const struct device *fx = kp_rgb_persist_member_at(i, j);
          kp_rgb_selector_select_locked(selector, kp_rgb_effect_cfg(fx)->slot);
          break;
        }
      }
    }
    for (size_t j = 0; j < ids->effect_count; j++) {
      struct kp_parameter_load *parameter = &load->parameters[j];
      if (parameter->scoped_pending ||
          (parameter->legacy_pending && !parameter->scoped_seen)) {
        const struct device *fx = kp_rgb_persist_member_at(i, j);
        struct kp_rgb_persist_effect defaults = kp_parameters_locked(fx),
                                     resolved;
        bool migrated = kp_rgb_persist_resolve_parameters(
            &defaults, parameter->legacy_pending ? &parameter->legacy : NULL,
            parameter->scoped_seen ? &parameter->scoped : NULL, alias,
            ids->effects[j].persist_parameters, &resolved);
        kp_restore_parameters_locked(fx, &resolved);
        imported |= migrated;
      }
      parameter->scoped_pending = parameter->legacy_pending = false;
    }
    load->scoped_pending = false;
    if (imported) {
      kp_dirty_selector_locked(i);
      kp_dirty_global_locked();
      save = true;
    }
  }
  if (kp_rgb_persist_selector_count() == 0) {
    if (kp_legacy_pending && kp_legacy.has_selected) {
      for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
        const struct device *fx = kp_rgb_effect_at(i);
        if (fx != NULL && strcmp(kp_rgb_effect_cfg(fx)->persist_id,
                                 kp_legacy.selected_id) == 0) {
          kp_rgb_select_effect_locked(i);
          break;
        }
      }
    }
    for (size_t i = 0; i < kp_rgb_effect_count(); i++) {
      if (kp_flat_parameters[i].legacy_pending) {
        kp_restore_parameters_locked(kp_rgb_effect_at(i),
                                     &kp_flat_parameters[i].legacy);
        kp_flat_parameters[i].legacy_pending = false;
      }
    }
  }
  kp_global_pending = kp_legacy_pending = false;
  zmk_rgb_matrix_flush();
  kp_rgb_matrix_unlock();
  if (save)
    k_work_reschedule(&kp_rgb_save_work,
                      K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(kp_rgb_matrix, KP_RGB_PERSIST_SUBTREE,
                                          NULL, kp_rgb_load_cb,
                                          kp_rgb_settings_commit, NULL,
                                          INT_MAX - 1);

static int kp_delete(const char *path) {
  int err = settings_delete(path);
  return err == -ENOENT ? 0 : err;
}

static int kp_save_selector(size_t index, uint32_t *revision) {
  const struct device *owner;
  const struct kp_rgb_persist_selector_ids *ids =
      kp_rgb_persist_selector_at(index, &owner);
  struct kp_rgb_persist_selector_record selection;
  kp_rgb_matrix_lock();
  struct kp_rgb_selector *selector =
      kp_rgb_selector_overlay_prepare_locked(owner);
  *revision = kp_selectors[index].revision;
  int err =
      selector == NULL || selector->active_fx == NULL
          ? -ENODEV
          : kp_rgb_persist_encode_selector(
                kp_rgb_effect_cfg(selector->active_fx)->persist_id, &selection);
  kp_rgb_matrix_unlock();
  if (err < 0)
    return err;
  char path[SETTINGS_MAX_NAME_LEN + 1];
  for (size_t j = 0; j < ids->effect_count; j++) {
    if (!ids->effects[j].persist_parameters)
      continue;
    struct kp_rgb_persist_parameters_record bytes;
    kp_rgb_matrix_lock();
    const struct device *fx = kp_rgb_persist_member_at(index, j);
    bool ready = device_is_ready(fx);
    if (ready) {
      struct kp_rgb_persist_effect value = kp_parameters_locked(fx);
      kp_rgb_persist_encode_parameters(&value, &bytes);
    }
    kp_rgb_matrix_unlock();
    kp_path(path, ids->id, ids->effects[j].id);
    int rc = ready ? settings_save_one(path, &bytes, sizeof(bytes)) : -ENODEV;
    if (err == 0)
      err = rc;
  }
  kp_path(path, ids->id, NULL);
  int rc = settings_save_one(path, &selection, sizeof(selection));
  return err < 0 ? err : rc;
}

static int kp_save_global(void) {
  struct kp_rgb_persist_blob legacy;
  struct kp_rgb_persist_global_record bytes;
  kp_rgb_matrix_lock();
  bool retain = kp_legacy_valid && kp_rgb_selector_alias() == NULL &&
                kp_legacy.has_selected && !kp_global_reset;
  kp_rgb_persist_pack(kp_rgb_global.user_on, kp_rgb_global.brightness,
                      retain ? kp_legacy.selected_id : NULL, &legacy);
  struct kp_rgb_persist_global_state state = {
      .user_on = kp_rgb_global.user_on, .brightness = kp_rgb_global.brightness};
  kp_rgb_persist_encode_global(&state, &bytes);
  kp_rgb_matrix_unlock();
  /* With no explicit migration target, preserve the only legacy selection copy. */
  return retain
      ? settings_save_one(KP_RGB_PERSIST_KEY, &legacy, sizeof(legacy))
      : settings_save_one(KP_RGB_PERSIST_KEY, &bytes, sizeof(bytes));
}

static int kp_clear_legacy_effects(size_t index) {
  const struct device *owner;
  const struct kp_rgb_persist_selector_ids *ids =
      kp_rgb_persist_selector_at(index, &owner);
  if (owner != kp_rgb_selector_alias())
    return 0;
  int err = 0;
  char path[SETTINGS_MAX_NAME_LEN + 1];
  kp_rgb_matrix_lock();
  bool clear_selection = kp_selectors[index].reset && kp_legacy_valid;
  uint32_t revision = kp_selectors[index].revision;
  struct kp_rgb_persist_blob legacy = kp_legacy;
  kp_rgb_matrix_unlock();
  if (clear_selection) {
    legacy.has_selected = 0;
    memset(legacy.selected_id, 0, sizeof(legacy.selected_id));
    err = settings_save_one(KP_RGB_PERSIST_KEY, &legacy, sizeof(legacy));
    kp_rgb_matrix_lock();
    if (err == 0 && revision == kp_selectors[index].revision)
      kp_legacy = legacy;
    kp_rgb_matrix_unlock();
  }
  for (size_t j = 0; j < ids->effect_count; j++) {
    if (!ids->effects[j].persist_parameters)
      continue;
    kp_path(path, NULL, ids->effects[j].id);
    int rc = kp_delete(path);
    if (err == 0)
      err = rc;
  }
  return err;
}
struct kp_legacy_key {
  char path[SETTINGS_MAX_NAME_LEN + 1];
  bool found;
};

static int kp_find_legacy_key(const char *name, size_t len,
                              settings_read_cb read_cb, void *cb_arg,
                              void *param) {
  ARG_UNUSED(read_cb);
  ARG_UNUSED(cb_arg);
  struct kp_legacy_key *key = param;
  if (len != 0 && !key->found && kp_path(key->path, NULL, name) == 0)
    key->found = true;
  return 0;
}

static int kp_purge_legacy_effects(void) {
  char previous[SETTINGS_MAX_NAME_LEN + 1] = {0};
  for (;;) {
    struct kp_legacy_key key = {0};
    int err = settings_load_subtree_direct(
        KP_RGB_PERSIST_SUBTREE "/state/effects", kp_find_legacy_key, &key);
    if (err < 0 || !key.found)
      return err;
    if (strcmp(previous, key.path) == 0)
      return -EIO;
    strcpy(previous, key.path);
    err = kp_delete(key.path);
    if (err < 0)
      return err;
  }
}

static void kp_rgb_save_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  int err = 0;
  for (size_t i = 0; i < kp_rgb_persist_selector_count(); i++) {
    kp_rgb_matrix_lock();
    bool dirty = kp_selectors[i].dirty;
    bool reset = kp_selectors[i].reset;
    const struct device *owner;
    kp_rgb_persist_selector_at(i, &owner);
    kp_rgb_matrix_unlock();
    if (!dirty)
      continue;
    uint32_t revision;
    int rc = kp_save_selector(i, &revision);
    /* Scoped defaults are durable before legacy fallback is removed. */
    if (rc == 0)
      rc = kp_clear_legacy_effects(i);
    if (rc == 0 && reset && owner == kp_rgb_selector_alias())
      rc = kp_purge_legacy_effects();
    kp_rgb_matrix_lock();
    if (rc == 0 && revision == kp_selectors[i].revision)
      kp_selectors[i].dirty = kp_selectors[i].reset = false;
    if (kp_selectors[i].dirty && err == 0)
      err = rc < 0 ? rc : -EAGAIN;
    kp_rgb_matrix_unlock();
  }
  kp_rgb_matrix_lock();
  bool dirty = kp_global_dirty;
  uint32_t revision = kp_global_revision;
  bool reset = kp_global_reset;
  kp_rgb_matrix_unlock();
  if (dirty && err == 0) {
    err = kp_save_global();
    if (err == 0 && reset)
      err = kp_purge_legacy_effects();
    kp_rgb_matrix_lock();
    if (err == 0 && revision == kp_global_revision) {
      kp_global_dirty = kp_global_reset = false;
      if (kp_rgb_persist_selector_count() != 0 &&
          kp_rgb_selector_alias() != NULL)
        kp_legacy_valid = false;
    }
    kp_rgb_matrix_unlock();
  }
  if (err < 0) {
    LOG_WRN("RGB settings save incomplete (err %d); retrying", err);
    k_work_reschedule(&kp_rgb_save_work, K_MSEC(1000));
  }
}

#else

int kp_rgb_settings_commit(void) { return kp_rgb_persist_ids_validate(); }

#endif

int kp_rgb_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
  kp_rgb_matrix_lock();
  kp_dirty_global_locked();
  kp_rgb_matrix_unlock();
  /* Flash I/O uses the larger system work queue stack. */
  return MIN(k_work_reschedule(&kp_rgb_save_work,
                               K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE)),
             0);
#else
  return 0;
#endif
}

int kp_rgb_save_selector_state(const struct device *selector, bool reset) {
#if IS_ENABLED(CONFIG_SETTINGS)
  bool found = false;
  kp_rgb_matrix_lock();
  for (size_t i = 0; i < kp_rgb_persist_selector_count(); i++) {
    const struct device *owner;
    kp_rgb_persist_selector_at(i, &owner);
    if (owner != selector)
      continue;
    kp_dirty_selector_locked(i);
    kp_selectors[i].reset |= reset;
    if (reset)
      kp_reset_load_locked(i);
    found = true;
    break;
  }
  kp_rgb_matrix_unlock();
  if (!found)
    return -ENODEV;
  return MIN(k_work_reschedule(&kp_rgb_save_work,
                               K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE)),
             0);
#else
  ARG_UNUSED(selector);
  ARG_UNUSED(reset);
  return 0;
#endif
}

void kp_rgb_reset_state(void) {
  kp_rgb_matrix_lock();
  (void)kp_rgb_apply_defaults();
  kp_rgb_selector_overlays_defaults_locked();
  kp_rgb_request_runtime_reset_locked();
#if IS_ENABLED(CONFIG_SETTINGS)
  kp_global_pending = kp_legacy_pending = kp_legacy_valid = false;
  kp_global_seen = true;
  kp_global_reset = true;
  memset(kp_flat_parameters, 0, sizeof(kp_flat_parameters));
  for (size_t i = 0; i < kp_rgb_persist_selector_count(); i++) {
    kp_reset_load_locked(i);
    kp_dirty_selector_locked(i);
    kp_selectors[i].reset = true;
  }
  kp_dirty_global_locked();
#endif
  kp_rgb_matrix_unlock();
#if IS_ENABLED(CONFIG_SETTINGS)
  k_work_reschedule(&kp_rgb_save_work, K_NO_WAIT);
#endif
}
