// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "lipsync_dsp/vowel_detector.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "vowel_model.hpp"

namespace stackchan::lipsync {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr std::size_t kDecimTaps = 15;
constexpr float kPreemph = 0.95f;
constexpr float kEnvLoHz = 200.0f, kEnvHiHz = 3500.0f;
constexpr std::size_t kPitchLagLo = 20, kPitchLagHi = 133;   // 400 Hz .. 60 Hz
constexpr float kFMin = 150.0f, kFMax = 3500.0f, kBwMax = 500.0f;
constexpr float kF1Lo = 200.0f, kF1Hi = 1100.0f, kF2Hi = 3200.0f, kF2Gap = 150.0f;
constexpr float kAdaptFrames = 150.0f;
constexpr float kChunkSec = 256.0f / 16000.0f;

// Mouth pose per vowel (open, form) — same table as main/speech.cpp.
constexpr std::array<std::array<float, 2>, 5> kMouth{{
    {1.00f, 0.30f},  // a
    {0.25f, 0.00f},  // i
    {0.30f, 1.00f},  // u
    {0.55f, 0.15f},  // e
    {0.75f, 0.85f},  // o
}};

// Default formant prototypes; their log-centroid seeds the speaker adaptation.
constexpr std::array<std::array<float, 2>, 5> kProtoF{{
    {800.0f, 1300.0f}, {300.0f, 2300.0f}, {350.0f, 1300.0f}, {500.0f, 1950.0f}, {480.0f, 850.0f}}};

struct Tables {
    std::array<float, kDecimTaps> fir{};
    std::array<float, VowelDetector::kWin> window{};
    std::array<std::array<float, VowelDetector::kLpcOrder + 1>, VowelDetector::kEnvBins> cos_t{};
    std::array<std::array<float, VowelDetector::kLpcOrder + 1>, VowelDetector::kEnvBins> sin_t{};
    float cent_lf1{0.0f}, cent_lf2{0.0f};

    Tables()
    {
        // Half-band windowed-sinc low-pass (cutoff fs/4), Hamming window,
        // normalised to unity DC gain — identical to proto._decim_fir.
        float sum = 0.0f;
        for (std::size_t i = 0; i < kDecimTaps; ++i) {
            const float t = static_cast<float>(i) - static_cast<float>(kDecimTaps - 1) / 2.0f;
            const float x = 0.5f * t;
            const float sinc = (std::fabs(x) < 1e-6f) ? 1.0f : std::sin(kPi * x) / (kPi * x);
            const float ham = 0.54f - 0.46f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                                      static_cast<float>(kDecimTaps - 1));
            fir[i] = 0.5f * sinc * ham;
            sum += fir[i];
        }
        for (auto& v : fir) v /= sum;
        // numpy.hanning(N): 0.5 - 0.5 cos(2 pi n / (N-1))
        for (std::size_t i = 0; i < window.size(); ++i) {
            window[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                               static_cast<float>(window.size() - 1));
        }
        // log-spaced envelope grid
        const float llo = std::log(kEnvLoHz), lhi = std::log(kEnvHiHz);
        for (std::size_t k = 0; k < VowelDetector::kEnvBins; ++k) {
            const float hz = std::exp(llo + (lhi - llo) * static_cast<float>(k) /
                                                static_cast<float>(VowelDetector::kEnvBins - 1));
            const float w = 2.0f * kPi * hz / static_cast<float>(VowelDetector::kAnalysisRate);
            for (std::size_t m = 0; m <= VowelDetector::kLpcOrder; ++m) {
                cos_t[k][m] = std::cos(w * static_cast<float>(m));
                sin_t[k][m] = std::sin(w * static_cast<float>(m));
            }
        }
        for (const auto& p : kProtoF) {
            cent_lf1 += std::log(p[0]) / 5.0f;
            cent_lf2 += std::log(p[1]) / 5.0f;
        }
    }
};

const Tables& tables()
{
    static const Tables t;
    return t;
}

// Levinson-Durbin: autocorrelation r[0..p] -> a[0..p] with a[0] = 1.
void levinson(const float* r, std::size_t p, float* a)
{
    std::array<float, VowelDetector::kLpcOrder + 1> tmp{};
    std::fill(a, a + p + 1, 0.0f);
    a[0] = 1.0f;
    float err = r[0];
    if (err <= 0.0f) return;
    for (std::size_t i = 1; i <= p; ++i) {
        float acc = r[i];
        for (std::size_t j = 1; j < i; ++j) acc += a[j] * r[i - j];
        const float k = -acc / err;
        for (std::size_t j = 1; j < i; ++j) tmp[j] = a[j] + k * a[i - j];
        for (std::size_t j = 1; j < i; ++j) a[j] = tmp[j];
        a[i] = k;
        err *= (1.0f - k * k);
        if (err <= 1e-12f) break;
    }
}

// Roots of z^p + a1 z^(p-1) + ... + ap by Durand-Kerner iteration. Fine for
// LPC polynomials (all roots inside the unit circle, well separated).
std::size_t lpc_roots(const float* a, std::size_t p, std::complex<float>* roots)
{
    using C = std::complex<float>;
    const C seed(0.4f, 0.9f);
    C z = C(1.0f, 0.0f);
    for (std::size_t i = 0; i < p; ++i) {
        z *= seed;
        roots[i] = z;
    }
    for (int iter = 0; iter < 60; ++iter) {
        float max_step = 0.0f;
        for (std::size_t i = 0; i < p; ++i) {
            // evaluate polynomial at roots[i] (Horner)
            C num = C(1.0f, 0.0f);
            for (std::size_t m = 1; m <= p; ++m) num = num * roots[i] + a[m];
            C den = C(1.0f, 0.0f);
            for (std::size_t j = 0; j < p; ++j) {
                if (j != i) den *= (roots[i] - roots[j]);
            }
            if (std::abs(den) < 1e-12f) den = C(1e-12f, 0.0f);
            const C step = num / den;
            roots[i] -= step;
            max_step = std::max(max_step, std::abs(step));
        }
        if (max_step < 1e-6f) break;
    }
    return p;
}

} // namespace

VowelDetector::VowelDetector()
{
    (void)tables();
    reset();
}

void VowelDetector::reset() noexcept
{
    buf_.fill(0.0f);
    decim_hist_.fill(0.0f);
    feat_.fill(0.0f);
    lpc_.fill(0.0f);
    p_s_.fill(0.2f);
    noise_db_ = -60.0f;
    cent_lf1_ = tables().cent_lf1;
    cent_lf2_ = tables().cent_lf2;
    energy_ = periodicity_ = f1_ = f2_ = 0.0f;
}

// 256 samples @ 16 kHz -> 128 samples @ 8 kHz (FIR then drop odd samples).
void VowelDetector::decimate(const float* in, float* out) noexcept
{
    const auto& fir = tables().fir;
    // x = [hist(14) | in(256)]; y[n] = sum_k fir[k] * x[n + 14 - k], n = 0..255
    std::array<float, kDecimTaps - 1 + kChunkSamples> x{};
    std::copy(decim_hist_.begin(), decim_hist_.end(), x.begin());
    std::copy(in, in + kChunkSamples, x.begin() + (kDecimTaps - 1));
    for (std::size_t n = 0; n < kChunkSamples; n += 2) {
        float acc = 0.0f;
        for (std::size_t k = 0; k < kDecimTaps; ++k) acc += fir[k] * x[n + (kDecimTaps - 1) - k];
        out[n / 2] = acc;
    }
    std::copy(x.end() - (kDecimTaps - 1), x.end(), decim_hist_.begin());
}

void VowelDetector::analyse() noexcept
{
    const auto& tb = tables();
    const float* x = buf_.data();

    // energy
    float e = 0.0f;
    for (float v : buf_) e += v * v;
    energy_ = e / static_cast<float>(kWin);

    // voicing: NCC over pitch lags
    float best = 0.0f;
    for (std::size_t k = kPitchLagLo; k <= kPitchLagHi; ++k) {
        float aa = 0.0f, bb = 0.0f, ab = 0.0f;
        const std::size_t n = kWin - k;
        for (std::size_t i = 0; i < n; ++i) {
            aa += x[i] * x[i];
            bb += x[i + k] * x[i + k];
            ab += x[i] * x[i + k];
        }
        const float den = std::sqrt(aa * bb) + 1e-12f;
        best = std::max(best, ab / den);
    }
    periodicity_ = best;

    // pre-emphasis + window, autocorrelation, LPC
    std::array<float, kWin> y{};
    y[0] = x[0] * tb.window[0];
    for (std::size_t i = 1; i < kWin; ++i) y[i] = (x[i] - kPreemph * x[i - 1]) * tb.window[i];
    std::array<float, kLpcOrder + 1> r{};
    for (std::size_t k = 0; k <= kLpcOrder; ++k) {
        float acc = 0.0f;
        for (std::size_t i = 0; i + k < kWin; ++i) acc += y[i] * y[i + k];
        r[k] = acc;
    }
    r[0] *= 1.0001f;
    levinson(r.data(), kLpcOrder, lpc_.data());

    // LPC envelope on the log grid, mean removed
    float mean = 0.0f;
    for (std::size_t k = 0; k < kEnvBins; ++k) {
        float re = 0.0f, im = 0.0f;
        for (std::size_t m = 0; m <= kLpcOrder; ++m) {
            re += tb.cos_t[k][m] * lpc_[m];
            im -= tb.sin_t[k][m] * lpc_[m];
        }
        const float mag2 = std::max(re * re + im * im, 1e-9f);
        feat_[k] = -10.0f * std::log10(mag2);
        mean += feat_[k];
    }
    mean /= static_cast<float>(kEnvBins);
    for (std::size_t k = 0; k < kEnvBins; ++k) feat_[k] -= mean;

    // formants from the LPC roots
    std::array<std::complex<float>, kLpcOrder> roots{};
    lpc_roots(lpc_.data(), kLpcOrder, roots.data());
    std::array<float, kLpcOrder> fm{};
    std::size_t nf = 0;
    for (const auto& rt : roots) {
        if (rt.imag() <= 0.0f) continue;
        const float f = std::atan2(rt.imag(), rt.real()) * static_cast<float>(kAnalysisRate) / (2.0f * kPi);
        const float bw = -std::log(std::max(std::abs(rt), 1e-9f)) * static_cast<float>(kAnalysisRate) / kPi;
        if (f >= kFMin && f <= kFMax && bw < kBwMax) fm[nf++] = f;
    }
    std::sort(fm.begin(), fm.begin() + nf);
    f1_ = f2_ = 0.0f;
    for (std::size_t i = 0; i < nf; ++i) {
        if (f1_ == 0.0f && fm[i] >= kF1Lo && fm[i] <= kF1Hi) {
            f1_ = fm[i];
        } else if (f1_ != 0.0f && fm[i] >= f1_ + kF2Gap && fm[i] <= kF2Hi) {
            f2_ = fm[i];
            break;
        }
    }
}

VowelResult VowelDetector::push(const std::int16_t* pcm, float gain) noexcept
{
    std::array<float, kChunkSamples> f{};
    const float s = gain / 32768.0f;
    for (std::size_t i = 0; i < kChunkSamples; ++i) f[i] = static_cast<float>(pcm[i]) * s;
    return push(f.data());
}

VowelResult VowelDetector::push(const float* pcm) noexcept
{
    // shift in the decimated chunk
    std::copy(buf_.begin() + kChunkSamples / 2, buf_.end(), buf_.begin());
    decimate(pcm, buf_.data() + (kWin - kChunkSamples / 2));
    analyse();

    VowelResult res;
    const float e_db = 10.0f * std::log10(energy_ + 1e-12f);
    if (e_db < noise_db_) {
        noise_db_ = std::max(noise_db_ - tuning_.noise_down_db_s * kChunkSec, e_db);
    } else {
        noise_db_ += tuning_.noise_up_db_s * kChunkSec;
    }
    noise_db_ = std::max(noise_db_, tuning_.noise_floor_min_db);
    res.snr_db = e_db - noise_db_;
    res.periodicity = periodicity_;
    res.voiced = res.snr_db > tuning_.snr_voiced_db && periodicity_ > tuning_.periodicity_voiced;

    // formant features (speaker-normalised log F1/F2 + missing flag) and
    // the speaker centroid's offset from the reference (voice depth)
    if (res.voiced && f1_ > 0.0f && f2_ > 0.0f) {
        const float lf1 = std::log(f1_), lf2 = std::log(f2_);
        cent_lf1_ += (lf1 - cent_lf1_) / kAdaptFrames;
        cent_lf2_ += (lf2 - cent_lf2_) / kAdaptFrames;
        feat_[kEnvBins + 0] = lf1 - cent_lf1_;
        feat_[kEnvBins + 1] = lf2 - cent_lf2_;
        feat_[kEnvBins + 2] = 0.0f;
        res.f1 = f1_;
        res.f2 = f2_;
    } else {
        feat_[kEnvBins + 0] = 0.0f;
        feat_[kEnvBins + 1] = 0.0f;
        feat_[kEnvBins + 2] = 1.0f;
    }
    feat_[kEnvBins + 3] = cent_lf1_ - tables().cent_lf1;
    feat_[kEnvBins + 4] = cent_lf2_ - tables().cent_lf2;

    // MLP -> probabilities (only on voiced frames), EMA smoothing
    if (res.voiced) {
        std::array<float, model::kHidden> h{};
        for (std::size_t j = 0; j < model::kHidden; ++j) {
            float acc = model::kB1[j];
            for (std::size_t i = 0; i < model::kInputs; ++i) {
                acc += (feat_[i] - model::kMu[i]) / model::kSd[i] * model::kW1[i * model::kHidden + j];
            }
            h[j] = std::max(acc, 0.0f);
        }
        std::array<float, 5> z{};
        float zmax = -1e30f;
        for (std::size_t k = 0; k < 5; ++k) {
            float acc = model::kB2[k];
            for (std::size_t j = 0; j < model::kHidden; ++j) acc += h[j] * model::kW2[j * 5 + k];
            z[k] = acc;
            zmax = std::max(zmax, acc);
        }
        float sum = 0.0f;
        for (std::size_t k = 0; k < 5; ++k) {
            z[k] = std::exp(z[k] - zmax);
            sum += z[k];
        }
        for (std::size_t k = 0; k < 5; ++k) p_s_[k] += (z[k] / sum - p_s_[k]) * tuning_.prob_smooth;
    } else {
        for (auto& p : p_s_) p += (0.2f - p) * tuning_.prob_smooth;
    }
    res.probs = p_s_;

    if (res.voiced) {
        std::size_t best = 0;
        float psum = 0.0f;
        for (std::size_t k = 0; k < 5; ++k) {
            psum += p_s_[k];
            if (p_s_[k] > p_s_[best]) best = k;
        }
        if (p_s_[best] > tuning_.min_conf) res.vowel = static_cast<Vowel>(best);
        float open = 0.0f, form = 0.0f;
        for (std::size_t k = 0; k < 5; ++k) {
            const float w = p_s_[k] / psum;
            open += w * kMouth[k][0];
            form += w * kMouth[k][1];
        }
        res.open = open;
        res.form = form;
    }
    return res;
}

} // namespace stackchan::lipsync
