// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace stackchan::acoustic {

enum class ReactionExpression : std::uint8_t { Neutral, Happy, Sad, Doubt };
enum class HeadGesture : std::uint8_t { None, Nod, LookDown, Tilt };

struct Reaction {
    ReactionExpression expression = ReactionExpression::Neutral;
    HeadGesture gesture = HeadGesture::None;
    std::string display;
    std::u32string reading;
    int priority = 0;
};

bool valid_utf8_payload(std::span<const std::uint8_t> payload);
std::optional<std::string> decode_payload(std::span<const std::uint8_t> payload);
std::string normalize_for_match(std::string_view text);

class DuplicateFilter {
public:
    explicit DuplicateFilter(std::uint32_t window_ms = 5000) : window_ms_(window_ms) {}
    bool accept(std::string_view message, std::uint32_t now_ms);
private:
    std::uint32_t hash_ = 0;
    std::uint32_t at_ms_ = 0;
    std::uint32_t window_ms_;
    bool have_ = false;
};

class ReactionEngine {
public:
    std::optional<Reaction> match(std::string_view message) const;
};

enum class AudioPhase : std::uint8_t { Listen, Decode, React, Speak, Recover };
enum class AudioOwner : std::uint8_t { None, Microphone, Speaker };

class AudioDuplexGate {
public:
    AudioPhase phase() const { return phase_; }
    AudioOwner owner() const;
    bool begin_decode();
    bool begin_reaction();
    bool begin_speech();
    bool finish_speech();
    bool resume_listening();
private:
    AudioPhase phase_ = AudioPhase::Listen;
};

} // namespace stackchan::acoustic
