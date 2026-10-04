#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production scene worker lifecycle, private clocks and bounded input delivery."""
from engine_harness import run_tests
from test_engine_scheduler import TESTS as SCHEDULER_TESTS

FIXTURE = SCHEDULER_TESTS[:SCHEDULER_TESTS.index('static void assert_pixels')]
TESTS = r'''
struct observation {
    struct kp_rgb_effect_runtime runtime;
    unsigned resets, activations, deactivations, events, paints;
    uint32_t elapsed;
    int64_t timestamp;
    bool animate;
    int parameter;
};
static unsigned replenishments;
static char order[2048];
static size_t order_len;
static void record(char c) { assert(held); assert(order_len < sizeof(order)-1); order[order_len++]=c; }
static void reset(const struct device *d,int64_t t) {
    (void)t; struct observation *o=d->data; o->resets++; record('R');
}
static void active(const struct device *d,bool value,int64_t t) {
    (void)t; struct observation *o=d->data;
    if(value) o->activations++; else o->deactivations++;
    record(value ? 'A' : 'D');
}
static bool event(const struct device *d,const struct kp_rgb_key_event *e) {
    struct observation *o=d->data; o->events++; o->timestamp=e->timestamp_ms;
    assert(e->pressed); record('E');
    if(replenishments) { replenishments--; assert(kp_rgb_pending_push(e)); }
    return true;
}
static bool paint(const struct device *d,const struct kp_rgb_frame *f) {
    assert(!f->scratch && f->targets && f->target_count==KP_LED_COUNT);
    struct observation *o=d->data; o->paints++; o->elapsed=f->elapsed_ms; record('P');
    return o->animate;
}
static const struct kp_rgb_effect_callbacks cb={.render=paint,.reset=reset,.set_active=active,.on_event=event};
static const struct kp_rgb_effect_callbacks optional={.render=paint};
static struct observation observations[3];
static struct kp_rgb_effect_api apis[3];
static struct device devices[3];
static const struct device *target(const struct device *d) { return ((struct host_overlay *)d->data)->child; }
static void overlay_paint(const struct device *d,const struct kp_rgb_frame *f) {
    assert(f->pixels==scene && f->scratch==scratch && f->targets);
    struct kp_rgb_frame child=*f;
    child.pixels=f->scratch; child.scratch=NULL;
    memset(child.pixels,0,child.count*sizeof(*child.pixels));
    kp_rgb_effect_render(target(d),&child);
}
static const struct kp_rgb_overlay_api overlay_api={.render=overlay_paint,.event_target=target};
static struct host_overlay overlay_state;
static struct device overlay={.api=&overlay_api,.data=&overlay_state};
static void setup(void) {
    fixture(true,false);
    for(size_t i=0;i<3;i++) {
        apis[i]=(struct kp_rgb_effect_api){.callbacks=&cb,.runtime=&observations[i].runtime};
        devices[i]=(struct device){.api=&apis[i],.data=&observations[i]};
        observations[i].parameter=123+(int)i;
    }
    host_effects[0]=&devices[0]; host_effects[1]=&devices[1]; host_effect_count=2;
    overlay_state=(struct host_overlay){.child=&devices[2],.gate=true};
    host_overlays[0]=&overlay; host_overlay_count=1;
    ctx.state.active_fx=&devices[0];
}
static void key(int64_t timestamp) {
    const struct zmk_position_state_changed p={.position=0,.state=true,.timestamp=timestamp};
    const zmk_event_t e={.position=&p};
    assert(kp_rgb_matrix_event_listener(&e)==0);
}
int main(void) {
    setup();
    if(!KP_LED_COUNT) {
        host_run_ready(); key(1); host_run_ready();
        assert(!observations[0].resets && !observations[2].paints && !kp_tick_timer.active);
        host_activity(true); return 0;
    }
    struct observation *a=&observations[0], *b=&observations[1], *c=&observations[2];
    key(-123); host_run_ready();
    assert(!strcmp(order,"RARAEEPP"));
    assert(a->timestamp==-123 && c->timestamp==-123 && a->elapsed==0 && c->elapsed==0);
    assert(!b->runtime.initialized);
    // Static effects remain periodic, with settled clocks at zero.
    host_run_until(32);
    assert(a->paints==3 && c->paints==3 && a->elapsed==0 && c->elapsed==0);
    a->animate=true; host_run_until(48); assert(a->elapsed==0);
    host_run_until(64); assert(a->elapsed==16 && c->elapsed==0);
    // Selection is sampled at delivery, not admission; unobserved transitions coalesce.
    key(77); ctx.state.active_fx=&devices[1]; zmk_rgb_matrix_flush(); host_run_ready();
    assert(a->deactivations==1 && b->resets==1 && b->activations==1 && b->events==1);
    assert(a->events==1 && c->events==2 && b->timestamp==77);
    ctx.state.active_fx=&devices[0]; ctx.state.active_fx=&devices[1];
    zmk_rgb_matrix_flush(); host_run_ready(); assert(b->activations==1 && b->deactivations==0);
    // A full cover hides the base; disabling the overlay resumes it without reset.
    overlay_state.cover=true; key(88); host_run_ready();
    assert(b->deactivations==1 && b->events==1 && c->events==3);
    overlay_state.gate=false; zmk_rgb_matrix_flush(); host_run_ready();
    assert(b->activations==2 && b->resets==1 && b->elapsed==0 && c->deactivations==1);
    // Runtime reset preserves all effect parameters, including inactive private children.
    kp_rgb_matrix_lock(); kp_rgb_request_runtime_reset_locked(); kp_rgb_matrix_unlock();
    host_run_ready();
    for(size_t i=0;i<3;i++) assert(observations[i].parameter==123+(int)i && observations[i].resets==2);
    assert(b->elapsed==0);
    // Optional callbacks and independent private instance storage.
    apis[0].callbacks=&optional; ctx.state.active_fx=&devices[0];
    overlay_state.gate=true; overlay_state.cover=false; zmk_rgb_matrix_flush(); host_run_ready();
    assert(a->runtime.active && c->runtime.active && !b->runtime.active);
    unsigned a_events=a->events, c_events=c->events;
    key(222); host_run_ready();
    assert(a->events==a_events && c->events==c_events+1);
    unsigned a_activations=a->activations, a_deactivations=a->deactivations;
    assert(zmk_rgb_matrix_off()==0 && zmk_rgb_matrix_on()==0);
    host_run_ready();
    assert(a->activations==a_activations && a->deactivations==a_deactivations && a->elapsed==0);
    a->animate=true; c->animate=false;
    k_timer_stop(&kp_tick_timer);
    host_elapse_to(now+(uint64_t)UINT32_MAX+100); zmk_rgb_matrix_flush(); host_run_ready();
    assert(a->elapsed==UINT32_MAX && c->elapsed==0);
    now++; zmk_rgb_matrix_flush(); host_run_ready(); assert(a->elapsed==1);
    a->animate=false; now+=5; zmk_rgb_matrix_flush(); host_run_ready(); assert(a->elapsed==5);
    now+=500; zmk_rgb_matrix_flush(); host_run_ready(); assert(a->elapsed==0);
    // Every pass drains at most 16 events, then paints, despite producer replenishment.
    apis[0].callbacks=&cb; overlay_state.gate=false;
    zmk_rgb_matrix_flush(); host_run_ready();
    unsigned before_events=a->events, before_paints=a->paints;
    replenishments=20; key(999);
    assert(kp_tick_work.queued); kp_tick_work.queued=false;
    kp_rgb_matrix_tick(&kp_tick_work);
    assert(a->events==before_events+16 && a->paints==before_paints+1);
    assert(kp_rgb_pending_available() && kp_tick_work.queued);
    host_run_ready();
    assert(a->events==before_events+21 && a->paints==before_paints+2 && !kp_rgb_pending_available());
    // OFF has one lifecycle pass and no recurring blocked wakeups.
    assert(zmk_rgb_matrix_off()==0); host_run_ready();
    assert(!a->runtime.active && !c->runtime.active);
    unsigned runs=kp_tick_work.runs;
    host_run_until(now+1000); assert(kp_tick_work.runs==runs && !kp_tick_timer.active);
    assert(zmk_rgb_matrix_on()==0); host_run_ready(); assert(a->elapsed==0);
    host_activity(false); host_run_ready(); assert(!a->runtime.active);
    host_activity(true); host_run_ready(); assert(a->elapsed==0);
    puts("lifecycle ordering, independent clocks, delivery and bounded input passed");
}
'''

if __name__ == '__main__':
    run_tests(FIXTURE + TESTS, scheduler=True)
