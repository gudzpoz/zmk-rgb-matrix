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
#include <string.h>
static size_t kp_rgb_led_for_position(uint32_t key) { return key<KP_LED_COUNT ? key : SIZE_MAX; }
#define KP_RGB_OVERLAY_WORDS 2
static const size_t kp_rgb_all_targets[4]={0,1,2,3};
struct device { const void *config; void *data; const void *api; };
struct kp_rgb_overlay_api { bool replaces_target; };
struct kp_rgb_overlay_common_config { bool all_leds; uint8_t opacity; };
struct kp_rgb_overlay_common_data { bool local,gate; size_t led_count,index; size_t leds[4]; };
static uint16_t kp_overlay_state[KP_RGB_OVERLAY_WORDS];
static unsigned held,locks,flushes;
static void kp_rgb_matrix_lock(void) { assert(!held); held++; locks++; }
static void kp_rgb_matrix_unlock(void) { assert(held); held--; }
static void zmk_rgb_matrix_flush(void) { assert(held); flushes++; }
'''
TESTS = r'''
int main(void) {
    const uint32_t keys[]={3,3,99,1}, leds[]={1,2,0,98};
    size_t resolved[5]={99,99,99,99,99};
    size_t count=kp_rgb_resolve_targets(keys,4,leds,4,resolved,4);
    assert(count==KP_LED_COUNT && resolved[4]==99);
    if(KP_LED_COUNT) {
        const size_t expected[]={3,1,2,0};
        assert(!memcmp(resolved,expected,sizeof(expected)));
        assert(kp_rgb_resolve_targets(keys,4,leds,4,resolved,1)==1 && resolved[0]==3);
    }
    assert(kp_rgb_resolve_targets(keys,4,leds,4,resolved,0)==0);
    struct kp_rgb_overlay_common_config cfg={.all_leds=true,.opacity=100};
    struct kp_rgb_overlay_common_data data={.local=true,.gate=true,.index=17};
    struct kp_rgb_overlay_api api={.replaces_target=true};
    const struct device dev={&cfg,&data,&api};
    assert(kp_rgb_overlay_target_count(&dev)==KP_LED_COUNT);
    if(!KP_LED_COUNT) {
        assert(kp_rgb_overlay_targets(&dev)!=NULL);
        assert(!kp_rgb_overlay_gate(&dev) && !kp_rgb_overlay_covers_all(&dev));
        return 0;
    }
    assert(kp_rgb_overlay_targets(&dev)==kp_rgb_all_targets);
    api.replaces_target=false; assert(!kp_rgb_overlay_covers_all(&dev));
    api.replaces_target=true;
    cfg.opacity=101; assert(!kp_rgb_overlay_covers_all(&dev));
    cfg.opacity=100;
    assert(kp_rgb_overlay_gate(&dev) && kp_rgb_overlay_covers_all(&dev));
    cfg.opacity=0; assert(!kp_rgb_overlay_gate(&dev) && !kp_rgb_overlay_covers_all(&dev));
    cfg.opacity=50; assert(kp_rgb_overlay_gate(&dev) && !kp_rgb_overlay_covers_all(&dev));
    cfg.all_leds=false; assert(!kp_rgb_overlay_gate(&dev));
    assert(kp_rgb_overlay_targets(&dev)==data.leds);
    data.led_count=1; assert(kp_rgb_overlay_gate(&dev));
    assert(!kp_rgb_overlay_covers_all(&dev));
    data.led_count=4; cfg.opacity=100;
    assert(kp_rgb_overlay_covers_all(&dev));
    api.replaces_target=false; assert(!kp_rgb_overlay_covers_all(&dev));
    api.replaces_target=true;
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
        'kp_rgb_target_add', 'kp_rgb_resolve_targets',
        'kp_rgb_overlay_set_word', 'kp_rgb_overlay_get_word',
        'kp_rgb_overlay_target_count', 'kp_rgb_overlay_targets',
        'kp_rgb_overlay_covers_all', 'kp_rgb_overlay_gate')) + TESTS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'gates.c'
        binary = Path(directory) / 'gates'
        for count in (4, 0):
            path.write_text(f'#define KP_LED_COUNT {count}\n' + code)
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-type-limits', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
