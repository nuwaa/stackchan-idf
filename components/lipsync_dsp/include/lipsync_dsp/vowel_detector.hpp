// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Formant-based vowel detector for mic lip-sync.
//
// Feed 16 ms chunks of 16 kHz mono PCM; get back a mouth pose (open / form)
// plus the detected vowel and its probabilities. Pure C++ (no ESP-IDF
// dependency) so the exact same code runs in the host test against the
// Python reference (tools/vowel-lipsync/proto.py), which documents the
// algorithm step by step.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace stackchan::lipsync {

enum class Vowel : std::uint8_t { A = 0, I, U, E, O };

struct VowelResult {
    bool voiced{false};
    // Mouth pose: open 0..1, form 0 (wide) .. 1 (narrow). form < 0 means
    // "not set" (silence) so the caller can fall back to a level meter.
    float open{0.0f};
    float form{-1.0f};
    std::optional<Vowel> vowel;     // set when voiced and confident
    std::array<float, 5> probs{};   // smoothed vowel probabilities (a i u e o)
    float f1{0.0f}, f2{0.0f};       // formants in Hz, 0 when not found
    float snr_db{0.0f};
    float periodicity{0.0f};
};

class VowelDetector {
public:
    static constexpr std::size_t kChunkSamples = 256;  // 16 ms @ 16 kHz
    static constexpr std::size_t kAnalysisRate = 8000;
    static constexpr std::size_t kWin = 256;           // 32 ms @ 8 kHz
    static constexpr std::size_t kLpcOrder = 10;
    static constexpr std::size_t kEnvBins = 24;
    static constexpr std::size_t kFeatures = kEnvBins + 5;

    struct Tuning {
        float snr_voiced_db{10.0f};
        float periodicity_voiced{0.5f};
        float prob_smooth{0.5f};   // EMA coefficient on the probabilities
        float min_conf{0.4f};      // smoothed prob needed to report a vowel
        float noise_floor_min_db{-70.0f};
        float noise_down_db_s{6.0f};
        float noise_up_db_s{0.5f};
    };

    VowelDetector();
    void set_tuning(const Tuning& t) noexcept { tuning_ = t; }
    const Tuning& tuning() const noexcept { return tuning_; }
    void reset() noexcept;

    // `pcm` must hold kChunkSamples samples; `gain` scales the input.
    VowelResult push(const std::int16_t* pcm, float gain = 1.0f) noexcept;
    // Same, with float input in [-1, 1] (host tests / wav replay).
    VowelResult push(const float* pcm) noexcept;

    // Raw feature vector of the last frame (for the host test comparison).
    const std::array<float, kFeatures>& last_features() const noexcept { return feat_; }

private:
    void decimate(const float* in, float* out) noexcept;
    void analyse() noexcept;

    Tuning tuning_{};
    std::array<float, kWin> buf_{};
    std::array<float, 14> decim_hist_{};
    std::array<float, kFeatures> feat_{};
    std::array<float, kLpcOrder + 1> lpc_{};
    std::array<float, 5> p_s_{};
    float noise_db_{-60.0f};
    float cent_lf1_{0.0f}, cent_lf2_{0.0f};
    // per-frame scratch results
    float energy_{0.0f}, periodicity_{0.0f}, f1_{0.0f}, f2_{0.0f};
};

} // namespace stackchan::lipsync
