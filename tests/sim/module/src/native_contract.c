/* Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */
#include "native_contract.h"
#include "cmdline.h"
#include "soc.h"

const char *kp_test_scenario;
static void options(void) {
    static struct args_struct_t args[] = {
        {.option = "rgb-test-scenario", .name = "scenario", .type = 's',
         .dest = (void *)&kp_test_scenario, .descript = "Run one isolated RGB contract"},
        ARG_TABLE_ENDMARKER};
    native_add_command_line_opts(args);
}
NATIVE_TASK(options, PRE_BOOT_1, 10);

static void scenario(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    KP_CHECK(kp_test_scenario != NULL);
#if defined(CONFIG_KEYPAW_SIM_ENGINE_CONTRACTS)
    kp_test_engine_run();
#elif defined(CONFIG_KEYPAW_SIM_CONDITION_CONTRACTS)
    kp_test_conditions_run();
#elif defined(CONFIG_KEYPAW_SIM_CONTROL_CONTRACTS)
    kp_test_controls_run();
#elif defined(CONFIG_KEYPAW_SIM_EFFECT_CONTRACTS)
    kp_test_effects_run();
#endif
    kp_test_complete();
}
K_THREAD_DEFINE(contract_thread, 8192, scenario, NULL, NULL, NULL, 5, 0, 1);
