// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once
/**
 * M1 drumlogue synth unit.
 *
 * Programs come from the ALL DATA dump when one is supplied; otherwise
 * PROGRAM picks a multisound and the unit builds a plain program. The M1
 * effect section is bypassed and its tables are not read.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "unit.h"

/* The SDK's attributes.h defines fast as a macro; the engine's effect table
 * has members of that name. */
#undef fast

#include "m1_engine/m1_engine.h"
#include "m1_log.h"
#include "m1_resampler.h"
#include "m1_rom.h"
#include "unit_note_queue.h"

enum {
    PARAM_PROGRAM,
    PARAM_LEVEL,
    PARAM_NOTE,
};

/* Voices at once. The engine has 16; this is the budget for the device. */
static constexpr unsigned k_voices = 8;

/* Headroom for a few voices summed, at the engine's own full scale. */
static constexpr float k_output_gain = 0.35f;

class Synth {
public:
    Synth() : engine_(nullptr), program_(0), level_(0.8f), note_(60),
              init_failed_(false), panic_requested_(false), program_dirty_(true),
              gate_note_(-1), note_on_handled_(false), render_blocks_(0),
              note_events_(0), max_block_ms_(0.0), total_block_ms_(0.0),
              late_blocks_(0), max_peak_(0.f), max_sounding_(0) {
        std::memset(sounding_, 0, sizeof sounding_);
        std::memset(programBuf_, 0, sizeof programBuf_);
    }

    ~Synth() { Teardown(); }

    int8_t Init(const unit_runtime_desc_t* desc) {
        m1unit::log("init: target=0x%04x api=0x%08x sr=%lu frames=%u out=%u",
                    static_cast<unsigned>(desc->target), desc->api,
                    static_cast<unsigned long>(desc->samplerate),
                    static_cast<unsigned>(desc->frames_per_buffer),
                    static_cast<unsigned>(desc->output_channels));
        if (desc->samplerate != 48000) return k_unit_err_samplerate;
        if (desc->output_channels != 2) return k_unit_err_geometry;
        sample_rate_ = desc->samplerate;
        ensureEngine();
        return k_unit_err_none;
    }

    void Teardown() {
        m1unit::log("teardown");
        logSummary("teardown");
        delete engine_;
        engine_ = nullptr;
        clearNotes();
        events_.clear();
    }

    void Reset() {
        m1unit::log("reset");
        logSummary("reset");
        /* panic() reads the voices, so it belongs on the render thread. */
        panic_requested_ = true;
        events_.clear();
    }

    void Resume() { m1unit::log("resume"); }
    void Suspend() {
        m1unit::log("suspend");
        logSummary("suspend");
    }

    void Render(float* out, size_t frames) {
        /* The resampler allocates; never let a failure unwind through the
         * daemon's C frames. */
        try {
            renderBlock(out, frames);
        } catch (...) {
            std::memset(out, 0, frames * 2 * sizeof(float));
        }
    }

    void setParameter(uint8_t index, int32_t value) {
        m1unit::log("param: %u = %ld", static_cast<unsigned>(index),
                    static_cast<long>(value));
        switch (index) {
        case PARAM_PROGRAM:
            /* playProgram() rewrites the part the render thread walks; apply
             * it there. */
            program_ = clampProgram(value);
            program_dirty_ = true;
            if (soundingCount() == 0) logSummary("program");
            break;
        case PARAM_LEVEL:
            level_ = value < 0 ? 0.f : value > 100 ? 1.f : value / 100.f;
            break;
        case PARAM_NOTE:
            note_ = clampMidi(value);
            break;
        default:
            break;
        }
    }

    int32_t getParameterValue(uint8_t index) const {
        switch (index) {
        case PARAM_PROGRAM: return program_;
        case PARAM_LEVEL:   return static_cast<int32_t>(level_ * 100.f);
        case PARAM_NOTE:    return note_;
        default:            return 0;
        }
    }

    /* PROGRAM shows the factory program's name when a preload is loaded, and
     * the multisound's name otherwise. */
    const char* getParameterStrValue(uint8_t index, int32_t value) {
        if (index != PARAM_PROGRAM) return nullptr;
        const int program = clampProgram(value);
        if (!engine_ || !rom_.ready()) return "---";
        const uint8_t* p = rom_.program(program);
        if (p) {
            std::memcpy(name_, p, 10);
        } else {
            std::snprintf(name_, sizeof name_, "%s", rom_.multisoundName(program));
        }
        name_[10] = 0;
        for (int i = 9; i >= 0 && (name_[i] == ' ' || name_[i] == 0); --i) name_[i] = 0;
        return name_;
    }
    const uint8_t* getParameterBmpValue(uint8_t, int32_t) const { return nullptr; }

    inline void NoteOn(uint8_t note, uint8_t velocity) {
        ++note_events_;
        events_.push(rfx::kUnitNoteOn, note, velocity);
    }
    inline void NoteOff(uint8_t note) {
        ++note_events_;
        events_.push(rfx::kUnitNoteOff, note, 0);
    }
    inline void GateOn(uint8_t velocity) {
        ++note_events_;
        events_.push(rfx::kUnitGateOn, 0, velocity);
    }
    inline void GateOff() {
        ++note_events_;
        events_.push(rfx::kUnitGateOff, 0, 0);
    }
    inline void AllNoteOff() {
        ++note_events_;
        events_.push(rfx::kUnitAllNoteOff, 0, 0);
    }
    void PitchBend(uint16_t bend) {
        if (engine_) engine_->pitchBend(0, static_cast<int16_t>(static_cast<int>(bend) - 8192));
    }
    void ChannelPressure(uint8_t pressure) {
        if (engine_) engine_->aftertouch(0, pressure);
    }
    void Aftertouch(uint8_t, uint8_t pressure) {
        if (engine_) engine_->aftertouch(0, pressure);
    }
    void LoadPreset(uint8_t) {}
    uint8_t getPresetIndex() const { return 0; }
    static const char* getPresetName(uint8_t) { return nullptr; }

private:
    static uint8_t clampMidi(int32_t value) {
        return static_cast<uint8_t>(value < 0 ? 0 : value > 127 ? 127 : value);
    }
    static uint8_t clampProgram(int32_t value) {
        return static_cast<uint8_t>(value < 0 ? 0 : value > 99 ? 99 : value);
    }

    bool ensureEngine() {
        if (engine_) return true;
        if (init_failed_) return false;

        m1unit::log("engine: reading ROMs");
        const double started = m1unit::seconds();
        if (!rom_.load(RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH), "m1-preload.syx")) {
            m1unit::log("engine: %s", rom_.failure());
            std::fprintf(stderr, "M1: %s\n", rom_.failure());
            init_failed_ = true;
            return false;
        }
        try {
            engine_ = new m1::Engine();
        } catch (...) {
            init_failed_ = true;
            return false;
        }
        if (!engine_->init(rom_.tables(), rom_.fxMem())) {
            delete engine_;
            engine_ = nullptr;
            init_failed_ = true;
            return false;
        }
        engine_->setBypassFx(true);
        engine_->setVoiceLimit(k_voices);
        res_.init(m1::kSampleRate, sample_rate_);
        program_dirty_ = true;
        m1unit::log("engine: ready, pcm %lu words, %s, %.2fs",
                    static_cast<unsigned long>(rom_.tables().pcmWords),
                    rom_.hasPrograms() ? "preload" : "no preload",
                    m1unit::seconds() - started);
        return true;
    }

    void renderBlock(float* out, size_t frames) {
        const double started = m1unit::seconds();
        enableFlushToZero();

        if (!ensureEngine()) {
            std::memset(out, 0, frames * 2 * sizeof(float));
            return;
        }
        if (panic_requested_) {
            panic_requested_ = false;
            engine_->allSoundOff();
            clearNotes();
        }
        /* The program has to be in place before a note in this block. */
        if (program_dirty_) {
            program_dirty_ = false;
            applyProgram();
        }
        drainNoteQueue();
        if (panic_requested_) {
            panic_requested_ = false;
            engine_->allSoundOff();
            clearNotes();
        }

        res_.render(*engine_, out, frames, level_ * k_output_gain);

        const double ms = (m1unit::seconds() - started) * 1000.0;
        float peak = 0.f;
        bool nonfinite = false;
        for (size_t i = 0; i < frames * 2; ++i) {
            const float a = out[i] < 0 ? -out[i] : out[i];
            if (!nonfinite && (!(out[i] == out[i]) || a > 1e30f)) nonfinite = true;
            if (a > peak) peak = a;
        }
        if (peak > max_peak_) max_peak_ = peak;
        if (nonfinite) max_peak_ = 1e30f;
        const double deadline = frames / 48.0;
        if (ms > max_block_ms_) max_block_ms_ = ms;
        total_block_ms_ += ms;
        if (ms > deadline) ++late_blocks_;
        const unsigned sounding = soundingCount();
        if (sounding > max_sounding_) max_sounding_ = sounding;
        ++render_blocks_;
    }

    void drainNoteQueue() {
        rfx::UnitNoteMessage msg;
        while (events_.pop(msg)) {
            switch (msg.type) {
            case rfx::kUnitNoteOn:     processNoteOn(msg.note, msg.velocity); break;
            case rfx::kUnitNoteOff:    processNoteOff(msg.note); break;
            case rfx::kUnitGateOn:     processGate(msg.velocity); break;
            case rfx::kUnitGateOff:    processGateOff(); break;
            case rfx::kUnitAllNoteOff: processAllNoteOff(); break;
            }
        }
    }

    void processNoteOn(uint8_t note, uint8_t velocity) {
        if (!engine_ || note > 127) return;
        if (velocity == 0) {
            processNoteOff(note);
            return;
        }
        gate_note_ = note;
        note_on_handled_ = true;
        startNote(note, velocity);
    }

    void processNoteOff(uint8_t note) {
        if (!engine_ || note > 127) return;
        note_on_handled_ = false;
        if (note == gate_note_) gate_note_ = -1;
        stopNote(note);
    }

    void processGate(uint8_t velocity) {
        if (!engine_ || note_on_handled_) return;
        gate_note_ = note_;
        startNote(note_, velocity);
    }

    void processGateOff() {
        note_on_handled_ = false;
        if (!engine_ || gate_note_ < 0) return;
        stopNote(static_cast<uint8_t>(gate_note_));
        gate_note_ = -1;
    }

    void processAllNoteOff() {
        panic_requested_ = true;
        events_.clear();
    }

    /* A key already sounding is stopped first: a second note-on for the same
     * key would otherwise release both notes at once. */
    void startNote(uint8_t note, uint8_t velocity) {
        if (sounding_[note]) engine_->noteOff(0, note);
        engine_->noteOn(0, note, velocity);
        sounding_[note] = true;
    }

    void stopNote(uint8_t note) {
        if (!sounding_[note]) return;
        engine_->noteOff(0, note);
        sounding_[note] = false;
    }

    void clearNotes() {
        std::memset(sounding_, 0, sizeof sounding_);
        gate_note_ = -1;
        note_on_handled_ = false;
    }

    unsigned soundingCount() const {
        unsigned count = 0;
        for (bool held : sounding_)
            if (held) ++count;
        return count;
    }

    /* The effect block is 25 bytes at 38; both effects are off. Type 33
     * configures the section and allocates nothing. */
    static void disableEffects(uint8_t* program) {
        std::memset(program + 38, 0, 25);
        program[38] = 33;
        program[39] = 33;
    }

    void applyProgram() {
        if (!engine_) return;
        const uint8_t* source = rom_.program(program_);
        if (source) {
            std::memcpy(programBuf_, source, m1::kProgLen);
            disableEffects(programBuf_);
        } else {
            buildProgram(program_);
        }
        engine_->playProgram(programBuf_);
        clearNotes();
    }

    /* A plain single-oscillator program: the split's own sample and level, an
     * open filter and a VDA that holds until release. */
    void buildProgram(int multisound) {
        std::memset(programBuf_, 0, m1::kProgLen);
        std::snprintf(reinterpret_cast<char*>(programBuf_), 11, "%-10.10s",
                      rom_.multisoundName(multisound));
        programBuf_[10] = 0;                 /* OSC mode: Single */
        programBuf_[11] = 0;                 /* no mono, no hold */
        programBuf_[12] = static_cast<uint8_t>(multisound);
        disableEffects(programBuf_);

        uint8_t* osc = programBuf_ + 63;
        osc[8] = 99;    /* VDF cutoff: open */
        osc[9] = 60;    /* VDF key centre */
        osc[11] = 0;    /* VDF EG intensity */
        osc[23] = 99;   /* VDA level */
        osc[24] = 60;   /* VDA key centre */
        osc[30] = 99;   /* VDA attack level */
        osc[31] = 0;    /* VDA decay time */
        osc[32] = 99;   /* VDA break point */
        osc[33] = 0;    /* VDA slope time */
        osc[34] = 99;   /* VDA sustain level */
        osc[35] = 10;   /* VDA release time */
    }

    void logSummary(const char* when) {
        const double average = render_blocks_ ? total_block_ms_ / render_blocks_ : 0.0;
        m1unit::log("%s: %lu blocks, %lu events, max %.2fms avg %.2fms, late %lu,"
                    " peak %.4f, notes %u, voices %u",
                    when, static_cast<unsigned long>(render_blocks_),
                    static_cast<unsigned long>(note_events_), max_block_ms_, average,
                    static_cast<unsigned long>(late_blocks_), max_peak_, max_sounding_,
                    k_voices);
        render_blocks_ = 0;
        note_events_ = 0;
        max_block_ms_ = 0.0;
        total_block_ms_ = 0.0;
        late_blocks_ = 0;
        max_peak_ = 0.f;
        max_sounding_ = 0;
    }

    /* ARM denormals cost much more than normal operations, and the VDF and
     * envelope ramps enter that range. FPSCR is per thread: set it every
     * block. */
    static void enableFlushToZero() {
#if defined(__arm__)
        uint32_t fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));
#endif
    }

    m1unit::M1Rom rom_;
    m1::Engine* engine_;
    m1unit::Resampler res_;
    uint8_t programBuf_[m1::kProgLen];
    char name_[16] = {0};
    uint8_t program_;
    float level_;
    uint8_t note_;
    uint32_t sample_rate_ = 48000;
    bool init_failed_;
    bool panic_requested_;
    bool program_dirty_;
    int gate_note_;
    bool note_on_handled_;
    bool sounding_[128];
    /* Diagnostics, written outside the audio path. */
    unsigned long render_blocks_;
    unsigned long note_events_;
    double max_block_ms_;
    double total_block_ms_;
    unsigned long late_blocks_;
    float max_peak_;
    unsigned max_sounding_;

    rfx::UnitNoteQueue<64> events_;
};
