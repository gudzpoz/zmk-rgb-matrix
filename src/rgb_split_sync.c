/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>
#include <zmk/behavior.h>
#include <zmk/split/central.h>
#include <zmk/split/transport/central.h>

#include <dt-bindings/keypaw/rgb_matrix.h>

#include "rgb_matrix_internal.h"
#include "rgb_replication_internal.h"
#include "rgb_selector_internal.h"
#include "rgb_split_sync_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define KP_RGB_SYNC_SOURCES MAX(ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT, 1)
#define KP_RGB_SYNC_SLOTS (2 * KP_RGB_SYNC_SOURCES)
#define KP_RGB_SYNC_PACE_MS 50
#define KP_RGB_SYNC_RETRY_MS 1000
#define KP_RGB_SYNC_YIELD_MS 1

enum kp_rgb_sync_phase {
  KP_RGB_SYNC_BRIGHTNESS,
  KP_RGB_SYNC_POWER,
  KP_RGB_SYNC_GATES,
  KP_RGB_SYNC_COLOR,
  KP_RGB_SYNC_PERIOD,
  KP_RGB_SYNC_SELECTION,
};

static const struct device *const kp_rgb_sync_endpoint = DEVICE_DT_GET(KP_RGB_NODE);

struct kp_rgb_sync_source {
  bool seen;
  bool pending;
  bool generation_captured;
  enum kp_rgb_sync_phase phase;
  size_t gate_word;
  size_t selector_index;
  size_t member_index;
  uint64_t generation;
  int64_t ready_at;
  int64_t clock_due;
};

static struct kp_rgb_sync_source kp_rgb_sync_sources[KP_RGB_SYNC_SOURCES];
static const struct zmk_split_transport_central *kp_rgb_sync_transport;
static void kp_rgb_sync_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(kp_rgb_sync_work, kp_rgb_sync_handler);
static atomic_t kp_rgb_sync_initialized;
static size_t kp_rgb_sync_cursor;
static uint64_t kp_rgb_sync_consumed_repair_epoch;
static int64_t kp_rgb_sync_next_state_at;
static int64_t kp_rgb_sync_poll_due;

/* Pinned ZMK selects the first available registered transport; observation can race enqueue. */
static const struct zmk_split_transport_central *kp_rgb_sync_pick_transport(void) {
  STRUCT_SECTION_FOREACH(zmk_split_transport_central, transport) {
    if (transport->api == NULL || transport->api->send_command == NULL ||
        transport->api->get_available_source_ids == NULL) {
      continue;
    }
    if (transport->api->get_status != NULL &&
        !transport->api->get_status().available) {
      continue;
    }
    return transport;
  }
  return NULL;
}

static int kp_rgb_sync_queue(k_timeout_t delay, bool external) {
  int result;
  if (external) {
    result = k_work_reschedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_work,
                                         delay);
  } else {
    result = k_work_schedule_for_queue(kp_rgb_work_q(), &kp_rgb_sync_work,
                                        delay);
  }
  if (result < 0) {
    LOG_ERR("Unable to schedule split sync work (err %d)", result);
  }
  return result;
}

void kp_rgb_split_sync_wake(void) {
  if (atomic_get(&kp_rgb_sync_initialized)) {
    (void)kp_rgb_sync_queue(K_NO_WAIT, true);
  }
}

static void kp_rgb_sync_reset_cursor(struct kp_rgb_sync_source *source) {
  source->phase = KP_RGB_SYNC_BRIGHTNESS;
  source->gate_word = 0;
  source->selector_index = 0;
  source->member_index = 0;
  source->generation_captured = false;
}

static void kp_rgb_sync_poll(int64_t now) {
  const struct zmk_split_transport_central *transport =
      kp_rgb_sync_pick_transport();
  if (transport != kp_rgb_sync_transport) {
    kp_rgb_sync_transport = transport;
    for (size_t i = 0; i < ARRAY_SIZE(kp_rgb_sync_sources); i++) {
      kp_rgb_sync_sources[i].seen = false;
      kp_rgb_sync_sources[i].pending = false;
      kp_rgb_sync_sources[i].clock_due = 0;
      kp_rgb_sync_reset_cursor(&kp_rgb_sync_sources[i]);
    }
  }

  bool present[KP_RGB_SYNC_SOURCES] = {false};
  if (transport != NULL) {
    uint8_t ids[KP_RGB_SYNC_SOURCES];
    int count = transport->api->get_available_source_ids(ids);
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

  for (size_t i = 0; i < ARRAY_SIZE(kp_rgb_sync_sources); i++) {
    struct kp_rgb_sync_source *source = &kp_rgb_sync_sources[i];
    if (present[i] && !source->seen) {
      source->seen = true;
      source->pending = true;
      kp_rgb_sync_reset_cursor(source);
      source->ready_at = now + CONFIG_KEYPAW_RGB_SPLIT_SYNC_SETTLE_MS;
      source->clock_due = source->ready_at;
      LOG_INF("Queued for source %u", (uint32_t)i);
    } else if (!present[i] && source->seen) {
      source->seen = false;
      source->pending = false;
      source->clock_due = 0;
      kp_rgb_sync_reset_cursor(source);
    }
  }
}

static int kp_rgb_sync_send_binding(uint8_t source,
                                    struct zmk_behavior_binding *binding) {
  if (!device_is_ready(kp_rgb_sync_endpoint)) {
    LOG_WRN("RGB split endpoint is not ready");
    return -ENODEV;
  }
  struct zmk_behavior_binding_event event = {
      .layer = 0, .position = 0, .timestamp = k_uptime_get(), .source = source};
  int err = zmk_split_central_invoke_behavior(source, binding, event, true);
  if (err < 0) {
    LOG_WRN("Source %u command %u failed (err %d)", source, binding->param1, err);
  }
  return err;
}

static int kp_rgb_sync_send(uint8_t source, uint32_t command, uint32_t argument) {
  struct zmk_behavior_binding binding = {
      .behavior_dev = kp_rgb_sync_endpoint->name,
      .param1 = command,
      .param2 = argument,
  };
  return kp_rgb_sync_send_binding(source, &binding);
}

static int kp_rgb_sync_send_clock(uint8_t source) {
  uint32_t now = (uint32_t)k_uptime_get();
  return kp_rgb_sync_send(source, RGB_CLOCK_CMD, now);
}

static void kp_rgb_sync_advance(struct kp_rgb_sync_source *source) {
  switch (source->phase) {
  case KP_RGB_SYNC_BRIGHTNESS:
    source->phase = KP_RGB_SYNC_POWER;
    break;
  case KP_RGB_SYNC_POWER:
    source->phase = KP_RGB_SYNC_GATES;
    break;
  case KP_RGB_SYNC_GATES:
    if (++source->gate_word < KP_RGB_OVERLAY_WORDS)
      break;
    source->phase = KP_RGB_SYNC_COLOR;
    if (kp_rgb_selector_registry_count() == 0) {
      source->pending = false;
      source->generation_captured = false;
    }
    break;
  case KP_RGB_SYNC_COLOR:
    source->phase = KP_RGB_SYNC_PERIOD;
    break;
  case KP_RGB_SYNC_PERIOD: {
    struct kp_rgb_selector *selector =
        kp_rgb_selector_registry_at(source->selector_index);
    source->member_index++;
    source->phase = source->member_index == selector->effect_count
                        ? KP_RGB_SYNC_SELECTION
                        : KP_RGB_SYNC_COLOR;
    break;
  }
  case KP_RGB_SYNC_SELECTION:
    source->selector_index++;
    source->member_index = 0;
    if (source->selector_index == kp_rgb_selector_registry_count()) {
      source->pending = false;
      source->generation_captured = false;
    } else {
      source->phase = KP_RGB_SYNC_COLOR;
    }
    break;
  }
}

static void kp_rgb_sync_binding_locked(struct kp_rgb_sync_source *source,
                                       struct zmk_behavior_binding *binding) {
  __ASSERT_NO_MSG(kp_rgb_control_started_locked());
  struct kp_rgb_replication_status status = kp_rgb_replication_status_locked();
  if (!source->generation_captured) {
    source->generation = status.generation;
    source->generation_captured = true;
  }

  uint32_t command;
  uint32_t argument;
  switch (source->phase) {
  case KP_RGB_SYNC_BRIGHTNESS:
    command = RGB_BRT_SET_CMD;
    argument = kp_rgb_global.brightness;
    binding->behavior_dev = kp_rgb_sync_endpoint->name;
    break;
  case KP_RGB_SYNC_POWER:
    command = kp_rgb_global.user_on ? RGB_ON_CMD : RGB_OFF_CMD;
    argument = 0;
    binding->behavior_dev = kp_rgb_sync_endpoint->name;
    break;
  case KP_RGB_SYNC_GATES:
    command = KP_RGB_OVERLAY_WORD_COMMAND(source->gate_word);
    argument = kp_rgb_overlay_get_word(source->gate_word);
    binding->behavior_dev = kp_rgb_sync_endpoint->name;
    break;
  case KP_RGB_SYNC_COLOR:
  case KP_RGB_SYNC_PERIOD:
  case KP_RGB_SYNC_SELECTION: {
    __ASSERT_NO_MSG(source->selector_index < kp_rgb_selector_registry_count());
    struct kp_rgb_selector *selector =
        kp_rgb_selector_registry_at(source->selector_index);
    __ASSERT_NO_MSG(selector->effect_count > 0);
    binding->behavior_dev = selector->owner->name;
    if (source->phase == KP_RGB_SYNC_SELECTION) {
      __ASSERT_NO_MSG(selector->effect_index < selector->effect_count);
      command = RGB_EFS_CMD;
      argument = selector->effect_index;
    } else {
      __ASSERT_NO_MSG(source->member_index < selector->effect_count);
      const struct device *effect =
          kp_rgb_selector_at(selector, source->member_index);
      const struct kp_rgb_effect_common_data *data = kp_rgb_effect_data(effect);
      if (source->phase == KP_RGB_SYNC_COLOR) {
        command = KP_RGB_SELECTOR_MEMBER_COMMAND(
            RGB_SELECTOR_COLOR_CMD, source->member_index);
        argument = RGB_COLOR_HSB_VAL(data->color.h, data->color.s,
                                     data->color.b);
      } else {
        command = KP_RGB_SELECTOR_MEMBER_COMMAND(
            RGB_SELECTOR_PERIOD_CMD, source->member_index);
        argument = data->duration_ms;
      }
    }
    break;
  }
  default:
    __ASSERT_NO_MSG(false);
    command = RGB_BRT_SET_CMD;
    argument = 0;
    binding->behavior_dev = kp_rgb_sync_endpoint->name;
    break;
  }
  binding->param1 = command;
  binding->param2 = argument;
}

static void kp_rgb_sync_attempt_state(size_t index) {
  struct kp_rgb_sync_source *source = &kp_rgb_sync_sources[index];
  struct zmk_behavior_binding binding = {0};

  kp_rgb_matrix_lock();
  kp_rgb_sync_binding_locked(source, &binding);
  kp_rgb_matrix_unlock();

  int err = kp_rgb_sync_send_binding((uint8_t)index, &binding);
  int64_t returned = k_uptime_get();
  if (err < 0) {
    source->ready_at = returned + KP_RGB_SYNC_RETRY_MS;
    return;
  }

  kp_rgb_sync_advance(source);
  source->ready_at = returned;
  kp_rgb_sync_next_state_at = returned + KP_RGB_SYNC_PACE_MS;
  if (!source->pending) {
    kp_rgb_matrix_lock();
    struct kp_rgb_replication_status status = kp_rgb_replication_status_locked();
    bool complete = status.generation == source->generation &&
                    status.outstanding == 0 &&
                    kp_rgb_sync_consumed_repair_epoch == status.repair_epoch;
    kp_rgb_matrix_unlock();
    if (!complete) {
      source->pending = true;
      kp_rgb_sync_reset_cursor(source);
      source->ready_at = returned;
    }
  }
}

static void kp_rgb_sync_attempt_clock(size_t index) {
  struct kp_rgb_sync_source *source = &kp_rgb_sync_sources[index];
  int err = kp_rgb_sync_send_clock((uint8_t)index);
  int64_t returned = k_uptime_get();
  if (err < 0) {
    LOG_WRN("Source %u clock correction failed (err %d)", (uint32_t)index, err);
  }
  source->clock_due = returned + CONFIG_KEYPAW_RGB_SPLIT_SYNC_CLOCK_MS;
}

static bool kp_rgb_sync_slot_due(size_t slot, int64_t now) {
  size_t index = slot / 2;
  struct kp_rgb_sync_source *source = &kp_rgb_sync_sources[index];
  if (!source->seen || source->clock_due == 0)
    return false;
  if ((slot & 1u) != 0) {
    return now >= source->clock_due;
  }
  return source->pending && now >= source->ready_at &&
         now >= kp_rgb_sync_next_state_at;
}

static void kp_rgb_sync_consume_repair(void) {
  kp_rgb_matrix_lock();
  bool started = kp_rgb_control_started_locked();
  struct kp_rgb_replication_status status = kp_rgb_replication_status_locked();
  if (started && status.repair_epoch != kp_rgb_sync_consumed_repair_epoch) {
    kp_rgb_sync_consumed_repair_epoch = status.repair_epoch;
    int64_t now = k_uptime_get();
    for (size_t i = 0; i < ARRAY_SIZE(kp_rgb_sync_sources); i++) {
      struct kp_rgb_sync_source *source = &kp_rgb_sync_sources[i];
      if (source->seen && !source->pending) {
        source->pending = true;
        kp_rgb_sync_reset_cursor(source);
        source->ready_at = MAX(source->ready_at, now);
      }
    }
  }
  kp_rgb_matrix_unlock();
}

static int64_t kp_rgb_sync_next_deadline(int64_t now, bool started) {
  int64_t next = kp_rgb_sync_poll_due;
  for (size_t i = 0; i < ARRAY_SIZE(kp_rgb_sync_sources); i++) {
    const struct kp_rgb_sync_source *source = &kp_rgb_sync_sources[i];
    if (!source->seen || !started)
      continue;
    if (source->clock_due > 0 && source->clock_due < next)
      next = source->clock_due;
    if (source->pending) {
      int64_t state_due = MAX(source->ready_at, kp_rgb_sync_next_state_at);
      if (state_due < next)
        next = state_due;
    }
  }
  return MAX(next - now, 0);
}

static void kp_rgb_sync_handler(struct k_work *work) {
  ARG_UNUSED(work);
  int64_t now = k_uptime_get();
  if (now >= kp_rgb_sync_poll_due) {
    kp_rgb_sync_poll(now);
    kp_rgb_sync_poll_due = now + CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS;
  }

  kp_rgb_sync_consume_repair();

  bool started;
  kp_rgb_matrix_lock();
  started = kp_rgb_control_started_locked();
  kp_rgb_matrix_unlock();
  if (started) {
    for (size_t n = 0; n < KP_RGB_SYNC_SLOTS; n++) {
      size_t slot = (kp_rgb_sync_cursor + n) % KP_RGB_SYNC_SLOTS;
      if (!kp_rgb_sync_slot_due(slot, now))
        continue;
      kp_rgb_sync_cursor = (slot + 1) % KP_RGB_SYNC_SLOTS;
      if ((slot & 1u) != 0) {
        kp_rgb_sync_attempt_clock(slot / 2);
      } else {
        kp_rgb_sync_attempt_state(slot / 2);
      }
      break;
    }
  }

  int64_t delay = kp_rgb_sync_next_deadline(k_uptime_get(), started);
  if (delay == 0)
    delay = KP_RGB_SYNC_YIELD_MS;
  (void)kp_rgb_sync_queue(K_MSEC(MIN(delay, INT32_MAX)), false);
}

static int kp_rgb_split_sync_init(void) {
  kp_rgb_sync_poll_due = k_uptime_get() + CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS;
  atomic_set(&kp_rgb_sync_initialized, 1);
  (void)kp_rgb_sync_queue(K_MSEC(CONFIG_KEYPAW_RGB_SPLIT_SYNC_POLL_MS), false);
  return 0;
}
SYS_INIT(kp_rgb_split_sync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
