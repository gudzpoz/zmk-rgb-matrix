/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <limits.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <dt-bindings/keypaw/rgb_matrix.h>

#include "rgb_replication_internal.h"

#include "rgb_matrix_internal.h"

BUILD_ASSERT(((RGB_TOG_CMD | RGB_ON_CMD | RGB_OFF_CMD | RGB_BRI_CMD | RGB_BRD_CMD |
               RGB_HUI_CMD | RGB_HUD_CMD | RGB_SAI_CMD | RGB_SAD_CMD | RGB_SPI_CMD |
               RGB_SPD_CMD | RGB_COLOR_HSB_CMD | RGB_SELECTOR_COLOR_CMD |
               RGB_SELECTOR_PERIOD_CMD | RGB_BRT_SET_CMD | RGB_RESET_CMD |
               RGB_EFF_CMD | RGB_EFR_CMD | RGB_EFS_CMD | RGB_OVL_STATE_CMD |
               RGB_CLOCK_CMD) & KP_RGB_INVOCATION_MARKER) == 0,
             "RGB invocation marker collides with a command opcode");
BUILD_ASSERT((KP_RGB_INVOCATION_MARKER & 0xffff0000u) == 0,
             "RGB invocation marker overlaps the member slot");

static struct k_spinlock kp_rgb_replication_lock;
static uint64_t kp_rgb_generation;
static uint32_t kp_rgb_outstanding;

void kp_rgb_replication_changed_locked(void) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_replication_lock);
  kp_rgb_generation++;
  k_spin_unlock(&kp_rgb_replication_lock, key);
}

void kp_rgb_replication_effect_changed_locked(const struct device *effect) {
  for (size_t i = 0; i < kp_rgb_selector_registry_count(); i++) {
    const struct kp_rgb_selector *selector = kp_rgb_selector_registry_at(i);
    for (size_t slot = 0; slot < selector->effect_count; slot++) {
      if (selector->members[slot].effect == effect) {
        kp_rgb_replication_changed_locked();
        return;
      }
    }
  }
}

struct kp_rgb_replication_status kp_rgb_replication_status_locked(void) {
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_replication_lock);
  struct kp_rgb_replication_status status = {
      .generation = kp_rgb_generation,
      .outstanding = kp_rgb_outstanding,
  };
  k_spin_unlock(&kp_rgb_replication_lock, key);
  return status;
}

bool kp_rgb_replication_complete_locked(uint64_t generation) {
  struct kp_rgb_replication_status status =
      kp_rgb_replication_status_locked();
  return status.generation == generation && status.outstanding == 0;
}

int kp_rgb_invocation_begin(struct zmk_behavior_binding *binding) {
  if (binding == NULL)
    return -EINVAL;
  if (binding->param1 & KP_RGB_INVOCATION_MARKER)
    return -EALREADY;
  if (kp_rgb_invocation_command(binding->param1) == RGB_CLOCK_CMD)
    return 0;
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_replication_lock);
  if (kp_rgb_outstanding == UINT32_MAX) {
    k_spin_unlock(&kp_rgb_replication_lock, key);
    return -EOVERFLOW;
  }
  kp_rgb_outstanding++;
  binding->param1 |= KP_RGB_INVOCATION_MARKER;
  k_spin_unlock(&kp_rgb_replication_lock, key);
  return 0;
}

void kp_rgb_invocation_finish(struct zmk_behavior_binding *binding) {
  if (binding == NULL || !(binding->param1 & KP_RGB_INVOCATION_MARKER))
    return;
  binding->param1 &= ~KP_RGB_INVOCATION_MARKER;
  k_spinlock_key_t key = k_spin_lock(&kp_rgb_replication_lock);
  __ASSERT(kp_rgb_outstanding > 0, "RGB invocation count underflow");
  if (kp_rgb_outstanding > 0) {
    kp_rgb_outstanding--;
    kp_rgb_generation++;
  }
  k_spin_unlock(&kp_rgb_replication_lock, key);
}
