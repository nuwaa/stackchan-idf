#include <acoustic_reaction/reaction_engine.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace stackchan::acoustic;

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

void expect(std::string_view input, std::string_view output, ReactionExpression expr)
{
    const auto r = ReactionEngine{}.match(input);
    CHECK(r.has_value());
    if (r) { CHECK(r->display == output); CHECK(r->expression == expr); }
}
} // namespace

int main()
{
    expect("ｽﾀｯｸﾁｬﾝ、こんにちは", "よんだ？", ReactionExpression::Happy);
    expect("こんにちは、ｽﾀｯｸﾁｬﾝ", "よんだ？", ReactionExpression::Happy);
    expect("スタックチャン、かわいい", "えへへ、ありがと", ReactionExpression::Happy);
    expect("すたっくちゃん、元気？", "げんきだよ！", ReactionExpression::Happy);
    expect("ｽﾀｯｸﾁｬﾝ、故障したの？", "こわれてないよ", ReactionExpression::Sad);
    expect("スタックチャン、故障", "こわれてないよ", ReactionExpression::Sad);
    expect("HELLO STACK-CHAN", "はーい！", ReactionExpression::Happy);
    expect("你好，Stack-chan", "にーはお！", ReactionExpression::Happy);
    expect("您好，Stack-chan", "にーはお！", ReactionExpression::Happy);
    expect("Hi Stack-chan", "はーい！", ReactionExpression::Happy);
    expect("スタックチャン、調子は？", "げんきだよ！", ReactionExpression::Happy);
    expect("スタックチャン、壊れた？", "こわれてないよ", ReactionExpression::Sad);
    expect("スタックチャン、こんちは", "よんだ？", ReactionExpression::Happy);
    // Higher-priority failure intent wins over greeting.
    expect("こんにちは Stackchan 故障したの？", "こわれてないよ", ReactionExpression::Sad);
    CHECK(!ReactionEngine{}.match("今日はいい天気").has_value());
    CHECK(!ReactionEngine{}.match("こんにちは、別のロボット").has_value());
    CHECK(!ReactionEngine{}.match("this is Stack-chan").has_value());

    const std::string multilingual = "日本語 简体字 繁體字 English";
    CHECK(valid_utf8_payload(std::span(reinterpret_cast<const std::uint8_t*>(multilingual.data()), multilingual.size())));
    for (const char* text : {"ｽﾀｯｸﾁｬﾝ、こんにちは", "简体字：你好", "繁體字：您好",
                             "絵文字：❤️🇯🇵👍🏽"}) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(text);
        const std::span payload{bytes, std::strlen(text)};
        CHECK(valid_utf8_payload(payload));
        const auto decoded = decode_payload(payload);
        CHECK(decoded.has_value());
        CHECK(*decoded == text);
    }
    const std::array<std::uint8_t, 2> bad_utf8{0xc0, 0xaf};
    CHECK(!valid_utf8_payload(bad_utf8));
    const std::array<std::uint8_t, 3> control{'a', 0, 'b'};
    CHECK(!valid_utf8_payload(control));
    const std::array<std::uint8_t, 141> too_long{};
    CHECK(!valid_utf8_payload(too_long));

    DuplicateFilter dedupe(5000);
    CHECK(dedupe.accept("same", 100));
    CHECK(!dedupe.accept("same", 4999));
    CHECK(dedupe.accept("same", 5100));
    CHECK(dedupe.accept("other", 5101));

    AudioDuplexGate gate;
    CHECK(gate.owner() == AudioOwner::Microphone);
    CHECK(gate.begin_decode());
    CHECK(!gate.begin_speech());
    CHECK(gate.begin_reaction());
    CHECK(gate.owner() == AudioOwner::None);
    CHECK(gate.begin_speech());
    CHECK(gate.owner() == AudioOwner::Speaker);
    CHECK(!gate.begin_decode());
    CHECK(gate.finish_speech());
    CHECK(gate.owner() == AudioOwner::None);
    CHECK(gate.resume_listening());
    CHECK(gate.owner() == AudioOwner::Microphone);

    if (failures == 0) std::puts("acoustic_reaction_test: all passed");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
