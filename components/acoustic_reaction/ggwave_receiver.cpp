// SPDX-License-Identifier: BSL-1.0
#include <acoustic_reaction/ggwave_receiver.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

#include <ggwave/ggwave.h>

namespace stackchan::acoustic {

struct GgWaveReceiver::Impl {
    std::unique_ptr<GGWave> modem;
    std::array<std::int16_t, kFrameSamples> frame{};
    std::size_t count = 0;
};

namespace {
GGWave::Parameters receiver_parameters()
{
    auto p = GGWave::getDefaultParameters();
    p.payloadLength = -1;
    p.sampleRateInp = GgWaveReceiver::kSampleRate;
    p.sampleRateOut = GgWaveReceiver::kSampleRate;
    p.sampleRate = GgWaveReceiver::kSampleRate;
    p.samplesPerFrame = static_cast<int>(GgWaveReceiver::kFrameSamples);
    p.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
    p.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
    p.operatingMode = GGWAVE_OPERATING_MODE_RX;
    p.soundMarkerThreshold = 1.5f;
    return p;
}
} // namespace

GgWaveReceiver::GgWaveReceiver() : impl_(std::make_unique<Impl>()) {}
GgWaveReceiver::~GgWaveReceiver() = default;

bool GgWaveReceiver::begin()
{
    reset();
    GGWave::setLogFile(nullptr);
    // The exhibition web app uses ggwave 0.4.0 variable-length framing and
    // Audible Normal protocol 0. Do not accept the project's private DT mode.
    GGWave::Protocols::rx().disableAll();
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_AUDIBLE_NORMAL, true);
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_AUDIBLE_FAST, true);
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_AUDIBLE_FASTEST, true);
    GGWave::Protocols::tx().disableAll();
    auto modem = std::unique_ptr<GGWave>(new (std::nothrow) GGWave());
    if (!modem || !modem->prepare(receiver_parameters())) return false;
    impl_->modem = std::move(modem);
    return true;
}

void GgWaveReceiver::reset()
{
    impl_->modem.reset();
    impl_->count = 0;
    impl_->frame.fill(0);
}

int GgWaveReceiver::feed(std::span<const std::int16_t> pcm,
                         std::span<std::uint8_t> output)
{
    if (!impl_->modem || output.empty()) return -1;
    std::size_t offset = 0;
    while (offset < pcm.size()) {
        const auto n = std::min(kFrameSamples - impl_->count, pcm.size() - offset);
        std::copy_n(pcm.data() + offset, n, impl_->frame.data() + impl_->count);
        impl_->count += n;
        offset += n;
        if (impl_->count != kFrameSamples) continue;
        impl_->count = 0;
        if (!impl_->modem->decode(impl_->frame.data(), sizeof(impl_->frame))) return -1;
        std::array<std::uint8_t, kMaxPayloadBytes> decoded{};
        GGWave::TxRxData data(decoded.data(), decoded.size());
        const int length = impl_->modem->rxTakeData(data);
        if (length <= 0) continue;
        if (length > static_cast<int>(kMaxPayloadBytes) ||
            length > static_cast<int>(output.size())) return -2;
        // rxTakeData() rebinds the lightweight TxRxData view to the modem's
        // decoded storage (it does not copy into `decoded`). Copy from that
        // returned view before the next decode call reuses the storage.
        std::memcpy(output.data(), data.data(), static_cast<std::size_t>(length));
        return length;
    }
    return 0;
}

int GgWaveReceiver::heap_bytes() const
{
    return impl_->modem ? impl_->modem->heapSize() : 0;
}

} // namespace stackchan::acoustic
