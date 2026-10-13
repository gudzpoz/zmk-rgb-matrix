#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production global-only defaults and selector-local selection/default isolation."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?(?:inline )?[\w\s*]+\b" + name + r"\([^;]*?\) \{", source, re.M)
    assert match, name
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def structure(source, name):
    match = re.search(r"struct " + name + r" \{.*?^\};", source, re.M | re.S)
    assert match, name
    return match.group()


def fixture_code():
    from test_selector_controls import production_fixture, TESTS as controls
    return production_fixture().removesuffix(controls)


TESTS = r'''
int main(void) {
 setup();
 struct kp_rgb_effect_common_data before[5]; memcpy(before,parameters,sizeof(before));
 size_t a=data_a.selector.effect_index,b=data_b.selector.effect_index;
 unsigned f=flushes,p=power_reconciles;
 kp_rgb_global.initial_brightness=120; kp_rgb_global.initial_on=false;
 assert(kp_rgb_apply_defaults()==0);
 assert(!kp_rgb_global.user_on && kp_rgb_global.brightness==100);
 assert(kp_rgb_global.tuning.max_brightness==80 && kp_rgb_global.tuning.idle_brightness==20);
 assert(flushes==f+1 && power_reconciles==p+1 && !held);
 assert(!memcmp(before,parameters,sizeof(before)));
 assert(data_a.selector.effect_index==a && data_b.selector.effect_index==b);
 kp_rgb_global.initial_brightness=-1; kp_rgb_global.initial_on=true;
 assert(kp_rgb_apply_defaults()==0 && kp_rgb_global.brightness==0 && kp_rgb_global.user_on);
 assert(!memcmp(before,parameters,sizeof(before)) && callbacks==0 && saves==0 && scoped_saves==0);
 test_reset(); test_reset_startup_edges(); test_arguments(); test_associations();
 setup();
 kp_rgb_matrix_lock();
 assert(kp_rgb_selector_count(&data_a.selector)==3);
 assert(kp_rgb_selector_at(&data_a.selector,1)==NULL && kp_rgb_selector_at(&data_a.selector,3)==NULL);
 assert(kp_rgb_selector_calc_index(&data_a.selector,0,1)==2);
 assert(kp_rgb_selector_calc_index(&data_a.selector,2,1)==0);
 assert(kp_rgb_selector_calc_index(&data_a.selector,0,-1)==2);
 struct kp_rgb_effect_defaults defaults[3]; memcpy(defaults,defaults_a,sizeof(defaults));
 data_a.selector.effect_defaults=defaults;
 defaults[0].duration_ms=65536u+700u; defaults[2].duration_ms=UINT32_MAX;
 kp_rgb_selector_apply_defaults_locked(&data_a.selector);
 assert(parameters[0].duration_ms==10000 && parameters[1].duration_ms==10000);
 defaults[0].duration_ms=0; defaults[2].duration_ms=1;
 data_a.selector.initial_duration_ms=-1;
 kp_rgb_selector_apply_defaults_locked(&data_a.selector);
 assert(parameters[0].duration_ms==100 && parameters[1].duration_ms==100);
 data_a.selector.initial_duration_ms=UINT32_MAX;
 kp_rgb_selector_apply_defaults_locked(&data_a.selector);
 assert(parameters[0].duration_ms==10000 && parameters[4].duration_ms==1000);
 kp_rgb_matrix_unlock();
 puts("global-only defaults, selector-local selection/defaults and private conversion isolation passed");
 return 0;
}
'''


def main():
    from test_selector_controls import TESTS as controls
    code = fixture_code() + controls[:controls.index('int main(void)')] + TESTS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'defaults.c'
        path.write_text(code)
        binary = Path(directory) / 'defaults'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
            '-DHAS_ALIAS=0', '-DCENTRAL_SYNC=0', str(path), '-o', str(binary)],
            check=True, timeout=20)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()
