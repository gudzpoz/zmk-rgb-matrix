#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile production parameter controls with checked lock/device boundaries."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/behavior_rgb_matrix.c"

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#define ARG_UNUSED(v) (void)(v)
#define RGB_COLOR_HSB_VAL(h,s,b) (((uint32_t)(h)<<16) | ((s)<<8) | (b))
enum {RGB_TOG_CMD, RGB_OFF_CMD, RGB_ON_CMD, RGB_BRI_CMD, RGB_BRD_CMD,
      RGB_HUI_CMD, RGB_HUD_CMD, RGB_SAI_CMD, RGB_SAD_CMD, RGB_COLOR_HSB_CMD,
      RGB_EFR_CMD, RGB_EFF_CMD, RGB_EFS_CMD, RGB_SPI_CMD, RGB_SPD_CMD};
struct zmk_behavior_binding { uint32_t param1, param2; };
struct zmk_behavior_binding_event { int unused; };
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define KP_RGB_HUE_MAX 360
#define KP_RGB_SAT_MAX 100
#define KP_RGB_BRT_MAX 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS 10000
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_STEP 100000
#define CONFIG_KEYPAW_RGB_MATRIX_HUE_STEP 37
#define CONFIG_KEYPAW_RGB_MATRIX_SAT_STEP 10
#define CONFIG_KEYPAW_RGB_MATRIX_BRT_STEP 10
struct kp_rgb_hsb { uint16_t h; uint8_t s, b; };
struct kp_rgb_effect_common_data { struct kp_rgb_hsb color; uint16_t duration_ms; };
struct kp_rgb_effect_defaults { struct kp_rgb_hsb color; uint32_t duration_ms; };
struct device { bool ready; struct kp_rgb_effect_common_data data; };
static struct device effects[3] = {{true}, {true}, {false}};
static struct device embedded_fx = {.ready=true};
static const struct device *const selectable[] = {&effects[0], NULL, &effects[2]};
static struct kp_rgb_effect_defaults defaults[3];
static struct {
    const struct device *const *effects;
    struct kp_rgb_effect_defaults *effect_defaults;
    size_t effect_count, effect_index, initial_effect;
    bool initial_on;
    int initial_brightness;
    int64_t initial_duration_ms;
    struct { const struct device *active_fx; bool user_on; } state;
    struct { uint8_t max_brightness; } tuning;
    uint8_t brightness;
} kp_rgb_controller = {.effects=selectable, .effect_defaults=defaults, .effect_count=3};
static bool isr, locked;
static unsigned flushes, lock_calls;
static const struct device *select_on_lock;
static bool k_is_in_isr(void) { return isr; }
static void kp_rgb_matrix_lock(void) {
    assert(!locked && !isr); locked=true; lock_calls++;
    if (select_on_lock) {
        kp_rgb_controller.state.active_fx=select_on_lock; select_on_lock=NULL;
    }
}
static void kp_rgb_matrix_unlock(void) { assert(locked); locked=false; }
static bool device_is_ready(const struct device *dev) {
    assert(dev != NULL); return dev->ready;
}
static struct kp_rgb_effect_common_data *kp_rgb_effect_data(const struct device *dev) {
    assert(locked && dev != NULL);
    return &((struct device *)dev)->data;
}
static uint32_t kp_rgb_effect_period(const struct device *dev) {
    return kp_rgb_effect_data(dev)->duration_ms;
}
static void zmk_rgb_matrix_flush(void) { assert(locked); flushes++; }
static void kp_rgb_reconcile_power_locked(void) { assert(locked); }
'''

TESTS = r'''
int main(void) {
    const struct device *private_fx=&effects[1];
    struct kp_rgb_hsb c={360,100,100};
    assert(zmk_rgb_matrix_set_color(NULL,&c)==-ENODEV);
    assert(zmk_rgb_matrix_set_period(NULL,100)==-ENODEV);
    assert(zmk_rgb_matrix_set_color(&effects[2],&c)==-ENODEV);
    assert(zmk_rgb_matrix_set_period(&effects[2],100)==-ENODEV);
    assert(zmk_rgb_matrix_set_color(private_fx,NULL)==-EINVAL);
    assert(zmk_rgb_matrix_set_color(private_fx,&c)==0);
    assert(effects[1].data.color.h==0 && flushes==1);
    assert(zmk_rgb_matrix_set_color(private_fx,&c)==0 && flushes==1);
    c.h=361; assert(zmk_rgb_matrix_set_color(private_fx,&c)==-EINVAL);
    c.h=0; c.s=101; assert(zmk_rgb_matrix_set_color(private_fx,&c)==-EINVAL);
    c.s=0; c.b=101; assert(zmk_rgb_matrix_set_color(private_fx,&c)==-EINVAL);
    c.b=0; assert(zmk_rgb_matrix_set_color(private_fx,&c)==0);
    uint32_t bad[]={0,99,10001,65536+100,0x80000000u,UINT32_MAX};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++)
        assert(zmk_rgb_matrix_set_period(private_fx,bad[i])==-EINVAL);
    assert(zmk_rgb_matrix_set_period(private_fx,100)==0);
    unsigned n=flushes;
    assert(zmk_rgb_matrix_set_period(private_fx,100)==0 && flushes==n);
    assert(zmk_rgb_matrix_set_period(private_fx,10000)==0);
    kp_rgb_controller.state.active_fx=NULL;
    assert(kp_rgb_set_hsb(c)==-ENODEV && kp_rgb_set_duration(100)==-ENODEV);
    select_on_lock=private_fx;
    assert(kp_rgb_set_duration(100)==0 && effects[1].data.duration_ms==100);
    assert(kp_rgb_set_duration(UINT32_MAX)==-EINVAL);
    assert(kp_rgb_change_duration(INT16_MAX)==0 && effects[1].data.duration_ms==10000);
    assert(kp_rgb_change_duration(INT16_MIN)==0 && effects[1].data.duration_ms==100);
    unsigned locks=lock_calls;
    assert(kp_rgb_change_hue(INT8_MIN)==0 && lock_calls==locks+1);
    assert(effects[1].data.color.h<360);
    assert(kp_rgb_change_sat(127)==0 && effects[1].data.color.s==100);
    assert(kp_rgb_change_sat(-128)==0 && effects[1].data.color.s==0);
    assert(kp_rgb_change_brt(127)==0 && kp_rgb_controller.brightness==100);
    assert(kp_rgb_change_brt(-128)==0 && kp_rgb_controller.brightness==0);
    (void)kp_rgb_calc_hue(1); (void)kp_rgb_calc_sat(1); (void)kp_rgb_calc_brt(1);
    assert(kp_rgb_select_effect(3)==-EINVAL);
    assert(kp_rgb_select_effect(1)==-ENOENT);
    assert(zmk_rgb_matrix_select_effect(0)==0);
    n=flushes; assert(zmk_rgb_matrix_select_effect(0)==0 && flushes==n);
    assert(zmk_rgb_matrix_cycle_effect(0)==0 && flushes==n);
    assert(kp_rgb_calc_effect(1)==2);
    isr=true; locks=lock_calls;
    assert(zmk_rgb_matrix_set_color(private_fx,&c)==-EWOULDBLOCK);
    assert(zmk_rgb_matrix_set_period(private_fx,100)==-EWOULDBLOCK);
    assert(kp_rgb_set_hsb(c)==-EWOULDBLOCK);
    assert(kp_rgb_set_duration(100)==-EWOULDBLOCK);
    assert(kp_rgb_change_hue(1)==-EWOULDBLOCK);
    assert(kp_rgb_change_sat(1)==-EWOULDBLOCK);
    assert(kp_rgb_change_brt(1)==-EWOULDBLOCK);
    assert(kp_rgb_change_duration(1)==-EWOULDBLOCK);
    assert(zmk_rgb_matrix_select_effect(0)==-EWOULDBLOCK);
    assert(zmk_rgb_matrix_cycle_effect(1)==-EWOULDBLOCK);
    assert(lock_calls==locks); isr=false;
    defaults[0].duration_ms=UINT32_MAX;
    defaults[2].duration_ms=1;
    kp_rgb_controller.initial_duration_ms=-1;
    kp_rgb_controller.initial_brightness=30;
    n=flushes; assert(kp_rgb_apply_defaults()==0 && flushes>n);
    assert(effects[0].data.duration_ms==10000 && effects[2].data.duration_ms==100);
    assert(kp_rgb_controller.brightness==30);
    defaults[0].duration_ms=0;
    assert(kp_rgb_apply_defaults()==0 && effects[0].data.duration_ms==100);
    kp_rgb_controller.initial_duration_ms=UINT32_MAX;
    assert(kp_rgb_apply_defaults()==0 && effects[0].data.duration_ms==10000);
    struct zmk_behavior_binding binding={.param1=RGB_SPD_CMD};
    struct zmk_behavior_binding_event event={0};
    locks=lock_calls;
    assert(on_keymap_binding_convert_central_state_dependent_params(&binding,event)==0);
    assert(binding.param1==RGB_SPI_CMD && binding.param2==100 && lock_calls==locks+1);
    binding.param1=RGB_HUI_CMD;
    assert(on_keymap_binding_convert_central_state_dependent_params(&binding,event)==0);
    assert(binding.param1==RGB_COLOR_HSB_CMD && (binding.param2>>16)==37);
    binding.param1=RGB_EFF_CMD;
    assert(on_keymap_binding_convert_central_state_dependent_params(&binding,event)==0);
    assert(binding.param1==RGB_EFS_CMD && binding.param2==2);
    binding.param1=999;
    assert(on_keymap_binding_convert_central_state_dependent_params(&binding,event)==0);
    c=(struct kp_rgb_hsb){47,66,88}; n=flushes;
    assert(zmk_rgb_matrix_set_color(&embedded_fx,&c)==0 && flushes==n+1);
    assert(embedded_fx.data.color.h==47 && embedded_fx.data.color.s==66 &&
           embedded_fx.data.color.b==100);
    assert(zmk_rgb_matrix_set_color(&embedded_fx,&c)==0 && flushes==n+1);
    assert(zmk_rgb_matrix_set_period(&embedded_fx,2345)==0 && flushes==n+2);
    assert(embedded_fx.data.duration_ms==2345);
    assert(zmk_rgb_matrix_set_period(&embedded_fx,2345)==0 && flushes==n+2);
    assert(zmk_rgb_matrix_set_period(&embedded_fx,UINT32_MAX)==-EINVAL);
    embedded_fx.ready=false;
    assert(zmk_rgb_matrix_set_color(&embedded_fx,&c)==-ENODEV);
    assert(zmk_rgb_matrix_set_period(&embedded_fx,100)==-ENODEV);
    assert(flushes==n+2 && embedded_fx.data.duration_ms==2345);
    assert(!locked);
}
'''


def main():
    text = SOURCE.read_text()
    # No persistence/split mocks: any direct side effect fails compilation/linking.
    production = text[text.index("size_t kp_rgb_effect_count("):
                      text.index("int kp_rgb_effect_convert_central_state_dependent_params(")]
    production += text[text.index("static int on_keymap_binding_convert_central_state_dependent_params("):
                       text.index("static int on_keymap_binding_pressed(")]
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "parameter_controls.c"
        binary = Path(tmp) / "parameter_controls"
        source.write_text(MOCKS + production + TESTS)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       ["-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", str(source), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)
    print("parameter controls: passed")


if __name__ == "__main__":
    main()
