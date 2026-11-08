// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

/*
 * Host check for the M1 drumlogue unit: the unit's own source, driven through
 * the callbacks the device calls, with the real engine and the real ROM set.
 * It checks that
 *   - the header's three parameters hold what it declares,
 *   - a gate sounds, and the same note played as a note gives the same samples,
 *   - the note ends when the gate does,
 *   - two held keys sound together, and releasing one leaves the other,
 *   - a step's gate does not add the NOTE knob's note under the step's note,
 *   - PROGRAM changes the sound and LEVEL's ends are silence and full scale,
 * and it reports the render time against the device's 1.33 ms block.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "unit.h"

static const uint32_t kFrames = 64;          /* the drumlogue block */
static const double kDeadlineMs = kFrames / 48.0;   /* 1.33 ms */
static int g_checks = 0;
static int g_failures = 0;

struct Result {
    double peak;      /* the loudest sample of the whole run */
    double last;      /* the loudest sample of the last block */
    bool finite;
};

static void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

static unit_runtime_desc_t makeDesc() {
    unit_runtime_desc_t d;
    std::memset(&d, 0, sizeof d);
    d.target = unit_header.target;
    d.api = UNIT_API_VERSION;
    d.samplerate = 48000;
    d.frames_per_buffer = kFrames;
    d.input_channels = 2;
    d.output_channels = 2;
    return d;
}

static bool start() {
    unit_runtime_desc_t d = makeDesc();
    return unit_init(&d) == k_unit_err_none;
}

static Result render(int blocks, std::vector<float>* capture = nullptr) {
    std::vector<float> in(kFrames * 2, 0.0f);
    std::vector<float> out(kFrames * 2);
    Result r = {0.0, 0.0, true};
    for (int b = 0; b < blocks; ++b) {
        unit_render(in.data(), out.data(), kFrames);
        double last = 0.0;
        for (float v : out) {
            const double a = std::fabs(v);
            if (!std::isfinite(v)) r.finite = false;
            if (a > last) last = a;
            if (a > r.peak) r.peak = a;
        }
        r.last = last;
        if (capture) capture->insert(capture->end(), out.begin(), out.end());
    }
    return r;
}

static double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double worst = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        worst = std::fmax(worst, std::fabs(a[i] - b[i]));
    return worst;
}

static double rms(const std::vector<float>& v) {
    double sum = 0.0;
    for (float s : v) sum += static_cast<double>(s) * s;
    return v.empty() ? 0.0 : std::sqrt(sum / v.size());
}

/* One second of four held notes, timed block by block. */
static void timing() {
    using clock = std::chrono::steady_clock;
    start();
    unit_note_on(48, 100);
    unit_note_on(55, 100);
    unit_note_on(60, 100);
    unit_note_on(64, 100);
    render(10);                     /* fill the resampler */
    const int blocks = 750;         /* 1 s at 48 kHz */
    std::vector<float> in(kFrames * 2, 0.0f);
    std::vector<float> out(kFrames * 2);
    double total = 0.0, worst = 0.0;
    int late = 0;
    for (int b = 0; b < blocks; ++b) {
        const clock::time_point t0 = clock::now();
        unit_render(in.data(), out.data(), kFrames);
        const double ms =
            std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        total += ms;
        if (ms > worst) worst = ms;
        if (ms > kDeadlineMs) ++late;
    }
    unit_teardown();
    const double average = total / blocks;
    std::printf("  timing: 4 voices, block %.0f frames at 48 kHz (%.2f ms)\n",
                (double)kFrames, kDeadlineMs);
    std::printf("          avg %.3f ms, max %.3f ms, %.0f%% of the block, %d late\n",
                average, worst, 100.0 * average / kDeadlineMs, late);
}

int main() {
    if (!start()) {
        std::printf("init failed (ROMs?)\n");
        return 1;
    }
    check(unit_get_param_value(0) == 0 && unit_get_param_value(1) == 80 &&
              unit_get_param_value(2) == 60,
          "the parameters start at 0, 80, 60");
    const char* program0 = unit_get_param_str_value(0, 0);
    check(program0 && program0[0] && std::strcmp(program0, "---") != 0,
          "PROGRAM 0 has a name");
    std::printf("          PROGRAM 0 is \"%s\"\n", program0 ? program0 : "(null)");
    unit_teardown();

    /* The gate's note and the same note played as a note are one path. */
    std::vector<float> gate, note;
    start();
    unit_gate_on(100);
    Result g = render(100, &gate);
    unit_teardown();
    start();
    unit_note_on(60, 100);
    Result n = render(100, &note);
    unit_teardown();
    check(g.peak > 0.02, "the gate sounds");
    check(n.peak > 0.02, "note 60 sounds");
    std::printf("          one note peaks at %.4f (LEVEL 80)\n", n.peak);
    check(g.finite && n.finite && g.peak <= 1.0,
          "every sample is finite and inside the device's range");
    check(maxDiff(gate, note) < 1e-6, "gate and NOTE 60 are the same samples");

    /* The gate holds the note only until it ends. */
    start();
    unit_gate_on(100);
    render(40);
    unit_gate_off();
    Result off = render(600);
    unit_teardown();
    check(off.finite && off.last < 1e-4, "the note ends when the gate does");

    /* Two keys held together are two notes. The second capture is the same
     * stretch of the first note with a second one added, so it must carry
     * more energy. */
    std::vector<float> no_second, with_second;
    start();
    unit_note_on(60, 100);
    render(20);
    render(20, &no_second);
    unit_teardown();
    start();
    unit_note_on(60, 100);
    render(20);
    unit_note_on(70, 100);
    render(20, &with_second);
    unit_note_off(70);
    Result back = render(20);
    unit_note_off(60);
    const Result mono = render(600);
    unit_teardown();
    check(rms(with_second) > rms(no_second) * 1.2, "a second key adds a second note");
    check(back.finite && back.peak > 0.005, "the held key keeps sounding");
    check(mono.finite && mono.last < 1e-4, "no note is left sounding");

    /* A sequencer step sends its NoteOn and then its GateOn. The gate must
     * not add the NOTE knob's note under the step's note, and the step's note
     * must end with the gate. */
    std::vector<float> step_note, step_note_gate;
    start();
    unit_note_on(72, 100);
    render(60, &step_note);
    unit_teardown();
    start();
    unit_note_on(72, 100);
    unit_gate_on(100);
    render(60, &step_note_gate);
    unit_teardown();
    check(maxDiff(step_note, step_note_gate) < 1e-6,
          "a step's gate does not add the NOTE knob note");
    start();
    unit_note_on(72, 100);
    unit_gate_on(100);
    render(20);
    unit_gate_off();
    const Result after_gate = render(600);
    unit_teardown();
    check(after_gate.finite && after_gate.last < 1e-4,
          "a step's note ends with its gate");

    /* PROGRAM changes the sound. */
    std::vector<float> p0, p1;
    start();
    unit_note_on(60, 100);
    render(60, &p0);
    unit_teardown();
    start();
    unit_set_param_value(0, 1);
    unit_note_on(60, 100);
    render(60, &p1);
    unit_teardown();
    check(maxDiff(p0, p1) > 0.01, "PROGRAM 1 sounds different from PROGRAM 0");

    /* LEVEL's ends: silence and full scale, against the default 80. */
    start();
    unit_set_param_value(1, 0);
    unit_note_on(60, 100);
    Result l0 = render(60);
    unit_teardown();
    start();
    unit_set_param_value(1, 100);
    unit_note_on(60, 100);
    Result l100 = render(60);
    unit_teardown();
    check(l0.finite && l0.peak == 0.0, "LEVEL 0 is silence");
    check(l100.peak > n.peak, "LEVEL 100 is louder than the default 80");

    timing();

    std::printf("summary: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
