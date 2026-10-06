/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "rgb_matrix_internal.h"

static const struct kp_rgb_effect_callbacks *kp_rgb_instance_callbacks(
    const struct kp_rgb_effect_instance *instance) {
  const struct kp_rgb_effect_api *api =
      (const struct kp_rgb_effect_api *)instance->effect->api;
  return api->callbacks;
}

void kp_rgb_callbacks_reset(const struct device *dev,
                            const struct kp_rgb_effect_callbacks *callbacks,
                            struct kp_rgb_callback_state *state, int64_t now_ms) {
  if (callbacks->reset != NULL) {
    callbacks->reset(dev, now_ms);
  }
  state->initialized = true;
  state->animating = false;
}

void kp_rgb_callbacks_set_active(const struct device *dev,
                                 const struct kp_rgb_effect_callbacks *callbacks,
                                 struct kp_rgb_callback_state *state, bool active,
                                 int64_t now_ms) {
  if (state->active == active) {
    return;
  }

  if (active && !state->initialized) {
    kp_rgb_callbacks_reset(dev, callbacks, state, now_ms);
  }

  state->active = active;
  state->animating = false;
  if (callbacks->set_active != NULL) {
    callbacks->set_active(dev, active, now_ms);
  }
}

bool kp_rgb_callbacks_on_event(const struct device *dev,
                               const struct kp_rgb_effect_callbacks *callbacks,
                               const struct kp_rgb_callback_state *state,
                               const struct kp_rgb_key_event *event) {
  if (!state->active || callbacks->on_event == NULL) {
    return false;
  }

  return callbacks->on_event(dev, event);
}

bool kp_rgb_callbacks_render(const struct device *dev,
                             const struct kp_rgb_effect_callbacks *callbacks,
                             struct kp_rgb_callback_state *state,
                             const struct kp_rgb_frame *frame) {
  if (!state->active) {
    return false;
  }

  struct kp_rgb_frame local = *frame;
  local.elapsed_ms = 0;
  if (state->animating && frame->local_ms > state->last_render_ms) {
    uint64_t elapsed = (uint64_t)frame->local_ms - (uint64_t)state->last_render_ms;
    local.elapsed_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
  }

  state->last_render_ms = frame->local_ms;
  state->animating = callbacks->render(dev, &local);
  return state->animating;
}

void kp_rgb_effect_instance_set_active(struct kp_rgb_effect_instance *instance,
                                       bool active, int64_t now_ms) {
  kp_rgb_callbacks_set_active(instance->effect, kp_rgb_instance_callbacks(instance),
                               &instance->state, active, now_ms);
}

void kp_rgb_effect_instance_reset(struct kp_rgb_effect_instance *instance,
                                  int64_t now_ms) {
  kp_rgb_callbacks_reset(instance->effect, kp_rgb_instance_callbacks(instance),
                         &instance->state, now_ms);
}

bool kp_rgb_effect_instance_on_event(struct kp_rgb_effect_instance *instance,
                                     const struct kp_rgb_key_event *event) {
  return kp_rgb_callbacks_on_event(instance->effect,
                                   kp_rgb_instance_callbacks(instance),
                                   &instance->state, event);
}

bool kp_rgb_effect_instance_render(struct kp_rgb_effect_instance *instance,
                                   const struct kp_rgb_frame *frame) {
  return kp_rgb_callbacks_render(instance->effect,
                                 kp_rgb_instance_callbacks(instance),
                                 &instance->state, frame);
}

void kp_rgb_effect_instance_restart_clock(struct kp_rgb_effect_instance *instance) {
  instance->state.animating = false;
}

bool kp_rgb_effect_render(const struct device *dev, const struct kp_rgb_frame *frame) {
  const struct kp_rgb_effect_api *api =
      (const struct kp_rgb_effect_api *)dev->api;
  return api->callbacks->render(dev, frame);
}
