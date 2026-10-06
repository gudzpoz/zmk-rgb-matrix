/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Split RGB state sync: pushes the selected effect (index, colour, period) and
 * the user on/off intent to a peripheral when it newly appears, and again when
 * the active effect changes to one it has not received. The second push covers
 * the effect colour and period: the GLOBAL invocation only carries the index, so
 * without it a peripheral renders a switched-to effect with its own stale
 * parameters. A send that fails is retried with bounded backoff while the
 * peripheral stays present.
 *
 * Central only; the trigger is a poll, as there is no central-side "peripheral
 * connected" event. See docs/development.md.
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
#define KP_RGB_SYNC_NO_EFFECT UINT16_MAX
#define KP_RGB_SYNC_STEP_DELAY_MS 50
#define KP_RGB_SYNC_MAX_BACKOFF_MS 1000
#define KP_RGB_SYNC_BACKOFF_SHIFT_MAX 5

enum kp_rgb_sync_phase {
  KP_RGB_SYNC_SELECT,   /* RGB_EFS_CMD: must precede colour/period */
  KP_RGB_SYNC_COLOR,    /* RGB_COLOR_HSB_CMD on the now-active effect */
  KP_RGB_SYNC_DURATION, /* RGB_SPI_CMD on the now-active effect */
  KP_RGB_SYNC_POWER,    /* RGB_ON_CMD / RGB_OFF_CMD */
  KP_RGB_SYNC_OVERLAYS,
};

enum kp_rgb_sync_emit {
  KP_RGB_SYNC_EMIT_SENT,  /* one command sent; the phase already advanced */
  KP_RGB_SYNC_EMIT_RETRY, /* transient failure; the same command stays armed */
  KP_RGB_SYNC_EMIT_DONE,  /* nothing left to send for this source */
};

struct kp_rgb_sync_source {
  bool seen;           /* present at the last sample */
  bool pending;        /* a sync is in flight for this source */
  uint8_t retry_count; /* consecutive failed sends for the in-flight command */
  uint16_t mask_word;  /* next overlay word to push this time round */
  int64_t ready_at;
  enum kp_rgb_sync_phase phase;
  /* Last effect fully pushed to this source; KP_RGB_SYNC_NO_EFFECT until one
   * completes. A change of active effect away from it re-queues a push. */
  uint16_t synced_effect;
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

/* Registration order is priority order, and a NULL get_status means "always
 * available". active_transport is private to the split central, so the transport
 * has to be re-derived here. */
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

/* Samples the connected set, queues a sync for every source that newly appeared
 * or whose active effect changed to one it has not received, and abandons one
 * that disappeared. Called from both handlers. */
static void kp_rgb_sync_refresh(void) {
  const struct zmk_split_transport_central *t = kp_rgb_sync_pick_transport();

  if (t != kp_rgb_sync_transport) {
    /* A different transport now owns the source ids, so what was seen before
     * says nothing about them; force every source to re-sync. */
    kp_rgb_sync_transport = t;
    for (size_t i = 0; i < ARRAY_SIZE(kp_rgb_sync_sources); i++) {
      kp_rgb_sync_sources[i].seen = false;
      kp_rgb_sync_sources[i].pending = false;
      kp_rgb_sync_sources[i].retry_count = 0;
      kp_rgb_sync_sources[i].synced_effect = KP_RGB_SYNC_NO_EFFECT;
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

  kp_rgb_matrix_lock();
  uint16_t active = (uint16_t)kp_rgb_controller.effect_index;
  kp_rgb_matrix_unlock();

  int64_t now = k_uptime_get();
  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    struct kp_rgb_sync_source *st = &kp_rgb_sync_sources[s];
    if (present[s] && !st->seen) {
      st->seen = true;
      st->pending = true;
      st->retry_count = 0;
      st->phase = KP_RGB_SYNC_SELECT;
      st->mask_word = 0;
      st->synced_effect = KP_RGB_SYNC_NO_EFFECT;
      /* A peripheral is marked connected before its GATT characteristics are
       * discovered, and the split worker silently drops commands in that
       * window. */
      st->ready_at = now + CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS;
      LOG_INF("Queued for source %u", (uint32_t)s);
    } else if (!present[s] && st->seen) {
      st->seen = false;
      st->pending = false; /* abort in flight; a reconnect re-syncs */
      st->synced_effect = KP_RGB_SYNC_NO_EFFECT;
    } else if (present[s] && st->seen && !st->pending &&
               st->synced_effect != active) {
      /* Active effect changed to one this source has not received. */
      st->pending = true;
      st->retry_count = 0;
      st->phase = KP_RGB_SYNC_SELECT;
      st->mask_word = 0;
      st->ready_at = now;
      LOG_INF("Effect change queued for source %u", (uint32_t)s);
    }
  }
}

/* Absolute commands only: the peripheral does not run the central-state
 * conversion pass, so a relative command would be applied against whatever the
 * peripheral already had. */
static bool kp_rgb_sync_send(uint8_t source, uint32_t cmd, uint32_t arg) {
  struct zmk_behavior_binding binding = {
      .behavior_dev = kp_rgb_controller.dev->name,
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

/* Emits at most one wire command. */
static enum kp_rgb_sync_emit kp_rgb_sync_emit_one(uint8_t source,
                                                  struct kp_rgb_sync_source *st) {
  if (st->phase != KP_RGB_SYNC_OVERLAYS) {
    uint32_t cmd, arg;
    switch (st->phase) {
    case KP_RGB_SYNC_SELECT: {
      kp_rgb_matrix_lock();
      st->effect_index = (uint16_t)kp_rgb_controller.effect_index;
      st->user_on = kp_rgb_controller.state.user_on;
      const struct device *fx = kp_rgb_controller.state.active_fx;
      if (fx != NULL) {
        const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(fx);
        st->color = data->color;
        /* Zero means "increase" in RGB_SPI_CMD, not an absolute period. */
        st->duration_ms = (uint16_t)MAX(data->duration_ms, 1u);
      }
      kp_rgb_matrix_unlock();
      if (fx == NULL) {
        st->phase = KP_RGB_SYNC_POWER;
        return KP_RGB_SYNC_EMIT_SENT;
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
      return KP_RGB_SYNC_EMIT_DONE;
    }

    if (!kp_rgb_sync_send(source, cmd, arg)) {
      return KP_RGB_SYNC_EMIT_RETRY;
    }
    st->phase++;
    return KP_RGB_SYNC_EMIT_SENT;
  }

  if (st->mask_word < kp_rgb_overlay_word_count()) {
    uint16_t word = st->mask_word;
    if (!kp_rgb_sync_send(source, RGB_OVL_STATE_CMD,
                          RGB_OVL_STATE_VAL(word, kp_rgb_overlay_get_word(word)))) {
      return KP_RGB_SYNC_EMIT_RETRY;
    }
    st->mask_word++;
    return KP_RGB_SYNC_EMIT_SENT;
  }

  LOG_INF("Complete for source %u", source);
  return KP_RGB_SYNC_EMIT_DONE;
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
    enum kp_rgb_sync_emit r = kp_rgb_sync_emit_one((uint8_t)s, st);
    if (r == KP_RGB_SYNC_EMIT_SENT) {
      st->retry_count = 0;
      next_ms = KP_RGB_SYNC_STEP_DELAY_MS;
      break; /* one command per work item, then yield */
    }
    if (r == KP_RGB_SYNC_EMIT_RETRY) {
      uint8_t shift = MIN(st->retry_count, KP_RGB_SYNC_BACKOFF_SHIFT_MAX);
      int64_t backoff = MIN((int64_t)(KP_RGB_SYNC_STEP_DELAY_MS << shift),
                            (int64_t)KP_RGB_SYNC_MAX_BACKOFF_MS);
      st->retry_count = MIN(st->retry_count + 1, KP_RGB_SYNC_BACKOFF_SHIFT_MAX);
      st->ready_at = now + backoff;
      if (next_ms < 0 || backoff < next_ms) {
        next_ms = backoff;
      }
      continue; /* other sources still make progress */
    }
    st->pending = false;
    st->synced_effect = st->effect_index;
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
