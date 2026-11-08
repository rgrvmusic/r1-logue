// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/*
 * Host check for the R1 microKORG2 oscillator.
 *
 * The unit is driven through the host's oscillator context: pitch[] and
 * trigger for each voice, the note callbacks beside them. Its stereo pair at
 * bufferOffset is compared, block by block, with one reference engine built
 * from the same ROMs and program and given the same notes. It checks that
 *   - a note, a chord and eight voices match the reference engine,
 *   - nothing outside the stereo pair is written,
 *   - a key released is released in the engine,
 *   - a host pitch jump starts the new key and stops the old one,
 *   - a gradual pitch movement bends the held note,
 *   - a key pressed again is one new attack, not a second note,
 *   - a tap inside one block starts nothing,
 *   - PROGRAM 1 sounds and matches its own reference,
 *   - after everything is released the pair is silence.
 */

#include "osc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static const size_t kFrames = 64;
static const uint32_t kStride = 4;
static const size_t kBlockLen = kFrames * 8;   /* the host's eight outputs */
static const float kPoison = -1234.5f;

static unit_runtime_osc_context_t g_ctxt;
static float g_block[kBlockLen];
static float g_expected[kFrames][2];
static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static void resetContext() {
    std::memset(&g_ctxt, 0, sizeof(g_ctxt));
    g_ctxt.outputStride = (uint16_t)kStride;
    g_ctxt.bufferOffset = 0;
    g_ctxt.voiceOffset = 0;
    g_ctxt.voiceLimit = kMk2MaxVoices;
}

static unit_runtime_desc_t makeDesc() {
    unit_runtime_desc_t d;
    std::memset(&d, 0, sizeof(d));
    d.samplerate = 48000;
    d.frames_per_buffer = (uint32_t)kFrames;
    d.input_channels = 0;
    d.output_channels = 8;
    d.hooks.runtime_context = &g_ctxt;
    return d;
}

/* The host tells the unit a key went down and which voice took it. */
static void press(Osc& unit, int voice, uint8_t key, uint8_t velocity = 100) {
    g_ctxt.pitch[voice] = (float)key;
    g_ctxt.trigger = (uint8_t)(g_ctxt.trigger | (1u << voice));
    unit.NoteOn(key, velocity);
}

static void release(Osc& unit, int voice, uint8_t key) {
    unit.NoteOff(key);
    g_ctxt.trigger = (uint8_t)(g_ctxt.trigger & ~(1u << voice));
}

/* One engine, driven by hand, with the same program the unit builds. */
class Ref {
public:
    bool init(int program) {
        if (!rom_.load(RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH),
                       "m1-preload.syx"))
            return false;
        if (!engine_.init(rom_.tables(), rom_.fxMem())) return false;
        engine_.setBypassFx(true);
        engine_.setVoiceLimit(kMk2MaxVoices);
        const uint8_t* source = rom_.program(program);
        if (source) {
            std::memcpy(program_, source, m1::kProgLen);
            std::memset(program_ + 38, 0, 25);
            program_[38] = 33;
            program_[39] = 33;
            program_[32] = (uint8_t)(int8_t)k_bend_range;
        } else {
            Osc::buildProgram(program_, rom_, program);
        }
        engine_.playProgram(program_);
        res_.init(m1::kSampleRate, 48000);
        lim_.reset();
        return true;
    }

    void on(uint8_t key, uint8_t velocity = 100) {
        engine_.noteOn(0, key, velocity);
    }
    void off(uint8_t key) { engine_.noteOff(0, key); }

    void bendSemitones(float semitones) {
        if (semitones > k_bend_range) semitones = k_bend_range;
        if (semitones < -k_bend_range) semitones = -k_bend_range;
        int value = (int)(8192.0f + semitones / k_bend_range * 8192.0f);
        if (value < 0) value = 0;
        if (value > 16383) value = 16383;
        engine_.pitchBend(0, (int16_t)(value - 8192));
    }

    void pull() {
        float tmp[kFrames * 2];
        res_.render(engine_, tmp, kFrames, k_output_gain);
        for (size_t i = 0; i < kFrames; ++i) {
            float l = tmp[i * 2];
            float r = tmp[i * 2 + 1];
            lim_.pair(l, r);
            g_expected[i][0] = l;
            g_expected[i][1] = r;
        }
    }

private:
    m1unit::M1Rom rom_;
    m1::Engine engine_;
    m1unit::Resampler res_;
    OutputLimiter lim_;
    uint8_t program_[m1::kProgLen];
};

/* Render one block on the unit and the same block on the reference. */
static void advance(Osc& unit, Ref& ref) {
    for (size_t i = 0; i < kBlockLen; ++i) g_block[i] = kPoison;
    unit.Render(g_block, kFrames);
    ref.pull();
}

static void compare(Osc& unit, Ref& ref, const char* what, bool report = true) {
    advance(unit, ref);

    double worst = 0.0;
    for (size_t i = 0; i < kFrames; ++i) {
        const double want_l = g_expected[i][0];
        const double want_r = g_expected[i][1];
        const double got_l = g_block[i * kStride];
        const double got_r = g_block[i * kStride + 1];
        worst = std::fmax(worst, std::fabs(got_l - want_l));
        worst = std::fmax(worst, std::fabs(got_r - want_r));
    }
    bool outside_clean = true;
    for (size_t i = 0; i < kBlockLen; ++i) {
        const bool owned = i < kFrames * kStride && (i % kStride) < 2;
        if (!owned && g_block[i] != kPoison) outside_clean = false;
    }
    if (report) std::printf("%s: worst %.3g\n", what, worst);
    check(worst < 1e-6, report ? "the stereo pair matches the reference engine"
                               : "the released pair still matches the reference");
    if (report) check(outside_clean, "nothing outside the stereo pair is written");
    else check(outside_clean, "nothing else is written while releasing");
}

static float pairPeak() {
    float peak = 0.f;
    for (size_t i = 0; i < kFrames; ++i) {
        const float a = std::fabs(g_block[i * kStride]);
        const float b = std::fabs(g_block[i * kStride + 1]);
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    return peak;
}

/* Ten compared blocks, then the tail, then one last comparison. */
static void settle(Osc& unit, Ref& ref) {
    for (int b = 0; b < 10; ++b) compare(unit, ref, "released");
    for (int b = 0; b < 790; ++b) advance(unit, ref);
    compare(unit, ref, "settled", false);
}

int main() {
    unit_runtime_desc_t desc = makeDesc();

    /* One note, a chord, eight voices, and every voice stolen. */
    {
        resetContext();
        Osc unit;
        Ref ref;
        if (unit.Init(&desc) != k_unit_err_none || !ref.init(0)) {
            std::printf("init failed (ROMs?)\n");
            return 1;
        }
        press(unit, 0, 60);
        ref.on(60);
        for (int b = 0; b < 4; ++b) compare(unit, ref, "one note");

        resetContext();
        Osc chord;
        Ref chord_ref;
        if (chord.Init(&desc) != k_unit_err_none || !chord_ref.init(0))
            return 1;
        static const uint8_t keys[4] = {60, 64, 67, 72};
        for (int v = 0; v < 4; ++v) {
            press(chord, v, keys[v]);
            chord_ref.on(keys[v]);
        }
        for (int b = 0; b < 8; ++b) compare(chord, chord_ref, "a four-note chord");

        resetContext();
        Osc eight;
        Ref eight_ref;
        if (eight.Init(&desc) != k_unit_err_none || !eight_ref.init(0))
            return 1;
        for (int v = 0; v < kMk2MaxVoices; ++v) {
            press(eight, v, (uint8_t)(60 + v));
            eight_ref.on((uint8_t)(60 + v));
        }
        for (int b = 0; b < 8; ++b) compare(eight, eight_ref, "eight voices");

        /* Every voice stolen: the bit stays up, the pitch steps. */
        static const uint8_t old_keys[8] = {60, 61, 62, 63, 64, 65, 66, 67};
        uint8_t held[8];
        std::memcpy(held, old_keys, sizeof(held));
        for (int round = 0; round < 4; ++round) {
            for (int v = 0; v < kMk2MaxVoices; ++v) {
                const uint8_t next = (uint8_t)(72 + v + 12 * round);
                g_ctxt.pitch[v] = (float)next;
                eight.NoteOn(next, 100);
                eight_ref.off(held[v]);
                eight_ref.on(next);
                held[v] = next;
            }
            char what[64];
            std::snprintf(what, sizeof(what), "eight voices stolen, round %d",
                          round + 1);
            compare(eight, eight_ref, what);
        }
        for (int v = 0; v < kMk2MaxVoices; ++v) {
            release(eight, v, held[v]);
            eight_ref.off(held[v]);
        }
        settle(eight, eight_ref);
        check(pairPeak() < 1e-4f, "everything released reaches silence");
    }

    /* A host pitch jump is a new key: the old note stops, the new one starts. */
    {
        resetContext();
        Osc unit;
        Ref ref;
        if (unit.Init(&desc) != k_unit_err_none || !ref.init(0)) return 1;
        press(unit, 0, 60);
        ref.on(60);
        for (int b = 0; b < 4; ++b) compare(unit, ref, "a held key");

        g_ctxt.pitch[0] = 84.0f;
        ref.off(60);
        ref.on(84);
        for (int b = 0; b < 4; ++b)
            compare(unit, ref, "a pitch stepped by the host");

        g_ctxt.pitch[0] = 88.0f;
        ref.off(84);
        ref.on(88);
        for (int b = 0; b < 4; ++b) compare(unit, ref, "a second step");
    }

    /* Gradual movement is the wheel: the note keeps its envelope. */
    {
        resetContext();
        Osc unit;
        Ref ref;
        if (unit.Init(&desc) != k_unit_err_none || !ref.init(0)) return 1;
        press(unit, 0, 60);
        ref.on(60);
        for (int b = 0; b < 4; ++b) compare(unit, ref, "a note before a bend");

        for (int step = 1; step <= 6; ++step) {
            g_ctxt.pitch[0] = 60.0f + 0.4f * (float)step;
            ref.bendSemitones(g_ctxt.pitch[0] - 60.0f);
            compare(unit, ref, "a pitch bent by the host");
        }
    }

    /* One key pressed again while it sounds: one new attack. */
    {
        resetContext();
        Osc unit;
        Ref ref;
        if (unit.Init(&desc) != k_unit_err_none || !ref.init(0)) return 1;
        press(unit, 0, 60);
        ref.on(60);
        for (int b = 0; b < 4; ++b) compare(unit, ref, "a note before the repeat");

        unit.NoteOn(60, 100);
        ref.off(60);
        ref.on(60);
        compare(unit, ref, "the same key pressed again");
        for (int b = 0; b < 2; ++b) compare(unit, ref, "the repeat continues");
    }

    /* A key tapped inside one block starts nothing. */
    {
        resetContext();
        Osc unit;
        Ref ref;
        if (unit.Init(&desc) != k_unit_err_none || !ref.init(0)) return 1;
        press(unit, 0, 60);
        ref.on(60);
        for (int b = 0; b < 2; ++b) compare(unit, ref, "a held note");

        unit.NoteOn(64, 100);
        unit.NoteOff(64);
        g_ctxt.pitch[1] = 64.0f;
        g_ctxt.trigger = (uint8_t)(g_ctxt.trigger | 0x02);
        compare(unit, ref, "a tap inside one block starts nothing");

        release(unit, 0, 60);
        ref.off(60);
        settle(unit, ref);
        check(pairPeak() < 1e-4f, "the tap left no note sounding");
    }

    /* PROGRAM 1 is a different sound and matches its own reference. */
    {
        resetContext();
        Osc unit;
        Ref ref;
        if (unit.Init(&desc) != k_unit_err_none) return 1;
        unit.setParameter(PARAM_PROGRAM, 1);
        if (!ref.init(1)) return 1;
        press(unit, 0, 60);
        ref.on(60);
        for (int b = 0; b < 4; ++b) compare(unit, ref, "PROGRAM 1");
    }

    std::printf("summary: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
