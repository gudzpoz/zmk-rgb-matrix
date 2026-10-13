#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Fixed registry rendering without any selector, including empty settled scenes."""
from engine_harness import SOURCE, function, run_tests
from test_engine_scheduler import FIXTURE

TESTS = r'''
static unsigned paints[2], events[2], activations[2], deactivations[2];
static bool animate[2]={false,true};
static unsigned order;
static size_t slot(const struct device *d) { return ((struct host_overlay *)d->data)->common.index; }
static bool status_render(const struct device *d,const struct kp_rgb_frame *f) {
    size_t n=slot(d);
    assert(held && f->scratch==scratch && f->pixels==scene && f->targets);
    assert(f->count==KP_LED_COUNT && f->target_count==1 && f->targets[0]==n);
    paints[n]++;order=order*10+n+1;
    f->pixels[n]=(struct led_rgb){n ? 200 : 100,0,0};
    return animate[n];
}
static bool status_event(const struct device *d,const struct kp_rgb_key_event *e) {
    assert(e->position==0 && e->timestamp_ms==123 && e->pressed);
    events[slot(d)]++;return true;
}
static void status_active(const struct device *d,bool active,int64_t now_ms) {
    (void)now_ms;size_t n=slot(d);
    if(active) activations[n]++;else deactivations[n]++;
}
static const struct kp_rgb_effect_callbacks status_callbacks={.render=status_render,.on_event=status_event,.set_active=status_active};
static const struct kp_rgb_overlay_api status_api={.callbacks=&status_callbacks};
static const size_t targets[2]={0,1};
static struct host_overlay states[2]={
    {.common={.index=0},.gate=true,.targets=&targets[0],.target_count=1},
    {.common={.index=1},.gate=true,.targets=&targets[1],.target_count=1},
};
static const struct device statuses[2]={
    {.api=&status_api,.data=&states[0]}, {.api=&status_api,.data=&states[1]},
};
static void key(uint32_t position) {
    const struct kp_rgb_key_event e={.position=position,.pressed=true,.timestamp_ms=123};
    assert(kp_rgb_pending_push(&e));kp_rgb_request_output_pass(false);host_run_ready();
}
static void assert_black(void) {
    for(size_t i=0;i<KP_LED_COUNT;i++) {
        assert(!scene[i].r && !scene[i].g && !scene[i].b);
        const struct led_rgb *p=&host_transfers[host_transfer_count-1].pixels[i];
        assert(!p->r && !p->g && !p->b);
    }
}
int main(int argc,char **argv) {
    assert(argc==2);
    fixture(true,false);host_activity(true);host_elapse_to(now);
    assert(ctx.selector.active_fx==NULL && ctx.selector.effect_count==0);
    ctx.dev=NULL;
    kp_rgb_global.brightness=50;kp_rgb_global.tuning.max_brightness=50;
    host_overlay_count=!strcmp(argv[1],"empty-registry") ? 0 : 2;
    host_overlays[0]=&statuses[0];host_overlays[1]=&statuses[1];
    if(!strcmp(argv[1],"no-active")) states[0].gate=states[1].gate=false;
    host_run_ready();
    if(!KP_LED_COUNT) {
        assert(!paints[0] && !paints[1] && !host_scale_calls && !host_transfer_count);
        assert(!activations[0] && !activations[1] && kp_rgb_next_frame_ms==INT64_MAX);
        return 0;
    }
    if(!strcmp(argv[1],"status-only")) {
        assert(order==12 && paints[0]==1 && paints[1]==1 && !renders);
        assert(activations[0]==1 && activations[1]==1);
        assert(scene[0].r==25 && scene[1].r==50 && host_scale_calls==KP_LED_COUNT);
        assert(kp_rgb_prefix_len==1 && kp_rgb_scene_animating);
        order=0;host_run_until(16);
        assert(order==2 && paints[0]==1 && paints[1]==2);
        assert(scene[0].r==25 && scene[1].r==50 && host_scale_calls==2*KP_LED_COUNT);
        key(0);assert(events[0]==1 && events[1]==1 && paints[0]==2 && paints[1]==3);
        unsigned scaled=host_scale_calls, first=paints[0],second=paints[1];
        key(99);assert(events[0]==1 && events[1]==1);
        assert(paints[0]==first && paints[1]==second && host_scale_calls==scaled);
        states[0].gate=states[1].gate=false;zmk_rgb_matrix_flush();host_run_ready();
        assert(deactivations[0]==1 && deactivations[1]==1);
    }
    assert(!kp_rgb_overlay_scenes[0].state.active && !kp_rgb_overlay_scenes[1].state.active);
    assert_black();
    assert(!kp_rgb_prefix_valid && !kp_rgb_scene_animating && kp_rgb_next_frame_ms==INT64_MAX);
    bool on=false;assert(zmk_rgb_matrix_get_state(&on)==0 && on);
    unsigned runs=kp_output_work.work.runs, scaled=host_scale_calls;
    host_run_until(now+1000);assert(kp_output_work.work.runs==runs);
    key(0);assert(!kp_rgb_pending_available() && host_scale_calls==scaled);
    assert(zmk_rgb_matrix_off()==0);host_run_ready();assert_black();
    assert(zmk_rgb_matrix_on()==0);host_run_ready();assert_black();
    assert(ctx.selector.active_fx==NULL && !renders && !feedback);
    puts("selector-free fixed scene, empty participation and single brightness scaling passed");
}
'''

if __name__ == '__main__':
    for name in ('kp_rgb_each_scene', 'kp_rgb_scene_pass_locked', 'kp_rgb_deliver_position'):
        body = function(name)
        for obsolete in ('active_fx', 'kp_rgb_controller', 'kp_rgb_effect_scene', 'overlays_len'):
            assert obsolete not in body, (name, obsolete)
    assert 'kp_rgb_effect_scenes' not in SOURCE
    run_tests(FIXTURE + TESTS, scheduler=True,
              cases=('status-only', 'no-active', 'empty-registry'))
