/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <drivers/behavior.h>

#include "rgb_matrix_internal.h"

#define KP_RGB_INVOCATION_MARKER BIT(15)
static inline uint32_t kp_rgb_invocation_command(uint32_t command) {
  return command & ~KP_RGB_INVOCATION_MARKER;
}

/* Generation covers publications and invocation completion. */
struct kp_rgb_replication_status {
  uint64_t generation;
  uint32_t outstanding;
  uint64_t repair_epoch;
};

/* Publication, status and completion checks require the matrix mutex. */
#if IS_ENABLED(CONFIG_KEYPAW_RGB_SPLIT_SYNC)
void kp_rgb_replication_changed_locked(void);
void kp_rgb_replication_effect_changed_locked(const struct device *effect);
struct kp_rgb_replication_status kp_rgb_replication_status_locked(void);
bool kp_rgb_replication_complete_locked(uint64_t generation);
/* Begin/finish use only the replication spinlock; finish pairs the marker. */
int kp_rgb_invocation_begin(struct zmk_behavior_binding *binding);
void kp_rgb_invocation_finish(struct zmk_behavior_binding *binding, int result);
#else
static inline void kp_rgb_replication_changed_locked(void) {}
static inline void kp_rgb_replication_effect_changed_locked(const struct device *effect) {
  ARG_UNUSED(effect);
}
static inline struct kp_rgb_replication_status kp_rgb_replication_status_locked(void) {
  return (struct kp_rgb_replication_status){0};
}
static inline bool kp_rgb_replication_complete_locked(uint64_t generation) {
  return generation == 0;
}
static inline int kp_rgb_invocation_begin(struct zmk_behavior_binding *binding) {
  ARG_UNUSED(binding);
  return 0;
}
static inline void kp_rgb_invocation_finish(struct zmk_behavior_binding *binding, int result) {
  ARG_UNUSED(result);
  binding->param1 = kp_rgb_invocation_command(binding->param1);
}
#endif

/* Drain converted accounting before asserting: an assertion handler may terminate
 * only the caller. This does not make ISR invocation supported. */
static inline void
kp_rgb_invocation_assert_thread(struct zmk_behavior_binding *binding) {
  if (k_is_in_isr())
    kp_rgb_invocation_finish(binding, -EWOULDBLOCK);
  kp_rgb_assert_thread_context();
}
