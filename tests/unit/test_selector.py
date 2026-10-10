#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Compile the production selector services with host-only device mocks."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "src/rgb_selector_internal.h").read_text()
SOURCE = (ROOT / "src/rgb_selector.c").read_text()
EFFECT_HEADER = (ROOT / "include/zmk/rgb_matrix.h").read_text()


def structure(source, name):
    match = re.search(r"struct " + name + r" \{.*?^\};", source, re.M | re.S)
    assert match, name
    return match.group()


def function(name, source=SOURCE):
    match = re.search(r"^(?:static )?(?:inline )?[\w\s*]+\b" + name + r"\([^;]*?\) \{", source, re.M)
    assert match, name
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MIN_MS 100
#define CONFIG_KEYPAW_RGB_MATRIX_DURATION_MAX_MS 10000
#define KP_RGB_BRT_MAX 100
struct device { const char *name; const void *config; void *data; };
struct kp_rgb_hsb { uint16_t h; uint8_t s, b; };
'''

TESTS = r'''
int main(void) {
    const struct device owner_a={.name="owner-a"}, owner_b={.name="owner-b"};
    struct kp_rgb_effect_common_data data[5] = {0};
    struct kp_rgb_effect_common_config configs[] = {
        {.owner=&owner_a, .slot=0}, {.owner=&owner_a, .slot=2},
        {.owner=&owner_b, .slot=0}, {.owner=&owner_b, .slot=1}, {.slot=0}
    };
    const struct device fx[] = {
        {"a0",&configs[0],&data[0]}, {"a1",&configs[1],&data[1]},
        {"b0",&configs[2],&data[2]}, {"b1",&configs[3],&data[3]},
        {"private",&configs[4],&data[4]}
    };
    const struct device *effects_a[] = {&fx[0], NULL, &fx[1]};
    const struct device *effects_b[] = {&fx[2], &fx[3]};
    const struct kp_rgb_effect_defaults defaults_a[] = {
        {{10,20,30},0}, {{0},0}, {{40,50,60},700}
    };
    const struct kp_rgb_effect_defaults defaults_b[] = {
        {{70,80,90},15000}, {{100,10,20},0}
    };
    struct kp_rgb_selector a = {
        .owner=&owner_a, .effects=effects_a, .effect_defaults=defaults_a,
        .effect_count=3, .initial_duration_ms=1, .initial_effect=1
    };
    struct kp_rgb_selector b = {
        .owner=&owner_b, .effects=effects_b, .effect_defaults=defaults_b,
        .effect_count=2, .initial_duration_ms=250, .initial_effect=0
    };

    assert(kp_rgb_selector_count(&a)==3 && kp_rgb_selector_count(&b)==2);
    assert(kp_rgb_selector_at(&a,0)==&fx[0] && kp_rgb_selector_at(&a,1)==NULL);
    assert(kp_rgb_selector_at(&a,3)==NULL);
    assert(kp_rgb_selector_calc_index(&a,0,1)==2); /* skip disabled */
    assert(kp_rgb_selector_calc_index(&a,2,1)==0); /* wrap */
    assert(kp_rgb_selector_calc_index(&a,0,-1)==2);
    assert(kp_rgb_selector_calc_index(&a,1,0)==2); /* disabled current resolves forward */
    assert(kp_rgb_selector_calc_index(&b,1,0)==1);
    assert(kp_rgb_selector_calc_index(&a,2,3)==2);
    assert(kp_rgb_selector_calc_index(&a,2,-3)==2);
    assert(kp_rgb_selector_calc_index(&a,0,INT16_MAX)==2);
    assert(kp_rgb_selector_calc_index(&a,2,INT16_MIN)==0);

    assert(kp_rgb_selector_member_slot(&a,&fx[0])==0);
    assert(kp_rgb_selector_member_slot(&a,&fx[1])==2);
    assert(kp_rgb_selector_member_slot(&a,&fx[2])==-ENOENT); /* another owner, colliding slot */
    assert(kp_rgb_selector_member_slot(&a,&fx[4])==-ENOENT); /* private */
    assert(kp_rgb_selector_member_slot(&a,NULL)==-ENOENT);
    assert(kp_rgb_selector_member_slot(&b,&fx[2])==0);
    assert(kp_rgb_selector_member_slot(&b,&fx[0])==-ENOENT);
    configs[0].owner=&owner_b;
    assert(kp_rgb_selector_member_slot(&a,&fx[0])==-ENOENT); /* mismatched metadata */
    configs[0].owner=&owner_a;
    configs[0].slot=2;
    assert(kp_rgb_selector_member_slot(&a,&fx[0])==-ENOENT); /* slot must point back */
    configs[0].slot=1;
    assert(kp_rgb_selector_member_slot(&a,&fx[0])==-ENOENT);
    configs[0].slot=3;
    assert(kp_rgb_selector_member_slot(&a,&fx[0])==-ENOENT);
    configs[0].slot=0;
    struct kp_rgb_selector unowned=a;
    unowned.owner=NULL;
    assert(kp_rgb_selector_member_slot(&unowned,&fx[0])==-ENOENT);
    const struct device no_config={.name="no-config"};
    assert(kp_rgb_selector_member_slot(&a,&no_config)==-ENOENT);

    assert(kp_rgb_selector_apply_defaults_locked(&a)==0);
    assert(a.effect_index==2 && a.active_fx==&fx[1]); /* initial disabled slot fallback */
    assert(data[0].color.h==10 && data[0].color.b==KP_RGB_BRT_MAX);
    assert(data[0].duration_ms==100); /* initial duration is clamped */
    assert(data[1].color.h==40 && data[1].duration_ms==700);
    data[0].color.h=11;
    data[0].duration_ms=121;
    assert(kp_rgb_selector_apply_defaults_locked(&b)==0);
    assert(b.active_fx==&fx[2] && b.effect_index==0);
    assert(data[2].duration_ms==10000); /* per-effect duration clamps */
    assert(data[3].duration_ms==250);
    assert(a.effect_index==2 && a.active_fx==&fx[1]);
    assert(data[0].color.h==11 && data[0].duration_ms==121);
    assert(data[1].color.h==40 && data[1].duration_ms==700);

    const struct device *selected_a=a.active_fx;
    assert(kp_rgb_selector_select_locked(&a,2)==0);
    assert(a.effect_index==2 && a.active_fx==selected_a);
    assert(kp_rgb_selector_select_locked(&a,1)==-ENOENT);
    assert(a.effect_index==2 && a.active_fx==selected_a);
    assert(kp_rgb_selector_select_locked(&a,3)==-EINVAL);
    assert(a.effect_index==2 && a.active_fx==selected_a);
    assert(kp_rgb_selector_select_locked(&a,0)==1 && a.active_fx==&fx[0]);
    assert(b.active_fx==&fx[2]); /* independent selection */
    assert(kp_rgb_selector_select_locked(&b,1)==1 && b.active_fx==&fx[3]);
    assert(a.active_fx==&fx[0]);
    assert(kp_rgb_selector_select_locked(&b,1)==0);

    struct kp_rgb_selector empty={0};
    assert(kp_rgb_selector_resolve_locked(&empty)==-ENOENT);
    assert(kp_rgb_selector_count(&empty)==0);
    assert(kp_rgb_selector_calc_index(&empty,42,INT16_MIN)==42);
    assert(kp_rgb_selector_select_locked(&empty,0)==-EINVAL);
    assert(kp_rgb_selector_apply_defaults_locked(&empty)==-ENOENT);
    assert(empty.effect_index==0 && empty.active_fx==NULL);

    const struct device *disabled_effects[]={NULL,NULL,NULL};
    struct kp_rgb_selector disabled={
        .owner=&owner_a, .effects=disabled_effects, .effect_defaults=defaults_a,
        .effect_count=3, .initial_effect=1, .initial_duration_ms=700
    };
    assert(kp_rgb_selector_calc_index(&disabled,1,1)==1);
    assert(kp_rgb_selector_calc_index(&disabled,1,-1)==1);
    assert(kp_rgb_selector_apply_defaults_locked(&disabled)==-ENOENT);
    assert(disabled.effect_index==1 && disabled.active_fx==NULL);
    assert(kp_rgb_selector_select_locked(&disabled,0)==-ENOENT);
    assert(kp_rgb_selector_select_locked(&disabled,3)==-EINVAL);
    assert(disabled.effect_index==1 && disabled.active_fx==NULL);
    assert(kp_rgb_selector_member_slot(&disabled,&fx[0])==-ENOENT);
    disabled.effect_index=99;
    assert(kp_rgb_selector_resolve_locked(&disabled)==-ENOENT);
    assert(disabled.effect_index==0 && disabled.active_fx==NULL);
    return 0;
}
'''


def main():
    structs = "\n".join(structure(EFFECT_HEADER, name) for name in
                       ("kp_rgb_effect_common_config", "kp_rgb_effect_common_data"))
    structs += "\n" + "\n".join(structure(HEADER, name) for name in
                               ("kp_rgb_effect_defaults", "kp_rgb_selector"))
    accessors = "\n".join(function(name, EFFECT_HEADER) for name in
                          ("kp_rgb_effect_data", "kp_rgb_effect_cfg"))
    # Notification delivery is exercised by test_selector_public_api.py.
    accessors += "\nstatic void kp_rgb_selector_publish_locked(struct kp_rgb_selector *s, const struct device *p) { (void)s; (void)p; }\n"
    services = "\n".join(function(name) for name in (
        "kp_rgb_selector_count", "kp_rgb_selector_at", "kp_rgb_selector_calc_index",
        "kp_rgb_selector_resolve_locked", "kp_rgb_selector_apply_defaults_locked",
        "kp_rgb_selector_select_locked", "kp_rgb_selector_member_slot"))
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "selector.c"
        binary = Path(tmp) / "selector"
        source.write_text(PRELUDE + structs + accessors + services + TESTS)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("selector services: passed")


if __name__ == "__main__":
    main()
