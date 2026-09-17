/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Split RGB state sync. A &kprgb command is BEHAVIOR_LOCALITY_GLOBAL, so every
 * command typed on the central reaches the peripheral and is persisted there. A
 * peripheral that was off while the settings changed never saw the command, keeps
 * its stale flash defaults, and reloads them on the next power-up. This pushes the
 * selected effect (index, colour, period) and the user on/off intent to a
 * peripheral when it newly appears.
 *
 * No peripheral code is needed: its command handler dispatches straight to
 * behavior_keymap_binding_pressed(), and this behavior's handler ends in
 * kp_rgb_save_state(), so a push is both applied and persisted.
 *
 * Central only. There is no central-side "peripheral connected" event, and the BLE
 * transport's single status callback is already owned by central_init(), so the
 * trigger is a poll of the transport's in-RAM connected-source list.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/behavior.h>
#include <zmk/split/central.h>
#include <zmk/split/transport/central.h>
#include <zmk/workqueue.h>

#include "rgb_matrix_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define KP_RGB_SYNC_SOURCES MAX(ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT, 1)
#define KP_RGB_SYNC_STEP_DELAY_MS 50

enum kp_rgb_sync_phase {
  KP_RGB_SYNC_SELECT,   /* RGB_EFS_CMD: must precede colour/period */
  KP_RGB_SYNC_COLOR,    /* RGB_COLOR_HSB_CMD on the now-active effect */
  KP_RGB_SYNC_DURATION, /* RGB_SPI_CMD on the now-active effect */
  KP_RGB_SYNC_POWER,    /* RGB_ON_CMD / RGB_OFF_CMD, last */
};

struct kp_rgb_sync_source {
  bool seen;    /* present at the last sample */
  bool pending; /* a sync is in flight for this source */
  int64_t ready_at;
  size_t ctx_index;
  enum kp_rgb_sync_phase phase;
  /* Snapshot taken under kp_rgb_matrix_lock() at KP_RGB_SYNC_SELECT. */
  uint16_t effect_index;
  uint16_t duration_ms;
  struct kp_rgb_hsb color;
  bool user_on;
};

static struct k_work_delayable kp_rgb_sync_poll;
static struct k_work_delayable kp_rgb_sync_step;
static struct kp_rgb_sync_source kp_rgb_sync_sources[KP_RGB_SYNC_SOURCES];
static const struct zmk_split_transport_central *kp_rgb_sync_transport;

/* Mirrors select_first_available_transport(): registration order is priority
 * order, and a NULL get_status means "always available". active_transport is
 * private to the split central, so the transport has to be re-derived here. */
static const struct zmk_split_transport_central *kp_rgb_sync_pick_transport(void) {
  STRUCT_SECTION_FOREACH(zmk_split_transport_central, t) {
    if (t->api == NULL || t->api->send_command == NULL ||
        t->api->get_available_source_ids == NULL) {
      continue;
    }
    if (t->api->get_status != NULL && !t->api->get_status().available) {
      continue;
    }
    return t;
  }
  return NULL;
}

/* Samples the connected set, queues a sync for every source that newly appeared,
 * and abandons one that disappeared. Called from both handlers. */
static void kp_rgb_sync_refresh(void) {
  const struct zmk_split_transport_central *t = kp_rgb_sync_pick_transport();

  if (t != kp_rgb_sync_transport) {
    /* A different transport now owns the source ids, so what was seen before
     * says nothing about them; force every source to re-sync. */
    kp_rgb_sync_transport = t;
    for (size_t i = 0; i < ARRAY_SIZE(kp_rgb_sync_sources); i++) {
      kp_rgb_sync_sources[i].seen = false;
      kp_rgb_sync_sources[i].pending = false;
    }
  }

  bool present[KP_RGB_SYNC_SOURCES] = {false};
  if (t != NULL) {
    uint8_t ids[KP_RGB_SYNC_SOURCES];
    int count = t->api->get_available_source_ids(ids);
    if (count < 0) {
      LOG_WRN("Source query failed (err %d)", count);
      count = 0;
    }
    for (int i = 0; i < count; i++) {
      if (ids[i] < ARRAY_SIZE(present)) {
        present[ids[i]] = true;
      }
    }
  }

  int64_t now = k_uptime_get();
  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    struct kp_rgb_sync_source *st = &kp_rgb_sync_sources[s];
    if (present[s] && !st->seen) {
      st->seen = true;
      st->pending = true;
      st->ctx_index = 0;
      st->phase = KP_RGB_SYNC_SELECT;
      /* A peripheral is marked connected before its GATT characteristics have
       * been discovered, and the split worker silently drops commands sent in
       * that window, so give discovery and the security upgrade time to land. */
      st->ready_at = now + CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS;
      LOG_INF("Queued for source %u", (uint32_t)s);
    } else if (!present[s] && st->seen) {
      st->seen = false;
      st->pending = false; /* abort in flight; a reconnect re-syncs */
    }
  }
}

/* Absolute commands only: the peripheral does not run the central-state
 * conversion pass, so a relative command would be applied against whatever the
 * peripheral already had. */
static bool kp_rgb_sync_send(uint8_t source, const struct device *dev, uint32_t cmd,
                             uint32_t arg) {
  struct zmk_behavior_binding binding = {
      .behavior_dev = dev->name,
      .param1 = cmd,
      .param2 = arg,
  };
  /* The peripheral handler ignores the event, and its position is truncated to a
   * byte on the wire, so keep it 0. state=true calls the pressed handler. */
  struct zmk_behavior_binding_event event = {
      .layer = 0,
      .position = 0,
      .timestamp = k_uptime_get(),
      .source = source,
  };

  int err = zmk_split_central_invoke_behavior(source, &binding, event, true);
  if (err < 0) {
    LOG_WRN("Source %u command %u failed (err %d)", source, cmd, err);
    return false;
  }
  return true;
}

/* Emits at most one wire command. Returns true if one was sent. */
static bool kp_rgb_sync_emit_one(uint8_t source, struct kp_rgb_sync_source *st) {
  while (st->ctx_index < kp_rgb_behavior_count()) {
    struct kp_rgb_behavior_context *ctx = kp_rgb_behavior_at(st->ctx_index);
    if (ctx == NULL) {
      st->ctx_index++;
      st->phase = KP_RGB_SYNC_SELECT;
      continue;
    }

    uint32_t cmd, arg;
    switch (st->phase) {
    case KP_RGB_SYNC_SELECT: {
      const struct device *fx;
      /* Snapshot under the lock rather than holding it across the split send. */
      kp_rgb_matrix_lock();
      st->effect_index = (uint16_t)ctx->effect_index;
      st->user_on = ctx->state.user_on;
      fx = ctx->state.active_fx;
      if (fx != NULL) {
        const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
        st->color = data->color;
        /* RGB_SPI_CMD with param2 == 0 means "increase" on the receiving side,
         * so an absolute zero period is not expressible. */
        st->duration_ms = (uint16_t)MAX(data->duration_ms, 1u);
      }
      kp_rgb_matrix_unlock();

      if (fx == NULL) {
        LOG_WRN("%s has no active effect", ctx->dev->name);
        st->ctx_index++;
        st->phase = KP_RGB_SYNC_SELECT;
        continue;
      }
      cmd = RGB_EFS_CMD;
      arg = st->effect_index;
      break;
    }
    case KP_RGB_SYNC_COLOR:
      cmd = RGB_COLOR_HSB_CMD;
      arg = RGB_COLOR_HSB_VAL(st->color.h, st->color.s, st->color.b);
      break;
    case KP_RGB_SYNC_DURATION:
      cmd = RGB_SPI_CMD;
      arg = st->duration_ms;
      break;
    case KP_RGB_SYNC_POWER:
      cmd = st->user_on ? RGB_ON_CMD : RGB_OFF_CMD;
      arg = 0;
      break;
    default:
      return false;
    }

    if (!kp_rgb_sync_send(source, ctx->dev, cmd, arg)) {
      return false; /* caller clears pending: no half-applied retry */
    }

    if (st->phase == KP_RGB_SYNC_POWER) {
      st->ctx_index++;
      st->phase = KP_RGB_SYNC_SELECT;
    } else {
      st->phase++;
    }
    return true;
  }

  LOG_INF("Complete for source %u", source);
  return false;
}

static void kp_rgb_sync_step_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_sync_refresh(); /* aborts vanished sources, queues new ones */

  int64_t now = k_uptime_get();
  int64_t next_ms = -1;

  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    struct kp_rgb_sync_source *st = &kp_rgb_sync_sources[s];
    if (!st->pending) {
      continue;
    }
    if (now < st->ready_at) {
      int64_t wait = st->ready_at - now;
      if (next_ms < 0 || wait < next_ms) {
        next_ms = wait;
      }
      continue;
    }
    if (kp_rgb_sync_emit_one((uint8_t)s, st)) {
      next_ms = KP_RGB_SYNC_STEP_DELAY_MS;
      break; /* one command per work item, then yield */
    }
    st->pending = false;
  }

  if (next_ms >= 0) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_rgb_sync_step,
                                K_MSEC((uint32_t)next_ms));
  }
}

static void kp_rgb_sync_poll_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_sync_refresh();

  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    if (kp_rgb_sync_sources[s].pending) {
      k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_rgb_sync_step,
                                  K_NO_WAIT);
      break;
    }
  }

  k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_rgb_sync_poll,
                              K_MSEC(CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS));
}

static int kp_rgb_split_sync_init(void) {
  k_work_init_delayable(&kp_rgb_sync_poll, kp_rgb_sync_poll_handler);
  k_work_init_delayable(&kp_rgb_sync_step, kp_rgb_sync_step_handler);
  /* The low-priority queue is already running (started at POST_KERNEL). The
   * first poll is delayed rather than immediate to stay clear of the
   * settings_load() that runs at the top of main(). */
  k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &kp_rgb_sync_poll,
                            K_MSEC(CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS));
  return 0;
}
SYS_INIT(kp_rgb_split_sync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
