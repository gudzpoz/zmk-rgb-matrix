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
#define SETTINGS_MAX_VAL_LEN 256
#define SETTINGS_MAX_NAME_LEN 64
#define snprintk snprintf
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
static unsigned char saved_blob[512], saved_global[512];
static size_t saved_size, saved_global_size;
static unsigned saves, deletes;
static int settings_save_one(const char *path, const void *value, size_t size) {
    assert(!held && size <= sizeof(saved_blob));
    saves++;
    if (!strcmp(path, "keypaw/rgb_matrix/state")) {
        memcpy(saved_global, value, size); saved_global_size = size;
    }
    strcpy(saved_path,path); memcpy(saved_blob,value,size); saved_size=size; return 0;
}
static int settings_delete(const char *path) {
    assert(!held); deletes++; strcpy(deleted_path,path); return 0;
}
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
    static const struct kp_rgb_effect_common_config cfg[2] = {
        {.index = 0, .persist_id = "a", .persist_parameters = true},
        {.index = 1, .persist_id = "b", .persist_parameters = true},
    };
    const struct device devices[] = {{&data[0], "a", &cfg[0]}, {&data[1], "b", &cfg[1]}};
    const struct device *effects[] = {&devices[0], &devices[1]};
    const struct kp_rgb_effect_defaults defaults[] = {{{10,20,30},500},{{40,50,60},600}};
    kp_rgb_controller.effects = effects;
    kp_rgb_controller.effect_count = 2;
    kp_rgb_controller.effect_defaults = defaults;
    kp_rgb_controller.initial_on = true;
    kp_rgb_controller.initial_brightness = 70;
    kp_rgb_controller.initial_duration_ms = 1000;
    kp_rgb_controller.initial_effect = 0;
    assert(kp_rgb_apply_defaults() == 0);
    assert(lock_entries == 1 && reconciles == 1 && reconciled_user_on && !held);
    assert(kp_rgb_controller.brightness == 70 && kp_rgb_controller.state.user_on);
    assert(kp_rgb_selected_effect() == 0);
    assert(!pending && !saves && !strcmp(registered_subtree, "keypaw/rgb_matrix"));

    /* Unknown subtree keys are ignored without touching the backend. */
    struct kp_rgb_persist_blob blob;
    kp_rgb_persist_pack(true, 88, "b", &blob);
    assert(registered_set("kprgb/state", sizeof(blob), read_blob, &blob) == -ENOENT);
    assert(registered_set("extra", sizeof(blob), read_blob, &blob) == -ENOENT);
    assert(reads == 0);

    /* A global record restores brightness, user intent and selection by id. */
    assert(registered_set("state", sizeof(blob), read_blob, &blob) == 0);
    assert(kp_rgb_controller.brightness == 88 && kp_rgb_controller.state.user_on);
    assert(kp_rgb_selected_effect() == 1 && !held);
    assert(reconciled_user_on && reconciles == 2 && lock_entries == 2);
    assert(flushes == locked_flushes && flushes == 3);

    /* A selection id that matches no effect leaves the current one in place. */
    struct kp_rgb_persist_blob unknown;
    kp_rgb_persist_pack(true, 55, "missing", &unknown);
    unsigned locks_before = lock_entries, flushes_before = flushes;
    assert(registered_set("state", sizeof(unknown), read_blob, &unknown) == 0);
    assert(kp_rgb_selected_effect() == 1 && kp_rgb_controller.brightness == 55);
    assert(lock_entries == locks_before + 1 && flushes == flushes_before + 1);

    /* Per-effect records land on the effect with the matching persist id. */
    struct kp_rgb_persist_effect record = {.duration_ms = 800, .h = 100, .s = 90};
    assert(registered_set("state/effects/a", sizeof(record), read_blob, &record) == 0);
    assert(data[0].color.h == 100 && data[0].color.s == 90 && data[0].duration_ms == 800);
    assert(data[1].color.h == 40 && kp_rgb_selected_effect() == 1);
    /* An unknown id is dropped without a lock or repaint. */
    locks_before = lock_entries; flushes_before = flushes;
    assert(registered_set("state/effects/zz", sizeof(record), read_blob, &record) == 0);
    assert(lock_entries == locks_before && flushes == flushes_before);
    /* Out-of-range and wrong-size records are rejected. */
    record.h = 400;
    assert(registered_set("state/effects/a", sizeof(record), read_blob, &record) == -EINVAL);
    assert(registered_set("state/effects/a", sizeof(record) - 1, read_blob, &record) == -EINVAL);
    locks_before = lock_entries;
    assert(registered_set("state/effects/a", sizeof(record), failed_read, &record) == -EIO);
    assert(lock_entries == locks_before);

    /* Global record size, read and field validation. */
    locks_before = lock_entries;
    assert(registered_set("state", sizeof(blob) - 1, read_blob, &blob) == -ENOENT);
    assert(registered_set("state", sizeof(blob), short_read, &blob) == -EINVAL);
    assert(registered_set("state", sizeof(blob), failed_read, &blob) == -EIO);
    blob.version++;
    assert(registered_set("state", sizeof(blob), read_blob, &blob) == -EINVAL);
    blob.version--;
    blob.has_selected = 2;
    assert(registered_set("state", sizeof(blob), read_blob, &blob) == -EINVAL);
    blob.has_selected = 0;
    blob.brightness = 101;
    assert(registered_set("state", sizeof(blob), read_blob, &blob) == -EINVAL);
    blob.brightness = 88;
    assert(lock_entries == locks_before);

    /* Saving writes the global blob, then one record per persisted effect. */
    assert(kp_rgb_save_state() == 0 && pending);
    pending->work.handler(&pending->work);
    pending = NULL;
    struct kp_rgb_persist_blob expect;
    kp_rgb_persist_pack(true, 55, "b", &expect);
    assert(saved_global_size == sizeof(expect) && !memcmp(saved_global, &expect, sizeof(expect)));
    assert(saves == 3 && !strcmp(saved_path, "keypaw/rgb_matrix/state/effects/b"));

    /* reset_state deletes the global key and every effect path, then restores. */
    unsigned resets_before = resets;
    kp_rgb_reset_state();
    assert(!pending && resets == resets_before + 1);
    assert(deletes == 3 && !strcmp(deleted_path, "keypaw/rgb_matrix/state/effects/b"));
    assert(kp_rgb_selected_effect() == 0 && kp_rgb_controller.brightness == 70);
    assert(kp_rgb_controller.state.user_on && data[0].color.h == 10 && !held);
    puts("singleton settings namespace/load/save/reset passed");
    return 0;
}
'''


def without_includes(path):
    return re.sub(r"^#(?:include|pragma).*\n", "", path.read_text(), flags=re.M)


def main():
    code = fixture_code().replace(
        "struct device { void *data; const char *name; };",
        "struct device { void *data; const char *name; const void *config; };\n"
        "struct kp_rgb_effect_common_config { uint16_t index; const char *persist_id; "
        "bool persist_parameters; };\n"
        "static const struct kp_rgb_effect_common_config *kp_rgb_effect_cfg("
        "const struct device *dev) { return dev->config; }")
    code += MOCKS
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
