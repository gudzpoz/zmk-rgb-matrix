#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Exercise production selector-overlay callbacks and owner-local effect clocks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_singleton import function, structure

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "include/zmk/rgb_matrix.h").read_text()
INTERNAL = (ROOT / "src/rgb_matrix_internal.h").read_text()
SELECTOR_HEADER = (ROOT / "src/rgb_selector_internal.h").read_text()
SELECTOR = (ROOT / "src/rgb_selector.c").read_text()
INSTANCE = (ROOT / "src/rgb_instance.c").read_text()
OVERLAY = (ROOT / "src/overlays/selector.c").read_text()
UTILS = (ROOT / "src/rgb_utils.c").read_text()

PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARG_UNUSED(x) ((void)(x))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS 10000
#define KP_RGB_BRT_MAX 100
struct device { const char *name; const void *config; void *data; const void *api; };
struct led_rgb { uint8_t r, g, b; };
struct kp_rgb_hsb { uint16_t h; uint8_t s, b; };
struct kp_rgb_coord { uint16_t x, y; };
struct kp_rgb_key_event { uint32_t position; bool pressed; int64_t timestamp_ms; };
struct kp_rgb_frame {
    size_t count; const size_t *targets; size_t target_count;
    struct led_rgb *pixels, *scratch; const struct kp_rgb_coord *coords;
    int64_t local_ms; uint32_t clock_ms, elapsed_ms;
    uint16_t board_length, board_height; bool is_idle;
};
struct kp_rgb_callback_state { int64_t last_render_ms; bool initialized, active, animating; };
struct kp_rgb_effect_instance { const struct device *effect; struct kp_rgb_callback_state state; };
#define KP_RGB_EFFECT_INSTANCE_INIT(dev) {.effect=(dev)}
struct kp_rgb_effect_callbacks {
    bool (*render)(const struct device *, const struct kp_rgb_frame *);
    bool (*on_event)(const struct device *, const struct kp_rgb_key_event *);
    void (*reset)(const struct device *, int64_t);
    void (*set_active)(const struct device *, bool, int64_t);
};
struct kp_rgb_effect_api { const struct kp_rgb_effect_callbacks *callbacks; };
struct kp_rgb_overlay_api { const void *condition; const struct kp_rgb_effect_callbacks *callbacks; bool replaces_target; };
struct kp_rgb_overlay_common_config { const uint32_t *keys; size_t keys_len; const uint32_t *leds; size_t leds_len; uint8_t opacity; bool all_leds; };
struct kp_rgb_overlay_common_data { size_t led_count; size_t *leds; uint16_t index; bool local, gate; };
struct kp_rgb_effect_defaults { struct kp_rgb_hsb color; uint32_t duration_ms; };
struct kp_rgb_selector { const struct device *owner; const struct device *const *effects; const struct kp_rgb_effect_defaults *effect_defaults; size_t effect_count, effect_index; const struct device *active_fx; int64_t initial_duration_ms; uint16_t initial_effect; };
struct kp_rgb_effect_common_config { const struct device *owner; uint16_t slot; };
struct kp_rgb_effect_common_data { struct kp_rgb_hsb color; uint16_t duration_ms; };
static bool ready = true;
static int64_t uptime;
static unsigned flushes, resets[8], activations[8], deactivations[8], events[8], paints[8];
static int64_t seen_event_timestamp[8];
static unsigned trace[128], trace_length;
static bool child_active[8], child_demand[8];
static int64_t child_reset_time[8];
static uint32_t child_elapsed[8];
static unsigned lock_depth;
static struct led_rgb *owner_scratch;
static const size_t *owner_targets;
static size_t owner_target_count;
static const struct device *unready_effect;
static bool children_ready = true;
static bool device_is_ready(const struct device *d) {
    return d != NULL && ready && d != unready_effect &&
           (children_ready || strcmp(d->name,"effect") != 0);
}
static int64_t k_uptime_get(void) { return uptime; }
static void kp_rgb_matrix_lock(void) { lock_depth++; }
static void kp_rgb_matrix_unlock(void) { assert(lock_depth); lock_depth--; }
static void zmk_rgb_matrix_flush(void) { flushes++; }
static void LOG_ERR(...) {}
static struct kp_rgb_effect_common_data *kp_rgb_effect_data(const struct device *d) { return d->data; }
static const struct kp_rgb_effect_common_config *kp_rgb_effect_cfg(const struct device *d) { return d->config; }
'''

TESTS = r'''
static struct kp_rgb_effect_common_config configs[5];
static int slot_of(const struct device *d) { return (int)(((const struct kp_rgb_effect_common_config *)d->config)-configs); }
static void child_reset(const struct device *d, int64_t now) {
    int i=slot_of(d); resets[i]++; child_reset_time[i]=now;
    trace[trace_length++]=(unsigned)i*10+1;
}
static void child_active_fn(const struct device *d, bool active, int64_t now) {
    (void)now; int i=slot_of(d); child_active[i]=active;
    if(active) activations[i]++; else deactivations[i]++;
    trace[trace_length++]=(unsigned)i*10+(active?2:3);
}
static bool child_event(const struct device *d, const struct kp_rgb_key_event *ev) {
    int i=slot_of(d); events[i]++; seen_event_timestamp[i]=ev->timestamp_ms;
    trace[trace_length++]=(unsigned)i*10+4;
    return true;
}
static bool child_render(const struct device *d, const struct kp_rgb_frame *f) {
    int i=slot_of(d); paints[i]++; child_elapsed[i]=f->elapsed_ms;
    assert(f->count==4 && f->scratch==NULL && f->pixels==owner_scratch);
    assert(f->targets==owner_targets && f->target_count==owner_target_count);
    for(size_t j=0;j<f->target_count;j++) {
        size_t led=f->targets[j];
        assert(f->pixels[led].r==0 && f->pixels[led].g==0 && f->pixels[led].b==0);
        f->pixels[led]=(struct led_rgb){(uint8_t)(40+i*30), (uint8_t)(20+i*10), 10};
    }
    return child_demand[i];
}
'''
MAIN = r'''
static const struct kp_rgb_overlay_api selector_api={.callbacks=&kp_selector_callbacks};
static const struct kp_rgb_effect_callbacks child_callbacks={.render=child_render,.on_event=child_event,.reset=child_reset,.set_active=child_active_fn};
static const struct kp_rgb_effect_api child_api={.callbacks=&child_callbacks};
static const struct device owner_a={.name="selector-a"}, owner_b={.name="selector-b"};
static struct kp_rgb_effect_common_data effect_data[5];
static struct device effects[5];
static const struct device *declared_a[]={&effects[0],NULL,&effects[1]};
static const struct device *declared_b[]={&effects[2],&effects[3]};
static const struct device *members_a[3], *members_b[2];
static struct kp_rgb_effect_defaults defaults_a[]={{{10,20,30},100},{{0},0},{{40,50,60},200}};
static struct kp_rgb_effect_defaults defaults_b[]={{{70,80,90},300},{{100,10,20},400}};
static const size_t targets_a[]={0,1}, targets_b[]={1,2};
static struct kp_selector_config cfg_a={.common={.opacity=50}};
static struct kp_selector_config cfg_b={.common={.opacity=100}};
static struct kp_selector_data data_a={0}, data_b={0};
static struct device selectors[2];
static void initialize_selector(unsigned n) {
    struct kp_selector_data *d=n ? &data_b : &data_a;
    struct kp_rgb_selector *s=&d->selector;
    s->owner=n ? &owner_b : &owner_a;
    s->effects=n ? members_b : members_a;
    d->members=n ? members_b : members_a;
    s->effect_defaults=n ? defaults_b : defaults_a;
    s->effect_count=n ? 2 : 3;
    s->initial_effect=n ? 0 : 1;
    s->initial_duration_ms=250;
    d->instances=calloc(s->effect_count,sizeof(*d->instances));
    d->reset_pending=calloc(s->effect_count,sizeof(*d->reset_pending));
    for(size_t i=0;i<s->effect_count;i++) d->instances[i].effect=n ? declared_b[i] : declared_a[i];
    d->live_slot=SIZE_MAX;
    selectors[n]=(struct device){.name=n ? "selector-b" : "selector-a",.config=n ? &cfg_b : &cfg_a,.data=d,.api=&selector_api};
}
'''

def production_fixture():
    parts = [PRELUDE, '#include <stdlib.h>\n',
             structure(OVERLAY, 'kp_selector_config'),
             structure(OVERLAY, 'kp_selector_data')]
    parts.extend(function(SELECTOR, n) for n in (
        'kp_rgb_selector_count','kp_rgb_selector_at','kp_rgb_selector_calc_index',
        'kp_rgb_selector_resolve_locked','kp_rgb_selector_apply_defaults_locked',
        'kp_rgb_selector_select_locked'))
    parts.append('static const struct kp_rgb_effect_callbacks kp_selector_callbacks;')
    parts.append(function((ROOT / 'src/rgb_color.c').read_text(), 'kp_rgb_rgb_mix'))
    parts.extend(function(INSTANCE, n) for n in (
        'kp_rgb_instance_callbacks','kp_rgb_callbacks_reset','kp_rgb_callbacks_set_active','kp_rgb_callbacks_on_event',
        'kp_rgb_callbacks_render','kp_rgb_effect_instance_set_active',
        'kp_rgb_effect_instance_reset','kp_rgb_effect_instance_on_event',
        'kp_rgb_effect_instance_render','kp_rgb_effect_instance_restart_clock'))
    parts.extend(function(UTILS, n) for n in ('kp_rgb_overlay_paint_pixels',))
    parts.extend(function(OVERLAY, n) for n in (
        'kp_selector_refresh_members','kp_selector_prepare_locked','kp_selector_prepare',
        'kp_selector_reconcile','kp_selector_reset','kp_selector_set_active',
        'kp_selector_on_event','kp_selector_render'))
    callback_def = OVERLAY[OVERLAY.index('static const struct kp_rgb_effect_callbacks kp_selector_callbacks ='):]
    callback_def = callback_def[:callback_def.index('};') + 2]
    parts.append(callback_def)
    parts.append(function(OVERLAY, 'kp_rgb_selector_overlay_select_locked'))
    parts.append(TESTS + MAIN)
    return '\n'.join(parts)


def main():
    code = production_fixture()
    code += r'''
int main(void) {
    for(int i=0;i<5;i++) { configs[i]=(struct kp_rgb_effect_common_config){.owner=i<2?&owner_a:&owner_b,.slot=(uint16_t)(i<2?i*2:i-2)};
      effects[i]=(struct device){.name="effect",.config=&configs[i],.data=&effect_data[i],.api=&child_api}; }
    effects[1].config=&configs[1];
    initialize_selector(0); initialize_selector(1);
    struct kp_selector_data *a=&data_a,*b=&data_b;
    assert(kp_selector_prepare(&selectors[0])==0 && a->prepared && !a->ready);
    assert(effect_data[0].color.b==100 && effect_data[0].duration_ms==100);
    effect_data[0].duration_ms=444; effect_data[0].color.h=123;
    assert(kp_selector_start(&selectors[0])==0);
    assert(kp_selector_start(&selectors[1])==0);
    assert(a->ready && b->ready && a->selector.effect_index==0 && b->selector.effect_index==0);
    assert(members_a[0]==&effects[0] && members_a[1]==NULL && members_a[2]==&effects[1]);
    assert(effect_data[0].color.b==100 && effect_data[0].color.h==123 && effect_data[0].duration_ms==444);
    assert(effect_data[1].duration_ms==200 && effect_data[2].duration_ms==300);
    /* Ready filtering does not invoke or change unavailable children. */
    a->ready=false; children_ready=false;
    assert(kp_selector_start(&selectors[0])==-ENODEV && !a->ready);
    assert(members_a[0]==NULL && members_a[2]==NULL && trace_length==0);
    children_ready=true; unready_effect=&effects[0];
    defaults_a[2].duration_ms=0;
    assert(kp_selector_start(&selectors[0])==0 && a->selector.effect_index==2);
    assert(members_a[0]==NULL && members_a[2]==&effects[1]);
    assert(effect_data[1].duration_ms==250);
    assert(kp_rgb_selector_overlay_select_locked(&selectors[0],0)==-ENOENT);
    a->ready=false; a->prepared=false; unready_effect=NULL; defaults_a[2].duration_ms=200;
    assert(kp_selector_start(&selectors[0])==0);
    assert(kp_rgb_selector_select_locked(&a->selector,2)==1);
    effect_data[1].duration_ms=555;
    assert(kp_selector_start(&selectors[0])==0);
    assert(a->selector.effect_index==2 && effect_data[1].duration_ms==555);

    struct led_rgb pixels[4]={{10,0,0},{10,0,0},{10,0,0},{10,0,0}}, scratch[4]={{0}};
    struct kp_rgb_frame f={.count=4,.targets=targets_a,.target_count=2,.pixels=pixels,.scratch=scratch,.local_ms=100,.elapsed_ms=100};
    owner_scratch=scratch; owner_targets=targets_a; owner_target_count=2;
    const struct kp_rgb_effect_callbacks *cb=kp_selector_callbacks.render ? &kp_selector_callbacks : NULL;
    assert(cb);
    kp_selector_set_active(&selectors[0],true,100);
    assert(child_active[1] && resets[1]==1 && activations[1]==1);
    /* Select while inactive: no callback or flush from direct locked mutation. */
    kp_selector_set_active(&selectors[1],false,100);
    unsigned before_flush=flushes;
    assert(kp_rgb_selector_overlay_select_locked(&selectors[1],1)==1);
    assert(flushes==before_flush+1 && resets[3]==0 && !child_active[3]);
    assert(kp_rgb_selector_overlay_select_locked(&selectors[1],1)==0 && flushes==before_flush+1);
    assert(kp_rgb_selector_overlay_select_locked(&selectors[1],9)==-EINVAL && flushes==before_flush+1);
    ready=false; assert(kp_rgb_selector_overlay_select_locked(&selectors[1],0)==-ENODEV); ready=true;
    assert(kp_rgb_selector_overlay_select_locked(NULL,0)==-ENODEV);
    struct kp_rgb_overlay_api wrong_api={0}; struct device wrong={.api=&wrong_api};
    assert(kp_rgb_selector_overlay_select_locked(&wrong,0)==-EINVAL);

    /* Continuous active switch deactivates old child before lazy reset/activation. */
    unsigned switch_trace=trace_length;
    assert(kp_rgb_selector_overlay_select_locked(&selectors[0],0)==1);
    uptime=177;
    struct kp_rgb_key_event ev={.position=3,.pressed=true,.timestamp_ms=INT64_C(0x100000009)};
    assert(kp_selector_on_event(&selectors[0],&ev));
    assert(!child_active[1] && child_active[0] && deactivations[1]==1);
    assert(resets[0]==1 && activations[0]==1 && events[0]==1 && seen_event_timestamp[0]==ev.timestamp_ms);
    assert(child_reset_time[0]==177);
    assert(trace_length==switch_trace+4);
    assert(trace[switch_trace]==13 && trace[switch_trace+1]==1 &&
           trace[switch_trace+2]==2 && trace[switch_trace+3]==4);
    /* Select an inactive slot, then deactivate: obsolete live child still goes down. */
    kp_selector_set_active(&selectors[1],true,200);
    assert(child_active[3]);
    assert(kp_rgb_selector_overlay_select_locked(&selectors[1],0)==1);
    assert(child_active[3]); /* selection alone has no callback */
    kp_selector_set_active(&selectors[1],false,210);
    assert(!child_active[3] && deactivations[3]==1 && !child_active[2]);

    /* Reset marks every child; only selected children reset now; others reset on use. */
    kp_selector_reset(&selectors[0],300);
    assert(resets[0]==2 && child_reset_time[0]==300 && child_active[0]);
    assert(a->reset_pending[1] && a->reset_pending[2]);
    assert(resets[3]==1 && !b->reset_pending[0] && !b->reset_pending[1]);
    assert(kp_rgb_selector_overlay_select_locked(&selectors[0],2)==1);
    kp_selector_set_active(&selectors[0],true,310);
    assert(resets[1]==2 && child_reset_time[1]==310 && !a->reset_pending[2] && a->reset_pending[1]);

    /* Each owner has independent clocks; zero-delta restarts even skipped child. */
    child_demand[1]=true; f.local_ms=500; f.elapsed_ms=0;
    owner_scratch=scratch; owner_targets=targets_a; owner_target_count=2;
    a->instances[0].state.animating=true; a->instances[0].state.last_render_ms=450;
    a->instances[2].state.animating=true; a->instances[2].state.last_render_ms=450;
    memset(scratch,0,sizeof(scratch));
    assert(kp_selector_render(&selectors[0],&f));
    assert(child_elapsed[1]==0 && !a->instances[0].state.animating);
    f.elapsed_ms=50; f.local_ms=550;
    assert(kp_selector_render(&selectors[0],&f));
    assert(child_elapsed[1]==50 && !a->instances[0].state.animating);
    child_demand[1]=false;
    assert(!kp_selector_render(&selectors[0],&f));

    /* Owner target overlap is legal; blending affects only owner pixels. */
    owner_scratch=scratch; owner_targets=targets_b; owner_target_count=2;
    f.targets=targets_b; f.target_count=2; f.local_ms=600; f.elapsed_ms=50;
    memset(pixels,0,sizeof(pixels)); memset(scratch,0,sizeof(scratch));
    kp_selector_set_active(&selectors[1],true,600);
    child_demand[2]=true;
    assert(kp_selector_render(&selectors[1],&f));
    f.local_ms=650;
    assert(kp_selector_render(&selectors[1],&f));
    assert(child_elapsed[2]==50); /* Independent B clock sample. */
    assert(pixels[0].r==0 && pixels[3].r==0);
    assert(pixels[1].r==100 && pixels[2].r==100); /* opacity 100 replaces */
    child_demand[2]=false;
    const size_t disjoint_targets[]={2,3};
    owner_scratch=scratch; owner_targets=disjoint_targets; owner_target_count=2;
    f.targets=disjoint_targets; f.target_count=2; f.local_ms=605;
    memset(pixels,0,sizeof(pixels)); memset(scratch,0,sizeof(scratch));
    assert(!kp_selector_render(&selectors[1],&f));
    assert(pixels[0].r==0 && pixels[1].r==0 && pixels[2].r==100 && pixels[3].r==100);
    owner_scratch=scratch; owner_targets=targets_b; owner_target_count=2;
    f.targets=targets_b; f.target_count=2; f.local_ms=610;
    cfg_a.common.opacity=50;
    kp_selector_set_active(&selectors[0],true,610);
    owner_scratch=scratch; owner_targets=targets_a; owner_target_count=2;
    f.targets=targets_a; f.target_count=2; f.local_ms=610; f.elapsed_ms=10;
    memset(pixels,0,sizeof(pixels)); memset(scratch,0,sizeof(scratch));
    assert(kp_selector_render(&selectors[0],&f)==false);
    assert(pixels[0].r==35 && pixels[1].r==35 && pixels[2].r==0 && pixels[3].r==0);
    cfg_b.common.opacity=50;
    owner_targets=targets_b; owner_target_count=2;
    f.targets=targets_b; f.target_count=2;
    assert(!kp_selector_render(&selectors[1],&f));
    assert(pixels[0].r==35 && pixels[1].r==67 && pixels[2].r==50 && pixels[3].r==0);
    const size_t indicator_targets[]={1};
    const struct led_rgb indicator[4]={{0},{200,0,0},{0},{0}};
    kp_rgb_overlay_paint_pixels(&f,indicator_targets,1,indicator,100);
    assert(pixels[0].r==35 && pixels[1].r==200 && pixels[2].r==50 && pixels[3].r==0);
    unsigned reset_a0=resets[0], reset_a1=resets[1];
    kp_selector_set_active(&selectors[0],false,700);
    kp_selector_reset(&selectors[0],800);
    assert(resets[0]==reset_a0 && resets[1]==reset_a1);
    kp_selector_set_active(&selectors[0],true,810);
    assert(resets[1]==reset_a1+1 && resets[0]==reset_a0 && a->reset_pending[0]);
    assert(effect_data[1].duration_ms==555 && effect_data[1].color.h==40);
    assert(kp_rgb_selector_overlay_select_locked(&selectors[0],0)==1);
    uptime=830;
    assert(kp_selector_on_event(&selectors[0],&ev));
    assert(resets[0]==reset_a0+1 && !a->reset_pending[0]);
    assert(lock_depth==0);
    free(a->instances); free(a->reset_pending);
    free(b->instances); free(b->reset_pending);
    puts("selector overlay lifecycle, selection, clocks, targets and composition passed");
    return 0;
}
'''
    # Compile actual startup function as well.
    code = code.replace('int main(void) {', function(OVERLAY, 'kp_selector_start') + '\nint main(void) {')
    with tempfile.TemporaryDirectory() as tmp:
        src=Path(tmp)/'selector_overlay.c'; binary=Path(tmp)/'selector_overlay'
        src.write_text(code)
        subprocess.run(shlex.split(os.environ.get('CC','cc')) + ['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__ == '__main__':
    main()
