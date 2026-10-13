#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production settled-bottom prefix cache: reuse on animation ticks, invalidation."""
from engine_harness import run_tests
from test_engine_scheduler import TESTS as SCHEDULER_TESTS

FIXTURE = SCHEDULER_TESTS[:SCHEDULER_TESTS.index('static void assert_pixels')]
CASES = (
    "static-bottom", "multi-prefix", "invalidate", "animating-bottom", "cover",
    "brightness", "idle", "child-change", "gate-change", "registry-holes",
)

TESTS = r'''
static struct host_overlay ov_state[8];
static struct device ov_dev[8];
static unsigned ov_paints[8];
static bool ov_anim[8];
static uint8_t ov_rgb[8][3];
static bool slot_paint(const struct device *d,const struct kp_rgb_frame *f) {
    size_t slot=((const struct host_overlay*)d->data)->common.index-1;
    ov_paints[slot]++;
    for(size_t j=0;j<f->target_count;j++)
        f->pixels[f->targets[j]]=(struct led_rgb){ov_rgb[slot][0],ov_rgb[slot][1],ov_rgb[slot][2]};
    return ov_anim[slot];
}
static const struct kp_rgb_effect_callbacks slot_cb={.render=slot_paint};
static const struct kp_rgb_overlay_api slot_api={.callbacks=&slot_cb};
static void add_overlay(size_t slot,const size_t *targets,size_t target_count,bool cover) {
    ov_state[slot].common.index=slot+1;
    ov_state[slot].gate=true;
    ov_state[slot].cover=cover;
    ov_state[slot].targets=targets;
    ov_state[slot].target_count=target_count;
    ov_dev[slot].api=&slot_api;
    ov_dev[slot].data=&ov_state[slot];
    host_overlays[slot+1]=&ov_dev[slot];
    if(slot+2>host_overlay_count) host_overlay_count=slot+2;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *name = argv[1];
    const size_t sparse[] = {1};
    if (!strcmp(name, "static-bottom")) {
        host_render_red = 5;
        host_render_animating = false;
        ov_anim[0] = true; ov_rgb[0][2] = 200;
        add_overlay(0, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders && !ov_paints[0] && !kp_rgb_prefix_valid); return 0; }
        assert(renders == 1 && ov_paints[0] == 1);
        assert(kp_rgb_prefix_valid && kp_rgb_prefix_len == 1);
        assert(scene[0].r == 5 && scene[0].g == 0 && scene[0].b == 0);
        assert(scene[1].b == 200);
        /* A repaint would expose this colour change; the cache must hide it. */
        host_render_red = 99;
        host_elapse_to(now + 8);
        assert(renders == 1 && ov_paints[0] == 1);
        host_run_until(now + 8);
        assert(renders == 1);
        assert(ov_paints[0] == 2);
        assert(scene[0].r == 5 && scene[0].g == 0 && scene[0].b == 0);
        assert(scene[1].b == 200);
        assert(kp_rgb_prefix_len == 1 && kp_rgb_scene_animating);
    } else if (!strcmp(name, "multi-prefix")) {
        const size_t zero[] = {0};
        host_render_red = 7;
        host_render_animating = false;
        ov_anim[0] = false; ov_rgb[0][0] = 40;
        ov_anim[1] = true;  ov_rgb[1][2] = 80;
        add_overlay(0, zero, 1, false);
        add_overlay(1, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders && !ov_paints[0] && !ov_paints[1] && !kp_rgb_prefix_valid); return 0; }
        assert(renders == 1 && ov_paints[0] == 1 && ov_paints[1] == 1);
        assert(kp_rgb_prefix_len == 2);
        assert(scene[0].r == 40 && scene[1].b == 80);
        /* Both bottom layers are cached and must not repaint. */
        host_render_red = 99; ov_rgb[0][0] = 99;
        host_run_until(now + 16);
        assert(renders == 1 && ov_paints[0] == 1 && ov_paints[1] == 2);
        assert(scene[0].r == 40 && scene[1].b == 80);
    } else if (!strcmp(name, "invalidate")) {
        host_render_red = 5;
        host_render_animating = false;
        ov_anim[0] = true; ov_rgb[0][2] = 200;
        add_overlay(0, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders && !kp_rgb_prefix_valid); return 0; }
        host_render_red = 99;
        host_run_until(now + 16);
        assert(renders == 1 && scene[0].r == 5);
        zmk_rgb_matrix_flush(); host_run_ready();
        assert(renders == 2 && scene[0].r == 99);
        assert(kp_rgb_prefix_len == 1);
    } else if (!strcmp(name, "animating-bottom")) {
        host_render_red = 11;
        host_render_animating = true;
        ov_anim[0] = false; ov_rgb[0][2] = 30;
        add_overlay(0, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders && !kp_rgb_prefix_valid); return 0; }
        assert(renders == 1 && !kp_rgb_prefix_valid);
        host_run_until(now + 16);
        assert(renders == 2 && !kp_rgb_prefix_valid);
        assert(scene[0].r == 11 && scene[1].b == 30);
    } else if (!strcmp(name, "cover")) {
        host_render_red = 5;
        host_render_animating = false;
        ov_anim[0] = false; ov_rgb[0][0] = 9;
        ov_anim[1] = true;  ov_rgb[1][2] = 60;
        add_overlay(0, NULL, 0, true);
        add_overlay(1, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders && !kp_rgb_prefix_valid); return 0; }
        /* Full cover hides the bottom overlay, excluding it from the prefix. */
        assert(renders == 0 && ov_paints[0] == 1 && ov_paints[1] == 1);
        assert(kp_rgb_prefix_valid && kp_rgb_prefix_len == 1);
        host_run_until(now + 16);
        assert(renders == 0 && ov_paints[0] == 1 && ov_paints[1] == 2);
        assert(scene[0].r == 9 && scene[0].b == 0);
        assert(scene[1].r == 0 && scene[1].b == 60);
    } else if (!strcmp(name, "brightness")) {
        kp_rgb_global.tuning.max_brightness = 50;
        host_render_red = 100;
        host_render_animating = false;
        ov_anim[0] = true; ov_rgb[0][2] = 200;
        add_overlay(0, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders); return 0; }
        assert(scene[0].r == 50);
        host_run_until(now + 16);
        /* The cache holds pre-brightness pixels; a post-scale cache would read 25. */
        assert(renders == 1 && scene[0].r == 50);
    } else if (!strcmp(name, "idle")) {
        kp_rgb_global.tuning.max_brightness = 100;
        kp_rgb_global.tuning.idle_brightness = 50;
        host_render_red = 40;
        host_render_animating = false;
        ov_anim[0] = true; ov_rgb[0][2] = 200;
        add_overlay(0, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders); return 0; }
        assert(renders == 1 && scene[0].r == 40);
        host_run_until(now + 16);
        assert(renders == 1 && scene[0].r == 40);
        /* Activity change forces a full recompose even while reusing. */
        host_activity(false); host_run_ready();
        assert(renders == 2 && scene[0].r == 20);
        assert(scene[1].b == 100);
    } else if (!strcmp(name, "child-change")) {
        static const struct kp_rgb_effect_config second_config = {.owner = &controller_dev, .slot = 1};
        const struct device second = {.api = &api, .config = &second_config};
        host_render_red = 5;
        host_render_animating = false;
        ov_anim[0] = true; ov_rgb[0][2] = 200;
        add_overlay(0, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders && !kp_rgb_prefix_valid); return 0; }
        assert(renders == 1 && kp_rgb_prefix_len == 1);
        host_run_until(now + 16);
        assert(renders == 1 && scene[0].r == 5);
        kp_rgb_matrix_lock();
        host_bottom_data.child = &second;
        host_render_red = 99;
        zmk_rgb_matrix_flush();
        kp_rgb_matrix_unlock();
        host_run_ready();
        assert(renders == 2 && scene[0].r == 99 && scene[1].b == 200);
        assert(runtime.active && kp_rgb_overlay_scenes[1].state.active);
        assert(kp_rgb_prefix_valid && kp_rgb_prefix_len == 1);
        host_run_until(now + 16);
        assert(renders == 2 && scene[0].r == 99);
    } else if (!strcmp(name, "gate-change")) {
        const size_t zero[] = {0};
        host_render_red = 7;
        host_render_animating = false;
        ov_anim[0] = false; ov_rgb[0][0] = 40;
        ov_anim[1] = true; ov_rgb[1][2] = 80;
        add_overlay(0, zero, 1, false);
        add_overlay(1, sparse, 1, false);
        fixture(true, false); host_run_ready();
        if (!KP_LED_COUNT) { assert(!renders && !kp_rgb_prefix_valid); return 0; }
        assert(kp_rgb_prefix_len == 2 && scene[0].r == 40);
        host_run_until(now + 16);
        assert(renders == 1 && ov_paints[0] == 1 && ov_paints[1] == 2);
        kp_rgb_matrix_lock();
        ov_state[0].gate = false;
        zmk_rgb_matrix_flush();
        kp_rgb_matrix_unlock();
        host_run_ready();
        assert(renders == 2 && ov_paints[0] == 1 && ov_paints[1] == 3);
        assert(!kp_rgb_overlay_scenes[1].state.active);
        assert(kp_rgb_prefix_valid && kp_rgb_prefix_len == 1);
        assert(scene[0].r == 7 && scene[1].b == 80);
        host_run_until(now + 16);
        assert(renders == 2 && ov_paints[0] == 1 && ov_paints[1] == 4);
        assert(scene[0].r == 7 && scene[1].b == 80);
    } else if (!strcmp(name, "registry-holes")) {
        const size_t zero[]={0};
        host_render_red=7;host_render_animating=false;
        ov_rgb[0][0]=40;ov_rgb[1][0]=99;ov_anim[3]=true;ov_rgb[3][2]=80;
        add_overlay(0,zero,1,false);
        add_overlay(1,zero,1,false);ov_state[1].gate=false;
        add_overlay(3,sparse,1,false); /* Registry slot 3 remains NULL. */
        fixture(true,false);host_run_ready();
        if(!KP_LED_COUNT) { assert(!renders && !kp_rgb_prefix_valid); return 0; }
        assert(renders==1 && ov_paints[0]==1 && !ov_paints[1] && ov_paints[3]==1);
        assert(kp_rgb_prefix_len==2 && scene[0].r==40 && scene[1].b==80);
        host_render_red=99;ov_rgb[0][0]=99;
        host_run_until(now+16);
        assert(renders==1 && ov_paints[0]==1 && !ov_paints[1] && ov_paints[3]==2);
        assert(scene[0].r==40 && scene[1].b==80 && kp_rgb_prefix_len==2);
        host_bottom_data.gate=false;zmk_rgb_matrix_flush();host_run_ready();
        assert(kp_rgb_prefix_len==1 && !runtime.active && ov_paints[0]==2 && ov_paints[3]==3);
        host_run_until(now+16);
        assert(renders==1 && ov_paints[0]==2 && ov_paints[3]==4);
        assert(scene[0].r==99 && scene[1].b==80);
    } else { assert(!"unknown case"); }
    assert(!polls && !refreshes && !dispatches);
    printf("%s leds=%d renders=%d prefix_len=%zu: passed\n",
           name,KP_LED_COUNT,renders,kp_rgb_prefix_len);
    return 0;
}
'''

if __name__ == '__main__':
    run_tests(FIXTURE + TESTS, scheduler=True, cases=CASES, auto_off_idle=False)
