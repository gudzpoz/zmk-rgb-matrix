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
static int convert_toggle(struct kp_rgb_controller *ctx, struct binding *binding) {
    (void)ctx;
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
struct k_work {
 void (*handler)(struct k_work *); bool queued,running,scheduled;
 uint64_t due,order,deadline,schedule_order; unsigned runs;
};
static struct k_work kp_off_work, kp_tick_work, kp_pending_work;
struct k_timer {
 void (*handler)(struct k_timer *); bool active; uint64_t due; uint32_t period;
};
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
struct kp_rgb_controller {
 const struct device *dev;
 struct { bool on,user_on; const struct device *active_fx; } state;
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
static bool kp_rgb_output_ready,kp_rgb_black_pending;
static uint32_t kp_rgb_black_retry_ms=100,last_tick;
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
static uint32_t k_uptime_get_32(void) { return now; }
static bool k_is_in_isr(void) { return isr; }
#define ZMK_ACTIVITY_ACTIVE 0
static int zmk_activity_get_state(void) { return idle; }
static bool kp_rgb_effective_on(bool on,bool apply) { return on && (!apply || !idle); }
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
static const struct device *const *kp_rgb_overlay_list(void) { return NULL; }
static size_t kp_rgb_overlay_count(void) { return 0; }
static bool kp_rgb_overlay_covers_all(const struct device *d) { (void)d;return false; }
static bool kp_rgb_overlay_gate(const struct device *d) { (void)d;return true; }
static void kp_rgb_deliver_position(const struct zmk_position_state_changed *e) { (void)e; assert(held);feedback++; }
static void (*host_render_hook)(void);
struct host_transfer {
 uint64_t at_ms; int result; struct led_rgb pixels[KP_LED_COUNT];
};
static struct host_transfer host_transfers[1024];
static size_t host_transfer_count;
static bool host_mutate_output = true;
static bool host_allow_color_failure;
static void render(const struct device *d,struct kp_rgb_frame *f) {
 (void)d; assert(held); renders++;rendered_elapsed=f->elapsed;
 for(size_t n=0;n<f->count;n++) f->pixels[n].r=17;
 if(host_render_hook) host_render_hook();
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
    "zmk_rgb_matrix_toggle", "zmk_rgb_matrix_get_state", "kp_rgb_matrix_init",
]


def run_tests(tests, *, scheduler=False, cases=(None,)):
    """Build fresh fixtures for LED and zero-LED configurations; propagate failures."""
    with tempfile.TemporaryDirectory() as directory:
        for leds in (2, 0):
            source = pathlib.Path(directory) / "engine.c"
            source.write_text(
                f"#define KP_LED_COUNT {leds}\n#define HOST_SCHEDULER {int(scheduler)}\n"
                + PRELUDE + "\n".join(map(function, FUNCTIONS))
                + TOGGLE_FUNCTION + tests
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
