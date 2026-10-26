#include <acoustic_reaction/ggwave_receiver.hpp>
#include <ggwave/ggwave.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

using stackchan::acoustic::GgWaveReceiver;

int main()
{
    const std::string message = "你好，Stack-chan";
    auto p = GGWave::getDefaultParameters();
    p.payloadLength = -1;
    p.sampleRateInp = p.sampleRateOut = p.sampleRate = GgWaveReceiver::kSampleRate;
    p.samplesPerFrame = GgWaveReceiver::kFrameSamples;
    p.sampleFormatInp = p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
    p.operatingMode = GGWAVE_OPERATING_MODE_TX;
    GGWave::Protocols::tx().only(GGWAVE_PROTOCOL_AUDIBLE_NORMAL);
    GGWave tx;
    if (!tx.prepare(p) || !tx.init(static_cast<int>(message.size()), message.data(),
                                   GGWAVE_PROTOCOL_AUDIBLE_NORMAL, 80)) return 1;
    const auto bytes = tx.encode();
    if (bytes == 0 || (bytes % sizeof(std::int16_t)) != 0 || tx.txWaveform() == nullptr) return 2;

    GgWaveReceiver rx;
    if (!rx.begin()) return 3;
    const auto* samples = static_cast<const std::int16_t*>(tx.txWaveform());
    const std::size_t count = bytes / sizeof(std::int16_t);
    std::array<std::uint8_t, GgWaveReceiver::kMaxPayloadBytes> decoded{};
    int result = 0;
    for (std::size_t off = 0; off < count && result <= 0; off += 173) {
        const auto n = std::min<std::size_t>(173, count - off);
        result = rx.feed(std::span(samples + off, n), decoded);
    }
    if (result != static_cast<int>(message.size()) ||
        std::memcmp(decoded.data(), message.data(), message.size()) != 0) {
        std::fprintf(stderr, "roundtrip failed: result=%d expected=%zu\n", result, message.size());
        return 4;
    }
    std::puts("ggwave_roundtrip_test: all passed");
    return 0;
}
