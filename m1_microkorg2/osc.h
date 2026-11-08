// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once
/**
 * R1 - microKORG2 oscillator.
 *
 * The engine renders at 31.25 kHz; the unit resamples to 48 kHz. A fresh
 * trigger bit starts the voice's note at pitch[], a note-off releases it, and
 * a pitch that moves while the bit stays up is a bend.
 *
 * Output is the engine's stereo pair in the first two output streams at
 * bufferOffset: out[offset + f * outputStride] left, + 1 right.
 */

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "unit.h"
#include "runtime.h"
#include "unit_osc.h"

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
};

static constexpr float k_output_gain = 1.0f;
static constexpr float k_limit_level = 0.7f;
static constexpr float k_limit_attack = 0.02f;       /* ~1 ms at 48 kHz */
static constexpr float k_limit_release = 0.00042f;   /* ~50 ms at 48 kHz */

struct OutputLimiter {
    float gain = 1.f;
    float peak = 0.f;

    /* The held peak spans the waveform's own peaks, so the gain settles at
     * one value: a chord loses level without its peaks being flattened. */
    void pair(float& l, float& r) {
        const float al = l < 0.f ? -l : l;
        const float ar = r < 0.f ? -r : r;
        const float p = al > ar ? al : ar;
        peak -= k_limit_release * peak;
        if (p > peak) peak = p;
        const float target = peak > k_limit_level ? k_limit_level / peak : 1.f;
        const float coeff = target < gain ? k_limit_attack : k_limit_release;
        gain += (target - gain) * coeff;
        if (gain > 1.f) gain = 1.f;
        l *= gain;
        r *= gain;
        /* The gain lags the start of a note; never leave the pair above
         * full scale. */
        if (l > 1.f) l = 1.f;
        else if (l < -1.f) l = -1.f;
        if (r > 1.f) r = 1.f;
        else if (r < -1.f) r = -1.f;
    }

    void reset() { gain = 1.f; peak = 0.f; }
};

static constexpr uint8_t k_default_velocity = 100;

/* A pitch that moves this much between two blocks is a new key, not a bend;
 * it also has to land on a key. */
static constexpr float k_step = 0.5f;
static constexpr float k_land = 0.05f;

/* The bend range the unit gives every program, in semitones. */
static constexpr float k_bend_range = 12.f;
static constexpr float k_bend_epsilon = 0.01f;

enum : uint8_t { kLastNone = 0, kLastOn = 1, kLastOff = 2 };

class Osc {
public:
    Osc() { clearLanes(); }
    ~Osc() { Teardown(); }

    int8_t Init(const unit_runtime_desc_t* desc) {
        if (!desc) return k_unit_err_undef;
        m1unit::log("init: target=0x%04x api=0x%08x sr=%lu frames=%u in=%u out=%u",
                    static_cast<unsigned>(desc->target), desc->api,
                    static_cast<unsigned long>(desc->samplerate),
                    desc->frames_per_buffer, desc->input_channels,
                    desc->output_channels);

        if (desc->samplerate != 48000) return k_unit_err_samplerate;
        if (desc->input_channels != 0 || desc->output_channels != 8)
            return k_unit_err_geometry;

        runtime_desc_ = *desc;
        sample_rate_ = desc->samplerate;
        output_channels_ = desc->output_channels;
        torn_down_ = false;
        ensureEngine();
        return k_unit_err_none;
    }

    void Teardown() {
        if (torn_down_) return;
        torn_down_ = true;
        m1unit::log("teardown");
        logSummary("teardown");
        delete engine_;
        engine_ = nullptr;
        clearLanes();
        events_.clear();
        last_trigger_ = 0;
    }

    void Reset() {
        m1unit::log("reset");
        /* The engine belongs to the render thread. */
        panic_requested_ = true;
        events_.clear();
    }

    void Resume() { m1unit::log("resume"); }
    void Suspend() {
        m1unit::log("suspend");
        logSummary("suspend");
    }

    void Render(float* out, size_t frames) {
        /* The resampler allocates; never let that unwind through the host. */
        try {
            renderBlock(out, frames);
        } catch (...) {
            silence(out, frames);
        }
    }

    void setParameter(uint8_t index, int32_t value) {
        m1unit::log("param: %u = %ld", static_cast<unsigned>(index),
                    static_cast<long>(value));
        if (index != PARAM_PROGRAM) return;
        /* playProgram() rewrites the part while the render thread walks it. */
        program_ = clampProgram(value);
        program_dirty_ = true;
    }

    int32_t getParameterValue(uint8_t index) const {
        return index == PARAM_PROGRAM ? program_.load() : 0;
    }

    /* The factory program's name when a preload is loaded, the multisound's
     * name otherwise. */
    const char* getParameterStrValue(uint8_t index, int32_t value) {
        if (index != PARAM_PROGRAM) return nullptr;
        if (!engine_ || !rom_.ready()) return "---";
        const int program = clampProgram(value);
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

    /* Deprecated, and redundant: the host bakes bend into pitch[]. */
    void PitchBend(uint16_t) {}
    void ChannelPressure(uint8_t) {}
    void Aftertouch(uint8_t, uint8_t) {}
    void LoadPreset(uint8_t) {}
    uint8_t getPresetIndex() const { return 0; }
    static const char* getPresetName(uint8_t) { return nullptr; }

    /*
     * A plain single-oscillator program: the split's own sample and level, an
     * open filter and a VDA that holds until release. Public so the host check
     * can drive a reference engine from the same bytes.
     */
    static void buildProgram(uint8_t* program, const m1unit::M1Rom& rom,
                             int multisound) {
        std::memset(program, 0, m1::kProgLen);
        std::snprintf(reinterpret_cast<char*>(program), 11, "%-10.10s",
                      rom.multisoundName(multisound));
        program[10] = 0;                 /* OSC mode: Single */
        program[11] = 0;                 /* no mono, no hold */
        program[12] = static_cast<uint8_t>(multisound);
        program[32] = static_cast<uint8_t>(static_cast<int8_t>(k_bend_range));
        disableEffects(program);

        uint8_t* osc = program + 63;
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

private:
    static constexpr unsigned k_max_voices = kMk2MaxVoices;

    static uint8_t clampMidi(int32_t value) {
        return static_cast<uint8_t>(value < 0 ? 0 : value > 127 ? 127 : value);
    }
    static uint8_t clampProgram(int32_t value) {
        return static_cast<uint8_t>(value < 0 ? 0 : value > 99 ? 99 : value);
    }
    static uint8_t noteFromPitch(float pitch) {
        if (!(pitch > 0.f)) return 60;
        const int note = static_cast<int>(pitch + 0.5f);
        return static_cast<uint8_t>(note < 0 ? 0 : note > 127 ? 127 : note);
    }

    /* The effect block is 25 bytes at 38 (program); both effects are off. */
    static void disableEffects(uint8_t* program) {
        std::memset(program + 38, 0, 25);
        program[38] = 33;
        program[39] = 33;
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
            if (!engine_->init(rom_.tables(), rom_.fxMem())) {
                delete engine_;
                engine_ = nullptr;
                init_failed_ = true;
                return false;
            }
            engine_->setBypassFx(true);
            engine_->setVoiceLimit(static_cast<int>(k_max_voices));
            res_.init(m1::kSampleRate, sample_rate_);
        } catch (...) {
            delete engine_;
            engine_ = nullptr;
            init_failed_ = true;
            return false;
        }
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
            silence(out, frames);
            return;
        }
        if (panic_requested_) {
            panic_requested_ = false;
            panic();
        }
        /* The program has to be in place before a note in this block. */
        if (program_dirty_) {
            program_dirty_ = false;
            applyProgram();
        }

        const unit_runtime_osc_context_t* ctxt = oscContext();
        const unsigned limit = voiceLimit(ctxt);

        /* The callbacks for this block. */
        uint8_t on_key[64];
        uint8_t on_velocity[64];
        uint8_t off_key[64];
        unsigned on_count = 0;
        unsigned off_count = 0;
        bool panic_event = false;
        std::memset(key_last_, kLastNone, sizeof(key_last_));
        std::memset(released_, 0, sizeof(released_));
        std::memset(attacked_, 0, sizeof(attacked_));
        drainNoteQueue(on_key, on_velocity, on_count, off_key, off_count,
                       panic_event);
        if (panic_event) panic();

        /* A key pressed and released inside this block starts nothing. */
        uint8_t tap_key[64];
        unsigned tap_count = 0;
        unsigned kept = 0;
        for (unsigned k = 0; k < on_count; ++k) {
            if (key_last_[on_key[k]] == kLastOff && !laneHolds(on_key[k]) &&
                !hostHolds(ctxt, limit, on_key[k])) {
                if (tap_count < 64) tap_key[tap_count++] = on_key[k];
                continue;
            }
            on_key[kept] = on_key[k];
            on_velocity[kept] = on_velocity[k];
            ++kept;
        }
        on_count = kept;

        /* Releases first: a key pressed again in the same block is a new note. */
        for (unsigned j = 0; j < off_count; ++j) releaseKey(off_key[j]);

        const uint8_t fresh =
            ctxt ? static_cast<uint8_t>(ctxt->trigger & ~last_trigger_) : 0;
        if (ctxt) last_trigger_ = ctxt->trigger;

        /* A fresh trigger bit starts the host voice's note from pitch[]. */
        bool on_used[64] = {};
        if (ctxt) {
            for (unsigned i = 0; i < limit; ++i) {
                if (!(fresh & static_cast<uint8_t>(1u << i))) continue;
                const uint8_t pitch_note = noteFromPitch(ctxt->pitch[i]);

                /* Prefer the callback whose key is the note the voice holds. */
                int match = -1;
                for (unsigned k = 0; k < on_count; ++k) {
                    if (!on_used[k] && on_key[k] == pitch_note) {
                        match = static_cast<int>(k);
                        break;
                    }
                }
                if (match >= 0) {
                    on_used[match] = true;
                    startLane(i, on_key[match], on_velocity[match], ctxt->pitch[i]);
                    continue;
                }

                bool tapped = false;
                for (unsigned t = 0; t < tap_count; ++t) {
                    if (tap_key[t] == pitch_note) {
                        tapped = true;
                        break;
                    }
                }
                if (tapped) continue;

                /* No callback for this key: take the first unused one. */
                uint8_t key = pitch_note;
                uint8_t velocity = last_velocity_;
                for (unsigned k = 0; k < on_count; ++k) {
                    if (!on_used[k]) {
                        key = on_key[k];
                        velocity = on_velocity[k];
                        on_used[k] = true;
                        break;
                    }
                }
                startLane(i, key, velocity, ctxt->pitch[i]);
            }
        }

        /* A key pressed again while it sounds: one new attack, not a second
         * copy. */
        if (ctxt) {
            for (unsigned k = 0; k < on_count; ++k) {
                if (on_used[k]) continue;
                const uint8_t key = on_key[k];
                if (!laneHolds(key) || attacked_[key]) continue;
                startKey(key, on_velocity[k]);
                attacked_[key] = true;
                on_used[k] = true;
            }
        }

        /* A key the host put on a voice whose trigger bit was already up: the
         * callback and the voice's new pitch say which one it landed on. */
        if (ctxt) {
            for (unsigned k = 0; k < on_count; ++k) {
                if (on_used[k]) continue;
                const uint8_t key = on_key[k];
                int best = -1;
                float best_pitch = 0.f;
                float best_score = k_step;
                for (unsigned i = 0; i < limit; ++i) {
                    if (lane_on_[i] && lane_key_[i] == key) continue;
                    const float now = ctxt->pitch[i];
                    const float expected =
                        lane_on_[i]
                            ? static_cast<float>(key) + lane_pitch_[i] -
                                  static_cast<float>(lane_key_[i])
                            : static_cast<float>(key);
                    float score = now - expected;
                    if (score < 0) score = -score;
                    if (score < best_score) {
                        best_score = score;
                        best = static_cast<int>(i);
                        best_pitch = now;
                    }
                }
                if (best >= 0)
                    startLane(static_cast<unsigned>(best), key, on_velocity[k],
                              best_pitch);
            }
        }

        /* A held voice follows its pitch: a jump to another key restarts the
         * note there, anything smaller is the wheel and is sent as a bend. */
        if (ctxt) {
            for (unsigned i = 0; i < limit; ++i) {
                const uint8_t bit = static_cast<uint8_t>(1u << i);
                if (fresh & bit) continue;
                if (!lane_on_[i] &&
                    (lane_key_[i] == 0xff || !(ctxt->trigger & bit)))
                    continue;
                followPitch(i, ctxt->pitch[i]);
            }
        }
        updateBend();

        /* The engine agrees with the lanes whatever order the events came in. */
        reconcileNotes();

        const uint32_t offset = outputOffset(ctxt);
        const uint32_t stride = outputStride(ctxt);
        const size_t size = frames * static_cast<size_t>(output_channels_);
        const size_t last = frames ? offset + (frames - 1) * stride + 1 : offset;
        if (frames == 0 || stride < 2 || last >= size) {
            silence(out, frames);
            return;
        }

        float* const first = out + offset;
        float peak = 0.f;
        for (size_t done = 0; done < frames;) {
            const size_t n = frames - done > m1::kRenderBlock
                                 ? static_cast<size_t>(m1::kRenderBlock)
                                 : frames - done;
            res_.render(*engine_, mix_, n, k_output_gain);
            for (size_t i = 0; i < n; ++i) {
                float left = mix_[i * 2];
                float right = mix_[i * 2 + 1];
                limiter_.pair(left, right);
                first[(done + i) * stride] = left;
                first[(done + i) * stride + 1] = right;
                const float l = left < 0 ? -left : left;
                const float r = right < 0 ? -right : right;
                if (l > peak) peak = l;
                if (r > peak) peak = r;
            }
            done += n;
        }

        if (peak > max_peak_) max_peak_ = peak;
        const double ms = (m1unit::seconds() - started) * 1000.0;
        const double deadline = frames / 48.0;
        if (ms > max_block_ms_) max_block_ms_ = ms;
        total_block_ms_ += ms;
        if (ms > deadline) ++late_blocks_;
        const unsigned sounding = soundingCount();
        if (sounding > max_sounding_) max_sounding_ = sounding;
        ++render_blocks_;
    }

    inline void drainNoteQueue(uint8_t* on_key, uint8_t* on_velocity,
                               unsigned& on_count, uint8_t* off_key,
                               unsigned& off_count, bool& panic_requested) {
        rfx::UnitNoteMessage msg;
        while (events_.pop(msg)) {
            switch (msg.type) {
            case rfx::kUnitNoteOn:
                if (msg.velocity == 0) {
                    if (off_count < 64) off_key[off_count++] = msg.note;
                    key_last_[msg.note] = kLastOff;
                } else {
                    if (on_count < 64) {
                        on_key[on_count] = msg.note;
                        on_velocity[on_count] = msg.velocity;
                        ++on_count;
                    }
                    key_last_[msg.note] = kLastOn;
                    last_velocity_ = msg.velocity;
                }
                break;
            case rfx::kUnitNoteOff:
                if (off_count < 64) off_key[off_count++] = msg.note;
                key_last_[msg.note] = kLastOff;
                break;
            case rfx::kUnitGateOn:
                last_velocity_ = msg.velocity;
                break;
            case rfx::kUnitGateOff:
                break;
            case rfx::kUnitAllNoteOff:
                panic_requested = true;
                break;
            }
        }
    }

    /* One engine note per key: a key on two host voices is heard once. */
    void startKey(uint8_t key, uint8_t velocity) {
        if (!engine_) return;
        if (engineOn_[key]) engine_->noteOff(0, key);
        engine_->noteOn(0, key, velocity);
        engineOn_[key] = true;
    }

    void stopKey(uint8_t key) {
        if (!engineOn_[key]) return;
        if (engine_) engine_->noteOff(0, key);
        engineOn_[key] = false;
    }

    void startLane(unsigned lane, uint8_t key, uint8_t velocity, float pitch) {
        const bool was_on = lane_on_[lane];
        const uint8_t old = lane_key_[lane];
        lane_key_[lane] = key;
        lane_pitch_[lane] = pitch;
        lane_on_[lane] = true;
        last_velocity_ = velocity;

        /* A voice handed another key lets the old one go first. */
        if (was_on && old != 0xff && old != key && !laneHolds(old)) stopKey(old);

        /* The same key again is a new attack; a key another voice already
         * holds is the same engine note. */
        const bool again = !attacked_[key] &&
                           ((was_on && old == key) || released_[key]);
        if (again) startKey(key, velocity);
        else if (!engineOn_[key]) startKey(key, velocity);
        attacked_[key] = true;
    }

    bool laneHolds(uint8_t key) const {
        for (unsigned v = 0; v < k_max_voices; ++v) {
            if (lane_on_[v] && lane_key_[v] == key) return true;
        }
        return false;
    }

    bool hostHolds(const unit_runtime_osc_context_t* ctxt, unsigned limit,
                   uint8_t key) const {
        if (!ctxt) return false;
        for (unsigned i = 0; i < limit; ++i) {
            if (lane_key_[i] != key) continue;
            if (!(ctxt->trigger & static_cast<uint8_t>(1u << i))) continue;
            if (noteFromPitch(ctxt->pitch[i]) == key) return true;
        }
        return false;
    }

    void releaseKey(uint8_t key) {
        bool any = false;
        for (unsigned v = 0; v < k_max_voices; ++v) {
            if (!lane_on_[v] || lane_key_[v] != key) continue;
            lane_on_[v] = false;
            any = true;
        }
        if (!any) return;
        released_[key] = true;
        stopKey(key);
    }

    /* A jump to another key restarts the note; a smaller move bends the
     * engine's part. A released lane still follows its pitch, which is how an
     * arpeggiator's next step arrives. */
    void followPitch(unsigned lane, float pitch) {
        const float last = lane_pitch_[lane];
        lane_pitch_[lane] = pitch;
        const float delta = pitch - last;
        if (delta == 0.f) return;

        const uint8_t old = lane_key_[lane];
        const uint8_t note = noteFromPitch(pitch);
        const float landing = pitch - static_cast<float>(note);
        const bool jump = delta <= -k_step || delta >= k_step;
        const bool keyed = jump && note != old && landing > -k_land &&
                           landing < k_land;
        if (!keyed) return;

        lane_key_[lane] = note;
        lane_on_[lane] = true;
        if (old != 0xff && old != note && !laneHolds(old)) stopKey(old);
        if (!engineOn_[note]) startKey(note, last_velocity_);
    }

    /*
     * One bend for the engine's channel. The host moves all voices together
     * for the wheel, so the first sounding lane's offset is the part's.
     */
    void updateBend() {
        if (!engine_) return;
        int lane = -1;
        for (unsigned v = 0; v < k_max_voices; ++v) {
            if (lane_on_[v]) {
                lane = static_cast<int>(v);
                break;
            }
        }
        if (lane < 0) return;
        float semitones = lane_pitch_[lane] - static_cast<float>(lane_key_[lane]);
        if (semitones > k_bend_range) semitones = k_bend_range;
        if (semitones < -k_bend_range) semitones = -k_bend_range;
        if (semitones > bend_ - k_bend_epsilon &&
            semitones < bend_ + k_bend_epsilon)
            return;
        bend_ = semitones;
        int value = static_cast<int>(8192.0f +
                                     semitones / k_bend_range * 8192.0f);
        if (value < 0) value = 0;
        if (value > 16383) value = 16383;
        engine_->pitchBend(0, static_cast<int16_t>(value - 8192));
    }

    void reconcileNotes() {
        for (unsigned v = 0; v < k_max_voices; ++v) {
            if (lane_on_[v] && lane_key_[v] != 0xff &&
                !engineOn_[lane_key_[v]])
                startKey(lane_key_[v], last_velocity_);
        }
        for (unsigned k = 0; k < 128; ++k) {
            if (engineOn_[k] && !laneHolds(static_cast<uint8_t>(k)))
                stopKey(static_cast<uint8_t>(k));
        }
    }

    void panic() {
        if (engine_) engine_->allSoundOff();
        clearLanes();
        res_.clear();
        last_trigger_ = 0;
    }

    void clearLanes() {
        std::memset(lane_key_, 0xff, sizeof(lane_key_));
        std::memset(lane_pitch_, 0, sizeof(lane_pitch_));
        std::memset(lane_on_, 0, sizeof(lane_on_));
        std::memset(engineOn_, 0, sizeof(engineOn_));
        std::memset(released_, 0, sizeof(released_));
        std::memset(attacked_, 0, sizeof(attacked_));
        std::memset(key_last_, kLastNone, sizeof(key_last_));
        limiter_.reset();
        bend_ = 0.f;
    }

    unsigned soundingCount() const {
        unsigned count = 0;
        for (unsigned k = 0; k < 128; ++k)
            if (engineOn_[k]) ++count;
        return count;
    }

    const unit_runtime_osc_context_t* oscContext() const {
        return static_cast<const unit_runtime_osc_context_t*>(
            runtime_desc_.hooks.runtime_context);
    }

    static uint32_t outputOffset(const unit_runtime_osc_context_t* ctxt) {
        return ctxt ? ctxt->bufferOffset : 0;
    }

    static uint32_t outputStride(const unit_runtime_osc_context_t* ctxt) {
        return (ctxt && ctxt->outputStride) ? ctxt->outputStride : 2;
    }

    static unsigned voiceLimit(const unit_runtime_osc_context_t* ctxt) {
        if (!ctxt) return 1;
        const unsigned limit = static_cast<unsigned>(ctxt->voiceLimit);
        const unsigned most = static_cast<unsigned>(kMk2MaxVoices);
        return limit < most ? limit : most;
    }

    void silence(float* out, size_t frames) {
        const unit_runtime_osc_context_t* ctxt = oscContext();
        const uint32_t stride = outputStride(ctxt);
        const size_t size = frames * static_cast<size_t>(output_channels_);
        float* const first = out + outputOffset(ctxt);
        for (size_t f = 0; f < frames; ++f) {
            const size_t at = f * stride;
            if (at >= size) break;
            first[at] = 0.f;
            if (stride >= 2 && at + 1 < size) first[at + 1] = 0.f;
        }
    }

    /* The program's bend range is set to k_bend_range for the engine, so the
     * mapping in updateBend() is always the same one. */
    void applyProgram() {
        if (!engine_) return;
        const uint8_t program = program_.load(std::memory_order_relaxed);
        const uint8_t* source = rom_.program(program);
        if (source) {
            std::memcpy(programBuf_, source, m1::kProgLen);
            disableEffects(programBuf_);
            programBuf_[32] =
                static_cast<uint8_t>(static_cast<int8_t>(k_bend_range));
        } else {
            buildProgram(programBuf_, rom_, program);
        }
        engine_->playProgram(programBuf_);
        /* playProgram() silences the engine; the held keys start again. */
        std::memset(engineOn_, 0, sizeof(engineOn_));
        bend_ = 0.f;
        res_.clear();
    }

    void logSummary(const char* when) {
        const double average = render_blocks_ ? total_block_ms_ / render_blocks_ : 0.0;
        m1unit::log("%s: %lu blocks, %lu events, max %.2fms avg %.2fms, late %lu,"
                    " peak %.4f, notes %u, voices %u",
                    when, static_cast<unsigned long>(render_blocks_),
                    static_cast<unsigned long>(note_events_), max_block_ms_, average,
                    static_cast<unsigned long>(late_blocks_), max_peak_, max_sounding_,
                    k_max_voices);
        render_blocks_ = 0;
        note_events_ = 0;
        max_block_ms_ = 0.0;
        total_block_ms_ = 0.0;
        late_blocks_ = 0;
        max_peak_ = 0.f;
        max_sounding_ = 0;
    }

    /*
     * ARM computes denormals by default and each one costs far more than a
     * normal operation. FPSCR is per thread, so this runs from Render.
     */
    static void enableFlushToZero() {
#if defined(__arm__)
        uint32_t fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));
#endif
    }

    m1unit::M1Rom rom_;
    m1::Engine* engine_ = nullptr;
    m1unit::Resampler res_;
    uint8_t programBuf_[m1::kProgLen];
    char name_[16] = {0};
    std::atomic<uint8_t> program_{0};
    uint32_t sample_rate_ = 48000;
    unsigned output_channels_ = 8;
    bool init_failed_ = false;
    bool torn_down_ = false;
    bool panic_requested_ = false;
    bool program_dirty_ = true;
    /* The trigger byte the last block saw; a new note is a fresh bit. */
    uint8_t last_trigger_ = 0;
    uint8_t last_velocity_ = k_default_velocity;
    /* Per host voice: the key it took, the pitch the context gave it, and
     * whether the host still holds it. */
    uint8_t lane_key_[kMk2MaxVoices];
    float lane_pitch_[kMk2MaxVoices];
    bool lane_on_[kMk2MaxVoices];
    /* The keys the engine is sounding. */
    bool engineOn_[128];
    /* Per block, from the callbacks. */
    uint8_t key_last_[128];
    bool released_[128];
    bool attacked_[128];
    float bend_ = 0.f;
    OutputLimiter limiter_;
    /* The engine at 31.25 kHz, resampled to the host's block. */
    float mix_[m1::kRenderBlock * 2];
    /* Diagnostics, written outside the audio path. */
    unsigned long render_blocks_ = 0;
    unsigned long note_events_ = 0;
    double max_block_ms_ = 0.0;
    double total_block_ms_ = 0.0;
    unsigned long late_blocks_ = 0;
    float max_peak_ = 0.f;
    unsigned max_sounding_ = 0;
    /* Snapshot of the descriptor: where the host keeps the context pointer. */
    unit_runtime_desc_t runtime_desc_;
    rfx::UnitNoteQueue<64> events_;
};
