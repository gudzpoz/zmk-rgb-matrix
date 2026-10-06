#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Split shared-clock emission and offset arithmetic."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_singleton import ROOT, function

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define KP_RGB_SYNC_SOURCES 3
#define RGB_CLOCK_CMD 0x103

typedef long atomic_t;
typedef long atomic_val_t;
static atomic_t kp_rgb_clock_offset_ms;
static atomic_val_t atomic_get(atomic_t *t) { return *t; }
static void atomic_set(atomic_t *t, atomic_val_t v) { *t = v; }

static int64_t fake_uptime;
static int64_t k_uptime_get(void) { return fake_uptime; }

struct zmk_split_transport_central_api {
  int (*get_available_source_ids)(uint8_t *sources);
};
struct zmk_split_transport_central {
  const struct zmk_split_transport_central_api *api;
};

static const struct zmk_split_transport_central *transport;
static const struct zmk_split_transport_central *kp_rgb_sync_pick_transport(void) {
  return transport;
}
static uint8_t available[KP_RGB_SYNC_SOURCES];
static int available_count;
static int get_ids(uint8_t *sources) {
  for (int i = 0; i < available_count; i++) sources[i] = available[i];
  return available_count;
}
static const struct zmk_split_transport_central_api api = {.get_available_source_ids = get_ids};
static const struct zmk_split_transport_central active = {.api = &api};

static unsigned sends;
static uint8_t sent_source[8];
static uint32_t sent_cmd[8], sent_arg[8];
static bool kp_rgb_sync_send(uint8_t source, uint32_t cmd, uint32_t arg) {
  assert(sends < 8);
  sent_source[sends] = source; sent_cmd[sends] = cmd; sent_arg[sends++] = arg;
  return true;
}
'''

TESTS = r'''
int main(void) {
  /* A peripheral reconstructs the central value from its own local uptime plus
   * the offset it derives, including across the 32-bit wrap. */
  for (int i = 0; i < 2; i++) {
    uint32_t central = i == 0 ? 123456u : 10u;
    uint32_t local = i == 0 ? 654321u : 0xFFFFFFF0u;
    int32_t offset = (int32_t)(central - local);
    assert((uint32_t)local + (uint32_t)offset == central);
  }

  /* kp_rgb_clock_ms = local uptime + offset, wrapping in uint32_t. */
  fake_uptime = 5000;
  assert(kp_rgb_clock_ms() == 5000u);
  kp_rgb_set_clock_offset(1500);
  assert(kp_rgb_clock_ms() == 6500u);
  kp_rgb_set_clock_offset(-2000);
  assert(kp_rgb_clock_ms() == 3000u);
  fake_uptime = 0xFFFFFF00;
  kp_rgb_set_clock_offset(0x200);
  assert(kp_rgb_clock_ms() == 0x100u);
  kp_rgb_set_clock_offset(0);

  /* The heartbeat sends the central uptime to every present source. */
  transport = &active;
  fake_uptime = 98765;
  available[0] = 0; available[1] = 2; available_count = 2;
  kp_rgb_sync_send_clock();
  assert(sends == 2);
  assert(sent_source[0] == 0 && sent_cmd[0] == RGB_CLOCK_CMD && sent_arg[0] == 98765u);
  assert(sent_source[1] == 2 && sent_cmd[1] == RGB_CLOCK_CMD && sent_arg[1] == 98765u);

  /* No transport -> no sends. */
  sends = 0; transport = NULL;
  kp_rgb_sync_send_clock();
  assert(sends == 0);

  /* A negative source count is ignored. */
  sends = 0; transport = &active; available_count = -1;
  kp_rgb_sync_send_clock();
  assert(sends == 0);

  puts("split clock sync passed");
  return 0;
}
'''


def main():
    engine = (ROOT / "src/rgb_matrix.c").read_text()
    sync = (ROOT / "src/rgb_split_sync.c").read_text()
    code = MOCKS
    code += function(engine, "kp_rgb_clock_ms")
    code += function(engine, "kp_rgb_set_clock_offset")
    code += function(sync, "kp_rgb_sync_send_clock")
    code += TESTS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "clock.c"
        path.write_text(code)
        binary = Path(directory) / "clock"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
