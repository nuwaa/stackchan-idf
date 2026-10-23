// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Dataset-free sanity test for VowelDetector (runs in CI):
//   * silence / white noise -> not voiced, mouth closed
//   * synthetic vowels (glottal pulse train through two resonators) -> voiced,
//     F1 / F2 recovered within tolerance, mouth open

#include <lipsync_dsp/vowel_detector.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using stackchan::lipsync::VowelDetector;
using stackchan::lipsync::VowelResult;

namespace {

int g_fail = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_fail;
    }
}

// 16 kHz pulse train at f0 through resonators at (F1, F2, F3), 1 s long.
std::vector<float> synth_vowel(float f0, float f1, float f2, float f3)
{
    const std::size_t n = 16000;
    std::vector<float> x(n, 0.0f);
    const std::size_t period = static_cast<std::size_t>(16000.0f / f0);
    for (std::size_t i = 0; i < n; i += period) x[i] = 1.0f;
    const float fs = 16000.0f;
    const float freqs[3] = {f1, f2, f3};
    const float bws[3] = {80.0f, 100.0f, 150.0f};
    for (int k = 0; k < 3; ++k) {
        const float r = std::exp(-3.14159265f * bws[k] / fs);
        const float th = 2.0f * 3.14159265f * freqs[k] / fs;
        const float a1 = -2.0f * r * std::cos(th), a2 = r * r;
        float y1 = 0.0f, y2 = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const float y = x[i] - a1 * y1 - a2 * y2;
            y2 = y1;
            y1 = y;
            x[i] = y;
        }
    }
    float peak = 1e-9f;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    for (auto& v : x) v = v / peak * 0.3f;
    return x;
}

VowelResult run(VowelDetector& det, const std::vector<float>& x)
{
    VowelResult last;
    for (std::size_t i = 0; (i + 1) * VowelDetector::kChunkSamples <= x.size(); ++i) {
        last = det.push(&x[i * VowelDetector::kChunkSamples]);
    }
    return last;
}

} // namespace

int main()
{
    VowelDetector det;

    // silence
    std::vector<float> zeros(16000, 0.0f);
    VowelResult r = run(det, zeros);
    check(!r.voiced && r.open == 0.0f && r.form < 0.0f, "silence is not voiced");

    // white noise
    std::srand(1);
    std::vector<float> noise(16000);
    for (auto& v : noise) v = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * 0.2f;
    r = run(det, noise);
    check(!r.voiced, "white noise is not voiced (periodicity gate)");

    // synthetic vowels — formant recovery (F1/F2 within 12 %)
    struct Case { const char* name; float f0, f1, f2, f3; };
    const Case cases[] = {
        {"a", 120.0f, 750.0f, 1250.0f, 2600.0f},
        {"i", 120.0f, 300.0f, 2300.0f, 3000.0f},
        {"u", 120.0f, 350.0f, 1300.0f, 2400.0f},
        {"o", 120.0f, 500.0f, 850.0f, 2500.0f},
        {"a (high voice)", 240.0f, 850.0f, 1400.0f, 2900.0f},
    };
    for (const auto& c : cases) {
        det.reset();
        run(det, zeros);  // let the noise floor settle
        r = run(det, synth_vowel(c.f0, c.f1, c.f2, c.f3));
        std::printf("%-15s voiced=%d per=%.2f snr=%.1f F1=%.0f (%.0f) F2=%.0f (%.0f) open=%.2f form=%.2f probs=%.2f %.2f %.2f %.2f %.2f\n",
                    c.name, r.voiced, r.periodicity, r.snr_db, r.f1, c.f1, r.f2, c.f2, r.open, r.form,
                    r.probs[0], r.probs[1], r.probs[2], r.probs[3], r.probs[4]);
        check(r.voiced, "synthetic vowel is voiced");
        check(r.open > 0.1f, "synthetic vowel opens the mouth");
        check(std::fabs(r.f1 - c.f1) < std::max(0.12f * c.f1, 60.0f), "F1 within 12 % (or 60 Hz)");
        check(std::fabs(r.f2 - c.f2) < 0.12f * c.f2, "F2 within 12 %");
    }
    std::printf(g_fail ? "%d failure(s)\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
