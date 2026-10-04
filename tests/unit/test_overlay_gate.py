#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Extract production visibility gates and serialized split-state publication."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_singleton import ROOT, function

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define KP_RGB_OVERLAY_WORDS 2
struct device { const void *config; void *data; };
struct kp_rgb_overlay_common_config { bool all_leds; uint8_t opacity; };
struct kp_rgb_overlay_common_data { bool local,gate; size_t led_count,index; };
static uint16_t kp_overlay_state[KP_RGB_OVERLAY_WORDS];
static unsigned held,locks,flushes;
static void kp_rgb_matrix_lock(void) { assert(!held); held++; locks++; }
static void kp_rgb_matrix_unlock(void) { assert(held); held--; }
static void zmk_rgb_matrix_flush(void) { assert(held); flushes++; }
'''
TESTS = r'''
int main(void) {
    struct kp_rgb_overlay_common_config cfg={.all_leds=true,.opacity=100};
    struct kp_rgb_overlay_common_data data={.local=true,.gate=true,.index=17};
    const struct device dev={&cfg,&data};
    assert(kp_rgb_overlay_gate(&dev) && kp_rgb_overlay_covers_all(&dev));
    cfg.opacity=0; assert(!kp_rgb_overlay_gate(&dev) && !kp_rgb_overlay_covers_all(&dev));
    cfg.opacity=50; assert(kp_rgb_overlay_gate(&dev) && !kp_rgb_overlay_covers_all(&dev));
    cfg.all_leds=false; assert(!kp_rgb_overlay_gate(&dev));
    data.led_count=1; assert(kp_rgb_overlay_gate(&dev));
    data.gate=false; assert(!kp_rgb_overlay_gate(&dev));
    data.local=false; assert(!kp_rgb_overlay_gate(&dev));
    assert(kp_rgb_overlay_set_word(1,2) && flushes==1 && !held);
    assert(kp_rgb_overlay_get_word(1)==2 && kp_rgb_overlay_gate(&dev));
    assert(!kp_rgb_overlay_set_word(1,2) && flushes==1);
    cfg.opacity=0; assert(!kp_rgb_overlay_gate(&dev));
    cfg.opacity=100; data.led_count=0; assert(!kp_rgb_overlay_gate(&dev));
    cfg.all_leds=true; assert(kp_rgb_overlay_gate(&dev));
    assert(!kp_rgb_overlay_set_word(2,0xffff) && kp_rgb_overlay_get_word(2)==0);
    assert(locks==5 && flushes==1 && !held);
    puts("overlay visibility and serialized state publication passed");
}
'''

def main():
    source = (ROOT / 'src/rgb_utils.c').read_text()
    code = MOCKS + '\n'.join(function(source, name) for name in (
        'kp_rgb_overlay_set_word', 'kp_rgb_overlay_get_word',
        'kp_rgb_overlay_covers_all', 'kp_rgb_overlay_gate')) + TESTS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'gates.c'
        path.write_text(code)
        binary = Path(directory) / 'gates'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
