#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host-execute selector APIs, startup/reset and asynchronous notification bodies.

Only devices, mutex, queue and event delivery are mocked. This is deterministic
single-thread execution, not a Zephyr workqueue/concurrency or DT integration test.
Set SELECTOR_PUBLIC_API_SANITIZE=1 to additionally enable ASan/UBSan.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_singleton import function, structure

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ARG_UNUSED(x) ((void)(x))
#define CLAMP(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS 10000
#define KP_RGB_BRT_MAX 100
#define LOG_ERR(...) ((void)0)
struct device { const char *name; const void *config; void *data; const void *api; bool ready; };
struct kp_rgb_hsb { uint16_t h; uint8_t s,b; };
struct kp_rgb_overlay_common_data { int unused; };
struct kp_rgb_effect_callbacks { int unused; };
struct kp_rgb_overlay_api { const struct kp_rgb_effect_callbacks *callbacks; };
struct kp_rgb_effect_instance { const struct device *effect; };
static const struct kp_rgb_effect_callbacks kp_selector_callbacks={0};
static const struct kp_rgb_overlay_api owner_api={&kp_selector_callbacks};
static unsigned held,locks,unlocks,flushes,submissions;
static bool isr;
static bool device_is_ready(const struct device *dev) { return dev && dev->ready; }
static bool k_is_in_isr(void) { return isr; }
static void kp_rgb_matrix_lock(void) { assert(!isr && !held); held=1; locks++; }
static void kp_rgb_matrix_unlock(void) { assert(held==1); held=0; unlocks++; }
static void zmk_rgb_matrix_flush(void) { assert(held); flushes++; }
static const struct device *kp_rgb_selector_control(const struct device *dev) { return dev; }
static unsigned refresh_holds,refresh_requests;
static void kp_rgb_control_refresh_locked(void) { assert(held); refresh_requests++; }
static void kp_rgb_control_refresh_hold_locked(void) { assert(held); refresh_holds++; kp_rgb_control_refresh_locked(); }
static void kp_rgb_control_refresh_release_locked(void) { assert(held && refresh_holds); refresh_holds--; }
struct k_work { void (*handler)(struct k_work *); bool pending; };
struct k_work_q { int unused; };
static struct k_work_q queue;
#define K_WORK_DEFINE(name,fn) struct k_work name={.handler=fn}
static struct k_work_q *kp_rgb_work_q(void) { return &queue; }
static int k_work_submit_to_queue(struct k_work_q *q,struct k_work *work) {
 assert(held && q==&queue); submissions++; work->pending=true; return 1;
}
static int raise_zmk_rgb_selector_selection_changed(struct zmk_rgb_selector_selection_changed event);
'''

FIXTURE = r'''
static struct device owners[2],effects[6];
static struct kp_selector_data data[2];
static struct kp_rgb_effect_common_data parameters[6];
static struct kp_rgb_effect_common_config configs[6];
static const struct device *members[2][3];
static struct kp_rgb_effect_instance instances[2][3];
static bool reset_pending[2][3];
static const struct kp_rgb_effect_defaults defaults[2][3]={
 {{{10,20,30},0},{{0},0},{{40,50,60},700}},
 {{{70,80,90},800},{{0},0},{{100,10,20},900}}
};
static const struct device *event_owners[32],*event_selected[32];
static unsigned events;
static bool reenter;
static void drain(void) {
 assert(!held && !isr);
 unsigned passes=0;
 while(notification_work.pending) {
   assert(++passes<16); notification_work.pending=false;
   notification_work.handler(&notification_work);
 }
 assert(notification_head==NULL && !held && locks==unlocks && !refresh_holds);
}
static int raise_zmk_rgb_selector_selection_changed(struct zmk_rgb_selector_selection_changed event) {
 assert(!held && !isr && events<32);
 assert(event.selector==&owners[0] || event.selector==&owners[1]);
 event_owners[events]=event.selector;
 event_selected[events++]=zmk_rgb_selector_get_selected(event.selector);
 if(reenter) {
   reenter=false;
   assert(event.selector==&owners[0]);
   assert(zmk_rgb_selector_select(event.selector,&effects[0])==0);
 }
 return 0;
}
static void setup(bool start) {
 drain();
 held=locks=unlocks=flushes=submissions=events=refresh_holds=refresh_requests=0; isr=reenter=false;
 memset(data,0,sizeof(data)); memset(parameters,0,sizeof(parameters));
 memset(members,0,sizeof(members)); memset(instances,0,sizeof(instances));
 memset(reset_pending,0,sizeof(reset_pending));
 for(size_t i=0;i<2;i++) {
   owners[i]=(struct device){.name=i?"b":"a",.data=&data[i],.api=&owner_api,.ready=true};
   data[i]=(struct kp_selector_data){
     .selector={.owner=&owners[i],.effects=members[i],.effect_defaults=defaults[i],
       .effect_count=3,.initial_duration_ms=600,.initial_effect=0,.publish_changes=true},
     .members=members[i],.instances=instances[i],.reset_pending=reset_pending[i],.live_slot=SIZE_MAX};
 }
 for(size_t i=0;i<6;i++) {
   unsigned owner=i<2?0:1;
   configs[i]=(struct kp_rgb_effect_common_config){.owner=i<4?&owners[owner]:NULL,.slot=i%2?2:0};
   effects[i]=(struct device){.name="fx",.config=&configs[i],.data=&parameters[i],.ready=true};
   parameters[i]=(struct kp_rgb_effect_common_data){.color={350,95,100},.duration_ms=1234};
   if(i<4) instances[owner][i%2?2:0].effect=&effects[i];
 }
 if(start) {
   assert(kp_selector_start(&owners[0])==0 && kp_selector_start(&owners[1])==0);
   drain(); assert(events==2);
   events=flushes=submissions=0;
 }
}
static void assert_idle(void) {
 assert(!held && locks==unlocks && !notification_head && !notification_work.pending);
}
'''

TESTS = r'''
static void test_startup(void) {
 setup(false);
 assert(zmk_rgb_selector_get_selected(&owners[0])==NULL);
 assert(zmk_rgb_selector_select(&owners[0],&effects[0])==-ENODEV);
 assert(zmk_rgb_selector_reset(&owners[0])==-ENODEV);
 kp_rgb_matrix_lock(); kp_selector_prepare_locked(&data[0]); kp_rgb_matrix_unlock();
 assert(data[0].prepared && !data[0].ready && data[0].selector.active_fx==NULL);
 assert(submissions==0 && notification_head==NULL); /* Temporary defaults must not escape. */
 parameters[0].color.h=123; parameters[0].duration_ms=4321;
 data[0].selector.effect_index=2; data[0].selector.active_fx=&effects[1];
 assert(kp_selector_start(&owners[0])==0);
 assert(zmk_rgb_selector_get_selected(&owners[0])==&effects[1]);
 assert(parameters[0].color.h==123 && parameters[0].duration_ms==4321);
 assert(events==0 && data[0].selector.notification_pending);
 drain(); assert(events==1 && event_owners[0]==&owners[0] && event_selected[0]==&effects[1]);
 unsigned s=submissions;
 assert(kp_selector_start(&owners[0])==0 && submissions==s); drain(); assert(events==1);
 assert(kp_selector_start(&owners[1])==0); drain();
 assert(events==2 && event_owners[1]==&owners[1] && event_selected[1]==&effects[2]);
 assert_idle();

 setup(false); effects[0].ready=effects[1].ready=false;
 assert(kp_selector_start(&owners[0])==-ENODEV);
 assert(zmk_rgb_selector_get_selected(&owners[0])==NULL && submissions==0);
 effects[1].ready=true;
 assert(kp_selector_start(&owners[0])==0); drain();
 assert(events==1 && event_selected[0]==&effects[1]); assert_idle();
}
static void test_select_getter_validation(void) {
 setup(true);
 assert(zmk_rgb_selector_get_selected(&owners[0])==&effects[0]);
 assert(zmk_rgb_selector_get_selected(&owners[1])==&effects[2]);
 assert(zmk_rgb_selector_select(&owners[0],&effects[0])==0);
 assert(flushes==0 && submissions==0 && events==0);
 struct kp_rgb_effect_common_data before[6]; memcpy(before,parameters,sizeof(before));
 assert(zmk_rgb_selector_select(&owners[0],&effects[1])==0);
 assert(data[0].selector.effect_index==2 && flushes==1 && events==0);
 assert(zmk_rgb_selector_get_selected(&owners[0])==&effects[1]);
 assert(zmk_rgb_selector_get_selected(&owners[1])==&effects[2]);
 assert(!memcmp(before,parameters,sizeof(before)) && !data[0].active && !data[0].reset_requested);
 unsigned f=flushes,s=submissions;
 for(unsigned i=0;i<4;i++) assert(zmk_rgb_selector_get_selected(&owners[0])==&effects[1]);
 assert(flushes==f && submissions==s && data[0].selector.notification_pending);
 assert(zmk_rgb_selector_select(&owners[0],NULL)==-EINVAL);
 assert(zmk_rgb_selector_select(&owners[0],&effects[2])==-EINVAL);
 assert(zmk_rgb_selector_select(&owners[0],&effects[4])==-EINVAL);
 /* Matching owner/slot metadata alone is not membership. */
 configs[4].owner=&owners[0]; configs[4].slot=2;
 assert(zmk_rgb_selector_select(&owners[0],&effects[4])==-EINVAL);
 configs[5].owner=&owners[0]; configs[5].slot=1; /* Disabled sibling slot. */
 assert(zmk_rgb_selector_select(&owners[0],&effects[5])==-EINVAL);
 effects[1].ready=false;
 assert(zmk_rgb_selector_get_selected(&owners[0])==&effects[1]); effects[1].ready=true;
 effects[0].ready=false;
 assert(zmk_rgb_selector_select(&owners[0],&effects[0])==-ENODEV); effects[0].ready=true;
 owners[0].ready=false;
 assert(zmk_rgb_selector_get_selected(&owners[0])==NULL);
 assert(zmk_rgb_selector_select(&owners[0],&effects[0])==-ENODEV);
 assert(zmk_rgb_selector_reset(&owners[0])==-ENODEV); owners[0].ready=true;
 data[0].ready=false;
 assert(zmk_rgb_selector_get_selected(&owners[0])==NULL);
 assert(zmk_rgb_selector_select(&owners[0],&effects[0])==-ENODEV);
 assert(zmk_rgb_selector_reset(&owners[0])==-ENODEV); data[0].ready=true;
 assert(zmk_rgb_selector_get_selected(NULL)==NULL);
 assert(zmk_rgb_selector_select(NULL,&effects[0])==-ENODEV);
 assert(zmk_rgb_selector_reset(NULL)==-ENODEV);
 assert(flushes==f && submissions==s && data[0].selector.active_fx==&effects[1]);
 drain(); assert(events==1 && event_selected[0]==&effects[1]); assert_idle();
}
static void test_publication_opt_out(void) {
 setup(true); data[0].selector.publish_changes=false;
 assert(zmk_rgb_selector_select(&owners[0],&effects[1])==0 && flushes==1);
 assert(zmk_rgb_selector_get_selected(&owners[0])==&effects[1]);
 assert(submissions==0 && !data[0].selector.notification_pending);
 kp_rgb_matrix_lock();
 struct kp_rgb_selector temporary=data[0].selector;
 assert(kp_rgb_selector_apply_defaults_locked(&temporary)==0);
 kp_rgb_matrix_unlock();
 assert(temporary.active_fx==&effects[0] && data[0].selector.active_fx==&effects[1]);
 assert(submissions==0 && notification_head==NULL); drain(); assert(events==0); assert_idle();
}
static void test_isr(void) {
 setup(true); isr=true; unsigned l=locks;
 assert(zmk_rgb_selector_get_selected(&owners[0])==NULL);
 assert(zmk_rgb_selector_select(&owners[0],&effects[1])==-EWOULDBLOCK);
 assert(zmk_rgb_selector_select(NULL,NULL)==-EWOULDBLOCK);
 assert(zmk_rgb_selector_reset(&owners[0])==-EWOULDBLOCK);
 assert(zmk_rgb_selector_reset(NULL)==-EWOULDBLOCK);
 assert(locks==l && flushes==0 && submissions==0 && events==0);
 assert(data[0].selector.active_fx==&effects[0]); isr=false; assert_idle();
}
static void test_reset(void) {
 setup(true);
 assert(zmk_rgb_selector_select(&owners[0],&effects[1])==0); drain(); events=0;
 parameters[0].color.h=199; parameters[0].duration_ms=999;
 parameters[1].color.h=299; parameters[1].duration_ms=1999;
 struct kp_rgb_effect_common_data other[4]; memcpy(other,&parameters[2],sizeof(other));
 unsigned f=flushes;
 assert(zmk_rgb_selector_reset(&owners[0])==0 && flushes==f+1);
 assert(data[0].reset_requested && !data[1].reset_requested && data[0].live_slot==SIZE_MAX);
 assert(zmk_rgb_selector_get_selected(&owners[0])==&effects[0]);
 assert(parameters[0].color.h==10 && parameters[0].color.b==100 && parameters[0].duration_ms==600);
 assert(parameters[1].color.h==40 && parameters[1].duration_ms==700);
 assert(!memcmp(other,&parameters[2],sizeof(other)) && data[1].selector.active_fx==&effects[2]);
 drain(); assert(events==1 && event_selected[0]==&effects[0]);
 unsigned s=submissions; parameters[0].duration_ms=999;
 assert(zmk_rgb_selector_reset(&owners[0])==0 && parameters[0].duration_ms==600);
 assert(submissions==s); drain(); assert(events==1); assert_idle();
}
static void test_coalescing_and_reentry(void) {
 setup(true);
 assert(zmk_rgb_selector_select(&owners[0],&effects[1])==0);
 assert(zmk_rgb_selector_select(&owners[0],&effects[0])==0);
 assert(zmk_rgb_selector_select(&owners[0],&effects[1])==0);
 assert(zmk_rgb_selector_select(&owners[1],&effects[3])==0);
 assert(events==0 && data[0].selector.notification_pending && data[1].selector.notification_pending);
 drain(); assert(events==2);
 for(unsigned i=0;i<events;i++) {
   assert(event_selected[i]==(event_owners[i]==&owners[0]?&effects[1]:&effects[3]));
 }
 assert(event_owners[0]!=event_owners[1]);
 assert(!data[0].selector.notification_pending && !data[1].selector.notification_pending);
 /* Coalescing to the original value still signals that retained selection changed. */
 events=0;
 assert(zmk_rgb_selector_select(&owners[0],&effects[0])==0);
 assert(zmk_rgb_selector_select(&owners[0],&effects[1])==0);
 drain(); assert(events==1 && event_selected[0]==&effects[1]);
 events=0; assert(zmk_rgb_selector_select(&owners[0],&effects[0])==0); drain();
 events=0; reenter=true;
 assert(zmk_rgb_selector_select(&owners[0],&effects[1])==0);
 drain(); assert(events==2 && event_owners[0]==&owners[0] && event_owners[1]==&owners[0]);
 assert(event_selected[0]==&effects[1] && event_selected[1]==&effects[0]);
 assert(zmk_rgb_selector_get_selected(&owners[0])==&effects[0]); assert_idle();
}
int main(void) {
 test_startup(); test_select_getter_validation(); test_publication_opt_out(); test_isr(); test_reset(); test_coalescing_and_reentry();
 puts("selector public API/startup/reset/notifications: passed"); return 0;
}
'''


def production_fixture():
    api = (ROOT / "src/rgb_selector_api.c").read_text()
    selector = (ROOT / "src/rgb_selector.c").read_text()
    overlay = (ROOT / "src/overlays/selector.c").read_text()
    header = (ROOT / "src/rgb_selector_internal.h").read_text()
    public = (ROOT / "include/zmk/rgb_matrix.h").read_text()
    event = (ROOT / "include/zmk/events/rgb_selector_selection_changed.h").read_text()
    # Forward event declaration precedes the mock raise prototype.
    parts = ["struct zmk_rgb_selector_selection_changed;", PRELUDE]
    parts += [structure(header, n) for n in ("kp_rgb_effect_defaults", "kp_rgb_selector")]
    parts += [structure(public, n) for n in ("kp_rgb_effect_common_config", "kp_rgb_effect_common_data")]
    parts += [structure(overlay, "kp_selector_data"), structure(event, "zmk_rgb_selector_selection_changed")]
    parts.append(function(public, "kp_rgb_effect_data"))
    # Keep the real intrusive queue state and work registration, not a replica.
    parts.append(api[api.index("static struct kp_rgb_selector *notification_head;"):
                     api.index("void kp_rgb_selector_publish_locked(")])
    parts += [function(api, n) for n in ("kp_rgb_selector_publish_locked", "kp_rgb_selector_notify_work")]
    parts += [function(selector, n) for n in (
        "kp_rgb_selector_count", "kp_rgb_selector_at", "kp_rgb_selector_calc_index",
        "kp_rgb_selector_resolve_locked", "kp_rgb_selector_apply_defaults_locked", "kp_rgb_selector_select_locked")]
    parts += [function(overlay, n) for n in (
        "kp_rgb_selector_overlay_state_locked", "kp_rgb_selector_overlay_select_locked",
        "kp_rgb_selector_overlay_reset_locked", "kp_selector_refresh_members",
        "kp_selector_prepare_locked", "kp_selector_start")]
    parts += [function(api, n) for n in (
        "zmk_rgb_selector_get_selected", "zmk_rgb_selector_select", "zmk_rgb_selector_reset")]
    return "\n".join(parts) + FIXTURE + TESTS


def main():
    code = production_fixture()
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    if os.environ.get("SELECTOR_PUBLIC_API_SANITIZE") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "selector_public_api.c"
        binary = Path(tmp) / "selector_public_api"
        source.write_text(code)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
            str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
