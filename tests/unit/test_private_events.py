#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production routing samples overlay lifecycle, not private-child discovery."""
from engine_harness import run_tests
from test_engine_scheduler import TESTS as SCHEDULER_TESTS

FIXTURE = SCHEDULER_TESTS[:SCHEDULER_TESTS.index('static void assert_pixels')]
TESTS = r'''
static unsigned counts[2], paints[2], activations[2], deactivations[2];
static uint32_t elapsed[2];
static bool animate[2]={true,true};
static size_t ordinal(const struct device *d) { return ((struct host_overlay *)d->data)->common.index; }
static bool native_event(const struct device *d,const struct kp_rgb_key_event *e) {
    assert(e->position==0 && e->timestamp_ms==123);
    size_t n=ordinal(d); assert(activations[n]>deactivations[n]); counts[n]++; return true;
}
static void native_active(const struct device *d,bool active,int64_t t) {
    (void)t; size_t n=ordinal(d); if(active) activations[n]++; else deactivations[n]++;
}
static bool native_render(const struct device *d,const struct kp_rgb_frame *f) {
    assert(f->scratch==scratch && f->pixels==scene);
    size_t n=ordinal(d); elapsed[n]=f->elapsed_ms; paints[n]++; return animate[n];
}
static const struct kp_rgb_effect_callbacks native_callbacks={.render=native_render,.on_event=native_event,.set_active=native_active};
static const struct kp_rgb_overlay_api native_api={.callbacks=&native_callbacks,.replaces_target=true};
static struct host_overlay data[2]={{.common={.index=0},.gate=true},{.common={.index=1},.gate=true}};
static struct device overlays[2]={{.api=&native_api,.data=&data[0]},{.api=&native_api,.data=&data[1]}};
static void key(void) {
    struct kp_rgb_key_event e={.position=0,.pressed=true,.timestamp_ms=123};
    assert(kp_rgb_pending_push(&e)); kp_rgb_request_output_pass(false); host_run_ready();
}
static unsigned child_paints[2];
static uint32_t child_elapsed[2];
static bool child_animate[2]={false,true}, skip_first;
static bool child_render(const struct device *d,const struct kp_rgb_frame *f) {
    size_t n=(size_t)(uintptr_t)d->data;
    assert(!f->scratch); child_paints[n]++; child_elapsed[n]=f->elapsed_ms; return child_animate[n];
}
static const struct kp_rgb_effect_callbacks child_callbacks={.render=child_render};
static const struct kp_rgb_effect_api child_api={.callbacks=&child_callbacks};
static const struct device children[2]={{.api=&child_api,.data=(void *)0},{.api=&child_api,.data=(void *)1}};
static struct kp_rgb_effect_instance instances[2]={KP_RGB_EFFECT_INSTANCE_INIT(&children[0]),KP_RGB_EFFECT_INSTANCE_INIT(&children[1])};
static void owner_active(const struct device *d,bool active,int64_t t) {
    (void)d; for(size_t n=0;n<2;n++) kp_rgb_effect_instance_set_active(&instances[n],active,t);
}
static void owner_reset(const struct device *d,int64_t t) {
    (void)d; for(size_t n=0;n<2;n++) kp_rgb_effect_instance_reset(&instances[n],t);
}
static bool owner_render(const struct device *d,const struct kp_rgb_frame *f) {
    (void)d;
    if(!f->elapsed_ms) for(size_t n=0;n<2;n++) kp_rgb_effect_instance_restart_clock(&instances[n]);
    struct kp_rgb_frame child=*f; child.pixels=f->scratch; child.scratch=NULL;
    memset(child.pixels,0,child.count*sizeof(*child.pixels));
    bool again=false;
    if(!skip_first) again |= kp_rgb_effect_instance_render(&instances[0],&child);
    again |= kp_rgb_effect_instance_render(&instances[1],&child);
    return again;
}
static const struct kp_rgb_effect_callbacks owner_callbacks={.render=owner_render,.reset=owner_reset,.set_active=owner_active};
static const struct kp_rgb_overlay_api owner_api={.callbacks=&owner_callbacks};
static void test_optional_children(void) {
    overlays[0].api=&owner_api; host_overlays[0]=&overlays[0]; host_overlay_count=1;
    ctx.state.active_fx=&fx; data[0].gate=true; data[0].cover=false;
    zmk_rgb_matrix_flush(); host_run_ready();
    assert(child_paints[0]==1 && child_paints[1]==1 && child_elapsed[0]==0 && child_elapsed[1]==0);
    host_run_until(now+16);
    assert(child_paints[0]==2 && child_paints[1]==2 && child_elapsed[0]==0 && child_elapsed[1]==16);
    child_animate[0]=true; zmk_rgb_matrix_flush(); host_run_ready();
    host_run_until(now+16); assert(child_elapsed[0]==16 && child_elapsed[1]==16);
    unsigned first=child_paints[0];
    skip_first=true; host_elapse_to(now+1000);
    assert(zmk_rgb_matrix_off()==0 && zmk_rgb_matrix_on()==0); host_run_ready();
    assert(instances[0].state.active && child_paints[0]==first && child_elapsed[1]==0);
    skip_first=false; host_run_until(now+16);
    assert(child_elapsed[0]==0 && child_elapsed[1]==16);
    child_animate[0]=false; child_animate[1]=false;
    host_run_until(now+16); unsigned terminal=child_paints[1];
    host_run_until(now+1000); assert(child_paints[1]==terminal && kp_rgb_next_frame_ms==INT64_MAX);
    assert(!kp_rgb_effect_scenes[7].state.initialized);
}
int main(void) {
    host_render_animating=false; fixture(true,false); host_activity(true);
    host_overlays[0]=&overlays[0]; host_overlays[1]=&overlays[1]; host_overlay_count=2;
    host_run_ready();
    if(!KP_LED_COUNT) { assert(!paints[0] && !activations[0]); return 0; }
    key(); assert(feedback==1 && counts[0]==1 && counts[1]==1);
    // True base input and true earlier overlay results must not short circuit delivery.
    host_run_until(16); assert(paints[0]==3 && paints[1]==3 && elapsed[0]==16 && elapsed[1]==16);
    assert(kp_rgb_scene_animating && !runtime.animating);
    data[0].gate=false; zmk_rgb_matrix_flush(); data[0].gate=true;
    host_run_ready(); assert(activations[0]==1 && !deactivations[0]);
    data[1].cover=true; key();
    assert(!runtime.active && !kp_rgb_overlay_scenes[0].state.active && kp_rgb_overlay_scenes[1].state.active);
    assert(feedback==1 && counts[0]==1 && counts[1]==2 && deactivations[0]==1);
    unsigned hidden=paints[0]; host_run_until(32); assert(paints[0]==hidden && elapsed[1]==16);
    data[1].gate=false; key();
    assert(runtime.active && counts[0]==2 && counts[1]==2 && activations[0]==2 && deactivations[1]==1);
    assert(elapsed[0]==0);
    // A controller-local overlay list does not change registry ordinals.
    const struct device *local[]={&overlays[1]};
    struct kp_rgb_effect_api selected_api=api; selected_api.overlays=local; selected_api.overlays_len=1;
    struct device selected=fx; selected.api=&selected_api; ctx.state.active_fx=&selected;
    data[1].gate=true; data[1].cover=false; key();
    assert(counts[0]==2 && counts[1]==3 && !kp_rgb_overlay_scenes[0].state.active);
    animate[1]=false; host_run_until(48); unsigned terminal=paints[1];
    host_run_until(1000); assert(paints[1]==terminal && kp_rgb_next_frame_ms==INT64_MAX);
    unsigned before=counts[1];
    struct kp_rgb_key_event unmapped={.position=99,.pressed=true,.timestamp_ms=123};
    kp_rgb_matrix_lock(); assert(!kp_rgb_deliver_position(&unmapped)); kp_rgb_matrix_unlock();
    assert(counts[1]==before);
    selected_api.overlays_len=0; key(); assert(counts[1]==before);
    ctx.state.active_fx=NULL; key(); assert(counts[1]==before);
    test_optional_children();
    puts("native overlay clocks, full-cover lifecycle, contributing frames, optional children and non-short-circuit input passed");
}
'''

if __name__ == '__main__':
    run_tests(FIXTURE + TESTS, scheduler=True)
