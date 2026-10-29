/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <dt-bindings/keypaw/rgb_matrix.h>
#include <zmk/rgb_matrix.h>

#include "rgb_selector_internal.h"

static inline void kp_rgb_assert_thread_context(void) {
  __ASSERT(!k_is_in_isr(), "RGB operation requires thread context");
}

struct kp_rgb_config {
  bool initial_on;
  uint8_t initial_brightness;
  uint8_t max_brightness;
  uint8_t idle_brightness;
};

struct kp_rgb_global {
  bool user_on;
  uint8_t brightness;
};

extern const struct kp_rgb_config kp_rgb_config;
extern struct kp_rgb_global kp_rgb_global;

void kp_rgb_callbacks_set_active(const struct device *dev,
                                const struct kp_rgb_effect_callbacks *callbacks,
                                struct kp_rgb_callback_state *state,
                                bool active, int64_t now_ms);
void kp_rgb_callbacks_reset(const struct device *dev,
                           const struct kp_rgb_effect_callbacks *callbacks,
                           struct kp_rgb_callback_state *state, int64_t now_ms);
bool kp_rgb_callbacks_on_event(const struct device *dev,
                              const struct kp_rgb_effect_callbacks *callbacks,
                              const struct kp_rgb_callback_state *state,
                              const struct kp_rgb_key_event *event);
bool kp_rgb_callbacks_render(const struct device *dev,
                            const struct kp_rgb_effect_callbacks *callbacks,
                            struct kp_rgb_callback_state *state,
                            const struct kp_rgb_frame *frame);

/* Never hold the matrix lock across flash I/O. */
void kp_rgb_matrix_lock(void);
void kp_rgb_matrix_unlock(void);

/* Every engine work item runs here, including behavior invocation chains.
 * The module owns the queue, which must remain running. Callers must not change
 * its lifecycle or cancel engine-owned work. */
struct k_work_q *kp_rgb_work_q(void);

/* Split-shared animation clock: local uptime plus the peripheral offset, in a
 * wrapping uint32_t. The offset is 0 on the central/standalone half. */
uint32_t kp_rgb_clock_ms(void);
/* Set the offset a peripheral derived from RGB_CLOCK_CMD; any context. */
void kp_rgb_set_clock_offset(int32_t offset_ms);

enum kp_rgb_request_reason {
  KP_RGB_REQUEST_CONTROL = BIT(0),
  KP_RGB_REQUEST_REPAINT = BIT(1),
  KP_RGB_REQUEST_INPUT = BIT(2),
  KP_RGB_REQUEST_OUTPUT = BIT(3),
};

/* Any context; CONTROL advances the epoch even when already pending. */
void kp_rgb_coordinator_request(uint32_t reasons);
void kp_rgb_coordinator_start(void);
/* Coordinator only, without the matrix lock. */
void kp_rgb_conditions_run(void);

/* Caller holds the matrix lock. */
void kp_rgb_reconcile_power_locked(void);
void kp_rgb_request_runtime_reset_locked(void);

extern const size_t kp_rgb_all_targets[MAX(1, KP_LED_COUNT)];
void kp_rgb_overlay_targets_init(void);
const size_t *kp_rgb_overlay_targets(const struct device *dev);
size_t kp_rgb_overlay_target_count(const struct device *dev);

/* refresh() runs in the coordinator control phase; dispatch() is central-only. */
bool kp_rgb_overlay_gate(const struct device *dev);
bool kp_rgb_overlay_covers_all(const struct device *dev);
bool kp_rgb_overlay_refresh(void);
void kp_rgb_overlay_dispatch(void);
const struct device *const *kp_rgb_overlay_list(void);
#define KP_RGB_OVERLAY_WORDS MAX(1, DIV_ROUND_UP(KP_RGB_OVERLAY_COUNT, 32))
#define KP_RGB_OVERLAY_WORD_COMMAND(index) \
  (RGB_OVL_STATE_CMD | ((uint32_t)(index) << 16))
#define KP_RGB_OVERLAY_COMMAND_WORD(command) ((uint32_t)(command) >> 16)

/* Word indices must be less than KP_RGB_OVERLAY_WORDS. */
bool kp_rgb_overlay_set_word(size_t index, uint32_t bits);
uint32_t kp_rgb_overlay_get_word(size_t index);

bool kp_rgb_condition_require(const struct device *dev);
bool kp_rgb_condition_valid(const struct device *dev);
void kp_rgb_conditions_start(void);
void kp_rgb_overlay_conditions_init(void);

#if IS_ENABLED(CONFIG_KEYPAW_RGB_TRIGGERS)
void kp_rgb_triggers_init(void);
void kp_rgb_triggers_evaluate(int64_t now_ms);
#else
static inline void kp_rgb_triggers_init(void) {}
static inline void kp_rgb_triggers_evaluate(int64_t now_ms) {
  ARG_UNUSED(now_ms);
}
#endif

#define RGB_BRT_SET_CMD 0x107

int kp_rgb_save_state(void);
int kp_rgb_save_selector_state(const struct device *selector, bool reset);
int kp_rgb_control_commit(void);
/* Caller holds the matrix lock. */
bool kp_rgb_control_started_locked(void);
void kp_rgb_settings_restore_locked(void);

/* Restore global defaults and reconcile local output permission. */
void kp_rgb_apply_defaults(void);
void kp_rgb_reset_state(void);
