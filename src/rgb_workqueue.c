/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include "rgb_matrix_internal.h"

K_THREAD_STACK_DEFINE(kp_rgb_work_q_stack, CONFIG_KEYPAW_RGB_WORKQUEUE_STACK_SIZE);

static struct k_work_q kp_rgb_work_q_instance;

struct k_work_q *kp_rgb_work_q(void) { return &kp_rgb_work_q_instance; }

static int kp_rgb_workqueue_init(void) {
  static const struct k_work_queue_config config = {.name = "RGB Matrix Work Queue"};
  k_work_queue_start(&kp_rgb_work_q_instance, kp_rgb_work_q_stack,
                     K_THREAD_STACK_SIZEOF(kp_rgb_work_q_stack),
                     CONFIG_KEYPAW_RGB_WORKQUEUE_PRIORITY, &config);
  return 0;
}

/* OBJECTS, not DEFAULT: the behavior and effect device inits run at DEFAULT and
 * flush during init, so the queue has to be running by then. */
SYS_INIT(kp_rgb_workqueue_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_OBJECTS);
