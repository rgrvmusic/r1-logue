// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

#pragma once
/**
 * 31.25 kHz to 48 kHz.
 *
 * The engine renders at 31.25 kHz (32 MHz / 1024); the host block is 48 kHz.
 * Each output sample is a 32-tap windowed-sinc interpolation. The engine is
 * pulled on demand in blocks of up to 64 input samples.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "m1_engine/m1_engine.h"

namespace m1unit {

class Resampler {
public:
    void init(uint32_t inRate, uint32_t outRate) {
        const double kPi = 3.14159265358979323846;
        step_ = (double)inRate / (double)outRate;
        coef_.assign((size_t)kPhases * kTaps, 0.0f);
        for (int p = 0; p < kPhases; ++p) {
            const double f = (double)p / (double)kPhases;
            double sum = 0.0;
            for (int t = 0; t < kTaps; ++t) {
                const double d = (double)(t - (kHalf - 1)) - f;
                const double s = d == 0.0 ? 1.0 : std::sin(kPi * d) / (kPi * d);
                const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * t / (kTaps - 1)) +
                                 0.08 * std::cos(4.0 * kPi * t / (kTaps - 1));
                const double c = s * w;
                coef_[(size_t)p * kTaps + t] = (float)c;
                sum += c;
            }
            for (int t = 0; t < kTaps; ++t) coef_[(size_t)p * kTaps + t] = (float)(coef_[(size_t)p * kTaps + t] / sum);
        }
        clear();
    }

    /* Drop the stream; the engine is cleared separately. */
    void clear() {
        l_.assign(kHalf, 0.0f);
        r_.assign(kHalf, 0.0f);
        base_ = -kHalf;
        total_ = 0;
        pos_ = 0.0;
    }

    void render(m1::Engine& engine, float* out, size_t frames, float gain) {
        size_t done = 0;
        while (done < frames) {
            const size_t n = frames - done > kBlock ? kBlock : frames - done;
            const double lastX = pos_ + (double)(n - 1) * step_;
            /* Samples up to floor(lastX) + kHalf, one each side of the tap. */
            const long need = (long)std::floor(lastX) + kHalf + 1;
            ensure(engine, need);

            for (size_t i = 0; i < n; ++i) {
                const double x = pos_ + (double)i * step_;
                long ip = (long)std::floor(x);
                double frac = x - (double)ip;
                int ph = (int)(frac * kPhases + 0.5);
                if (ph >= kPhases) {
                    ph = 0;
                    ++ip;
                }
                const float* c = &coef_[(size_t)ph * kTaps];
                const size_t at = (size_t)((ip - (kHalf - 1)) - base_);
                const float* inL = &l_[at];
                const float* inR = &r_[at];
                float sumL = 0.0f, sumR = 0.0f;
                for (int t = 0; t < kTaps; ++t) {
                    sumL += c[t] * inL[t];
                    sumR += c[t] * inR[t];
                }
                out[(done + i) * 2] = sumL * gain;
                out[(done + i) * 2 + 1] = sumR * gain;
            }
            pos_ += (double)n * step_;
            done += n;
        }
        compact();
    }

private:
    static constexpr int kTaps = 32;
    static constexpr int kHalf = 16;
    static constexpr int kPhases = 192;
    static constexpr size_t kBlock = 64;

    /* Append engine samples until `absolute` input samples exist. */
    void ensure(m1::Engine& engine, long absolute) {
        while (total_ < absolute) {
            const size_t chunk = (size_t)(absolute - total_) > kBlock
                                     ? kBlock
                                     : (size_t)(absolute - total_);
            const size_t at = l_.size();
            l_.resize(at + chunk);
            r_.resize(at + chunk);
            engine.render(&l_[at], &r_[at], (int)chunk);
            total_ += (long)chunk;
        }
    }

    /* Drop the history that no tap can reach any more. */
    void compact() {
        const long keep = (long)std::floor(pos_) - kHalf;
        if (keep <= base_) return;
        const size_t drop = (size_t)(keep - base_);
        if (drop >= l_.size()) {
            clear();
            return;
        }
        const size_t left = l_.size() - drop;
        std::memmove(l_.data(), l_.data() + drop, left * sizeof(float));
        std::memmove(r_.data(), r_.data() + drop, left * sizeof(float));
        l_.resize(left);
        r_.resize(left);
        base_ = keep;
    }

    std::vector<float> coef_;
    std::vector<float> l_, r_;
    double step_ = 1.0;
    double pos_ = 0.0;   /* absolute input position of the next output sample */
    long base_ = 0;      /* absolute input index of l_[0] */
    long total_ = 0;     /* absolute input index of the next unrendered sample */
};

}  // namespace m1unit
