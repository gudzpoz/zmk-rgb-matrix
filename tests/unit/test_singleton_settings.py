#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Exercise production persistence against an in-memory settings backend."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from test_singleton import ROOT, fixture_code

MOCKS = r'''
#include <string.h>
#define ARG_UNUSED(x) (void)(x)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define BUILD_ASSERT(x,...) _Static_assert(x, #x)
#undef CONFIG_SETTINGS
#define CONFIG_SETTINGS 1
#define CONFIG_KEYPAW_RGB_MATRIX_AUTO_OFF_IDLE 1
#define CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE 100
#define DT_HAS_COMPAT_STATUS_OKAY(x) 1
#define LOG_MODULE_DECLARE(...)
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define SYS_INIT(...)
#define K_MSEC(x) (x)
#define ZMK_ACTIVITY_ACTIVE 0
static unsigned resets;
static void kp_rgb_request_runtime_reset_locked(void) { assert(held); resets++; zmk_rgb_matrix_flush(); }
struct k_work { void (*handler)(struct k_work *); };
struct k_work_delayable { struct k_work work; };
struct k_work_sync { int unused; };
#define K_WORK_DEFINE(name,fn) static struct k_work name = {fn}
#define K_WORK_DELAYABLE_DEFINE(name,fn) static struct k_work_delayable name = {{fn}}
static struct k_work_delayable *pending;
static int k_work_reschedule(struct k_work_delayable *w, int delay) { assert(delay == 100); pending = w; return 1; }
static int k_work_cancel_delayable_sync(struct k_work_delayable *w, struct k_work_sync *sync) {
    (void)sync; assert(!held); if (pending == w) pending = NULL; return 0;
}
static void *zmk_workqueue_lowprio_work_q(void) { return NULL; }
static int k_work_submit_to_queue(void *queue, struct k_work *w) { (void)queue; w->handler(w); return 1; }
typedef int (*settings_read_cb)(void *, void *, size_t);
#define SETTINGS_STATIC_HANDLER_DEFINE(name,path,get,set,commit,export) \
    static const char *registered_subtree = path; \
    static int (*registered_set)(const char *,size_t,settings_read_cb,void *) = set
static char saved_path[128], deleted_path[128];
static unsigned char saved_blob[512];
static size_t saved_size;
static unsigned saves;
static int settings_save_one(const char *path, const void *value, size_t size) {
    assert(!held && size <= sizeof(saved_blob));
    saves++;
    strcpy(saved_path,path); memcpy(saved_blob,value,size); saved_size=size; return 0;
}
static int settings_delete(const char *path) { assert(!held); strcpy(deleted_path,path); return 0; }
static unsigned reads;
static int read_blob(void *arg, void *out, size_t len) {
    reads++; memcpy(out,arg,len); return (int)len;
}
static int short_read(void *arg, void *out, size_t len) {
    memcpy(out,arg,len-1); return (int)len-1;
}
static int failed_read(void *arg, void *out, size_t len) {
    (void)arg; (void)out; (void)len; return -EIO;
}
'''

TESTS = r'''
int main(void) {
    struct kp_rgb_effect_common_data data[2] = {0};
    const struct device devices[] = {{&data[0], "a"}, {&data[1], "b"}};
    const struct device *effects[] = {&devices[0], &devices[1]};
    const struct kp_rgb_effect_defaults defaults[] = {{{10,20,30},500},{{40,50,60},600}};
    kp_rgb_controller.effects = effects;
    kp_rgb_controller.effect_count = 2;
    kp_rgb_controller.effect_defaults = defaults;
    kp_rgb_controller.initial_on = true;
    kp_rgb_controller.initial_brightness = 70;
    kp_rgb_controller.initial_duration_ms = 1000;
    assert(kp_rgb_apply_defaults() == 0);
    assert(lock_entries == 1 && reconciles == 1 && reconciled_user_on && !held);
    assert(flushes == 1 && locked_flushes == 1);
    assert(!pending && !saves);
    assert(!strcmp(registered_subtree, "keypaw/rgb_matrix"));
    struct kp_rgb_persist_blob blob;
    const struct kp_rgb_persist_effect stored[] = {{800,100,90,80},{900,200,70,60}};
    kp_rgb_persist_pack(1, true, stored, 2, &blob);
    assert(registered_set("kprgb/state",sizeof(blob),read_blob,&blob) == -ENOENT);
    assert(registered_set("state/extra",sizeof(blob),read_blob,&blob) == -ENOENT);
    assert(reads == 0);
    unsigned locks_before = lock_entries, reconciles_before = reconciles;
    unsigned flushes_before=flushes, locked_before=locked_flushes;
    assert(registered_set("state",sizeof(blob),read_blob,&blob) == 0);
    assert(flushes==flushes_before+1 && locked_flushes==locked_before+1);
    assert(lock_entries == locks_before + 1 && reconciles == reconciles_before + 1);
    assert(reconciled_user_on && kp_rgb_controller.state.user_on && !held);
    assert(!pending && !saves);
    assert(kp_rgb_selected_effect() == 1 && data[0].color.h == 100 && data[1].color.h == 200);
    /* Unchanged selection and permission must still repaint restored parameters. */
    flushes_before=flushes;locked_before=locked_flushes;
    assert(registered_set("state",sizeof(blob),read_blob,&blob) == 0);
    assert(flushes==flushes_before+1 && locked_flushes==locked_before+1);
    assert(!pending && !saves && !held);
    blob.selected_index=99;blob.effects[1].h=201;
    flushes_before=flushes;locked_before=locked_flushes;
    assert(registered_set("state",sizeof(blob),read_blob,&blob) == 0);
    assert(flushes==flushes_before+1 && locked_flushes==locked_before+1);
    assert(kp_rgb_selected_effect()==1 && data[1].color.h==201 && !held);
    effects[1]=NULL;
    blob.selected_index=1;blob.effects[0].h=101;
    flushes_before=flushes;locked_before=locked_flushes;
    assert(registered_set("state",sizeof(blob),read_blob,&blob) == 0);
    assert(flushes==flushes_before+1 && locked_flushes==locked_before+1);
    assert(kp_rgb_selected_effect()==0 && data[0].color.h==101 && !held);
    effects[1]=&devices[1];blob.effects[0].h=100;blob.effects[1].h=200;
    blob.user_on = false;
    assert(registered_set("state",sizeof(blob),read_blob,&blob) == 0);
    assert(!reconciled_user_on && !kp_rgb_controller.state.user_on);
    blob.user_on = true;
    assert(registered_set("state",sizeof(blob),read_blob,&blob) == 0);
    reconciles_before = reconciles;
    assert(registered_set("state",sizeof(blob)-1,read_blob,&blob) == -EINVAL);
    assert(registered_set("state",sizeof(blob),short_read,&blob) == -EINVAL);
    assert(registered_set("state",sizeof(blob),failed_read,&blob) == -EIO);
    blob.version++;
    assert(registered_set("state",sizeof(blob),read_blob,&blob) == -EINVAL);
    blob.version--;
    assert(reconciles == reconciles_before && !pending && !saves);
    assert(kp_rgb_save_state() == 0 && pending);
    pending->work.handler(&pending->work);
    pending = NULL;
    assert(!strcmp(saved_path,"keypaw/rgb_matrix/state"));
    assert(saved_size == sizeof(blob));
    struct kp_rgb_persist_blob saved;
    memcpy(&saved,saved_blob,sizeof(saved));
    assert(saved.selected_index == 1 && saved.effects[0].h == 100);
    assert(kp_rgb_save_state() == 0 && pending);
    locks_before = lock_entries; reconciles_before = reconciles;
    unsigned saves_before = saves;
    flushes_before=flushes;locked_before=locked_flushes;
    kp_rgb_reset_state();
    /* Defaults repaint even when power reconciliation itself changes nothing. */
    assert(flushes==flushes_before+2 && locked_flushes==locked_before+2);
    assert(lock_entries == locks_before + 1 && reconciles == reconciles_before + 1);
    assert(saves == saves_before && !held);
    assert(!pending && !strcmp(deleted_path,"keypaw/rgb_matrix/state"));
    assert(kp_rgb_selected_effect() == 0 && data[0].color.h == 10);
    assert(reconciled_user_on && kp_rgb_controller.state.user_on && flushes && resets == 1);
    puts("singleton settings namespace/load/save/reset passed");
    return 0;
}
'''


def without_includes(path):
    return re.sub(r"^#(?:include|pragma).*\n", "", path.read_text(), flags=re.M)


def main():
    code = fixture_code() + MOCKS
    code += without_includes(ROOT / "include/zmk/rgb_persist.h")
    code += without_includes(ROOT / "src/rgb_persist.c")
    code += without_includes(ROOT / "src/rgb_settings.c") + TESTS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "settings.c"
        path.write_text(code)
        binary = Path(directory) / "settings"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
