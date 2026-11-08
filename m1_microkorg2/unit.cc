// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/* The logue SDK seam for the microKORG2 oscillator. */

#include <cstddef>
#include <cstdint>

#include "unit.h"
#include "runtime.h"
#include "osc.h"

static Osc s_osc_instance;

__unit_callback int8_t unit_init(const unit_runtime_desc_t* desc) {
    if (!desc) return k_unit_err_undef;
    if (desc->target != unit_header.target) return k_unit_err_target;
    if (!UNIT_API_IS_COMPAT(desc->api)) return k_unit_err_api_version;
    return s_osc_instance.Init(desc);
}

__unit_callback void unit_teardown() { s_osc_instance.Teardown(); }
__unit_callback void unit_reset() { s_osc_instance.Reset(); }
__unit_callback void unit_resume() { s_osc_instance.Resume(); }
__unit_callback void unit_suspend() { s_osc_instance.Suspend(); }
__unit_callback void unit_render(const float*, float* out, uint32_t frames) { s_osc_instance.Render(out, frames); }
__unit_callback void unit_set_param_value(uint8_t id, int32_t value) { s_osc_instance.setParameter(id, value); }
__unit_callback int32_t unit_get_param_value(uint8_t id) { return s_osc_instance.getParameterValue(id); }
__unit_callback const char* unit_get_param_str_value(uint8_t id, int32_t value) { return s_osc_instance.getParameterStrValue(id, value); }
__unit_callback const uint8_t* unit_get_param_bmp_value(uint8_t id, int32_t value) { return s_osc_instance.getParameterBmpValue(id, value); }
__unit_callback void unit_set_tempo(uint32_t) {}
__unit_callback void unit_note_on(uint8_t note, uint8_t velocity) { s_osc_instance.NoteOn(note, velocity); }
__unit_callback void unit_note_off(uint8_t note) { s_osc_instance.NoteOff(note); }
__unit_callback void unit_gate_on(uint8_t velocity) { s_osc_instance.GateOn(velocity); }
__unit_callback void unit_gate_off() { s_osc_instance.GateOff(); }
__unit_callback void unit_all_note_off() { s_osc_instance.AllNoteOff(); }
__unit_callback void unit_pitch_bend(uint16_t bend) { s_osc_instance.PitchBend(bend); }
__unit_callback void unit_channel_pressure(uint8_t pressure) { s_osc_instance.ChannelPressure(pressure); }
__unit_callback void unit_aftertouch(uint8_t note, uint8_t pressure) { s_osc_instance.Aftertouch(note, pressure); }
__unit_callback void unit_load_preset(uint8_t index) { s_osc_instance.LoadPreset(index); }
__unit_callback uint8_t unit_get_preset_index() { return s_osc_instance.getPresetIndex(); }
__unit_callback const char* unit_get_preset_name(uint8_t index) { return Osc::getPresetName(index); }
