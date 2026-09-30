/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * native_sim preview harness support for the Keypaw RGB matrix module.
 *
 * Three parts, all sim-only:
 *
 *   1. a led_strip device that appends every frame the engine paints to a
 *      capture file instead of driving hardware,
 *   2. the --effect / --capture native simulator command line options,
 *   3. an APPLICATION-priority hook that applies the requested effect and only
 *      then starts recording, so a GIF never opens on the previous effect's
 *      frames.
 *
 * The engine reaches the strip through the ordinary `strip` phandle on
 * keypaw,rgb-matrix, so nothing in the module itself knows this exists.
 *
 * Capture file layout (little-endian, host byte order):
 *
 *   header  "KPRC" | u16 version | u16 led_count | led_count * (u16 x, u16 y)
 *   frames  u32 uptime_ms | led_count * 3 bytes (r, g, b) in chain order
 *
 * The per-LED coordinates come from the engine's own layout resolution, so the
 * host renderer needs no devicetree knowledge.
 */

#define DT_DRV_COMPAT keypaw_led_strip_capture

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_indicators.h>

#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/rgb_matrix.h>

#include "cmdline.h"
#include "soc.h"

#if !DT_HAS_COMPAT_STATUS_OKAY(keypaw_rgb_matrix)
#error "the preview harness needs a keypaw,rgb-matrix node (see tests/sim/config)"
#endif

#if !DT_NODE_EXISTS(DT_NODELABEL(kprgb))
#error "the preview harness needs a &kprgb node (see tests/sim/config/native_sim.overlay)"
#endif

#define KP_SIM_LED_COUNT DT_INST_PROP(0, chain_length)
#define KP_SIM_VERSION 1

static FILE *capture_fp;
static bool capture_enabled;
static bool capture_header_written;

/* ------------------------------------------------------------------------- */
/* 1. the capture led_strip device                                           */
/* ------------------------------------------------------------------------- */

static void capture_fail(const char *what) {
  printk("keypaw-rgb-sim: %s failed, aborting capture\n", what);
  capture_enabled = false;
  if (capture_fp != NULL) {
    fclose(capture_fp);
    capture_fp = NULL;
  }
  /* A partial capture is worse than none: fail the run so the harness sees it. */
  exit(1);
}

static void capture_write(const void *buf, size_t len) {
  if (capture_fp == NULL || fwrite(buf, 1, len, capture_fp) != len) {
    capture_fail("capture write");
  }
}

static void capture_write_header(void) {
  const uint8_t magic[4] = {'K', 'P', 'R', 'C'};
  const uint16_t version = KP_SIM_VERSION;
  const uint16_t led_count = KP_SIM_LED_COUNT;

  capture_write(magic, sizeof(magic));
  capture_write(&version, sizeof(version));
  capture_write(&led_count, sizeof(led_count));

  for (size_t led = 0; led < KP_SIM_LED_COUNT; led++) {
    const struct kp_rgb_coord *coord = kp_rgb_led_coord(led);
    const uint16_t x = coord != NULL ? coord->x : 0;
    const uint16_t y = coord != NULL ? coord->y : 0;

    capture_write(&x, sizeof(x));
    capture_write(&y, sizeof(y));
  }

  capture_header_written = true;
}

static int capture_update_rgb(const struct device *dev, struct led_rgb *pixels,
                              size_t num_pixels) {
  ARG_UNUSED(dev);

  if (!capture_enabled || capture_fp == NULL) {
    return 0;
  }

  if (!capture_header_written) {
    capture_write_header();
  }

  const uint32_t timestamp = (uint32_t)k_uptime_get();
  capture_write(&timestamp, sizeof(timestamp));

  for (size_t led = 0; led < num_pixels; led++) {
    /* Written field by field on purpose: struct led_rgb may carry a scratch
     * byte under CONFIG_LED_STRIP_RGB_SCRATCH, which is not pixel data. */
    const uint8_t rgb[3] = {pixels[led].r, pixels[led].g, pixels[led].b};
    capture_write(rgb, sizeof(rgb));
  }

  return 0;
}

static size_t capture_length(const struct device *dev) {
  ARG_UNUSED(dev);
  return KP_SIM_LED_COUNT;
}

static const struct led_strip_driver_api capture_api = {
    .update_rgb = capture_update_rgb,
    .update_channels = NULL,
    .length = capture_length,
};

static int capture_init(const struct device *dev) {
  ARG_UNUSED(dev);
  return 0;
}

DEVICE_DT_INST_DEFINE(0, capture_init, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_LED_STRIP_INIT_PRIORITY, &capture_api);

/* ------------------------------------------------------------------------- */
/* 2. native simulator command line options                                  */
/* ------------------------------------------------------------------------- */

/* Display names of every effect child of &kprgb, in declaration order. The
 * array position is the effect index the engine uses, so the preview runner can
 * name its output files from here instead of hard-coding a list. Falls back to
 * the node's full name when an effect has no `display-name`.
 *
 * DT_FOREACH_CHILD* inserts no separator, so the trailing comma is required --
 * without it the 45 string literals concatenate into one. */
#define KP_SIM_EFFECT_NAME(node_id)                                            \
  COND_CODE_1(DT_NODE_HAS_PROP(node_id, display_name),                         \
              (DT_PROP(node_id, display_name)),                                \
              (DT_NODE_FULL_NAME(node_id))),                                   \

static const char *const kp_sim_effect_names[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(kprgb), KP_SIM_EFFECT_NAME)};

/* type 'u' defaults to UINT32_MAX and type 's' to NULL, so both are also the
 * "not given" sentinels. */
static uint32_t requested_effect;
static char *capture_path;
static char *list_effects_path;

/* Host-scheduled Caps Lock edges (ms since boot); UINT32_MAX = not requested.
 * The harness cannot press a latch, so Caps Lock is the injectable condition
 * with no layer event. */
static uint32_t caps_on_at;
static uint32_t caps_off_at;

/* Raising zmk_hid_indicators_changed is what the caps-lock condition listens to. */
static void kp_sim_caps_set(bool on) {
  zmk_hid_indicators_t flags = on ? HID_INDICATOR_CAPS_LOCK : 0;
  zmk_hid_indicators_set_profile(flags, zmk_endpoint_get_selected());
}

static void kp_sim_caps_on_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_sim_caps_set(true);
}
static void kp_sim_caps_off_handler(struct k_work *work) {
  ARG_UNUSED(work);
  kp_sim_caps_set(false);
}
static K_WORK_DELAYABLE_DEFINE(kp_sim_caps_on, kp_sim_caps_on_handler);
static K_WORK_DELAYABLE_DEFINE(kp_sim_caps_off, kp_sim_caps_off_handler);

static void kp_sim_add_options(void) {
  static struct args_struct_t options[] = {
      {.option = "effect",
       .name = "index",
       .type = 'u',
       .dest = (void *)&requested_effect,
       .descript = "RGB effect index to select before capturing"},
      {.option = "capture",
       .name = "file",
       .type = 's',
       .dest = (void *)&capture_path,
       .descript = "Append captured frames to this file"},
      {.option = "list-effects",
       .name = "file",
       .type = 's',
       .dest = (void *)&list_effects_path,
       .descript = "Write 'index<TAB>display-name' for every effect to this "
                   "file, then exit"},
      {.option = "caps-on",
       .name = "ms",
       .type = 'u',
       .dest = (void *)&caps_on_at,
       .descript = "Raise the Caps Lock HID indicator this many ms after boot"},
      {.option = "caps-off",
       .name = "ms",
       .type = 'u',
       .dest = (void *)&caps_off_at,
       .descript = "Clear the Caps Lock HID indicator this many ms after boot"},
      ARG_TABLE_ENDMARKER};

  native_add_command_line_opts(options);
}

/* Must be PRE_BOOT_1: those tasks run before the command line is parsed. */
NATIVE_TASK(kp_sim_add_options, PRE_BOOT_1, 1);

/* ------------------------------------------------------------------------- */
/* 3. apply the requested effect, then start recording                       */
/* ------------------------------------------------------------------------- */

/* After the engine's own SYS_INIT(APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY),
 * which Zephyr defaults to 90. Behavior and layout init are POST_KERNEL, so they
 * have long finished by here. */
#define KP_SIM_APPLY_PRIORITY 99

static int kp_sim_apply(void) {
  if (list_effects_path != NULL) {
    /* Previews are named from these, so a typo here shows up as a wrong
     * filename rather than a silently wrong GIF.
     *
     * A file rather than stdout: the POSIX console shim buffers output and
     * treats '\n' as a flush trigger rather than content, so records do not
     * stay newline-delimited on the console. */
    FILE *list = fopen(list_effects_path, "wb");
    if (list == NULL) {
      exit(1);
    }
    for (size_t i = 0; i < ARRAY_SIZE(kp_sim_effect_names); i++) {
      fprintf(list, "%u\t%s\n", (unsigned int)i, kp_sim_effect_names[i]);
    }
    fclose(list);
    exit(0);
  }

  if (capture_path != NULL) {
    capture_fp = fopen(capture_path, "wb");
    if (capture_fp == NULL) {
      printk("keypaw-rgb-sim: cannot open %s for writing\n", capture_path);
      exit(1);
    }
    /* Unbuffered, so a write error surfaces at the failing write rather than
     * being deferred to fclose. */
    setvbuf(capture_fp, NULL, _IONBF, 0);
  }

  if (requested_effect != UINT32_MAX) {
    const int err = zmk_rgb_matrix_select_effect(
        DEVICE_DT_GET(DT_NODELABEL(kprgb)), (uint16_t)requested_effect);
    if (err < 0) {
      printk("keypaw-rgb-sim: effect %u unavailable (%d)\n",
             (unsigned int)requested_effect, err);
    }
  }

  if (capture_fp != NULL) {
    capture_enabled = true;
    /* Repaint now so the capture does not wait a whole animation tick for its
     * first frame. Safe from any context. */
    zmk_rgb_matrix_flush();
    printk("keypaw-rgb-sim: capturing to %s\n", capture_path);
  }

  /* The command line parser leaves an unrequested 'u' option at UINT32_MAX. */
  if (caps_on_at != UINT32_MAX) {
    k_work_schedule(&kp_sim_caps_on, K_MSEC(caps_on_at));
  }
  if (caps_off_at != UINT32_MAX) {
    k_work_schedule(&kp_sim_caps_off, K_MSEC(caps_off_at));
  }

  return 0;
}

SYS_INIT(kp_sim_apply, APPLICATION, KP_SIM_APPLY_PRIORITY);
