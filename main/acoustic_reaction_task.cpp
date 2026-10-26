// SPDX-License-Identifier: BSL-1.0
#include "acoustic_reaction_task.hpp"

#include "speech.hpp"

#include <acoustic_reaction/ggwave_receiver.hpp>
#include <acoustic_reaction/reaction_engine.hpp>
#include <avatar/expression.hpp>
#include <config_service/task_stack.hpp>

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace stackchan::app {
namespace {

constexpr const char* kTag = "acoustic-rx";
constexpr TickType_t kAudioSettle = pdMS_TO_TICKS(30);
constexpr std::uint32_t kMicSampleRate = 48'000;

struct TaskArgs {
    SharedState* state;
    std::string jtts_config_json;
};

int avatar_expression(acoustic::ReactionExpression e)
{
    using avatar::Expression;
    switch (e) {
    case acoustic::ReactionExpression::Happy: return static_cast<int>(Expression::Happy);
    case acoustic::ReactionExpression::Sad:   return static_cast<int>(Expression::Sad);
    case acoustic::ReactionExpression::Doubt: return static_cast<int>(Expression::Doubt);
    case acoustic::ReactionExpression::Neutral: break;
    }
    return static_cast<int>(Expression::Neutral);
}

void apply_gesture(SharedState& state, acoustic::HeadGesture g)
{
    // Complete the gesture before speech starts. The servo task deliberately
    // ignores goal updates while `masked` is true to protect audio from power
    // rail dips, so a gesture must be a visible timed sequence here rather
    // than a single target immediately followed by the speech mask.
    state.servo.masked.store(false, std::memory_order_release);
    switch (g) {
    case acoustic::HeadGesture::Nod:
        state.servo.speed_override.store(1000, std::memory_order_relaxed);
        state.servo.target_pitch_deg.store(-20.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(750));
        state.servo.target_pitch_deg.store(20.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(450));
        state.servo.target_pitch_deg.store(-20.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(650));
        state.servo.target_pitch_deg.store(20.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(450));
        state.servo.target_pitch_deg.store(-20.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(700));
        break;
    case acoustic::HeadGesture::LookDown:
        state.servo.speed_override.store(900, std::memory_order_relaxed);
        state.servo.target_pitch_deg.store(25.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(1000));
        state.servo.target_yaw_deg.store(-25.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(450));
        state.servo.target_yaw_deg.store(25.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(450));
        state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(350));
        break;
    case acoustic::HeadGesture::Tilt:
        state.servo.speed_override.store(1000, std::memory_order_relaxed);
        state.servo.target_yaw_deg.store(-35.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(500));
        state.servo.target_yaw_deg.store(35.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(650));
        state.servo.target_yaw_deg.store(-35.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(650));
        state.servo.target_yaw_deg.store(35.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(650));
        state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(500));
        break;
    case acoustic::HeadGesture::None: break;
    }
}

void task_entry(void* raw)
{
    std::unique_ptr<TaskArgs> args(static_cast<TaskArgs*>(raw));
    auto& state = *args->state;
    Speech speech;
    speech.configure(args->jtts_config_json);
    speech.set_mouth_sink([&state](const Speech::Mouth& m) {
        state.face.mouth_open.store(m.open, std::memory_order_relaxed);
        state.face.mouth_form.store(m.form, std::memory_order_relaxed);
    });
    speech.set_subtitle_sink([&state](const std::string& text, std::uint32_t hold_ms) {
        state.set_balloon_text(text, hold_ms);
    });

    acoustic::GgWaveReceiver receiver;
    acoustic::ReactionEngine rules;
    acoustic::DuplicateFilter dedupe;
    acoustic::AudioDuplexGate audio;
    std::array<std::int16_t, acoustic::GgWaveReceiver::kFrameSamples> pcm{};
    using CaptureBuffer = std::array<std::int16_t, acoustic::GgWaveReceiver::kFrameSamples * 2>;
    std::array<CaptureBuffer, 3> capture{};
    std::size_t capture_index = 0;
    bool capture_primed = false;
    std::array<std::uint8_t, acoustic::GgWaveReceiver::kMaxPayloadBytes> payload{};
    std::uint64_t level_sumsq = 0;
    std::uint32_t level_samples = 0;
    std::uint16_t level_peak = 0;
    std::uint32_t next_level_log_ms = 0;

    // The boot arpeggio owns the same I2S peripheral. Do not initialise the
    // receiver until it has completely released the speaker.
    while (!state.boot_sounds_done.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    auto start_listening = [&]() -> bool {
        M5.Speaker.end();
        vTaskDelay(kAudioSettle);
        if (!M5.Mic.begin()) return false;
        if (!receiver.begin()) { M5.Mic.end(); return false; }
        ESP_LOGI(kTag, "listening: ggwave web Audible/0..2, mic=%u codec=%u frame=%u heap=%d",
                 static_cast<unsigned>(kMicSampleRate),
                 static_cast<unsigned>(acoustic::GgWaveReceiver::kSampleRate),
                 static_cast<unsigned>(acoustic::GgWaveReceiver::kFrameSamples),
                 receiver.heap_bytes());
        return true;
    };

    while (!start_listening()) {
        ESP_LOGE(kTag, "receiver or microphone init failed; retrying in 5 s");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    for (;;) {
        if (!capture_primed) {
            bool ok = true;
            // M5Unified exposes two pending recording slots. Queueing a
            // third buffer blocks until the first is complete, so buffer 0
            // is safe to consume while buffers 1 and 2 keep I2S continuous.
            for (auto& buffer : capture) {
                if (!M5.Mic.record(buffer.data(), buffer.size(), kMicSampleRate,
                                   /*stereo=*/false)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                ESP_LOGW(kTag, "microphone pipeline init failed; recovering");
                M5.Mic.end();
                receiver.reset();
                vTaskDelay(pdMS_TO_TICKS(250));
                while (!start_listening()) vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            capture_index = 0;
            capture_primed = true;
        }

        auto& pcm48 = capture[capture_index];

        // Match AtomS3R-Acoustic-Modem's proven Web receiver path: browser
        // and codec run at 48 kHz, then adjacent samples are averaged to the
        // 24 kHz / 512-sample stream expected by ggwave's Web decoder.
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            pcm[i] = static_cast<std::int16_t>(
                (static_cast<std::int32_t>(pcm48[2 * i]) + pcm48[2 * i + 1]) / 2);
        }

        // Requeue the buffer we just consumed. This blocks until the next
        // queued capture has completed, guaranteeing that the next loop
        // reads a finished buffer without stopping the I2S stream.
        if (!M5.Mic.record(pcm48.data(), pcm48.size(), kMicSampleRate,
                           /*stereo=*/false)) {
            ESP_LOGW(kTag, "microphone pipeline failed; recovering");
            M5.Mic.end();
            receiver.reset();
            capture_primed = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            while (!start_listening()) vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        capture_index = (capture_index + 1) % capture.size();

        for (const auto sample : pcm) {
            const auto magnitude = static_cast<std::uint16_t>(
                sample == INT16_MIN ? INT16_MAX : std::abs(static_cast<int>(sample)));
            level_peak = std::max(level_peak, magnitude);
            level_sumsq += static_cast<std::uint64_t>(
                static_cast<std::int64_t>(sample) * sample);
        }
        level_samples += pcm.size();
        const auto now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        if (next_level_log_ms == 0) next_level_log_ms = now_ms + 2000;
        if (static_cast<std::int32_t>(now_ms - next_level_log_ms) >= 0) {
            const auto rms = level_samples == 0 ? 0u : static_cast<unsigned>(
                std::sqrt(static_cast<double>(level_sumsq) / level_samples));
            ESP_LOGI(kTag, "mic level: rms=%u peak=%u", rms,
                     static_cast<unsigned>(level_peak));
            level_sumsq = 0;
            level_samples = 0;
            level_peak = 0;
            next_level_log_ms = now_ms + 2000;
        }

        const int n = receiver.feed(pcm, payload);
        // ggwave's variable-length decoder is CPU-heavy and this task runs
        // above the idle priority. Give IDLE1 a scheduling window for every
        // 512-sample frame, including the common no-payload path, so the task
        // watchdog is serviced during continuous listening.
        vTaskDelay(pdMS_TO_TICKS(1));
        if (n == 0) continue;
        if (n < 0) {
            ESP_LOGW(kTag, "ggwave decode error=%d; continuing", n);
            continue;
        }
        if (!audio.begin_decode()) {
            ESP_LOGE(kTag, "invalid audio phase before decode");
            continue;
        }
        const auto message = acoustic::decode_payload(
            std::span<const std::uint8_t>(payload.data(), static_cast<std::size_t>(n)));
        if (!message) {
            ESP_LOGW(kTag, "rejected payload: invalid UTF-8/control data (%d bytes)", n);
            audio.resume_listening();
            continue;
        }
        ESP_LOGI(kTag, "GGWAVE RX (%d bytes): %.*s", n, n, message->c_str());
        if (!dedupe.accept(*message, static_cast<std::uint32_t>(esp_timer_get_time() / 1000))) {
            ESP_LOGI(kTag, "duplicate suppressed");
            audio.resume_listening();
            continue;
        }

        state.set_balloon_text(*message, 2500);
        const auto reaction = rules.match(*message);
        if (!reaction) { audio.resume_listening(); continue; }
        audio.begin_reaction();

        M5.Mic.end();
        receiver.reset();
        vTaskDelay(kAudioSettle);
        const int previous_expression = state.face.expression.load(std::memory_order_relaxed);
        const float previous_yaw = state.servo.target_yaw_deg.load(std::memory_order_relaxed);
        const float previous_pitch = state.servo.target_pitch_deg.load(std::memory_order_relaxed);
        state.face.expression.store(avatar_expression(reaction->expression), std::memory_order_relaxed);
        apply_gesture(state, reaction->gesture);
        state.set_balloon_text(reaction->display, 3000);

        audio.begin_speech();
        state.servo.masked.store(true, std::memory_order_release);
        const bool speaking = speech.say(reaction->reading);
        if (!speaking) ESP_LOGW(kTag, "local speech unavailable; visual reaction only");
        while (speech.is_speaking()) vTaskDelay(pdMS_TO_TICKS(20));
        state.servo.masked.store(false, std::memory_order_release);
        audio.finish_speech();

        M5.Speaker.end();
        state.face.mouth_open.store(0.0f, std::memory_order_relaxed);
        state.face.mouth_form.store(-1.0f, std::memory_order_relaxed);
        state.face.expression.store(previous_expression, std::memory_order_relaxed);
        state.servo.target_yaw_deg.store(previous_yaw, std::memory_order_relaxed);
        state.servo.target_pitch_deg.store(previous_pitch, std::memory_order_relaxed);
        state.servo.speed_override.store(0, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(150));
        while (!start_listening()) {
            ESP_LOGE(kTag, "listen recovery failed; retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        capture_primed = false;
        audio.resume_listening();
    }
}

} // namespace

void start_acoustic_reaction_task(SharedState& state, const std::string& jtts_config_json)
{
    auto* args = new TaskArgs{&state, jtts_config_json};
    const auto ok = xTaskCreatePinnedToCoreWithCaps(
        task_entry, "acoustic-rx", 12 * 1024, args,
        tskIDLE_PRIORITY + 2, nullptr, 1, stackchan::kNoFlashTaskStackCaps);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "failed to create acoustic receiver task");
        delete args;
    }
}

} // namespace stackchan::app
