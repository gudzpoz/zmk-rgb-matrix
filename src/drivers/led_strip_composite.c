/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_led_strip_composite

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

struct composite_child {
  const struct device *dev;
  size_t length;
};

struct composite_config {
  const struct composite_child *children;
  size_t child_count;
  size_t length;
};

static const struct led_strip_driver_api composite_api;

static bool composite_overlaps(const struct device *left, const struct device *right) {
  if (left == right) {
    return true;
  }
  if (left->api == &composite_api) {
    const struct composite_config *cfg = left->config;
    for (size_t i = 0; i < cfg->child_count; i++) {
      if (composite_overlaps(cfg->children[i].dev, right)) {
        return true;
      }
    }
    return false;
  }
  if (right->api == &composite_api) {
    return composite_overlaps(right, left);
  }
  return false;
}

static int composite_init(const struct device *dev) {
  const struct composite_config *cfg = dev->config;

  for (size_t i = 0; i < cfg->child_count; i++) {
    const struct composite_child *child = &cfg->children[i];
    if (!device_is_ready(child->dev)) {
      return -ENODEV;
    }
    const struct led_strip_driver_api *api = child->dev->api;
    if (api == NULL || api->update_rgb == NULL || api->length == NULL) {
      return -ENOTSUP;
    }
    if (led_strip_length(child->dev) != child->length) {
      return -EINVAL;
    }
    for (size_t j = 0; j < i; j++) {
      if (composite_overlaps(child->dev, cfg->children[j].dev)) {
        return -EINVAL;
      }
    }
  }
  return 0;
}

static int composite_update_rgb(const struct device *dev, struct led_rgb *pixels,
                                size_t num_pixels) {
  const struct composite_config *cfg = dev->config;
  if (num_pixels > cfg->length) {
    return -ERANGE;
  }
  if (num_pixels == 0) {
    return 0;
  }
  if (pixels == NULL) {
    return -EINVAL;
  }

  int first_error = 0;
  size_t offset = 0;
  for (size_t i = 0; i < cfg->child_count && offset < num_pixels; i++) {
    const struct composite_child *child = &cfg->children[i];
    size_t count = MIN(child->length, num_pixels - offset);
    int err = led_strip_update_rgb(child->dev, pixels + offset, count);
    if (err < 0 && first_error == 0) {
      first_error = err;
    }
    offset += count;
  }
  return first_error;
}

static size_t composite_length(const struct device *dev) {
  const struct composite_config *cfg = dev->config;
  return cfg->length;
}

static const struct led_strip_driver_api composite_api = {
    .update_rgb = composite_update_rgb,
    .length = composite_length,
};

#define COMPOSITE_CHILD(node, prop, idx)                                       \
  {                                                                            \
      .dev = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node, prop, idx)),                  \
      .length = DT_PROP(DT_PHANDLE_BY_IDX(node, prop, idx), chain_length),       \
  },

#define COMPOSITE_CHILD_LENGTH(node, prop, idx)                                \
  + (uint64_t)DT_PROP(DT_PHANDLE_BY_IDX(node, prop, idx), chain_length)

#define COMPOSITE_POSITIVE_LENGTH(node)                                        \
  (DT_PROP(node, chain_length) > 0 && DT_PROP(node, chain_length) <= INT32_MAX)

#define COMPOSITE_ASSERT_CHILD(node, prop, idx)                                \
  BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_PHANDLE_BY_IDX(node, prop, idx), okay),     \
               "composite strips must be enabled");                            \
  BUILD_ASSERT(COMPOSITE_POSITIVE_LENGTH(DT_PHANDLE_BY_IDX(node, prop, idx)),    \
               "composite child chain-length must be positive");              \
  BUILD_ASSERT(DT_DEP_ORD(node) !=                                              \
                   DT_DEP_ORD(DT_PHANDLE_BY_IDX(node, prop, idx)),             \
               "composite strip must not reference itself");

#define COMPOSITE_UNIQUE_CHILD(node, prop, idx)                                \
  UTIL_CAT(composite_child_, DT_DEP_ORD(DT_PHANDLE_BY_IDX(node, prop, idx))),

#define COMPOSITE_DEFINE(inst)                                                \
  BUILD_ASSERT(DT_INST_PROP_LEN(inst, strips) > 0,                             \
               "composite strips must not be empty");                          \
  BUILD_ASSERT(COMPOSITE_POSITIVE_LENGTH(DT_DRV_INST(inst)),                   \
               "composite chain-length must be positive");                    \
  DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), strips, COMPOSITE_ASSERT_CHILD)        \
  BUILD_ASSERT(DT_INST_PROP(inst, chain_length) ==                              \
                   (0 DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), strips,          \
                                           COMPOSITE_CHILD_LENGTH)),          \
               "composite chain-length must equal the sum of its strips");     \
  static const struct composite_child composite_##inst##_children[] = {       \
      DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), strips, COMPOSITE_CHILD)};         \
  static const struct composite_config composite_##inst##_config = {          \
      .children = composite_##inst##_children,                                 \
      .child_count = ARRAY_SIZE(composite_##inst##_children),                  \
      .length = DT_INST_PROP(inst, chain_length),                              \
  };                                                                           \
  static int composite_##inst##_init(const struct device *dev) {               \
    enum {                                                                     \
      DT_FOREACH_PROP_ELEM(DT_DRV_INST(inst), strips, COMPOSITE_UNIQUE_CHILD)    \
    };                                                                         \
    return composite_init(dev);                                                \
  }                                                                            \
  DEVICE_DT_INST_DEFINE(inst, composite_##inst##_init, NULL, NULL,             \
                        &composite_##inst##_config, POST_KERNEL,               \
                        CONFIG_LED_STRIP_COMPOSITE_INIT_PRIORITY,              \
                        &composite_api);

DT_INST_FOREACH_STATUS_OKAY(COMPOSITE_DEFINE)
