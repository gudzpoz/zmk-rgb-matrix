/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT keypaw_led_strip_composite_probe

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_matrix.h>

#define FRAME_CAPACITY 16
#define PIXEL_CAPACITY 3

struct probe_config {
  size_t length;
};

struct probe_frame {
  struct led_rgb pixels[PIXEL_CAPACITY];
  size_t count;
  unsigned sequence;
  int result;
};

struct probe_data {
  struct probe_frame frames[FRAME_CAPACITY];
  unsigned count;
  unsigned failures;
};

static struct k_spinlock probe_lock;
static unsigned sequence;

static void require(bool success, const char *message) {
  if (!success) {
    printk("FAIL composite engine: %s\n", message);
    exit(1);
  }
}

static int probe_update(const struct device *dev, struct led_rgb *pixels, size_t count) {
  struct probe_data *data = dev->data;
  require(count <= PIXEL_CAPACITY, "probe pixel capacity");
  k_spinlock_key_t key = k_spin_lock(&probe_lock);
  require(data->count < FRAME_CAPACITY, "probe frame capacity");
  struct probe_frame *frame = &data->frames[data->count++];
  memcpy(frame->pixels, pixels, count * sizeof(*pixels));
  frame->count = count;
  frame->sequence = sequence++;
  frame->result = data->failures ? -EIO : 0;
  int result = frame->result;
  if (data->failures) {
    data->failures--;
  }
  k_spin_unlock(&probe_lock, key);
  memset(pixels, 0xa5, count * sizeof(*pixels));
  return result;
}

static size_t probe_length(const struct device *dev) {
  const struct probe_config *cfg = dev->config;
  return cfg->length;
}

static const struct led_strip_driver_api probe_api = {
    .update_rgb = probe_update,
    .length = probe_length,
};

#define PROBE_DEFINE(inst)                                                    \
  static const struct probe_config probe_##inst##_config = {                   \
      .length = DT_INST_PROP(inst, chain_length),                              \
  };                                                                           \
  static struct probe_data probe_##inst##_data;                                 \
  DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &probe_##inst##_data,                  \
                        &probe_##inst##_config, POST_KERNEL, 90, &probe_api);

DT_INST_FOREACH_STATUS_OKAY(PROBE_DEFINE)

static const struct device *const keys = DEVICE_DT_GET(DT_NODELABEL(probe_keys));
static const struct device *const glow = DEVICE_DT_GET(DT_NODELABEL(probe_glow));

static unsigned probe_count(const struct device *dev) {
  struct probe_data *data = dev->data;
  k_spinlock_key_t key = k_spin_lock(&probe_lock);
  unsigned count = data->count;
  k_spin_unlock(&probe_lock, key);
  return count;
}

static struct probe_frame probe_snapshot(const struct device *dev, unsigned index) {
  struct probe_data *data = dev->data;
  k_spinlock_key_t key = k_spin_lock(&probe_lock);
  require(index < data->count, "snapshot index");
  struct probe_frame frame = data->frames[index];
  k_spin_unlock(&probe_lock, key);
  return frame;
}

static void fail_next(unsigned count) {
  struct probe_data *data = keys->data;
  k_spinlock_key_t key = k_spin_lock(&probe_lock);
  data->failures = count;
  k_spin_unlock(&probe_lock, key);
}

static void await_count(unsigned count) {
  int64_t deadline = k_uptime_get() + 1500;
  while (probe_count(keys) < count || probe_count(glow) < count) {
    require(k_uptime_get() < deadline, "output count timeout");
    k_sleep(K_MSEC(1));
  }
  k_sleep(K_MSEC(5));
}

static void check_frame(unsigned index, bool black) {
  static const uint32_t colors[] = {0x102030, 0x405060, 0x708090, 0xa0b0c0, 0xd0e0f0};
  struct probe_frame first = probe_snapshot(keys, index);
  struct probe_frame second = probe_snapshot(glow, index);
  require(first.count == 2 && second.count == 3, "concatenated child lengths");
  require(first.sequence == index * 2 && second.sequence == index * 2 + 1,
          "sequential child order");
  for (size_t i = 0; i < ARRAY_SIZE(colors); i++) {
    struct led_rgb pixel = i < 2 ? first.pixels[i] : second.pixels[i - 2];
    uint32_t color = black ? 0 : colors[i];
    require(pixel.r == (uint8_t)(color >> 16) && pixel.g == (uint8_t)(color >> 8) &&
                pixel.b == (uint8_t)color,
            "fresh routed pixels despite child mutation");
  }
}

static int prepare_automatic_output(void) {
  require(probe_count(keys) == 1 && probe_count(glow) == 1, "startup black before boundary");
  check_frame(0, true);
  fail_next(2);
  return 0;
}
SYS_INIT(prepare_automatic_output, APPLICATION, 98);

static void composite_test(void *a, void *b, void *c) {
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  const struct device *output = DEVICE_DT_GET(DT_NODELABEL(rgb_output));
  require(device_is_ready(output), "composite ready before engine startup");
  require(KP_LED_COUNT == 5 && led_strip_length(output) == 5, "combined matrix length");
  static const uint32_t positions[] = {11, 4, 27, 9, 36};
  static const struct kp_rgb_coord expected[] = {
      {1100, 0}, {400, 0}, {300, 200}, {900, 0}, {0, 300}};
  for (size_t i = 0; i < ARRAY_SIZE(positions); i++) {
    require(kp_rgb_led_for_position(positions[i]) == i, "combined key mapping");
    const struct kp_rgb_coord *coord = kp_rgb_led_coord(i);
    require(coord && coord->x == expected[i].x && coord->y == expected[i].y,
            "combined normalized geometry");
  }

  await_count(1);
  check_frame(0, true);

  await_count(2);
  check_frame(1, false);
  require(probe_snapshot(keys, 1).result == -EIO, "first color failure");
  require(probe_snapshot(glow, 1).result == 0, "later child executes after first child failure");
  k_sleep(K_MSEC(250));
  require(probe_count(keys) == 2 && probe_count(glow) == 2,
          "failed output does not retry autonomously");

  zmk_rgb_matrix_flush();
  await_count(3);
  check_frame(2, false);
  require(probe_snapshot(keys, 2).result == -EIO, "second color failure");
  require(probe_snapshot(glow, 2).result == 0, "later child executes after first child failure");
  k_sleep(K_MSEC(250));
  require(probe_count(keys) == 3 && probe_count(glow) == 3,
          "second failed output does not retry autonomously");

  zmk_rgb_matrix_flush();
  await_count(4);
  check_frame(3, false);
  require(probe_snapshot(keys, 3).result == 0, "color output succeeds");
  k_sleep(K_MSEC(250));
  require(probe_count(keys) == 4 && probe_count(glow) == 4, "settled static output sleeps");

  fail_next(1);
  require(zmk_rgb_matrix_off() == 0, "off request");
  await_count(5);
  check_frame(4, true);
  require(probe_snapshot(keys, 4).result == -EIO, "black output failure");
  k_sleep(K_MSEC(250));
  require(probe_count(keys) == 5 && probe_count(glow) == 5,
          "failed black output does not retry autonomously");

  zmk_rgb_matrix_flush();
  k_sleep(K_MSEC(250));
  require(probe_count(keys) == 5 && probe_count(glow) == 5, "off flush submits no output");

  require(zmk_rgb_matrix_on() == 0, "on request");
  await_count(6);
  check_frame(5, false);
  require(probe_snapshot(keys, 5).result == 0, "resume output succeeds");

  require(zmk_rgb_matrix_off() == 0, "second off request");
  await_count(7);
  check_frame(6, true);
  require(probe_snapshot(keys, 6).result == 0, "black output succeeds");

  require(zmk_rgb_matrix_on() == 0, "second on request");
  await_count(8);
  check_frame(7, false);
  require(probe_snapshot(keys, 7).result == 0, "second resume output succeeds");
  k_sleep(K_MSEC(250));
  require(probe_count(keys) == 8 && probe_count(glow) == 8, "resume settled");
  printk("PASS composite engine integration\n");
  exit(0);
}

K_THREAD_DEFINE(composite_test_thread, 2048, composite_test, NULL, NULL, NULL, 5, 0, 10);
