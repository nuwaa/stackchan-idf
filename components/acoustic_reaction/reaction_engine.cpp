// SPDX-License-Identifier: BSL-1.0
#include <acoustic_reaction/reaction_engine.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace stackchan::acoustic {
namespace {

bool next_codepoint(std::string_view s, std::size_t& i, std::uint32_t& cp)
{
    if (i >= s.size()) return false;
    const auto c0 = static_cast<std::uint8_t>(s[i++]);
    if (c0 < 0x80) { cp = c0; return true; }
    int n = 0;
    if ((c0 & 0xe0) == 0xc0) { cp = c0 & 0x1f; n = 1; if (cp < 2) return false; }
    else if ((c0 & 0xf0) == 0xe0) { cp = c0 & 0x0f; n = 2; }
    else if ((c0 & 0xf8) == 0xf0) { cp = c0 & 0x07; n = 3; }
    else return false;
    if (i + static_cast<std::size_t>(n) > s.size()) return false;
    for (int k = 0; k < n; ++k) {
        const auto c = static_cast<std::uint8_t>(s[i++]);
        if ((c & 0xc0) != 0x80) return false;
        cp = (cp << 6) | (c & 0x3f);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return false;
    return true;
}

void append_utf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

bool contains_any(std::string_view s, std::initializer_list<std::string_view> needles)
{
    return std::any_of(needles.begin(), needles.end(), [s](auto n) { return s.find(n) != s.npos; });
}

bool has_name(std::string_view s)
{
    return contains_any(s, {"ｽﾀｯｸﾁｬﾝ", "スタックチャン", "すたっくちゃん", "stack-chan", "stackchan"});
}

} // namespace

bool valid_utf8_payload(std::span<const std::uint8_t> payload)
{
    if (payload.empty() || payload.size() > 140) return false;
    const std::string_view s(reinterpret_cast<const char*>(payload.data()), payload.size());
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint32_t cp = 0;
        if (!next_codepoint(s, i, cp)) return false;
        if (cp == 0 || cp == 0x7f || (cp < 0x20 && cp != '\n' && cp != '\r' && cp != '\t')) return false;
    }
    return true;
}

std::optional<std::string> decode_payload(std::span<const std::uint8_t> payload)
{
    if (!valid_utf8_payload(payload)) return std::nullopt;
    std::string out(reinterpret_cast<const char*>(payload.data()), payload.size());
    std::string normalized;
    normalized.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r') {
            normalized.push_back('\n');
            if (i + 1 < out.size() && out[i + 1] == '\n') ++i;
        } else normalized.push_back(out[i]);
    }
    return normalized;
}

std::string normalize_for_match(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        std::uint32_t cp = 0;
        if (!next_codepoint(text, i, cp)) return {};
        if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0; // full-width ASCII
        if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == ',' ||
            cp == '.' || cp == '!' || cp == '?' || cp == '-' || cp == 0x3000 ||
            cp == 0x3001 || cp == 0x3002 || cp == 0xff0c || cp == 0xff01 || cp == 0xff1f) {
            if (cp == '-' && !out.empty()) out.push_back('-');
            continue;
        }
        append_utf8(out, cp);
    }
    return out;
}

bool DuplicateFilter::accept(std::string_view message, std::uint32_t now_ms)
{
    std::uint32_t h = 2166136261u;
    for (const unsigned char c : message) { h ^= c; h *= 16777619u; }
    const bool duplicate = have_ && h == hash_ && static_cast<std::uint32_t>(now_ms - at_ms_) < window_ms_;
    if (!duplicate) { hash_ = h; at_ms_ = now_ms; have_ = true; }
    return !duplicate;
}

std::optional<Reaction> ReactionEngine::match(std::string_view message) const
{
    const auto s = normalize_for_match(message);
    if (s.empty() || !has_name(s)) return std::nullopt;
    // Highest-priority intent first. One message produces at most one reaction.
    if (contains_any(s, {"故障", "壊れ", "こわれ", "坏了吗", "壞了嗎"}))
        return Reaction{ReactionExpression::Sad, HeadGesture::LookDown, "こわれてないよ", U"こわれてないよ", 100};
    if (contains_any(s, {"かわいい", "可爱", "可愛", "cute"}))
        return Reaction{ReactionExpression::Happy, HeadGesture::Nod, "えへへ、ありがと", U"えへへ ありがと", 90};
    if (contains_any(s, {"元気", "げんき", "調子", "howareyou"}))
        return Reaction{ReactionExpression::Happy, HeadGesture::Nod, "げんきだよ！", U"げんきだよ", 80};
    if (contains_any(s, {"你好", "您好"}))
        return Reaction{ReactionExpression::Happy, HeadGesture::Nod, "にーはお！", U"にーはお", 70};
    if (contains_any(s, {"hello", "histack", "hiｽﾀｯｸ", "hiスタック", "hiすたっく"}))
        return Reaction{ReactionExpression::Happy, HeadGesture::Nod, "はーい！", U"はーい", 65};
    if (contains_any(s, {"こんにちは", "こんちは"}))
        return Reaction{ReactionExpression::Happy, HeadGesture::Nod, "よんだ？", U"よんだ", 60};
    return std::nullopt;
}

AudioOwner AudioDuplexGate::owner() const
{
    return phase_ == AudioPhase::Listen || phase_ == AudioPhase::Decode ? AudioOwner::Microphone
         : phase_ == AudioPhase::Speak ? AudioOwner::Speaker : AudioOwner::None;
}
bool AudioDuplexGate::begin_decode() { if (phase_ != AudioPhase::Listen) return false; phase_ = AudioPhase::Decode; return true; }
bool AudioDuplexGate::begin_reaction() { if (phase_ != AudioPhase::Decode) return false; phase_ = AudioPhase::React; return true; }
bool AudioDuplexGate::begin_speech() { if (phase_ != AudioPhase::React) return false; phase_ = AudioPhase::Speak; return true; }
bool AudioDuplexGate::finish_speech() { if (phase_ != AudioPhase::Speak) return false; phase_ = AudioPhase::Recover; return true; }
bool AudioDuplexGate::resume_listening() { if (phase_ != AudioPhase::Recover && phase_ != AudioPhase::Decode && phase_ != AudioPhase::React) return false; phase_ = AudioPhase::Listen; return true; }

} // namespace stackchan::acoustic
