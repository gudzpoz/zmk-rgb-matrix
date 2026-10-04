# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host fixtures for production-extracted RGB engine functions (not a Zephyr emulator)."""
import os
import pathlib
import re
import shlex
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
static int convert_toggle(struct kp_rgb_controller *controller, struct binding *binding) {
    (void)controller;
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
#define IS_ENABLED(x) (x)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define KP_RGB_EVENT_QUEUE_LEN 16
#define RGB_TOG_CMD 0
#define RGB_ON_CMD 1
#define RGB_OFF_CMD 2
struct binding { uint32_t param1; };
struct k_work {
 void (*handler)(struct k_work *); bool queued,running,scheduled;
 uint64_t due,order,deadline,schedule_order; unsigned runs;
};
static struct k_work kp_off_work, kp_tick_work;
struct k_timer {
 void (*handler)(struct k_timer *); bool active; uint64_t due; uint32_t period;
};
static struct k_timer kp_tick_timer;
static int timer_period,tick_submits;
typedef _Atomic int atomic_t;
#define atomic_get(p) atomic_load(p)
#define atomic_set(p,v) atomic_store(p,v)
struct device { const void *api; const char *name; void *data; const void *config; };
typedef void (*effect_visitor)(const struct device *dev, int64_t now_ms);
struct led_rgb { uint8_t r,g,b; };
struct kp_rgb_coord { uint16_t x,y; };
struct kp_rgb_tuning { int sentinel; };
struct kp_rgb_frame {
 const struct kp_rgb_tuning *tune; size_t count;
 const struct kp_rgb_coord *coords; struct led_rgb *pixels;
 int64_t now_ms; uint32_t elapsed_ms; uint16_t board_length,board_height; bool is_idle;
};
struct zmk_position_state_changed { uint32_t position; bool state; int64_t timestamp; };
struct kp_rgb_key_event { uint32_t position; bool pressed; int64_t timestamp_ms; };
struct kp_rgb_effect_runtime { int64_t last_render_ms; bool initialized,active,wanted,animating,reset_pending; };
struct kp_rgb_effect_callbacks {
 bool (*render)(const struct device *,const struct kp_rgb_frame *);
 bool (*on_event)(const struct device *,const struct kp_rgb_key_event *);
 void (*reset)(const struct device *,int64_t);
 void (*set_active)(const struct device *,bool,int64_t);
};
struct kp_rgb_effect_api {
 const struct kp_rgb_effect_callbacks *callbacks;
 struct kp_rgb_effect_runtime *runtime;
 const struct device *const *overlays; size_t overlays_len;
};
struct kp_rgb_overlay_api { void (*render)(const struct device *,struct kp_rgb_frame *); const struct device *(*event_target)(const struct device *); };
struct kp_rgb_controller {
 const struct device *dev;
 struct { bool user_on; const struct device *active_fx; } state;
 struct kp_rgb_tuning tuning;
};
static struct kp_rgb_controller kp_rgb_controller;
#define ctx kp_rgb_controller
static pthread_mutex_t lock;
static _Thread_local unsigned int held;
static void kp_rgb_matrix_lock(void) { pthread_mutex_lock(&lock); held++; }
static void kp_rgb_matrix_unlock(void) { assert(held); held--; pthread_mutex_unlock(&lock); }
static unsigned host_lock_failures;
static bool kp_rgb_matrix_lock_patiently(void) {
 if (host_lock_failures) { host_lock_failures--; return false; }
 kp_rgb_matrix_lock(); return true;
}
static int host_try_lock_error;
#define KP_TRY_LOCK() (host_try_lock_error ? host_try_lock_error : (kp_rgb_matrix_lock(),0))
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static struct k_spinlock kp_rgb_pending_lock;
static bool inject_pending_press,pending_press_accepted;
static bool kp_rgb_pending_push(const struct kp_rgb_key_event *ev);
static int k_spin_lock(struct k_spinlock *l) {
 (void)l;
 if(inject_pending_press) {
  inject_pending_press=false;
  const struct kp_rgb_key_event ev={.position=0,.pressed=true};
  pending_press_accepted=kp_rgb_pending_push(&ev);
 }
 pthread_mutex_lock(&pending_lock);return 0;
}
static bool inject_after_unlock,post_release_accepted;
static void k_spin_unlock(struct k_spinlock *l,int key) {
 (void)l; (void)key; pthread_mutex_unlock(&pending_lock);
 if(inject_after_unlock) {
  inject_after_unlock=false;
  const struct kp_rgb_key_event ev={.position=0,.pressed=true};
  post_release_accepted=kp_rgb_pending_push(&ev);
 }
}
static struct kp_rgb_key_event kp_rgb_pending[KP_RGB_EVENT_QUEUE_LEN];
static uint8_t kp_rgb_pending_head,kp_rgb_pending_tail;
static uint32_t kp_rgb_pending_dropped;
static atomic_t kp_rgb_output_allowed,kp_rgb_inhibited;
static bool kp_rgb_output_ready,kp_rgb_black_pending;
static uint32_t kp_rgb_black_retry_ms=100;
static uint64_t now;
static struct led_rgb pixels[KP_LED_COUNT];
static struct kp_rgb_coord kp_led_coords[KP_LED_COUNT];
static uint16_t kp_rgb_board_length=100,kp_rgb_board_height=100;
static const struct device dummy = {.name="strip"};
static const struct device *strip=&dummy;
static bool isr,idle;
static bool inhibit_from_render;
int zmk_rgb_matrix_set_inhibited(bool inhibited);
static int polls,refreshes,dispatches,renders,feedback,writes,colored,failures,retry_delay;
static atomic_t transfer_entered,transfer_release,block_transfer;
#if !HOST_SCHEDULER
static atomic_t setter_entered,setter_done;
#endif
static uint32_t rendered_elapsed;
static int64_t k_uptime_get(void) { return now; }
static bool k_is_in_isr(void) { return isr; }
#define ZMK_ACTIVITY_ACTIVE 0
static int zmk_activity_get_state(void) { return idle; }
typedef struct { bool activity; const struct zmk_position_state_changed *position; } zmk_event_t;
#define ZMK_EV_EVENT_BUBBLE 0
static const struct zmk_position_state_changed *as_zmk_position_state_changed(const zmk_event_t *e) { return e->position; }
#if CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE
static const zmk_event_t *as_zmk_activity_state_changed(const zmk_event_t *e) { return e->activity ? e : NULL; }
#endif
static void *zmk_workqueue_lowprio_work_q(void) { return NULL; }
#if HOST_SCHEDULER
#include "engine_scheduler.h"
#else
static int k_work_submit_to_queue(void *q,struct k_work *w) { (void)q;if(w==&kp_tick_work) tick_submits++;return 0; }
static int k_work_reschedule_for_queue(void *q,struct k_work *w,int delay) { (void)q;(void)w; retry_delay=delay;return 0; }
static void k_timer_stop(struct k_timer *t) { (void)t;timer_period=0; }
static void k_timer_start(struct k_timer *t,int delay,int period) { (void)t;(void)delay;timer_period=period; }
#endif
static bool host_strip_ready = true;
static bool device_is_ready(const struct device *d) { return d!=NULL && host_strip_ready; }
void kp_rgb_triggers_poll(void) { assert(!held); polls++; }
bool kp_rgb_overlay_refresh(void) { assert(!held); refreshes++; return true; }
void kp_rgb_overlay_dispatch(void) { assert(!held); dispatches++; }
struct host_overlay { const struct device *child; bool gate,cover; };
static const struct device *host_overlays[8];
static size_t host_overlay_count;
static const struct device *const *kp_rgb_overlay_list(void) { return host_overlays; }
static size_t kp_rgb_overlay_count(void) { return host_overlay_count; }
static bool kp_rgb_overlay_covers_all(const struct device *d) { return ((struct host_overlay *)d->data)->cover; }
static bool kp_rgb_overlay_gate(const struct device *d) { return ((struct host_overlay *)d->data)->gate; }
static void kp_rgb_deliver_position(const struct kp_rgb_key_event *e);
static bool kp_rgb_pending_pop(struct kp_rgb_key_event *e);
static bool kp_rgb_pending_available(void);
static uint32_t kp_rgb_pending_take_dropped(void);
static size_t kp_rgb_led_for_position(uint32_t p) { return p < KP_LED_COUNT ? p : SIZE_MAX; }
static void (*host_render_hook)(void);
struct host_transfer {
 uint64_t at_ms; int result; struct led_rgb pixels[KP_LED_COUNT];
};
static struct host_transfer host_transfers[1024];
static size_t host_transfer_count;
static bool host_mutate_output = true;
static bool host_allow_color_failure;
static bool render(const struct device *d,const struct kp_rgb_frame *f) {
 (void)d; assert(held); renders++;rendered_elapsed=f->elapsed_ms;
 for(size_t n=0;n<f->count;n++) f->pixels[n].r=17;
 if(host_render_hook) host_render_hook();
 if(inhibit_from_render) {
  inhibit_from_render=false;
  assert(zmk_rgb_matrix_set_inhibited(true)==0);
 }
 return true;
}
static bool on_event(const struct device *d,const struct kp_rgb_key_event *e) { (void)d;(void)e;assert(held);feedback++;return true; }
static struct kp_rgb_effect_runtime runtime;
static const struct kp_rgb_effect_callbacks callbacks={.render=render,.on_event=on_event};
static const struct kp_rgb_effect_api api={.callbacks=&callbacks,.runtime=&runtime};
static const struct device fx={.api=&api};
static const struct device *host_effects[8]={&fx};
static size_t host_effect_count=1;
static size_t kp_rgb_effect_count(void) { return host_effect_count; }
static const struct device *kp_rgb_effect_at(size_t n) { return host_effects[n]; }
static int led_strip_update_rgb(const struct device *d,struct led_rgb *p,size_t n) {
 (void)d; assert(held);assert(n>0);writes++;
 bool color=false;
 for(size_t i=0;i<n;i++) color |= p[i].r || p[i].g || p[i].b;
 if(color) colored++;
 if(atomic_get(&block_transfer)) {
  atomic_set(&transfer_entered,1);
  while(!atomic_get(&transfer_release)) sched_yield();
 }
 assert(host_transfer_count < sizeof(host_transfers)/sizeof(host_transfers[0]));
 struct host_transfer *record = &host_transfers[host_transfer_count++];
 record->at_ms = now;
 memcpy(record->pixels,p,n*sizeof(*p));
 record->result = failures ? -EIO : 0;
 if(host_mutate_output) memset(p,0xa5,n*sizeof(*p));
 if(failures) { assert(!color || host_allow_color_failure); failures--; }
 return record->result;
}
static void kp_rgb_request_black_locked(void);
void kp_rgb_reconcile_power_locked(void);
void zmk_rgb_matrix_flush(void);
'''

ACTIVITY_FUNCTION = r'''
static void host_activity(bool active) {
    idle = !active;
    const zmk_event_t event = {.activity = true};
    assert(kp_rgb_matrix_event_listener(&event) ==
           (CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE ? ZMK_EV_EVENT_BUBBLE : -ENOTSUP));
}
'''

FUNCTIONS = [
    "kp_rgb_has_leds", "kp_rgb_logical_on_locked",
    "kp_rgb_each_effect", "kp_rgb_restart_clock", "kp_rgb_clear_wanted", "kp_rgb_mark_reset",
    "kp_rgb_request_runtime_reset_locked", "kp_rgb_reconcile_effect", "kp_rgb_effect_render",
    "kp_rgb_matrix_tick_handler", "kp_start_timer",
    "kp_last_covering_overlay", "kp_render_overlays", "kp_rgb_matrix_tick",
    "kp_rgb_request_black_locked", "kp_rgb_matrix_off_handler",
    "kp_rgb_reconcile_power_locked",
    "zmk_rgb_matrix_set_inhibited", "zmk_rgb_matrix_is_inhibited",
    "zmk_rgb_matrix_flush", "kp_rgb_pending_push",
    "kp_rgb_pending_pop", "kp_rgb_pending_take_dropped", "kp_rgb_pending_available",
    "kp_rgb_deliver_event", "kp_rgb_deliver_position",
    "kp_rgb_matrix_event_listener", "kp_rgb_set_user_on", "zmk_rgb_matrix_on", "zmk_rgb_matrix_off",
    "zmk_rgb_matrix_toggle", "zmk_rgb_matrix_get_state", "kp_rgb_matrix_init",
]


def run_tests(tests, *, scheduler=False, cases=(None,), auto_off_idle=True):
    """Build fresh fixtures for LED and zero-LED configurations; propagate failures."""
    with tempfile.TemporaryDirectory() as directory:
        for leds in (2, 0):
            source = pathlib.Path(directory) / "engine.c"
            source.write_text(
                f"#define KP_LED_COUNT {leds}\n#define HOST_SCHEDULER {int(scheduler)}\n"
                f"#define CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE {int(auto_off_idle)}\n"
                + PRELUDE + "\n".join(map(function, FUNCTIONS))
                + TOGGLE_FUNCTION + ACTIVITY_FUNCTION + tests
            )
            binary = pathlib.Path(directory) / "engine"
            subprocess.run(
                shlex.split(os.environ.get("CC", "cc"))
                + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-type-limits",
                   "-pthread", "-I", str(ROOT / "tests/unit"), str(source), "-o", str(binary)],
                check=True,
            )
            for case in cases:
                argv = [str(binary)] + ([] if case is None else [case])
                subprocess.run(argv, check=True, timeout=10)
