#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production snapshot emission: singleton commands, immutable preset and retry."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from test_singleton import ROOT, fixture_code, function, structure

MOCKS = r'''
#define MAX(a,b) ((a)>(b)?(a):(b))
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define RGB_EFS_CMD 1
#define RGB_COLOR_HSB_CMD 2
#define RGB_SPI_CMD 3
#define RGB_ON_CMD 4
#define RGB_OFF_CMD 5
#define RGB_OVL_STATE_CMD 6
#define RGB_COLOR_HSB_VAL(h,s,b) (((uint32_t)(h)<<16)|((uint32_t)(s)<<8)|(b))
#define RGB_OVL_STATE_VAL(w,v) (((uint32_t)(w)<<16)|(v))
static uint16_t kp_rgb_overlay_word_count(void) { return 1; }
static uint16_t kp_rgb_overlay_get_word(uint16_t word) { assert(word == 0); return 0x1234; }
static unsigned sends;
static uint32_t commands[16], arguments[16];
static bool fail;
static bool kp_rgb_sync_send(uint8_t source,uint32_t cmd,uint32_t arg) {
    assert(!held && source == 0);
    assert(sends < 16); commands[sends] = cmd; arguments[sends++] = arg;
    return !fail;
}
'''

TESTS = r'''
int main(void) {
    struct kp_rgb_effect_common_data data = {{123,45,67},890};
    const struct device device = {&data,"rgb"};
    kp_rgb_controller.dev = &device;
    kp_rgb_controller.state.active_fx = &device;
    kp_rgb_controller.state.user_on = true;
    kp_rgb_controller.effect_index = 3;
    struct kp_rgb_sync_source state = {.phase = KP_RGB_SYNC_SELECT};
    fail = true;
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_RETRY);
    assert(commands[0] == RGB_EFS_CMD && arguments[0] == 3);
    fail = false;
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[1] == RGB_EFS_CMD && arguments[1] == 3);
    data.color.h = 300; data.duration_ms = 2000;
    kp_rgb_controller.effect_index = 9;
    kp_rgb_controller.state.user_on = false;
    fail = true;
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_RETRY);
    fail = false;
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[2] == RGB_COLOR_HSB_CMD && commands[3] == commands[2]);
    assert(arguments[2] == RGB_COLOR_HSB_VAL(123,45,67) && arguments[3] == arguments[2]);
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[4] == RGB_SPI_CMD && arguments[4] == 890);
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[5] == RGB_ON_CMD);
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_SENT);
    assert(commands[6] == RGB_OVL_STATE_CMD && arguments[6] == 0x1234);
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_DONE && sends == 7);
    state = (struct kp_rgb_sync_source){.phase = KP_RGB_SYNC_SELECT};
    assert(kp_rgb_sync_emit_one(0,&state) == KP_RGB_SYNC_EMIT_SENT);
    assert(arguments[7] == 9);
    puts("singleton split snapshot and same-command retries passed");
    return 0;
}
'''


def main():
    source = (ROOT / "src/rgb_split_sync.c").read_text()
    enums = "\n".join(re.search(r"enum " + name + r" \{.*?\};", source, re.S).group()
                      for name in ("kp_rgb_sync_phase", "kp_rgb_sync_emit"))
    code = fixture_code() + MOCKS + enums + structure(source, "kp_rgb_sync_source")
    code += function(source, "kp_rgb_sync_emit_one") + TESTS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "split.c"
        path.write_text(code)
        binary = Path(directory) / "split"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
