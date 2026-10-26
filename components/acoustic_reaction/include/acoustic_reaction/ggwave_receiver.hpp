// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace stackchan::acoustic {

class GgWaveReceiver {
public:
    static constexpr std::size_t kFrameSamples = 512;
    static constexpr std::size_t kMaxPayloadBytes = 140;
    static constexpr std::uint32_t kSampleRate = 24'000;

    GgWaveReceiver();
    ~GgWaveReceiver();
    GgWaveReceiver(const GgWaveReceiver&) = delete;
    GgWaveReceiver& operator=(const GgWaveReceiver&) = delete;

    bool begin();
    void reset();
    // >0: decoded byte count, 0: no complete message, -1: decoder error,
    // -2: payload larger than the supplied output buffer.
    int feed(std::span<const std::int16_t> pcm, std::span<std::uint8_t> output);
    int heap_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stackchan::acoustic
