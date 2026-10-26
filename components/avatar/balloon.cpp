// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "balloon.hpp"

#include "balloon_layout.hpp"
#include "emoji_sprites.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace stackchan::avatar::internal {

namespace {

// Geometry of the bottom-of-screen balloon. Panel dimensions are computed at
// draw time from the live canvas size so the same code paths drive both the
// CoreS3 (320x240) and the AtomS3R (128x128) displays.
constexpr std::int16_t kMargin = 4;
constexpr std::int16_t kPanelRadius = 10;
constexpr std::int16_t kInnerPadding = 8;
// Small panels (e.g. AtomS3R's 128x128) get the 12-px glyph; the 24-px glyph
// would eat half the screen height and leave no room for the avatar above.
constexpr std::int16_t kSmallPanelHeightThreshold = 160;
constexpr std::int16_t kBigPanelH = 40;   // for 24-px font
constexpr std::int16_t kSmallPanelH = 22; // for 12-px font

struct Utf8Glyph {
    std::uint32_t cp{};
    std::size_t bytes{};
};

Utf8Glyph decode_one(std::string_view text, std::size_t pos)
{
    const auto* p = reinterpret_cast<const std::uint8_t*>(text.data() + pos);
    const std::size_t n = text.size() - pos;
    if (n == 0) return {};
    if (p[0] < 0x80) return {p[0], 1};
    if ((p[0] & 0xe0) == 0xc0 && n >= 2)
        return {static_cast<std::uint32_t>(((p[0] & 0x1f) << 6) | (p[1] & 0x3f)), 2};
    if ((p[0] & 0xf0) == 0xe0 && n >= 3)
        return {static_cast<std::uint32_t>(((p[0] & 0x0f) << 12) |
                                          ((p[1] & 0x3f) << 6) | (p[2] & 0x3f)), 3};
    if ((p[0] & 0xf8) == 0xf0 && n >= 4)
        return {static_cast<std::uint32_t>(((p[0] & 0x07) << 18) |
                                          ((p[1] & 0x3f) << 12) |
                                          ((p[2] & 0x3f) << 6) | (p[3] & 0x3f)), 4};
    return {'?', 1};
}

std::size_t encode_utf8(std::uint32_t cp, std::array<char, 5>& out)
{
    std::size_t n = 0;
    if (cp < 0x80) out[n++] = static_cast<char>(cp);
    else if (cp < 0x800) {
        out[n++] = static_cast<char>(0xc0 | (cp >> 6));
        out[n++] = static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out[n++] = static_cast<char>(0xe0 | (cp >> 12));
        out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out[n++] = static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out[n++] = static_cast<char>(0xf0 | (cp >> 18));
        out[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out[n++] = static_cast<char>(0x80 | (cp & 0x3f));
    }
    out[n] = '\0';
    return n;
}

std::uint32_t full_width_kana(std::uint32_t cp)
{
    static constexpr std::array<std::uint16_t, 63> mapped{{
        0x3002, 0x300c, 0x300d, 0x3001, 0x30fb, 0x30f2, 0x30a1, 0x30a3,
        0x30a5, 0x30a7, 0x30a9, 0x30e3, 0x30e5, 0x30e7, 0x30c3, 0x30fc,
        0x30a2, 0x30a4, 0x30a6, 0x30a8, 0x30aa, 0x30ab, 0x30ad, 0x30af,
        0x30b1, 0x30b3, 0x30b5, 0x30b7, 0x30b9, 0x30bb, 0x30bd, 0x30bf,
        0x30c1, 0x30c4, 0x30c6, 0x30c8, 0x30ca, 0x30cb, 0x30cc, 0x30cd,
        0x30ce, 0x30cf, 0x30d2, 0x30d5, 0x30d8, 0x30db, 0x30de, 0x30df,
        0x30e0, 0x30e1, 0x30e2, 0x30e4, 0x30e6, 0x30e8, 0x30e9, 0x30ea,
        0x30eb, 0x30ec, 0x30ed, 0x30ef, 0x30f3, 0x309b, 0x309c,
    }};
    return cp >= 0xff61 && cp <= 0xff9f ? mapped[cp - 0xff61] : cp;
}

bool traditional_chinese_hint(std::uint32_t cp)
{
    switch (cp) {
    case 0x570b: // 國
    case 0x5b78: // 學
    case 0x5ee3: // 廣
    case 0x7063: // 灣
    case 0x81fa: // 臺
    case 0x842c: // 萬
    case 0x865f: // 號
    case 0x9ad4: // 體
        return true;
    default:
        return false;
    }
}

bool has_glyph(const lgfx::IFont* font, std::uint32_t cp)
{
    lgfx::FontMetrics metric{};
    return cp <= 0xffff && font->updateFontMetric(&metric, static_cast<std::uint16_t>(cp));
}

struct GlyphStyle {
    const lgfx::IFont* font{};
    float x_scale{1.0f};
    float y_scale{1.0f};
    std::uint32_t cp{};
};

GlyphStyle glyph_style(std::uint32_t cp, bool small_panel)
{
    const auto* japanese = small_panel ? &fonts::lgfxJapanGothic_12
                                       : &fonts::lgfxJapanGothic_24;
    if (cp >= 0xff61 && cp <= 0xff9f)
        return {japanese, 0.5f, 1.0f, full_width_kana(cp)};

    const auto* simplified = &fonts::efontCN_10;
    const auto* traditional = &fonts::efontTW_10;
    const float cjk_scale = small_panel ? 1.0f : 2.0f;
    if (traditional_chinese_hint(cp) && has_glyph(traditional, cp))
        return {traditional, cjk_scale, cjk_scale, cp};
    if (has_glyph(japanese, cp)) return {japanese, 1.0f, 1.0f, cp};
    if (has_glyph(simplified, cp)) return {simplified, cjk_scale, cjk_scale, cp};
    if (has_glyph(traditional, cp)) return {traditional, cjk_scale, cjk_scale, cp};
    return {japanese, 1.0f, 1.0f, cp <= 0xffff ? cp : static_cast<std::uint32_t>('?')};
}

std::int32_t glyph_width(RichCanvas& canvas, const GlyphStyle& style)
{
    std::array<char, 5> encoded{};
    encode_utf8(style.cp, encoded);
    canvas.setFont(style.font);
    canvas.setTextSize(style.x_scale, style.y_scale);
    return canvas.textWidth(encoded.data());
}

std::int32_t rich_text_width(RichCanvas& canvas, std::string_view text, bool small_panel,
                             std::int32_t emoji_px)
{
    std::int32_t width = 0;
    for (std::size_t pos = 0; pos < text.size();) {
        EmojiMatch emoji{};
        if (findEmojiSprite(reinterpret_cast<const std::uint8_t*>(text.data() + pos),
                            text.size() - pos, emoji)) {
            width += emoji_px;
            pos += emoji.bytes;
            continue;
        }
        const auto glyph = decode_one(text, pos);
        if (glyph.bytes == 0) break;
        width += glyph_width(canvas, glyph_style(glyph.cp, small_panel));
        pos += glyph.bytes;
    }
    return width;
}

void replace_emoji_background(std::array<std::uint16_t, 256>& pixels, std::uint16_t bg)
{
    if (bg == 0) return;
    std::array<std::uint16_t, 256> queue{};
    std::size_t head = 0;
    std::size_t tail = 0;
    auto enqueue = [&](std::uint16_t pos) {
        if (pixels[pos] == 0) {
            pixels[pos] = bg;
            queue[tail++] = pos;
        }
    };
    for (std::uint16_t x = 0; x < 16; ++x) {
        enqueue(x);
        enqueue(static_cast<std::uint16_t>(240 + x));
    }
    for (std::uint16_t y = 1; y < 15; ++y) {
        enqueue(static_cast<std::uint16_t>(y * 16));
        enqueue(static_cast<std::uint16_t>(y * 16 + 15));
    }
    while (head < tail) {
        const std::uint16_t pos = queue[head++];
        const std::uint16_t x = pos & 15;
        const std::uint16_t y = pos >> 4;
        if (x > 0) enqueue(static_cast<std::uint16_t>(pos - 1));
        if (x < 15) enqueue(static_cast<std::uint16_t>(pos + 1));
        if (y > 0) enqueue(static_cast<std::uint16_t>(pos - 16));
        if (y < 15) enqueue(static_cast<std::uint16_t>(pos + 16));
    }
}

void draw_rich_text(RichCanvas& canvas, std::string_view text, bool small_panel,
                    std::int32_t x, std::int32_t mid_y, std::int32_t emoji_px,
                    std::uint16_t bg)
{
    static std::array<std::uint16_t, 256> emoji_pixels{};
    for (std::size_t pos = 0; pos < text.size();) {
        EmojiMatch emoji{};
        if (findEmojiSprite(reinterpret_cast<const std::uint8_t*>(text.data() + pos),
                            text.size() - pos, emoji)) {
            decodeEmojiSprite(emoji.sprite, emoji_pixels.data());
            replace_emoji_background(emoji_pixels, bg);
            canvas.drawRgb565Image(x, mid_y - 8, 16, 16, emoji_pixels.data());
            x += emoji_px;
            pos += emoji.bytes;
            continue;
        }
        const auto glyph = decode_one(text, pos);
        if (glyph.bytes == 0) break;
        const auto style = glyph_style(glyph.cp, small_panel);
        std::array<char, 5> encoded{};
        encode_utf8(style.cp, encoded);
        canvas.setFont(style.font);
        canvas.setTextSize(style.x_scale, style.y_scale);
        canvas.drawString(encoded.data(), x, mid_y);
        x += canvas.textWidth(encoded.data());
        pos += glyph.bytes;
    }
    canvas.setTextSize(1.0f);
}

} // namespace

void draw_balloon(RichCanvas& canvas, DrawContext& ctx)
{
    if (!ctx.balloon_text.has_value()) {
        return;
    }
    const auto& text = *ctx.balloon_text;
    if (text.empty()) {
        return;
    }

    const std::uint16_t fg = ctx.palette.balloon_foreground;
    const std::uint16_t bg = ctx.palette.balloon_background;

    const std::int16_t canvas_w = static_cast<std::int16_t>(canvas.width());
    const std::int16_t canvas_h = static_cast<std::int16_t>(canvas.height());
    const bool small_panel = canvas_h <= kSmallPanelHeightThreshold;
    const auto* font = small_panel ? &fonts::lgfxJapanGothic_12 : &fonts::lgfxJapanGothic_24;
    const std::int32_t emoji_px = 16;
    const std::int16_t panel_h = small_panel ? kSmallPanelH : kBigPanelH;
    const std::int16_t panel_x = kMargin;
    const std::int16_t panel_w = canvas_w - kMargin * 2;
    const std::int16_t panel_y = canvas_h - panel_h - kMargin;

    // Composite the bottom balloon strip as one group (direct strategy clears +
    // blits the whole panel; buffered strategy treats it as a no-op).
    canvas.begin_group(panel_x, panel_y, panel_w, panel_h);
    canvas.fillRoundRect(panel_x, panel_y, panel_w, panel_h, kPanelRadius, bg);
    canvas.drawRoundRect(panel_x, panel_y, panel_w, panel_h, kPanelRadius, fg);

    canvas.setTextColor(fg, bg);
    canvas.setFont(font);
    canvas.setTextSize(1);

    const std::int32_t inner_x = panel_x + kInnerPadding;
    const std::int32_t inner_w = panel_w - 2 * kInnerPadding;
    const std::int32_t text_w = rich_text_width(canvas, text, small_panel, emoji_px);
    const std::int32_t mid_y = panel_y + panel_h / 2;
    const std::uint32_t elapsed_ms = ctx.now_ms - ctx.balloon_set_ms;

    // Left-aligned; scrolls only when the text overflows the balloon (starts at
    // the beginning of the text, then reveals the rest — see balloon_layout.hpp).
    const BalloonScroll scroll = compute_balloon_scroll(text_w, inner_w, elapsed_ms, ctx.balloon_hold_ms);
    canvas.setClipRect(inner_x, panel_y, inner_w, panel_h);
    canvas.setTextDatum(lgfx::textdatum_t::middle_left);
    draw_rich_text(canvas, text, small_panel, inner_x - scroll.offset_px, mid_y, emoji_px, bg);
    canvas.clearClipRect();
    if (scroll.done) {
        ctx.balloon_done = true;
    }
    canvas.end_group();
}

} // namespace stackchan::avatar::internal
