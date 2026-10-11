#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Host tests for the production scoped RGB persistence codec and resolvers."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

TEST = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zmk/rgb_persist.h>

static void test_wire_codecs(void) {
    struct kp_rgb_persist_global_state global = {.user_on = true, .brightness = 73};
    struct kp_rgb_persist_global_record gr = {0};
    assert(kp_rgb_persist_encode_global(&global, &gr) == 0);
    const uint8_t global_bytes[] = {3, 1, 73, 0};
    assert(sizeof(gr) == sizeof(global_bytes) && memcmp(&gr, global_bytes, sizeof(gr)) == 0);
    struct kp_rgb_persist_global_state gd = {false, 12};
    assert(kp_rgb_persist_decode_global(global_bytes, sizeof(global_bytes), &gd) == 0);
    assert(gd.user_on && gd.brightness == 73);

    struct kp_rgb_persist_selector_record sr = {0};
    assert(kp_rgb_persist_encode_selector("solid", &sr) == 0);
    assert(sr.version == 1 && !strcmp(sr.selected_id, "solid"));
    assert(sizeof(sr) == 33);
    assert(((const uint8_t *)&sr)[0] == 1);
    assert(!strcmp((const char *)&sr + 1, "solid"));

    struct kp_rgb_persist_effect effect = {.duration_ms = 0x1234, .h = 0x0123, .s = 67, .reserved = 99};
    struct kp_rgb_persist_parameters_record pr = {0};
    assert(kp_rgb_persist_encode_parameters(&effect, &pr) == 0);
    const uint8_t parameter_bytes[] = {1, 0x34, 0x12, 0x23, 0x01, 67};
    assert(sizeof(pr) == sizeof(parameter_bytes) && memcmp(&pr, parameter_bytes, sizeof(pr)) == 0);
    struct kp_rgb_persist_effect pd = {0};
    assert(kp_rgb_persist_decode_parameters(parameter_bytes, sizeof(parameter_bytes), &pd) == 0);
    assert(pd.duration_ms == 0x1234 && pd.h == 0x0123 && pd.s == 67 && pd.reserved == 0);

    effect.h = 360;
    assert(kp_rgb_persist_encode_parameters(&effect, &pr) == 0);
    assert(pr.hue_le[0] == 0 && pr.hue_le[1] == 0);
    const uint8_t hue360[] = {1, 9, 0, 104, 1, 22};
    assert(kp_rgb_persist_decode_parameters(hue360, sizeof(hue360), &pd) == 0 && pd.h == 0);
}

#define REJECT_UNCHANGED(fn, bytes, length, output) do { \
    __typeof__(output) before = (output); \
    assert((fn)((bytes), (length), &(output)) == -EINVAL); \
    assert(memcmp(&(output), &before, sizeof(output)) == 0); \
} while (0)

static void test_invalid_records_and_legacy(void) {
    struct kp_rgb_persist_global_state g = {true, 55};
    uint8_t badg[] = {3, 1, 20, 0};
    REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 3, g);
    badg[0] = 99; REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 4, g);
    badg[0] = 3; badg[1] = 2; REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 4, g);
    badg[1] = 1; badg[2] = 101; REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 4, g);
    badg[2] = 20; badg[3] = 1; REJECT_UNCHANGED(kp_rgb_persist_decode_global, badg, 4, g);
    REJECT_UNCHANGED(kp_rgb_persist_decode_global, NULL, 4, g);

    uint8_t bads[33] = {1}; strcpy((char *)bads + 1, "ok");
    struct kp_rgb_persist_selector_record s = {.version = 77}; strcpy(s.selected_id, "keep");
    REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 32, s);
    bads[0] = 9; REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 33, s);
    bads[0] = 1; strcpy((char *)bads + 1, "bad/id"); REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 33, s);
    memset(bads + 1, 'a', 32); REJECT_UNCHANGED(kp_rgb_persist_decode_selector, bads, 33, s);
    REJECT_UNCHANGED(kp_rgb_persist_decode_selector, NULL, 33, s);

    struct kp_rgb_persist_effect p = {.duration_ms = 123, .h = 45, .s = 67, .reserved = 88};
    uint8_t badp[] = {0, 1, 0, 2, 0, 3};
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, badp, 6, p);
    badp[0] = 1; badp[3] = 105; badp[4] = 1;
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, badp, 6, p);
    badp[3] = 2; badp[4] = 0; badp[5] = 101;
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, badp, 6, p);
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, badp, 5, p);
    REJECT_UNCHANGED(kp_rgb_persist_decode_parameters, NULL, 6, p);

    struct kp_rgb_persist_blob old = {0};
    old.version = KP_RGB_PERSIST_VERSION; old.user_on = 7; old.brightness = 50;
    old.has_selected = 1; strcpy(old.selected_id, "legacy");
    struct kp_rgb_persist_blob decoded = {0};
    assert(kp_rgb_persist_decode_legacy(&old, sizeof(old), &decoded) == 0);
    assert(decoded.user_on == 7 && decoded.has_selected == 1 && !strcmp(decoded.selected_id, "legacy"));
    REJECT_UNCHANGED(kp_rgb_persist_decode_legacy, &old, sizeof(old) - 1, decoded);
    struct kp_rgb_persist_effect old_effect = {.duration_ms = 400, .h = 360, .s = 80, .reserved = 0xa5};
    struct kp_rgb_persist_effect old_out = {0};
    assert(kp_rgb_persist_decode_legacy_effect(&old_effect, sizeof(old_effect), &old_out) == 0);
    assert(old_out.h == 360 && old_out.reserved == 0xa5);
}

static void test_ids(void) {
    char id31[32]; memset(id31, 'x', 31); id31[31] = 0;
    char id32[33]; memset(id32, 'x', 32); id32[32] = 0;
    assert(kp_rgb_persist_id_valid(id31) && !kp_rgb_persist_id_valid(id32));
    assert(!kp_rgb_persist_id_valid(NULL) && !kp_rgb_persist_id_valid(""));
    assert(!kp_rgb_persist_id_valid("a/b") && !kp_rgb_persist_id_valid("a=b"));
    assert(!kp_rgb_persist_id_valid("a b") && !kp_rgb_persist_id_valid("a\t"));
    const char nonascii[] = {(char)0xc3, (char)0xa9, 0};
    assert(!kp_rgb_persist_id_valid(nonascii));

    const struct kp_rgb_persist_effect_id a[] = {{"same", true}, {NULL, true}, {"other", false}};
    const struct kp_rgb_persist_effect_id b[] = {{"same", false}};
    const struct kp_rgb_persist_selector_ids selectors[] = {
        {"selA", a, 3}, {"selB", b, 1}};
    assert(kp_rgb_persist_validate_ids(selectors, 2) == 0); /* member IDs may cross selectors */
    const struct kp_rgb_persist_selector_ids duplicate_selector[] = {{"x", NULL, 0}, {"x", NULL, 0}};
    assert(kp_rgb_persist_validate_ids(duplicate_selector, 2) == -EEXIST);
    const struct kp_rgb_persist_effect_id duplicate_members[] = {{"x", false}, {"x", false}};
    const struct kp_rgb_persist_selector_ids duplicate_member_selector[] = {{"sel", duplicate_members, 2}};
    assert(kp_rgb_persist_validate_ids(duplicate_member_selector, 1) == -EEXIST);
    const struct kp_rgb_persist_effect_id invalid_enabled[] = {{"bad/id", false}};
    const struct kp_rgb_persist_selector_ids invalid_selector[] = {{"sel", invalid_enabled, 1}};
    assert(kp_rgb_persist_validate_ids(invalid_selector, 1) == -EINVAL);
    assert(kp_rgb_persist_validate_ids(NULL, 1) == -EINVAL);

    char selector[32], effect[32]; memset(selector, 's', 13); selector[13] = 0;
    memset(effect, 'e', 14); effect[14] = 0;
    const struct kp_rgb_persist_effect_id budget_ok[] = {{effect, true}};
    const struct kp_rgb_persist_selector_ids budget_selector[] = {{selector, budget_ok, 1}};
    assert(KP_RGB_PERSIST_EFFECT_ID_BUDGET == 27);
    assert(kp_rgb_persist_validate_ids(budget_selector, 1) == 0);
    selector[13] = 's'; selector[14] = 0;
    assert(kp_rgb_persist_validate_ids(budget_selector, 1) == -ENAMETOOLONG);
    char id30[31]; memset(id30, 's', 30); id30[30] = 0;
    const struct kp_rgb_persist_effect_id optout[] = {{id31, false}};
    const struct kp_rgb_persist_selector_ids long_optout[] = {{id30, optout, 1}};
    assert(kp_rgb_persist_validate_ids(long_optout, 1) == 0);
    const struct kp_rgb_persist_selector_ids oversized_selector[] = {{id31, optout, 1}};
    assert(kp_rgb_persist_validate_ids(oversized_selector, 1) == -ENAMETOOLONG);
}

static void test_resolvers(void) {
    const struct kp_rgb_persist_global_state defaults = {false, 11}, scoped_global = {true, 72};
    struct kp_rgb_persist_global_state out_global = {0};
    struct kp_rgb_persist_blob legacy = {0}; legacy.user_on = 1; legacy.brightness = 44;
    assert(!kp_rgb_persist_resolve_global(&defaults, &legacy, &scoped_global, &out_global));
    assert(out_global.user_on && out_global.brightness == 72);
    assert(kp_rgb_persist_resolve_global(&defaults, &legacy, NULL, &out_global));
    assert(out_global.user_on && out_global.brightness == 44);
    assert(!kp_rgb_persist_resolve_global(&defaults, NULL, NULL, &out_global));
    assert(!out_global.user_on && out_global.brightness == 11);

    const struct kp_rgb_persist_effect_id members[] = {{"first", true}, {NULL, false}, {"second", true}};
    struct kp_rgb_persist_selector_record selected = {0}, stored = {0};
    legacy.has_selected = 1; strcpy(legacy.selected_id, "second");
    assert(kp_rgb_persist_resolve_selector("first", &legacy, false, NULL, members, 3, &selected) == false);
    assert(!strcmp(selected.selected_id, "first")); /* alias is explicit */
    assert(kp_rgb_persist_resolve_selector("first", &legacy, true, NULL, members, 3, &selected));
    assert(!strcmp(selected.selected_id, "second"));
    assert(!kp_rgb_persist_encode_selector("first", &stored));
    assert(!kp_rgb_persist_resolve_selector("second", &legacy, true, &stored, members, 3, &selected));
    assert(!strcmp(selected.selected_id, "first")); /* scoped wins regardless of legacy */
    strcpy(stored.selected_id, "gone");
    assert(!kp_rgb_persist_resolve_selector("first", &legacy, true, &stored, members, 3, &selected));
    assert(!strcmp(selected.selected_id, "first")); /* invalid scoped selection doesn't fall back */
    strcpy(legacy.selected_id, "gone");
    assert(!kp_rgb_persist_resolve_selector("first", &legacy, true, NULL, members, 3, &selected));
    assert(!strcmp(selected.selected_id, "first"));
    strcpy(legacy.selected_id, "disabled");
    assert(!kp_rgb_persist_resolve_selector("first", &legacy, true, NULL, members, 3, &selected));
    assert(!strcmp(selected.selected_id, "first"));

    struct kp_rgb_persist_effect defaults_p = {.duration_ms = 100, .h = 360, .s = 30, .reserved = 9};
    struct kp_rgb_persist_effect legacy_p = {.duration_ms = 200, .h = 50, .s = 60, .reserved = 8};
    struct kp_rgb_persist_effect scoped_p = {.duration_ms = 300, .h = 70, .s = 80, .reserved = 7};
    struct kp_rgb_persist_effect out_p;
    assert(!kp_rgb_persist_resolve_parameters(&defaults_p, &legacy_p, NULL, false, true, &out_p));
    assert(out_p.duration_ms == 100 && out_p.h == 0 && out_p.reserved == 0);
    assert(!kp_rgb_persist_resolve_parameters(&defaults_p, &legacy_p, NULL, true, false, &out_p));
    assert(out_p.duration_ms == 100); /* non-opted-in parameter ignores legacy */
    assert(kp_rgb_persist_resolve_parameters(&defaults_p, &legacy_p, NULL, true, true, &out_p));
    assert(out_p.duration_ms == 200 && out_p.h == 50);
    assert(!kp_rgb_persist_resolve_parameters(&defaults_p, &legacy_p, &scoped_p, true, true, &out_p));
    assert(out_p.duration_ms == 300 && out_p.h == 70); /* scoped takes precedence */
    assert(!kp_rgb_persist_resolve_parameters(&defaults_p, NULL, NULL, true, true, &out_p));
    assert(out_p.duration_ms == 100);

    /* Stable IDs, not array position, preserve a selection across member reordering. */
    const struct kp_rgb_persist_effect_id reordered[] = {{"second", true}, {"first", true}};
    assert(!kp_rgb_persist_resolve_selector("second", NULL, false, &stored, reordered, 2, &selected));
    strcpy(stored.selected_id, "first");
    assert(!kp_rgb_persist_resolve_selector("second", NULL, false, &stored, reordered, 2, &selected));
    assert(!strcmp(selected.selected_id, "first"));

    /* Independent selectors and parameter records resolve independently. */
    struct kp_rgb_persist_selector_record first_sel, second_sel;
    assert(kp_rgb_persist_encode_selector("first", &stored) == 0);
    assert(!kp_rgb_persist_resolve_selector("second", NULL, false, &stored, members, 3, &first_sel));
    assert(kp_rgb_persist_encode_selector("second", &stored) == 0);
    assert(!kp_rgb_persist_resolve_selector("first", NULL, false, &stored, members, 3, &second_sel));
    assert(!strcmp(first_sel.selected_id, "first") && !strcmp(second_sel.selected_id, "second"));
    struct kp_rgb_persist_effect p1 = {.duration_ms = 111, .h = 1, .s = 2};
    struct kp_rgb_persist_effect p2 = {.duration_ms = 222, .h = 3, .s = 4};
    struct kp_rgb_persist_effect o1, o2;
    assert(!kp_rgb_persist_resolve_parameters(&defaults_p, NULL, &p1, false, true, &o1));
    assert(!kp_rgb_persist_resolve_parameters(&defaults_p, NULL, &p2, false, true, &o2));
    assert(o1.duration_ms == 111 && o2.duration_ms == 222);

    /* Applying migration repeatedly to decoded legacy data is idempotent. */
    assert(kp_rgb_persist_resolve_selector("first", &legacy, true, NULL, members, 3, &selected) == false);
    legacy.has_selected = 1; strcpy(legacy.selected_id, "second");
    assert(kp_rgb_persist_resolve_selector("first", &legacy, true, NULL, members, 3, &selected));
    assert(kp_rgb_persist_encode_selector(selected.selected_id, &stored) == 0);
    assert(!kp_rgb_persist_resolve_selector("first", &legacy, true, &stored, members, 3, &selected));
    assert(!strcmp(selected.selected_id, "second"));
    assert(kp_rgb_persist_resolve_parameters(&defaults_p, &legacy_p, NULL, true, true, &out_p));
    assert(kp_rgb_persist_encode_parameters(&out_p, &(struct kp_rgb_persist_parameters_record){0}) == 0);
    assert(!kp_rgb_persist_resolve_parameters(&defaults_p, &legacy_p, &legacy_p, true, true, &out_p));
}

int main(void) {
    test_wire_codecs();
    test_invalid_records_and_legacy();
    test_ids();
    test_resolvers();
    puts("scoped persistence codec/resolvers passed");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        stub = tmp / "stub/zephyr"
        (stub / "settings").mkdir(parents=True)
        (stub / "sys").mkdir(parents=True)
        (stub / "drivers").mkdir(parents=True)
        (stub / "settings/settings.h").write_text(
            "#pragma once\n#define SETTINGS_MAX_NAME_LEN 64\n#define SETTINGS_MAX_VAL_LEN 256\n")
        (stub / "sys/util.h").write_text(
            "#pragma once\n#define BUILD_ASSERT(c, ...) _Static_assert(c, #c)\n"
            "#define MIN(a,b) ((a) < (b) ? (a) : (b))\n"
            "#define CLAMP(v,lo,hi) MIN(MAX(v,lo),hi)\n"
            "#define MAX(a,b) ((a) > (b) ? (a) : (b))\n")
        (stub / "drivers/led_strip.h").write_text(
            "#pragma once\n#include <stdint.h>\nstruct led_rgb { uint8_t r, g, b; };\n")
        harness = tmp / "scoped_persist.c"
        harness.write_text('#include "' + str(ROOT / 'src/rgb_persist.c') + '"\n' + TEST)
        binary = tmp / "scoped_persist"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-I", str(tmp / "stub"), "-I", str(ROOT / "include"),
            str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
