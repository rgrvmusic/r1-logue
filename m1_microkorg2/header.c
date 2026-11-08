// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/**
 * R1 - microKORG2 oscillator unit
 *
 * The M1 engine as a microKORG2 oscillator, built from the user's ROM set in
 * the userfs. One parameter, because the keyboard supplies the note and its
 * own level:
 *   0 PROGRAM - M1 program, or multisound when no preload is loaded
 */

#include "unit.h"
#include "runtime.h"

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target      = UNIT_TARGET_PLATFORM | k_unit_module_osc,
    .api         = UNIT_API_VERSION,
    .dev_id      = 0x72677276U,
    .unit_id     = 0x09U,
    .version     = 0x00010000U,
    .name        = "R1",
    .num_presets = 0,
    .num_params  = 1,
    .params = {
        /* Strings so the display shows the program's name, not its number. */
        {0, 99, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"PROGRAM"}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
    }
};
