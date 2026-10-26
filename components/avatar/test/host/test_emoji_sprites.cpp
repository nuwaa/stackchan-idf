#include "../../emoji_sprites.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);   \
        ++failures;                                                             \
    }                                                                           \
} while (false)

void expect_emoji(const char* text)
{
    EmojiMatch match{};
    CHECK(findEmojiSprite(reinterpret_cast<const std::uint8_t*>(text),
                          std::strlen(text), match));
    CHECK(match.bytes == std::strlen(text));
    std::array<std::uint16_t, 256> pixels{};
    decodeEmojiSprite(match.sprite, pixels.data());
    bool has_colour = false;
    for (const auto pixel : pixels) has_colour |= pixel != pixels.front();
    CHECK(has_colour);
}

} // namespace

int main()
{
    expect_emoji("❤️");
    expect_emoji("🇯🇵");
    expect_emoji("👍🏽");
    expect_emoji("👨‍👩‍👧‍👦");

    EmojiMatch match{};
    const char* plain = "スタックチャン";
    CHECK(!findEmojiSprite(reinterpret_cast<const std::uint8_t*>(plain),
                           std::strlen(plain), match));
    return failures == 0 ? 0 : 1;
}
