#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production battery overlay preserves non-target pixels and orders physical targets."""
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
#include <stddef.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define ARG_UNUSED(x) (void)(x)
#define IS_ENABLED(x) (x)
#define CONFIG_ZMK_BATTERY_REPORTING 1
typedef struct { int unused; } zmk_event_t;
#define ZMK_EV_EVENT_BUBBLE 0
static unsigned flushes;
static void zmk_rgb_matrix_flush(void) { flushes++; }
struct device { const void *config; void *data; };
struct led_rgb { uint8_t r,g,b; };
struct kp_rgb_coord { uint16_t x,y; };
struct kp_rgb_frame {
 size_t count; const struct kp_rgb_coord *coords; struct led_rgb *pixels;
 const size_t *targets; size_t target_count; struct led_rgb *scratch;
};
struct kp_rgb_overlay_common_config { bool all_leds; uint8_t opacity; };
struct kp_rgb_overlay_common_data { size_t leds[4],led_count; };
struct kp_ovl_battery_config {
 struct kp_rgb_overlay_common_config common; uint32_t color,background_color; bool reverse;
};
struct kp_ovl_battery_data { struct kp_rgb_overlay_common_data common; };
static size_t kp_ovl_battery_order_buf[KP_LED_COUNT ? KP_LED_COUNT : 1];
static uint8_t soc;
static uint8_t zmk_battery_state_of_charge(void) { return soc; }
static struct led_rgb kp_hex_to_rgb(uint32_t c) { return (struct led_rgb){c>>16,c>>8,c}; }
'''
TESTS = r'''
int main(void) {
 const zmk_event_t event={0};
 assert(kp_rgb_battery_overlay_listener(&event)==ZMK_EV_EVENT_BUBBLE && flushes==1);
 struct kp_ovl_battery_config cfg={.common={.opacity=100},.color=0xc94965,.background_color=0x07111f};
 struct kp_ovl_battery_data data={0};
 struct device dev={&cfg,&data};
 const struct kp_rgb_coord coords[4]={{300,0},{100,0},{200,0},{0,0}};
 const size_t targets[4]={2,3,0,1};
 struct led_rgb pixels[4],workspace[4];
 struct kp_rgb_frame frame={.count=KP_LED_COUNT,.coords=coords,.pixels=pixels,
     .targets=targets,.target_count=KP_LED_COUNT,.scratch=workspace};
 memset(pixels,0xa5,sizeof(pixels)); soc=50;
 kp_ovl_battery_render(&dev,&frame);
 if(!KP_LED_COUNT) {
   for(size_t i=0;i<4;i++) assert(pixels[i].r==0xa5);
   return 0;
 }
 assert(pixels[3].r==201 && pixels[1].r==201 && pixels[2].r==7 && pixels[0].r==7);
 assert(pixels[3].g==73 && pixels[3].b==101);
 assert(pixels[0].g==17 && pixels[0].b==31);
 memset(pixels,0xa5,sizeof(pixels)); frame.target_count=2;
 kp_ovl_battery_render(&dev,&frame);
 assert(pixels[3].r==201 && pixels[2].r==7);
 assert(pixels[0].r==0xa5 && pixels[1].r==0xa5);
 cfg.reverse=true; kp_ovl_battery_render(&dev,&frame);
 assert(pixels[2].r==201 && pixels[3].r==7);
 assert(targets[0]==2 && targets[1]==3);
 frame.target_count=0; memset(pixels,0xa5,sizeof(pixels));
 kp_ovl_battery_render(&dev,&frame);
 for(size_t i=0;i<4;i++) assert(pixels[i].r==0xa5);
 frame.target_count=4; soc=255;
 kp_ovl_battery_render(&dev,&frame);
 for(size_t i=0;i<4;i++) assert(pixels[i].r==201);
}
'''

if __name__ == '__main__':
    source = (ROOT / 'src/overlays/battery.c').read_text()
    code = MOCKS + function((ROOT / 'src/rgb_color.c').read_text(), 'kp_rgb_rgb_mix')
    code += '\n'.join(function(source, name) for name in (
        'kp_ovl_battery_before', 'kp_ovl_battery_order', 'kp_ovl_battery_render',
        'kp_rgb_battery_overlay_listener'))
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'battery.c'
        binary = path.with_suffix('')
        for count in (4, 0):
            path.write_text(f'#define KP_LED_COUNT {count}\n' + code + TESTS)
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                '-Wno-unused-variable', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print('battery target ordering, intrinsic colors, sparse and empty sentinels passed')
