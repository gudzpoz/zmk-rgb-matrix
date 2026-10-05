#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production Scene/Scratch composition, target clearing and mutable-driver isolation."""
from engine_harness import ROOT, run_tests
from test_singleton import function
from test_engine_scheduler import TESTS as SCHEDULER_TESTS

FIXTURE = SCHEDULER_TESTS[:SCHEDULER_TESTS.index('static void assert_pixels')]
MOCKS = r'''
struct kp_rgb_overlay_common_config { bool all_leds; uint8_t opacity; };
struct kp_ovl_config { struct kp_rgb_overlay_common_config common; const struct device *effect; };
struct kp_ovl_data { struct kp_rgb_overlay_common_data common; };
'''
TESTS = r'''
#define child_state (kp_rgb_overlay_scenes[0].state)
static const size_t *expected_child_targets;
static size_t expected_child_count;
static unsigned paint_order;
static bool check_raw_elapsed;
static bool child_paint(const struct device *d,const struct kp_rgb_frame *f) {
    (void)d;
    if(check_raw_elapsed) assert(f->elapsed_ms==37);
    assert(f->pixels==scratch && f->scratch==NULL && f->targets);
    if(expected_child_targets) {
        assert(f->targets==expected_child_targets && f->target_count==expected_child_count);
        paint_order=paint_order*10+1;
    }
    for(size_t j=0;j<f->target_count;j++) {
        size_t led=f->targets[j];
        assert(!f->pixels[led].r && !f->pixels[led].g && !f->pixels[led].b);
        f->pixels[led]=(struct led_rgb){104,73,101};
    }
    return false;
}
static const struct kp_rgb_effect_callbacks child_cb={.render=child_paint};
static const struct kp_rgb_effect_api child_api={.callbacks=&child_cb};
static const struct device child={.api=&child_api};
static const struct kp_rgb_effect_callbacks ov_callbacks={.render=kp_ovl_render,.reset=kp_ovl_reset,.set_active=kp_ovl_set_active,.on_event=kp_ovl_on_event};
static const struct kp_rgb_overlay_api ov_api={.callbacks=&ov_callbacks,.replaces_target=true};

#define second_state_runtime (kp_rgb_overlay_scenes[1].state)
static const size_t *expected_second_targets;
static size_t expected_second_count;
static bool leave_last_black;
static bool second_paint(const struct device *d,const struct kp_rgb_frame *f) {
    (void)d;
    assert(f->pixels==scratch && !f->scratch && f->count==KP_LED_COUNT);
    assert(f->targets && f->targets==expected_second_targets);
    assert(f->target_count==expected_second_count);
    paint_order=paint_order*10+2;
    for(size_t j=0;j<f->target_count;j++) {
        size_t led=f->targets[j];
        assert(!f->pixels[led].r && !f->pixels[led].g && !f->pixels[led].b);
        if(!leave_last_black || j+1<f->target_count)
            f->pixels[led]=(struct led_rgb){200,40,10};
    }
    return false;
}
static const struct kp_rgb_effect_callbacks second_cb={.render=second_paint};
static const struct kp_rgb_effect_api second_api={.callbacks=&second_cb};
static const struct device second_child={.api=&second_api};
static unsigned filter_paints;
static bool filter_paint(const struct device *d,const struct kp_rgb_frame *f) {
    const struct host_overlay *o=d->data;
    assert(f->pixels==scene && f->scratch==scratch && f->targets==o->targets);
    assert(f->target_count==1 && f->targets[0]==1 && runtime.active);
    for(size_t i=0;i<f->count;i++)
        assert(f->pixels[i].r==17 && !f->pixels[i].g && !f->pixels[i].b);
    f->pixels[f->targets[0]]=(struct led_rgb){51,25,9};
    filter_paints++;
    return false;
}
static const struct kp_rgb_effect_callbacks filter_callbacks={.render=filter_paint};
static const struct kp_rgb_overlay_api filter_api={.callbacks=&filter_callbacks,.replaces_target=false};
static void expect_output(struct led_rgb first,struct led_rgb second) {
    const struct led_rgb expected[2]={first,second};
    assert(host_transfer_count);
    for(size_t i=0;i<KP_LED_COUNT;i++) {
        assert(!memcmp(&scene[i],&expected[i],sizeof(expected[i])));
        assert(!memcmp(&host_transfers[host_transfer_count-1].pixels[i],&expected[i],sizeof(expected[i])));
        assert(scratch[i].r==0xa5 && scratch[i].g==0xa5 && scratch[i].b==0xa5);
    }
}
static void test_engine_composition(void) {
    const size_t sparse[]={1}, reordered[]={1,0};
    struct kp_ovl_config first_cfg={.common={.opacity=37},.effect=&child};
    struct kp_ovl_config second_cfg={.common={.opacity=61},.effect=&second_child};
    struct host_overlay first_state={.child=&child,.gate=true,.targets=sparse,.target_count=1};
    struct host_overlay second_state={.common={.index=1},.child=&second_child,.gate=true,.targets=reordered,.target_count=2};
    struct device first={.api=&ov_api,.config=&first_cfg,.data=&first_state};
    struct device second={.api=&ov_api,.config=&second_cfg,.data=&second_state};
    struct host_overlay filter_state={.common={.index=2},.cover=false,.targets=sparse,.target_count=1};
    struct device filter={.api=&filter_api,.data=&filter_state};
    expected_child_targets=sparse; expected_child_count=1;
    expected_second_targets=reordered; expected_second_count=2;
    host_overlays[0]=&first; host_overlays[1]=&second;
    host_overlays[2]=&filter; host_overlay_count=3;
    host_activity(true); assert(zmk_rgb_matrix_on()==0); host_run_ready();
    assert(paint_order==12 && child_state.active && second_state_runtime.active && runtime.active);
    expect_output((struct led_rgb){55,10,2},(struct led_rgb){60,14,9});

    const struct device *ordered[]={&second,&first,&filter};
    struct kp_rgb_effect_api reordered_api=api;
    reordered_api.overlays=ordered; reordered_api.overlays_len=3;
    struct device composed_fx=fx; composed_fx.api=&reordered_api;
    host_effects[0]=&composed_fx; ctx.state.active_fx=&composed_fx;
    paint_order=0; zmk_rgb_matrix_flush(); host_run_ready();
    assert(paint_order==21);
    expect_output((struct led_rgb){55,10,2},(struct led_rgb){51,18,17});

    /* A skipped target must replace the base with black, not stale scratch. */
    first_state.gate=false;
    second_state.targets=sparse; second_state.target_count=1;
    expected_second_targets=sparse; expected_second_count=1;
    second_cfg.common.opacity=100; leave_last_black=true;
    memset(scratch,0xa5,sizeof(scratch));
    paint_order=0; zmk_rgb_matrix_flush(); host_run_ready();
    assert(paint_order==2 && !child_state.active && second_state_runtime.active && runtime.active);
    expect_output((struct led_rgb){7,0,0},(struct led_rgb){0,0,0});
    second_state.targets=reordered; second_state.target_count=2; second_state.cover=true;
    expected_second_targets=reordered; expected_second_count=2;
    memset(scratch,0xa5,sizeof(scratch));
    paint_order=0; zmk_rgb_matrix_flush(); host_run_ready();
    assert(paint_order==2 && !runtime.active);
    expect_output((struct led_rgb){0,0,0},(struct led_rgb){86,17,4});

    second_state.gate=false; filter_state.gate=true;
    int before=renders;
    zmk_rgb_matrix_flush(); host_run_ready();
    assert(filter_paints==1 && runtime.active && renders==before+1 && !second_state_runtime.active);
    expect_output((struct led_rgb){7,0,0},(struct led_rgb){21,10,3});
    host_overlay_count=0; host_effects[0]=&fx; ctx.state.active_fx=&fx;
    expected_child_targets=NULL;
}
int main(void) {
    fixture(true,false);
    struct kp_ovl_config cfg={.common={.all_leds=true,.opacity=37},.effect=&child};
    struct host_overlay state={.child=&child,.gate=true};
    struct device ov={.api=&ov_api,.config=&cfg,.data=&state};
    host_overlays[0]=&ov; host_overlay_count=1;
    ctx.tuning.max_brightness=43; ctx.tuning.idle_brightness=17;
    host_run_ready();
    if(!KP_LED_COUNT) { assert(host_transfer_count==0); return 0; }
    struct led_rgb intrinsic=kp_rgb_rgb_mix((struct led_rgb){17,0,0},(struct led_rgb){104,73,101},37);
    /* Pre-scaling the layers would round red to 20 instead of 21. */
    for(size_t i=0;i<KP_LED_COUNT;i++) {
        struct led_rgb expected={21,11,15};
        assert(!memcmp(&scene[i],&expected,sizeof(expected)));
        assert(!memcmp(&host_transfers[host_transfer_count-1].pixels[i],&expected,sizeof(expected)));
        assert(scratch[i].r==0xa5);
    }
    assert(intrinsic.r==49 && intrinsic.g==27 && intrinsic.b==37);
    host_elapse_to(now+1);
    host_activity(false); zmk_rgb_matrix_flush(); host_run_ready();
    for(size_t i=0;i<KP_LED_COUNT;i++) {
        struct led_rgb expected={8,4,6};
        assert(!memcmp(&scene[i],&expected,sizeof(expected)));
        assert(!memcmp(&host_transfers[host_transfer_count-1].pixels[i],&expected,sizeof(expected)));
    }
    struct led_rgb idle_output={8,4,6};
    host_allow_color_failure=true; failures=1;
    zmk_rgb_matrix_flush(); host_run_ready();
    assert(host_transfers[host_transfer_count-1].result==-EIO);
    assert(!memcmp(&scene[0],&idle_output,sizeof(idle_output)));
    host_run_until(kp_rgb_output_retry_deadline_ms);
    assert(host_transfers[host_transfer_count-1].result==0);
    assert(!memcmp(&scene[0],&idle_output,sizeof(idle_output)));

    /* A terminal callback paints caller-cleared targets without touching other pixels. */
    kp_rgb_matrix_lock();
    size_t target=1;
    struct kp_rgb_frame f={.count=KP_LED_COUNT,.targets=&target,.target_count=1,.pixels=scratch,.now_ms=now,.elapsed_ms=37};
    check_raw_elapsed=true;
    memset(scratch,0xa5,sizeof(scratch));
    scratch[target]=(struct led_rgb){0};
    assert(!kp_rgb_effect_render(&child,&f));
    assert(scratch[0].r==0xa5 && scratch[0].g==0xa5 && scratch[0].b==0xa5);
    assert(scratch[1].r==104);
    f.target_count=0; memset(scratch,0xa5,sizeof(scratch));
    assert(!kp_rgb_effect_render(&child,&f));
    for(size_t i=0;i<KP_LED_COUNT;i++) assert(scratch[i].r==0xa5);
    struct led_rgb destination[2]={{9,11,13},{21,23,25}};
    f.pixels=destination; f.scratch=scratch; f.target_count=1;
    cfg.common.opacity=100;
    kp_ovl_render(&ov,&f);
    assert(scratch[0].r==0 && scratch[0].g==0 && scratch[0].b==0);
    assert(destination[0].r==9 && destination[0].g==11 && destination[0].b==13);
    assert(destination[1].r==104 && destination[1].g==73 && destination[1].b==101);
    const size_t reordered[2]={1,0}; f.targets=reordered; f.target_count=2;
    kp_ovl_render(&ov,&f);
    assert(destination[0].r==104 && destination[1].r==104);
    f.target_count=0; memset(destination,0xa5,sizeof(destination));
    kp_ovl_render(&ov,&f);
    assert(destination[0].r==0xa5 && destination[1].r==0xa5);
    check_raw_elapsed=false;
    kp_rgb_matrix_unlock();

    failures=2; assert(zmk_rgb_matrix_off()==0); host_run_ready();
    host_run_until(now+1000);
    for(size_t n=host_transfer_count-3;n<host_transfer_count;n++)
        for(size_t i=0;i<KP_LED_COUNT;i++) {
            struct led_rgb black={0};
            assert(!memcmp(&host_transfers[n].pixels[i],&black,sizeof(black)));
        }
    assert(!memcmp(&scene[0],&idle_output,sizeof(idle_output)));
    test_engine_composition();
    puts("scene, scratch, intrinsic blend, final brightness and mutable-driver isolation passed");
}
'''

if __name__ == '__main__':
    utils = (ROOT / 'src/rgb_utils.c').read_text()
    color = (ROOT / 'src/rgb_color.c').read_text()
    overlay = (ROOT / 'src/overlays/overlay.c').read_text()
    production = function(color, 'kp_rgb_rgb_mix') + '\n'
    production += '\n'.join(function(utils, name) for name in (
        'kp_rgb_overlay_paint', 'kp_rgb_overlay_paint_pixels'))
    production += '\n' + '\n'.join(function(overlay, name) for name in (
        'kp_ovl_effect', 'kp_ovl_reset', 'kp_ovl_set_active', 'kp_ovl_on_event', 'kp_ovl_render'))
    run_tests(MOCKS + production + FIXTURE + TESTS, scheduler=True, auto_off_idle=False)
