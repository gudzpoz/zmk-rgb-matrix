#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile actual engine functions with host queue/strip mocks, with and without LEDs."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/rgb_matrix.c").read_text()
BEHAVIOR_SOURCE = (ROOT / "src/behavior_rgb_matrix.c").read_text()


def function(name):
    match = re.search(r"^(?:static )?(?:inline )?[\w *]+\b" + name + r"\([^;]*?\) \{", SOURCE, re.M)
    assert match, name
    start = match.start()
    pos = SOURCE.index("{", match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]

TOGGLE_CASE = re.search(r"^  case RGB_TOG_CMD: \{\n.*?^  \}\n", BEHAVIOR_SOURCE, re.M | re.S)
assert TOGGLE_CASE, "RGB toggle conversion"
TOGGLE_FUNCTION = """
static int convert_toggle(struct kp_rgb_behavior_context *ctx, struct binding *binding) {
    switch (binding->param1) {
""" + TOGGLE_CASE.group() + """
    default: return -EINVAL;
    }
    return 0;
}
"""

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#define ARG_UNUSED(x) (void)(x)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define CONFIG_KEYPAW_RGB_MATRIX_TICK_MS 16
#define CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE 1
#define IS_ENABLED(x) (x)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define KP_RGB_EVENT_QUEUE_LEN 16
#define RGB_TOG_CMD 0
#define RGB_ON_CMD 1
#define RGB_OFF_CMD 2
struct binding { uint32_t param1; };
struct k_work { int unused; };
static struct k_work kp_off_work, kp_tick_work, kp_pending_work;
struct k_timer { int unused; };
static struct k_timer kp_tick_timer;
static int timer_period,tick_submits;
typedef _Atomic int atomic_t;
#define atomic_get(p) atomic_load(p)
#define atomic_set(p,v) atomic_store(p,v)
struct device { const void *api; const char *name; };
struct led_rgb { uint8_t r,g,b; };
struct kp_rgb_coord { uint16_t x,y; };
struct kp_rgb_tuning { int sentinel; };
struct kp_rgb_frame {
 const struct kp_rgb_tuning *tune; size_t count;
 const struct kp_rgb_coord *coords; struct led_rgb *pixels;
 uint32_t elapsed; uint16_t board_length,board_height; bool is_idle;
};
struct zmk_position_state_changed { uint32_t position; bool state; };
struct kp_rgb_effect_api {
 void (*render)(const struct device *,struct kp_rgb_frame *);
 const struct device *const *overlays; size_t overlays_len;
};
struct kp_rgb_overlay_api { void (*render)(const struct device *,struct kp_rgb_frame *); };
struct kp_rgb_behavior_context {
 const struct device *dev;
 struct { bool on,user_on; const struct device *active_fx; } state;
 bool zone_valid,all_leds; const uint32_t *leds; size_t leds_len;
 struct kp_rgb_tuning tuning;
};
static struct kp_rgb_behavior_context ctx;
static size_t kp_rgb_behavior_count(void) { return 1; }
static struct kp_rgb_behavior_context *kp_rgb_behavior_at(size_t n) { return n?NULL:&ctx; }
static struct kp_rgb_behavior_context *kp_rgb_behavior_context_from_device(const struct device *d) { return d?&ctx:NULL; }
static pthread_mutex_t lock;
static _Thread_local unsigned int held;
static void kp_rgb_matrix_lock(void) { pthread_mutex_lock(&lock); held++; }
static void kp_rgb_matrix_unlock(void) { assert(held); held--; pthread_mutex_unlock(&lock); }
static bool kp_rgb_matrix_lock_patiently(void) { kp_rgb_matrix_lock(); return true; }
#define KP_TRY_LOCK() (kp_rgb_matrix_lock(),0)
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static struct k_spinlock kp_rgb_pending_lock;
static bool inject_pending_press,pending_press_accepted;
static bool kp_rgb_pending_push(const struct zmk_position_state_changed *ev);
static int k_spin_lock(struct k_spinlock *l) {
 (void)l;
 if(inject_pending_press) {
  inject_pending_press=false;
  const struct zmk_position_state_changed ev={.position=0,.state=true};
  pending_press_accepted=kp_rgb_pending_push(&ev);
 }
 pthread_mutex_lock(&pending_lock);return 0;
}
static void k_spin_unlock(struct k_spinlock *l,int key) { (void)l; (void)key; pthread_mutex_unlock(&pending_lock); }
static struct zmk_position_state_changed kp_rgb_pending[KP_RGB_EVENT_QUEUE_LEN];
static uint8_t kp_rgb_pending_head,kp_rgb_pending_tail;
static uint32_t kp_rgb_pending_dropped;
static atomic_t kp_rgb_any_on,kp_rgb_inhibited;
static bool kp_rgb_matrix_valid,kp_rgb_output_ready,kp_rgb_black_pending;
static uint32_t kp_rgb_black_retry_ms=100,last_tick,now;
static struct led_rgb pixels[KP_LED_COUNT],scratch[KP_LED_COUNT];
static struct kp_rgb_coord kp_led_coords[KP_LED_COUNT];
static uint16_t kp_rgb_board_length=100,kp_rgb_board_height=100;
static const struct device dummy = {.name="strip"};
static const struct device *strip=&dummy;
static bool isr,idle;
static bool inhibit_from_render;
int zmk_rgb_matrix_set_inhibited(bool inhibited);
static int polls,refreshes,dispatches,renders,feedback,writes,colored,failures,retry_delay;
static atomic_t transfer_entered,transfer_release,block_transfer,setter_entered,setter_done;
static uint32_t rendered_elapsed;
static uint32_t k_uptime_get_32(void) { return now; }
static bool k_is_in_isr(void) { return isr; }
#define ZMK_ACTIVITY_ACTIVE 0
static int zmk_activity_get_state(void) { return idle; }
static bool kp_rgb_effective_on(bool on,bool apply) { return on && (!apply || !idle); }
static void *zmk_workqueue_lowprio_work_q(void) { return NULL; }
static int k_work_submit_to_queue(void *q,struct k_work *w) { (void)q;if(w==&kp_tick_work) tick_submits++;return 0; }
static int k_work_reschedule_for_queue(void *q,struct k_work *w,int delay) { (void)q;(void)w; retry_delay=delay;return 0; }
static void k_timer_stop(struct k_timer *t) { (void)t;timer_period=0; }
static void k_timer_start(struct k_timer *t,int delay,int period) { (void)t;(void)delay;timer_period=period; }
static bool device_is_ready(const struct device *d) { return d!=NULL; }
static int kp_rgb_validate_zones(void) { kp_rgb_matrix_valid=true;return 0; }
static void kp_rgb_triggers_poll(void) { assert(!held); polls++; }
static bool kp_rgb_overlay_refresh(void) { assert(!held); refreshes++; return true; }
static void kp_rgb_overlay_dispatch(void) { assert(!held); dispatches++; }
static const struct device *const *kp_rgb_overlay_list(void) { return NULL; }
static size_t kp_rgb_overlay_count(void) { return 0; }
static bool kp_rgb_overlay_covers_all(const struct device *d) { (void)d;return false; }
static bool kp_rgb_overlay_gate(const struct device *d,const void *api) { (void)d;(void)api;return true; }
static void kp_rgb_deliver_position(const struct zmk_position_state_changed *e) { (void)e; assert(held);feedback++; }
static void render(const struct device *d,struct kp_rgb_frame *f) {
 (void)d; assert(held); renders++;rendered_elapsed=f->elapsed;
 for(size_t n=0;n<f->count;n++) f->pixels[n].r=17;
 if(inhibit_from_render) {
  inhibit_from_render=false;
  assert(zmk_rgb_matrix_set_inhibited(true)==0);
 }
}
static const struct kp_rgb_effect_api api={.render=render};
static const struct device fx={.api=&api};
static int led_strip_update_rgb(const struct device *d,struct led_rgb *p,size_t n) {
 (void)d; assert(held);assert(n>0);writes++;
 bool color=false;
 for(size_t i=0;i<n;i++) color |= p[i].r || p[i].g || p[i].b;
 if(color) colored++;
 if(atomic_get(&block_transfer)) {
  atomic_set(&transfer_entered,1);
  while(!atomic_get(&transfer_release)) sched_yield();
 }
 memset(p,0xa5,n*sizeof(*p));
 if(failures) { assert(!color);failures--;return -EIO; }
 return 0;
}
static void kp_rgb_request_black_locked(void);
static void kp_rgb_pending_purge(void);
void zmk_rgb_matrix_flush(void);
'''

FUNCTIONS = [
    "kp_rgb_has_leds", "kp_any_on_locked", "kp_rgb_publish_any_on_locked",
    "kp_rgb_matrix_tick_handler", "kp_start_timer",
    "kp_last_covering_overlay", "kp_render_overlays", "kp_rgb_matrix_tick",
    "kp_rgb_request_black_locked", "kp_rgb_matrix_off_handler",
    "zmk_rgb_matrix_set_inhibited", "zmk_rgb_matrix_is_inhibited",
    "zmk_rgb_matrix_flush", "kp_rgb_pending_purge", "kp_rgb_pending_push",
    "kp_rgb_pending_pop", "kp_rgb_pending_take_dropped", "kp_rgb_matrix_pending_handler",
    "kp_rgb_permission_handler", "zmk_rgb_matrix_on", "zmk_rgb_matrix_off",
    "zmk_rgb_matrix_get_state", "kp_rgb_matrix_init",
]

TESTS = r'''
static void *tick_thread(void *unused) { (void)unused;kp_rgb_matrix_tick(NULL);return NULL; }
static void *setter_thread(void *unused) {
 (void)unused;atomic_set(&setter_entered,1);
 assert(zmk_rgb_matrix_set_inhibited(true)==0);atomic_set(&setter_done,1);return NULL;
}
static void *off_thread(void *unused) { (void)unused;kp_rgb_matrix_off_handler(NULL);return NULL; }
static void *on_thread(void *unused) {
 (void)unused;atomic_set(&setter_entered,1);
 assert(zmk_rgb_matrix_on(&dummy)==0);atomic_set(&setter_done,1);return NULL;
}
int main(void) {
 pthread_mutexattr_t attributes;
 assert(pthread_mutexattr_init(&attributes)==0);
 assert(pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE)==0);
 assert(pthread_mutex_init(&lock,&attributes)==0);
 assert(pthread_mutexattr_destroy(&attributes)==0);
 assert(!zmk_rgb_matrix_is_inhibited());
 isr=true;assert(zmk_rgb_matrix_set_inhibited(true)==-EWOULDBLOCK);isr=false;
 ctx.state.on=true;ctx.state.user_on=true;ctx.state.active_fx=&fx;
 ctx.zone_valid=ctx.all_leds=true;ctx.tuning.sentinel=123;
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(!kp_rgb_output_ready && writes==0);
 ctx.dev=&dummy;
 failures=KP_LED_COUNT?1:0;
 assert(kp_rgb_matrix_init()==0);
 assert(ctx.state.on && ctx.state.user_on && ctx.tuning.sentinel==123);
 assert(ctx.state.active_fx==&fx);
 bool state=false;assert(zmk_rgb_matrix_get_state(&dummy,&state)==0 && state);
 struct zmk_position_state_changed ev={.position=0,.state=true};
 assert(!kp_rgb_pending_push(&ev));
 assert(timer_period==CONFIG_KEYPAW_RGB_MATRIX_TICK_MS);
 int submitted=tick_submits;
 kp_rgb_matrix_tick_handler(&kp_tick_timer);
 assert(tick_submits==submitted+1);
 now=10000;kp_rgb_matrix_tick(NULL);
 assert(polls==1 && refreshes==1 && dispatches==1 && renders==0 && feedback==0 && colored==0);
 for(int i=0;i<3;i++) { now+=CONFIG_KEYPAW_RGB_MATRIX_TICK_MS;kp_rgb_matrix_tick(NULL); }
 assert(polls==4 && refreshes==4 && dispatches==4 && renders==0 && feedback==0);
 if(KP_LED_COUNT) {
  assert(kp_rgb_black_pending && retry_delay==100);failures=7;
  int delays[]={200,400,800,1000,1000,1000,1000};
  for(int i=0;i<7;i++) { kp_rgb_matrix_off_handler(NULL);assert(retry_delay==delays[i]);assert(colored==0); }
  kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 struct binding binding={.param1=RGB_TOG_CMD};
 assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_OFF_CMD);
 assert(zmk_rgb_matrix_off(&dummy)==0);
 binding.param1=RGB_TOG_CMD;
 assert(convert_toggle(&ctx,&binding)==0 && binding.param1==RGB_ON_CMD);
 assert(zmk_rgb_matrix_on(&dummy)==0 && ctx.state.on);
 assert(zmk_rgb_matrix_is_inhibited() && !colored);
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 now+=16;kp_rgb_matrix_tick(NULL);
 if(KP_LED_COUNT) assert(renders==1 && rendered_elapsed==16);
 assert(kp_rgb_pending_push(&ev));
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 assert(zmk_rgb_matrix_set_inhibited(false)==0);
 kp_rgb_matrix_pending_handler(NULL);assert(feedback==0);
 assert(kp_rgb_pending_push(&ev));kp_rgb_matrix_pending_handler(NULL);assert(feedback==1);
 int before=writes;kp_rgb_matrix_off_handler(NULL);assert(writes==before);
 // Normal OFF failure must retry even after the driver's buffer mutation.
 assert(zmk_rgb_matrix_off(&dummy)==0);
 if(KP_LED_COUNT) {
  failures=1;kp_rgb_matrix_off_handler(NULL);assert(kp_rgb_black_pending);
  kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 assert(zmk_rgb_matrix_on(&dummy)==0);
 // A queued normal OFF clear cannot overwrite a newer ON.
 assert(zmk_rgb_matrix_off(&dummy)==0);
 assert(zmk_rgb_matrix_on(&dummy)==0);
 before=writes;kp_rgb_matrix_off_handler(NULL);assert(writes==before);
 // Local idle policy remains independent of the inhibition gate.
 idle=true;kp_rgb_permission_handler(NULL);assert(!ctx.state.on && ctx.state.user_on);
 idle=false;kp_rgb_permission_handler(NULL);assert(ctx.state.on);
 if(KP_LED_COUNT) {
  pthread_t tick,setter;
  atomic_set(&block_transfer,1);
  pthread_create(&tick,NULL,tick_thread,NULL);
  while(!atomic_get(&transfer_entered)) sched_yield();
  pthread_create(&setter,NULL,setter_thread,NULL);
  while(!atomic_get(&setter_entered)) sched_yield();
  assert(!atomic_get(&setter_done));
  atomic_set(&transfer_release,1);
  pthread_join(tick,NULL);pthread_join(setter,NULL);
  atomic_set(&block_transfer,0);
  before=colored;kp_rgb_matrix_tick(NULL);assert(colored==before);
  kp_rgb_matrix_off_handler(NULL);assert(colored==before);
 }
 if(KP_LED_COUNT) {
  // ON waits for an in-flight black transfer and suppresses its failed retry.
  pthread_t off,on;
  zmk_rgb_matrix_set_inhibited(false);zmk_rgb_matrix_off(&dummy);
  atomic_set(&transfer_entered,0);atomic_set(&transfer_release,0);
  atomic_set(&setter_entered,0);atomic_set(&setter_done,0);
  atomic_set(&block_transfer,1);failures=1;
  pthread_create(&off,NULL,off_thread,NULL);
  while(!atomic_get(&transfer_entered)) sched_yield();
  pthread_create(&on,NULL,on_thread,NULL);
  while(!atomic_get(&setter_entered)) sched_yield();
  assert(!atomic_get(&setter_done));atomic_set(&transfer_release,1);
  pthread_join(off,NULL);pthread_join(on,NULL);atomic_set(&block_transfer,0);
  before=writes;kp_rgb_matrix_off_handler(NULL);assert(writes==before);
 }
 if(KP_LED_COUNT) {
  zmk_rgb_matrix_set_inhibited(false);
  assert(zmk_rgb_matrix_on(&dummy)==0);
  before=colored;inhibit_from_render=true;
  kp_rgb_matrix_tick(NULL);
  assert(zmk_rgb_matrix_is_inhibited() && colored==before);
 }
 assert(zmk_rgb_matrix_set_inhibited(true)==0);
 inject_pending_press=true;
 assert(zmk_rgb_matrix_set_inhibited(false)==0 && !inject_pending_press);
 struct zmk_position_state_changed received;
 assert(kp_rgb_pending_pop(&received)==pending_press_accepted);
 assert(kp_rgb_pending_push(&ev) && kp_rgb_pending_pop(&received));
 // Initial black failure with an already OFF controller is not lost.
 zmk_rgb_matrix_set_inhibited(false);
 zmk_rgb_matrix_off(&dummy);
 kp_rgb_black_pending=false;failures=KP_LED_COUNT?1:0;
 assert(kp_rgb_matrix_init()==0);
 if(KP_LED_COUNT) {
  assert(kp_rgb_black_pending);kp_rgb_matrix_off_handler(NULL);assert(!kp_rgb_black_pending);
 }
 puts("production-extracted output gate tests passed");
}
'''

with tempfile.TemporaryDirectory() as directory:
    for leds in (2, 0):
        source = pathlib.Path(directory) / "gate.c"
        source.write_text(f"#define KP_LED_COUNT {leds}\n" + PRELUDE + "\n".join(map(function, FUNCTIONS)) + TOGGLE_FUNCTION + TESTS)
        binary = pathlib.Path(directory) / "gate"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-type-limits", "-pthread", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
