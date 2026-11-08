// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/**
 * R1 - drumlogue synth unit
 *
 * A Korg M1 player on PicoFaceM1's engine. It needs the user's M1 ROM set in
 * the userfs (see README.md):
 *   /var/lib/drumlogued/userfs/Programs/m1-{ic23,ic32,ic30,ic36..ic43}.bin
 * and optionally m1-preload.syx for the factory programs.
 *
 * Three parameters:
 *   0 PROGRAM - factory program, or a multisound when no preload is present
 *   1 LEVEL   - output level, as a percentage
 *   2 NOTE    - the note a gate plays
 */

#include "unit.h"

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target      = UNIT_TARGET_PLATFORM | k_unit_module_synth,
    .api         = UNIT_API_VERSION,
    .dev_id      = 0x72677276U,
    .unit_id     = 0x09U,
    .version     = 0x00010000U,
    .name        = "R1",
    .num_presets = 0,
    .num_params  = 3,
    .params = {
        /* The row shows the program's name, not its number. */
        {0, 99, 0, 0,   k_unit_param_type_strings,   0, 0, 0, {"PROGRAM"}},
        {0, 100, 0, 80, k_unit_param_type_percent,   0, 0, 0, {"LEVEL"}},
        {0, 127, 60, 60, k_unit_param_type_midi_note, 0, 0, 0, {"NOTE"}},
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
