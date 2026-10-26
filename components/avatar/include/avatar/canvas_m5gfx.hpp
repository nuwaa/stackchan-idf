// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "avatar/canvas.hpp"

// Firmware-only concrete Canvas strategies. NOT compiled for the WASM preview
// (which supplies its own Canvas adapter over the framebuffer shim).
namespace stackchan::avatar {

// Buffered strategy: composes the whole frame into one full-screen RGB565
// sprite (PSRAM) and pushes it to the panel once per frame. Reproduces the
// original avatar rendering exactly. Drawing primitives forward straight to the
// owned sprite; the grouping / full-repaint hooks are no-ops because everything
// already composites into the single buffer.
class BufferedCanvas final : public RichCanvas {
public:
    explicit BufferedCanvas(M5GFX& panel, bool circular = false) noexcept
        : panel_{panel}, circular_{circular}
    {}

    bool is_circular() const noexcept override { return circular_; }

    // Allocate the full-screen sprite. Standalone (no display parent) + explicit
    // target on present, to dodge the CoreS3 GPIO35 MISO/DC read hang. Returns
    // false if PSRAM couldn't satisfy the allocation.
    bool begin(std::int32_t w, std::int32_t h)
    {
        sprite_.setColorDepth(16);
        sprite_.setPsram(true);
        return sprite_.createSprite(w, h) != nullptr;
    }

    std::int32_t width() const override { return sprite_.width(); }
    std::int32_t height() const override { return sprite_.height(); }

    void fillScreen(std::uint16_t color) override { sprite_.fillScreen(color); }
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::uint16_t color) override
    {
        sprite_.fillRect(x, y, w, h, color);
    }
    void fillCircle(std::int32_t x, std::int32_t y, std::int32_t r, std::uint16_t color) override
    {
        sprite_.fillCircle(x, y, r, color);
    }
    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::int32_t x2, std::int32_t y2, std::uint16_t color) override
    {
        sprite_.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    }

    void begin_group(std::int32_t, std::int32_t, std::int32_t, std::int32_t) override {}
    void end_group() override {}
    void begin_frame(std::uint16_t bg) override { sprite_.fillScreen(bg); }
    void end_frame() override { sprite_.pushSprite(&panel_, 0, 0); }
    void request_full_repaint() override {}

    std::uint16_t color565(std::uint8_t r, std::uint8_t g, std::uint8_t b) override
    {
        return sprite_.color565(r, g, b);
    }
    void fillRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint16_t color) override
    {
        sprite_.fillRoundRect(x, y, w, h, r, color);
    }
    void drawRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint16_t color) override
    {
        sprite_.drawRoundRect(x, y, w, h, r, color);
    }
    void setTextColor(std::uint16_t fg) override { sprite_.setTextColor(fg); }
    void setTextColor(std::uint16_t fg, std::uint16_t bg) override { sprite_.setTextColor(fg, bg); }
    void setFont(const lgfx::IFont* font) override { sprite_.setFont(font); }
    void setTextSize(float size) override { sprite_.setTextSize(size); }
    void setTextSize(float x_scale, float y_scale) override
    {
        sprite_.setTextSize(x_scale, y_scale);
    }
    void setTextDatum(lgfx::textdatum_t datum) override { sprite_.setTextDatum(datum); }
    void drawString(const char* str, std::int32_t x, std::int32_t y) override
    {
        sprite_.drawString(str, x, y);
    }
    std::int32_t textWidth(const char* str) override { return sprite_.textWidth(str); }
    void setClipRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override
    {
        sprite_.setClipRect(x, y, w, h);
    }
    void clearClipRect() override { sprite_.clearClipRect(); }
    void drawRgb565Image(std::int32_t x, std::int32_t y, std::int32_t w,
                         std::int32_t h, const std::uint16_t* pixels) override
    {
        sprite_.pushImage(x, y, w, h,
                          reinterpret_cast<const lgfx::rgb565_t*>(pixels));
    }

private:
    M5GFX& panel_;
    bool circular_{false};
    M5Canvas sprite_;
};

// Direct strategy (no full-screen buffer — for PSRAM-less devices).
//   - Outside a group: primitives draw straight to the panel (rule 1).
//   - Inside begin_group/end_group: primitives are composited into a small
//     internal-RAM scratch sprite (translated to the group origin) and blitted
//     once, clipped to the group rect, so multi-primitive elements and
//     size-varying rects don't flicker / leave residue (rules 2 & 3).
//   - The panel is persistent; the background is only cleared on a full repaint
//     (begin_frame after request_full_repaint), e.g. expression / layout change
//     or returning from the on-device UI.
// The scratch sprite is grown on demand (grow-only) and the blit is clipped to
// the group rect, so different-sized groups in one frame reuse one allocation.
class DirectCanvas final : public RichCanvas {
public:
    explicit DirectCanvas(M5GFX& panel, bool circular = false) noexcept
        : panel_{panel}, circular_{circular}
    {}

    bool is_circular() const noexcept override { return circular_; }

    bool begin() { return true; } // nothing to allocate up front

    std::int32_t width() const override { return panel_.width(); }
    std::int32_t height() const override { return panel_.height(); }

    void fillScreen(std::uint16_t color) override { active().fillScreen(color); }
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::uint16_t color) override
    {
        active().fillRect(x - ox_, y - oy_, w, h, color);
    }
    void fillCircle(std::int32_t x, std::int32_t y, std::int32_t r, std::uint16_t color) override
    {
        active().fillCircle(x - ox_, y - oy_, r, color);
    }
    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::int32_t x2, std::int32_t y2, std::uint16_t color) override
    {
        active().fillTriangle(x0 - ox_, y0 - oy_, x1 - ox_, y1 - oy_, x2 - ox_, y2 - oy_, color);
    }

    void begin_group(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override
    {
        // Clamp the group rect to the panel.
        std::int32_t x0 = x < 0 ? 0 : x;
        std::int32_t y0 = y < 0 ? 0 : y;
        std::int32_t x1 = x + w, y1 = y + h;
        if (x1 > panel_.width()) x1 = panel_.width();
        if (y1 > panel_.height()) y1 = panel_.height();
        gx_ = x0;
        gy_ = y0;
        gw_ = x1 - x0;
        gh_ = y1 - y0;
        if (gw_ <= 0 || gh_ <= 0) { // fully off-screen
            in_group_ = false;
            ox_ = oy_ = 0;
            return;
        }
        // Wide groups (the balloon strip) would force a large scratch; draw them
        // straight to the panel. They self-clear via their own opaque fill each
        // frame and disappear via request_full_repaint(), so no pre-clear here.
        if (gw_ > kMaxScratchWidth) {
            in_group_ = false;
            ox_ = oy_ = 0;
            return;
        }
        if (!ensure_scratch(gw_, gh_)) {
            // Scratch allocation failed (low internal RAM). Clear the region on
            // the panel first, then draw straight to it — flickers, but no
            // residue (unlike leaving the previous frame's content behind).
            panel_.fillRect(gx_, gy_, gw_, gh_, bg_);
            in_group_ = false;
            ox_ = oy_ = 0;
            return;
        }
        ox_ = gx_;
        oy_ = gy_;
        in_group_ = true;
        // Clear just the group's region of the (grow-only, possibly larger)
        // scratch to the frame background.
        scratch_.fillRect(0, 0, gw_, gh_, bg_);
    }
    void end_group() override
    {
        if (!in_group_) return; // off-screen, drawn direct, or alloc failed
        in_group_ = false;
        ox_ = oy_ = 0;
        // Blit the scratch's [0,0,gw,gh] region; the panel clip restricts the
        // (grow-only, possibly larger) sprite to exactly the group rect.
        panel_.setClipRect(gx_, gy_, gw_, gh_);
        scratch_.pushSprite(&panel_, gx_, gy_);
        panel_.clearClipRect();
    }
    void begin_frame(std::uint16_t bg) override
    {
        bg_ = bg;
        if (full_repaint_pending_) {
            panel_.fillScreen(bg);
            full_repaint_pending_ = false;
        }
    }
    void end_frame() override {} // already on the panel
    void request_full_repaint() override { full_repaint_pending_ = true; }

    std::uint16_t color565(std::uint8_t r, std::uint8_t g, std::uint8_t b) override
    {
        return panel_.color565(r, g, b);
    }
    void fillRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint16_t color) override
    {
        active().fillRoundRect(x - ox_, y - oy_, w, h, r, color);
    }
    void drawRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint16_t color) override
    {
        active().drawRoundRect(x - ox_, y - oy_, w, h, r, color);
    }
    void setTextColor(std::uint16_t fg) override { active().setTextColor(fg); }
    void setTextColor(std::uint16_t fg, std::uint16_t bg) override { active().setTextColor(fg, bg); }
    void setFont(const lgfx::IFont* font) override { active().setFont(font); }
    void setTextSize(float size) override { active().setTextSize(size); }
    void setTextSize(float x_scale, float y_scale) override
    {
        active().setTextSize(x_scale, y_scale);
    }
    void setTextDatum(lgfx::textdatum_t datum) override { active().setTextDatum(datum); }
    void drawString(const char* str, std::int32_t x, std::int32_t y) override
    {
        active().drawString(str, x - ox_, y - oy_);
    }
    std::int32_t textWidth(const char* str) override { return active().textWidth(str); }
    void setClipRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override
    {
        active().setClipRect(x - ox_, y - oy_, w, h);
    }
    void clearClipRect() override { active().clearClipRect(); }
    void drawRgb565Image(std::int32_t x, std::int32_t y, std::int32_t w,
                         std::int32_t h, const std::uint16_t* pixels) override
    {
        active().pushImage(x - ox_, y - oy_, w, h,
                           reinterpret_cast<const lgfx::rgb565_t*>(pixels));
    }

private:
    lgfx::LGFXBase& active()
    {
        return in_group_ ? static_cast<lgfx::LGFXBase&>(scratch_)
                         : static_cast<lgfx::LGFXBase&>(panel_);
    }
    bool ensure_scratch(std::int32_t w, std::int32_t h)
    {
        // Grow-only: one internal-RAM scratch ≥ the largest *narrow* group seen
        // (wide groups bypass the scratch — see begin_group). The end_group blit
        // is clipped to the group rect, so a larger scratch is fine. Grow-only
        // avoids per-frame createSprite/deleteSprite churn (which fragments the
        // heap and intermittently fails). Capped at kMaxScratchWidth wide so it
        // stays allocatable (~kMaxScratchWidth × tallest-group ≈ tens of KiB).
        if (w <= scratch_w_ && h <= scratch_h_) return true;
        const std::int32_t nw = w > scratch_w_ ? w : scratch_w_;
        const std::int32_t nh = h > scratch_h_ ? h : scratch_h_;
        scratch_.deleteSprite();
        scratch_.setColorDepth(16);
        scratch_.setPsram(false); // internal RAM — the whole point of this strategy
        if (scratch_.createSprite(nw, nh) != nullptr) {
            scratch_w_ = nw;
            scratch_h_ = nh;
            return true;
        }
        scratch_w_ = scratch_h_ = 0;
        if (!alloc_warned_) {
            alloc_warned_ = true; // log once to avoid per-frame spam
            ESP_LOGW("DirectCanvas",
                     "scratch createSprite(%d,%d)=%dKiB failed; largest free internal=%uKiB. "
                     "Falling back to direct draw (residue likely).",
                     static_cast<int>(nw), static_cast<int>(nh),
                     static_cast<int>(nw * nh * 2 / 1024),
                     static_cast<unsigned>(
                         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) /
                         1024));
        }
        return false;
    }
    bool alloc_warned_ = false;

    // Groups wider than this draw straight to the panel rather than via the
    // scratch, keeping the scratch small enough to allocate in internal RAM.
    static constexpr std::int32_t kMaxScratchWidth = 160;

    M5GFX& panel_;
    bool circular_{false};
    M5Canvas scratch_;
    std::int32_t scratch_w_ = 0, scratch_h_ = 0;
    std::uint16_t bg_ = 0;
    std::int32_t gx_ = 0, gy_ = 0, gw_ = 0, gh_ = 0; // current group rect (clamped)
    std::int32_t ox_ = 0, oy_ = 0;                    // translation origin while grouped
    bool in_group_ = false;
    bool full_repaint_pending_ = true;
};

} // namespace stackchan::avatar
