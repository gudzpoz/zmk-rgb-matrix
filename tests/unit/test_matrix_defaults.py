#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT
"""Production matrix-owned global initialization without a control or selector."""
import os
import pathlib
import re
import shlex
import subprocess
import tempfile
from engine_harness import ROOT, SOURCE
from test_singleton import structure


def main():
    declaration = re.search(r'^struct kp_rgb_global kp_rgb_global = \{.*?^\};', SOURCE, re.M | re.S)
    assert declaration, 'matrix-owned global initializer'
    assert 'struct kp_rgb_global kp_rgb_global =' not in (ROOT / 'src/behavior_rgb_matrix.c').read_text()
    internal = (ROOT / 'src/rgb_matrix_internal.h').read_text()
    prelude = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define KP_RGB_NODE matrix
#define KP_RGB_BRT_MAX 100
#define CLAMP(x,l,h) ((x)<(l)?(l):((x)>(h)?(h):(x)))
struct kp_rgb_tuning { uint8_t max_brightness,idle_brightness; };
'''
    # The DT mock accepts only the matrix node; no behavior/selector node exists.
    for name, on, brightness, maximum, idle in (
        ('defaults', 0, 30, 40, 30), ('custom', 0, 73, 91, 12),
        ('clamp-high', 1, 150, 40, 30), ('clamp-low', 0, -5, 40, 30),
    ):
        if name == 'defaults':
            macros = ('#define value_initial_on 0\n#define value_initial_brightness 30\n'
                      '#define value_max_brightness 40\n#define value_idle_brightness 30\n'
                      '#define DT_PROP_OR_matrix(prop,fallback) value_##prop\n')
        else:
            macros = ('#define DT_PROP_OR_matrix(prop,fallback) value_##prop\n'
                      f'#define value_initial_on {on}\n#define value_initial_brightness {brightness}\n'
                      f'#define value_max_brightness {maximum}\n#define value_idle_brightness {idle}\n')
        macros += ('#define DT_PROP_OR_(node,prop,fallback) DT_PROP_OR_##node(prop,fallback)\n'
                   '#define DT_PROP_OR(node,prop,fallback) DT_PROP_OR_(node,prop,fallback)\n')
        test = f'''
int main(void) {{
 assert(kp_rgb_global.initial_on=={on} && kp_rgb_global.user_on=={on});
 assert(kp_rgb_global.initial_brightness=={brightness});
 assert(kp_rgb_global.brightness=={max(0, min(100, brightness))});
 assert(kp_rgb_global.tuning.max_brightness=={maximum});
 assert(kp_rgb_global.tuning.idle_brightness=={idle});
}}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'defaults.c'
            binary = pathlib.Path(directory) / 'defaults'
            source.write_text(prelude + macros + structure(internal, 'kp_rgb_global') + '\n'
                              + declaration.group() + test)
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print('matrix-only initial intent, brightness, tuning and clamping passed')


if __name__ == '__main__':
    main()
