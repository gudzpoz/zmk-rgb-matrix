/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

#include "rgb_matrix_internal.h"
#include "rgb_selector_sync_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define KP_RGB_SYNC_SOURCES MAX(ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT, 1)
#define KP_RGB_SYNC_STEP_DELAY_MS 50
#define KP_RGB_SYNC_MAX_BACKOFF_MS 1000
#define KP_RGB_SYNC_BACKOFF_SHIFT_MAX 5

static const struct device *const kp_rgb_sync_endpoint = DEVICE_DT_GET(KP_RGB_NODE);

enum kp_rgb_sync_phase {
  KP_RGB_SYNC_BRIGHTNESS,
  KP_RGB_SYNC_POWER,
  KP_RGB_SYNC_OVERLAYS,
};

enum kp_rgb_sync_emit {
  KP_RGB_SYNC_EMIT_SENT,  /* one command sent; the phase already advanced */
  KP_RGB_SYNC_EMIT_RETRY, /* transient failure; the same command stays armed */
  KP_RGB_SYNC_EMIT_DONE,  /* nothing left to send for this source */
};

static uint64_t kp_rgb_global_revision;

struct kp_rgb_global_snapshot {
  uint64_t revision;
  bool user_on;
  uint8_t brightness;
};

static void kp_rgb_global_snapshot_locked(struct kp_rgb_global_snapshot *snapshot) {
  *snapshot = (struct kp_rgb_global_snapshot){
      .revision = kp_rgb_global_revision,
      .user_on = kp_rgb_global.user_on, .brightness = kp_rgb_global.brightness};
}

struct kp_rgb_sync_selector {
  const struct device *owner;
  uint64_t revision;
  uint64_t reset_generation;
};

#define KP_RGB_SYNC_SELECTOR(node_id) {.owner = DEVICE_DT_GET(node_id)},
static struct kp_rgb_sync_selector kp_rgb_sync_selectors[] = {
    DT_FOREACH_STATUS_OKAY(keypaw_rgb_overlay_selector, KP_RGB_SYNC_SELECTOR)
    {0},
};
#define KP_RGB_SYNC_SELECTOR_CAPACITY MAX(ARRAY_SIZE(kp_rgb_sync_selectors) - 1, 1)

static size_t kp_rgb_sync_selector_count(void) {
  return ARRAY_SIZE(kp_rgb_sync_selectors) - 1;
}

struct kp_rgb_sync_selector_progress {
  bool pending;
  bool captured;
  bool synced;
  uint8_t step;
  uint8_t retry_count;
  int64_t ready_at;
  uint64_t revision;
  uint64_t reset_generation;
  uint64_t completed_reset_generation;
  bool sweep;
  size_t member_cursor;
  struct kp_rgb_selector_snapshot snapshot;
};

struct kp_rgb_sync_source {
  bool seen;           /* present at the last sample */
  bool pending;        /* a sync is in flight for this source */
  uint8_t retry_count; /* consecutive failed sends for the in-flight command */
  uint16_t mask_word;  /* next overlay word to push this time round */
  int64_t ready_at;
  enum kp_rgb_sync_phase phase;
  bool global_captured;
  struct kp_rgb_global_snapshot global;
  int64_t selectors_ready_at;
  struct kp_rgb_sync_selector_progress selectors[KP_RGB_SYNC_SELECTOR_CAPACITY];
};

static struct k_work_delayable kp_rgb_sync_poll;
static struct k_work_delayable kp_rgb_sync_step;
static struct k_work_delayable kp_rgb_sync_clock;
/* Source progress belongs to the RGB work queue; revisions use the matrix mutex. */
static struct kp_rgb_sync_source kp_rgb_sync_sources[KP_RGB_SYNC_SOURCES];
static const struct zmk_split_transport_central *kp_rgb_sync_transport;

static struct k_work_delayable kp_rgb_sync_selector_step;
static size_t kp_rgb_sync_selector_cursor;

void kp_rgb_split_selector_dirty_locked(const struct device *selector) {
  for (size_t i = 0; i < kp_rgb_sync_selector_count(); i++) {
    if (kp_rgb_sync_selectors[i].owner == selector) {
      kp_rgb_sync_selectors[i].revision++;
      k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_selector_step,
                                  K_NO_WAIT);
      return;
    }
  }
}

void kp_rgb_split_global_dirty_locked(void) {
  kp_rgb_global_revision++;
  k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_step, K_NO_WAIT);
}

void kp_rgb_split_selector_reset_locked(const struct device *selector) {
  for (size_t i = 0; i < kp_rgb_sync_selector_count(); i++) {
    if (kp_rgb_sync_selectors[i].owner == selector) {
      kp_rgb_sync_selectors[i].reset_generation++;
      kp_rgb_split_selector_dirty_locked(selector);
      return;
    }
  }
}

void kp_rgb_split_selectors_dirty_locked(void) {
  for (size_t i = 0; i < kp_rgb_sync_selector_count(); i++)
    kp_rgb_split_selector_reset_locked(kp_rgb_sync_selectors[i].owner);
}

static void kp_rgb_sync_selectors_reset(struct kp_rgb_sync_source *source) {
  memset(source->selectors, 0, sizeof(source->selectors));
}

static void kp_rgb_sync_selectors_refresh(int64_t now) {
  kp_rgb_matrix_lock();
  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    struct kp_rgb_sync_source *source = &kp_rgb_sync_sources[s];
    if (!source->seen)
      continue;
    if (!source->pending &&
        source->global.revision != kp_rgb_global_revision) {
      source->pending = true;
      source->global_captured = false;
      source->phase = KP_RGB_SYNC_BRIGHTNESS;
      source->mask_word = 0;
    }
    for (size_t i = 0; i < kp_rgb_sync_selector_count(); i++) {
      struct kp_rgb_sync_selector_progress *st = &source->selectors[i];
      if (!st->pending &&
          (!st->synced || st->revision != kp_rgb_sync_selectors[i].revision ||
           st->completed_reset_generation != kp_rgb_sync_selectors[i].reset_generation)) {
        st->pending = true;
        st->captured = false;
        st->step = 0;
        st->retry_count = 0;
        st->ready_at = MAX(st->ready_at, MAX(now, source->selectors_ready_at));
        st->sweep = st->completed_reset_generation !=
                    kp_rgb_sync_selectors[i].reset_generation;
        st->member_cursor = 0;
        st->revision = kp_rgb_sync_selectors[i].revision;
        st->reset_generation = kp_rgb_sync_selectors[i].reset_generation;
      }
    }
  }
  kp_rgb_matrix_unlock();
}

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
      kp_rgb_sync_selectors_reset(&kp_rgb_sync_sources[i]);
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
      st->retry_count = 0;
      st->phase = KP_RGB_SYNC_BRIGHTNESS;
      st->mask_word = 0;
      st->global_captured = false;
      /* A peripheral is marked connected before its GATT characteristics are
       * discovered, and the split worker silently drops commands in that
       * window. */
      st->ready_at = now + CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS;
      st->selectors_ready_at = st->ready_at;
      LOG_INF("Queued for source %u", (uint32_t)s);
      /* Align the shared clock once discovery has settled as well. */
      k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_clock,
                                  K_MSEC(CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS));
    } else if (!present[s] && st->seen) {
      st->seen = false;
      st->pending = false; /* abort in flight; a reconnect re-syncs */
      kp_rgb_sync_selectors_reset(st);
    }
  }
  kp_rgb_sync_selectors_refresh(now);
}

static int kp_rgb_sync_send_binding(uint8_t source,
                                    struct zmk_behavior_binding *binding) {
  struct zmk_behavior_binding_event event = {
      .layer = 0, .position = 0, .timestamp = k_uptime_get(), .source = source};
  int err = zmk_split_central_invoke_behavior(source, binding, event, true);
  if (err < 0)
    LOG_WRN("Source %u command %u failed (err %d)", source, binding->param1, err);
  return err;
}

int kp_rgb_split_send_selector(uint8_t source,
                               const struct kp_rgb_selector_snapshot *snapshot,
                               uint8_t step) {
  struct zmk_behavior_binding binding;
  int err = kp_rgb_selector_snapshot_binding(snapshot, step, &binding);
  return err < 0 ? err : kp_rgb_sync_send_binding(source, &binding);
}

static bool kp_rgb_sync_send(uint8_t source, uint32_t cmd, uint32_t arg) {
  if (!device_is_ready(kp_rgb_sync_endpoint))
    return false;
  struct zmk_behavior_binding binding = {
      .behavior_dev = kp_rgb_sync_endpoint->name, .param1 = cmd, .param2 = arg};
  return kp_rgb_sync_send_binding(source, &binding) >= 0;
}

/* Pushes the central's shared animation clock to every connected source. Kept
 * out of the snapshot state machine so it keeps running while a snapshot is
 * stalled or retrying; a dropped send self-heals on the next period. */
static void kp_rgb_sync_send_clock(void) {
  const struct zmk_split_transport_central *t = kp_rgb_sync_pick_transport();
  if (t == NULL) {
    return;
  }
  uint8_t ids[KP_RGB_SYNC_SOURCES];
  int count = t->api->get_available_source_ids(ids);
  if (count < 0) {
    return;
  }
  uint32_t now = (uint32_t)k_uptime_get();
  for (int i = 0; i < count; i++) {
    if (ids[i] < KP_RGB_SYNC_SOURCES) {
      (void)kp_rgb_sync_send(ids[i], RGB_CLOCK_CMD, now);
    }
  }
}

static void kp_rgb_sync_clock_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_sync_send_clock();
  k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_clock,
                              K_MSEC(CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS));
}

static enum kp_rgb_sync_emit
kp_rgb_sync_emit_global(uint8_t source, struct kp_rgb_sync_source *st) {
  if (!st->global_captured) {
    kp_rgb_matrix_lock();
    kp_rgb_global_snapshot_locked(&st->global);
    kp_rgb_matrix_unlock();
    st->global_captured = true;
  }
  uint32_t command, argument;
  switch (st->phase) {
  case KP_RGB_SYNC_BRIGHTNESS:
    command = RGB_BRT_SET_CMD;
    argument = st->global.brightness;
    break;
  case KP_RGB_SYNC_POWER:
    command = st->global.user_on ? RGB_ON_CMD : RGB_OFF_CMD;
    argument = 0;
    break;
  case KP_RGB_SYNC_OVERLAYS:
    if (st->mask_word >= kp_rgb_overlay_word_count())
      return KP_RGB_SYNC_EMIT_DONE;
    command = RGB_OVL_STATE_CMD;
    argument = RGB_OVL_STATE_VAL(st->mask_word,
                                kp_rgb_overlay_get_word(st->mask_word));
    break;
  default:
    return KP_RGB_SYNC_EMIT_DONE;
  }
  if (!kp_rgb_sync_send(source, command, argument))
    return KP_RGB_SYNC_EMIT_RETRY;
  if (st->phase == KP_RGB_SYNC_OVERLAYS)
    st->mask_word++;
  else
    st->phase++;
  return KP_RGB_SYNC_EMIT_SENT;
}

static void kp_rgb_sync_step_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_sync_refresh(); /* aborts vanished sources, queues new ones */

  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    struct kp_rgb_sync_source *st = &kp_rgb_sync_sources[s];
    if (!st->pending) {
      continue;
    }
    if (k_uptime_get() < st->ready_at)
      continue;
    enum kp_rgb_sync_emit r = kp_rgb_sync_emit_global((uint8_t)s, st);
    if (r == KP_RGB_SYNC_EMIT_SENT) {
      st->retry_count = 0;
      st->ready_at = k_uptime_get() + KP_RGB_SYNC_STEP_DELAY_MS;
      break; /* one command per work item, then yield */
    }
    if (r == KP_RGB_SYNC_EMIT_RETRY) {
      uint8_t shift = MIN(st->retry_count, KP_RGB_SYNC_BACKOFF_SHIFT_MAX);
      int64_t backoff = MIN((int64_t)(KP_RGB_SYNC_STEP_DELAY_MS << shift),
                            (int64_t)KP_RGB_SYNC_MAX_BACKOFF_MS);
      st->retry_count = MIN(st->retry_count + 1, KP_RGB_SYNC_BACKOFF_SHIFT_MAX);
      st->ready_at = k_uptime_get() + backoff;
      continue; /* other sources still make progress */
    }
    kp_rgb_matrix_lock();
    st->pending = st->global.revision != kp_rgb_global_revision;
    if (st->pending) {
      st->global_captured = false;
      st->phase = KP_RGB_SYNC_BRIGHTNESS;
      st->mask_word = 0;
    }
    kp_rgb_matrix_unlock();
  }

  int64_t now = k_uptime_get();
  int64_t next_ms = -1;
  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    const struct kp_rgb_sync_source *st = &kp_rgb_sync_sources[s];
    if (st->pending) {
      int64_t wait = MAX(st->ready_at - now, 0);
      if (next_ms < 0 || wait < next_ms)
        next_ms = wait;
    }
  }
  if (next_ms >= 0) {
    k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_step,
                                K_MSEC((uint32_t)next_ms));
  }
}

static void kp_rgb_sync_selector_step_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_sync_refresh();
  size_t count = kp_rgb_sync_selector_count();
  size_t lanes = ARRAY_SIZE(kp_rgb_sync_sources) * count;
  int64_t now = k_uptime_get();

  for (size_t n = 0; n < lanes; n++) {
    size_t lane = (kp_rgb_sync_selector_cursor + n) % lanes;
    size_t s = lane / count, i = lane % count;
    struct kp_rgb_sync_selector_progress *st =
        &kp_rgb_sync_sources[s].selectors[i];
    if (!st->pending || now < st->ready_at)
      continue;

    int err = 0;
    /* Finish the frozen member before sampling a newer command revision. */
    if (!st->captured) {
      kp_rgb_matrix_lock();
      if (st->sweep && st->step != 2) {
        err = kp_rgb_selector_next_snapshot_locked(kp_rgb_sync_selectors[i].owner,
                                                   &st->member_cursor, &st->snapshot);
        if (err == 0)
          st->step = 2;
        else if (err > 0)
          err = 0;
      }
      if (!st->sweep || st->step == 2)
        err = kp_rgb_selector_snapshot_locked(kp_rgb_sync_selectors[i].owner,
                                              &st->snapshot);
      if (err == 0) {
        if (!st->sweep)
          st->revision = kp_rgb_sync_selectors[i].revision;
        st->captured = true;
      }
      kp_rgb_matrix_unlock();
    }
    if (err == 0)
      err = kp_rgb_split_send_selector((uint8_t)s, &st->snapshot, st->step);

    now = k_uptime_get();
    if (err < 0) {
      uint8_t shift = MIN(st->retry_count, KP_RGB_SYNC_BACKOFF_SHIFT_MAX);
      st->ready_at = now + MIN((int64_t)(KP_RGB_SYNC_STEP_DELAY_MS << shift),
                               (int64_t)KP_RGB_SYNC_MAX_BACKOFF_MS);
      st->retry_count = MIN(st->retry_count + 1, KP_RGB_SYNC_BACKOFF_SHIFT_MAX);
    } else {
      st->retry_count = 0;
      st->ready_at = now + KP_RGB_SYNC_STEP_DELAY_MS;
      if (st->sweep && st->step == 1) {
        st->captured = false;
        st->step = 0;
      } else if (++st->step == 3) {
        kp_rgb_matrix_lock();
        if (st->sweep && st->revision == kp_rgb_sync_selectors[i].revision)
          st->completed_reset_generation = st->reset_generation;
        kp_rgb_matrix_unlock();
        st->pending = false;
        st->synced = true;
      }
    }
    kp_rgb_sync_selector_cursor = (lane + 1) % lanes;
    break;
  }

  kp_rgb_sync_selectors_refresh(now);
  int64_t next_ms = -1;
  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    for (size_t i = 0; i < count; i++) {
      struct kp_rgb_sync_selector_progress *st =
          &kp_rgb_sync_sources[s].selectors[i];
      if (st->pending) {
        int64_t wait = MAX(st->ready_at - now, KP_RGB_SYNC_STEP_DELAY_MS);
        if (next_ms < 0 || wait < next_ms)
          next_ms = wait;
      }
    }
  }
  if (next_ms >= 0)
    k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_selector_step,
                                K_MSEC((uint32_t)next_ms));
}

static void kp_rgb_sync_poll_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_rgb_sync_refresh();

  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++) {
    if (kp_rgb_sync_sources[s].pending) {
      k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_step,
                                  K_NO_WAIT);
      break;
    }
  }

  bool selector_pending = false;
  for (size_t s = 0; s < ARRAY_SIZE(kp_rgb_sync_sources); s++)
    for (size_t i = 0; i < kp_rgb_sync_selector_count(); i++)
      selector_pending |= kp_rgb_sync_sources[s].selectors[i].pending;
  if (selector_pending)
    k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_selector_step,
                                K_NO_WAIT);
  k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_poll,
                              K_MSEC(CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS));
}

static int kp_rgb_split_sync_init(void) {
  k_work_init_delayable(&kp_rgb_sync_poll, kp_rgb_sync_poll_handler);
  k_work_init_delayable(&kp_rgb_sync_step, kp_rgb_sync_step_handler);
  k_work_init_delayable(&kp_rgb_sync_clock, kp_rgb_sync_clock_handler);
  k_work_init_delayable(&kp_rgb_sync_selector_step,
                        kp_rgb_sync_selector_step_handler);
  /* The module work queue is started at POST_KERNEL, well before this. The
   * first poll is delayed rather than immediate to stay clear of the
   * settings_load() that runs at the top of main(). */
  k_work_schedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_poll,
                            K_MSEC(CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS));
  k_work_schedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_clock,
                            K_MSEC(CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS));
  return 0;
}
SYS_INIT(kp_rgb_split_sync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
