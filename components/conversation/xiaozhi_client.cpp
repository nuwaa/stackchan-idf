// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "conversation/xiaozhi_client.hpp"
#include "conversation/json.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "esp_audio_types.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "psram_allocator.hpp"
#include "ws_extra_headers.hpp"

namespace stackchan::conversation {

using namespace ArduinoJson;

namespace {

constexpr const char* kTag = "xiaozhi";

// Uplink Opus: 16 kHz mono, 60 ms frames (= 960 samples) as required by the
// XiaoZhi protocol. The conversation task feeds us 40 ms (640-sample) chunks,
// so the sender accumulates until a full 60 ms frame is available.
constexpr int kUplinkSampleRate = 16000;
constexpr int kUplinkFrameMs = 60;
constexpr std::size_t kUplinkFrameSamples = kUplinkSampleRate * kUplinkFrameMs / 1000; // 960

constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(10000);

constexpr UBaseType_t kSenderTaskPrio = 5;
// libopus' CELT/SILK encode path is stack-hungry (large on-stack arrays); 8 KB
// overflows the moment the first frame is encoded. PSRAM-backed, so generous.
constexpr std::size_t kSenderTaskStack = 32768;
constexpr BaseType_t kSenderTaskCore = 1;

// Audio tx ring. 40 ms PCM16 mono chunks (640 samples, 1280 B); 96 slots
// ≈ 3.8 s of buffering before evictions.
constexpr std::size_t kAudioTxQueueLen = 96;
constexpr std::size_t kMaxChunkSamples = 2048;

// Decoded-PCM scratch. 120 ms @ 48 kHz mono = 5760 samples (11520 B); allocate
// generous headroom and grow on demand if the server uses larger frames.
constexpr std::size_t kDecodeOutInitialBytes = 16384;

struct AudioChunk {
    std::uint16_t len_samples; // PCM16 little-endian samples
    std::int16_t data[];       // [len_samples]
};

// Linear-resample mono PCM16 from src_rate to dst_rate, appending to `out`.
// Equal rates copy straight through. Cheap and good enough for speech.
void resample_into(const std::int16_t* src, std::size_t n,
                   std::uint32_t src_rate, std::uint32_t dst_rate,
                   std::vector<std::int16_t>& out)
{
    if (n == 0) return;
    if (src_rate == dst_rate || src_rate == 0 || dst_rate == 0) {
        out.insert(out.end(), src, src + n);
        return;
    }
    const std::size_t dst_n =
        static_cast<std::size_t>(static_cast<std::uint64_t>(n) * dst_rate / src_rate);
    const float step = static_cast<float>(src_rate) / static_cast<float>(dst_rate);
    for (std::size_t i = 0; i < dst_n; ++i) {
        const float pos = i * step;
        const std::size_t i0 = static_cast<std::size_t>(pos);
        const float frac = pos - i0;
        const std::int32_t s0 = src[i0];
        const std::int32_t s1 = (i0 + 1 < n) ? src[i0 + 1] : src[i0];
        out.push_back(static_cast<std::int16_t>(s0 + (s1 - s0) * frac));
    }
}

} // namespace

class XiaoZhiClient::Impl {
public:
    Impl(std::string url, std::string token)
        : url_{std::move(url)}, token_{std::move(token)}
    {
    }

    ~Impl() { teardown(); }

    tl::expected<void, ConversationError> start(const ConversationConfig& config)
    {
        if (client_ != nullptr) {
            return tl::unexpected{ConversationError::InvalidState};
        }
        config_ = config;
        tx_evicted_total_.store(0, std::memory_order_relaxed);
        last_evict_log_ms_ = 0;
        // Output rate the conversation task will replay at; downlink Opus is
        // resampled to this regardless of what the server announces.
        output_rate_ = config_.output_sample_rate_hz != 0 ? config_.output_sample_rate_hz : 24000;
        server_out_rate_ = output_rate_; // until the server hello says otherwise

        compose_device_headers();

        esp_websocket_client_config_t cfg{};
        cfg.uri = url_.c_str();
        cfg.crt_bundle_attach = esp_crt_bundle_attach; // used only for wss://
        cfg.headers = headers_.c_str();
        cfg.buffer_size = 8192;
        // The WS event task only parses control JSON and copies audio frames
        // onto audio_rx_queue_; Opus decode (stack-hungry) runs in the
        // xiaozhi_rx task whose stack lives in PSRAM. esp_websocket_client
        // allocates this stack from internal RAM, and the 16 KiB it used to
        // be was the single biggest reason wss:// ran out of DMA-capable RAM
        // ("esp-aes: Failed to allocate memory" on every first audio send).
        cfg.task_stack = 6144;
        cfg.task_prio = 5;
        cfg.disable_auto_reconnect = true;
        cfg.network_timeout_ms = 10000;
        cfg.ping_interval_sec = 15;
        cfg.pingpong_timeout_sec = 25;
        cfg.keep_alive_enable = true;
        cfg.keep_alive_idle = 10;
        cfg.keep_alive_interval = 5;
        cfg.keep_alive_count = 3;

        const std::size_t rx_cap = static_cast<std::size_t>(CONFIG_STACKCHAN_CONV_WS_RX_BUFFER);
        rx_buffer_ = static_cast<std::uint8_t*>(heap_caps_malloc(rx_cap, MALLOC_CAP_SPIRAM));
        if (rx_buffer_ == nullptr) {
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        rx_capacity_ = rx_cap;
        rx_len_ = 0;
        rx_op_code_ = 0;

        decode_out_.assign(kDecodeOutInitialBytes, 0);

        if (!open_encoder()) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }

        client_ = esp_websocket_client_init(&cfg);
        if (client_ == nullptr) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }
        apply_extra_ws_headers(client_, config_.extra_headers);
        esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY,
                                      &Impl::websocket_event_trampoline, this);

        hello_received_ = false;
        session_id_.clear();
        audio_seq_ = 0;
        set_state(ConversationState::Connecting);

        audio_tx_queue_ = xQueueCreate(kAudioTxQueueLen, sizeof(AudioChunk*));
        if (audio_tx_queue_ == nullptr) {
            teardown();
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        sender_should_exit_.store(false, std::memory_order_relaxed);
        if (xTaskCreatePinnedToCoreWithCaps(&Impl::sender_trampoline, "xiaozhi_tx",
                                            kSenderTaskStack, this, kSenderTaskPrio,
                                            &sender_task_, kSenderTaskCore,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }

        // Downlink: WS task → audio_rx_queue_ → xiaozhi_rx (Opus decode +
        // resample, PSRAM stack) → AssistantAudioChunk events.
        audio_rx_queue_ = xQueueCreate(kRxQueueLen, sizeof(RxFrame*));
        if (audio_rx_queue_ == nullptr) {
            teardown();
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        decoder_should_exit_.store(false, std::memory_order_relaxed);
        if (xTaskCreatePinnedToCoreWithCaps(&Impl::decoder_trampoline, "xiaozhi_rx",
                                            kDecoderTaskStack, this, kSenderTaskPrio,
                                            &decoder_task_, kSenderTaskCore,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }

        if (esp_websocket_client_start(client_) != ESP_OK) {
            teardown();
            return tl::unexpected{ConversationError::TransportInit};
        }
        return {};
    }

    void stop() { teardown(); }

    tl::expected<void, ConversationError> push_audio(std::span<const std::int16_t> pcm)
    {
        if (state_.load(std::memory_order_relaxed) != ConversationState::Listening) {
            return {};
        }
        if (pcm.empty()) {
            return {};
        }
        if (audio_tx_queue_ == nullptr) {
            return tl::unexpected{ConversationError::NotConnected};
        }
        if (pcm.size() > kMaxChunkSamples) {
            return tl::unexpected{ConversationError::InvalidState};
        }

        const std::size_t alloc = sizeof(AudioChunk) + pcm.size() * sizeof(std::int16_t);
        auto* chunk = static_cast<AudioChunk*>(
            heap_caps_malloc(alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (chunk == nullptr) {
            return tl::unexpected{ConversationError::OutOfMemory};
        }
        chunk->len_samples = static_cast<std::uint16_t>(pcm.size());
        std::memcpy(chunk->data, pcm.data(), pcm.size() * sizeof(std::int16_t));

        if (xQueueSend(audio_tx_queue_, &chunk, 0) != pdTRUE) {
            AudioChunk* old = nullptr;
            if (xQueueReceive(audio_tx_queue_, &old, 0) == pdTRUE && old != nullptr) {
                heap_caps_free(old);
            }
            if (xQueueSend(audio_tx_queue_, &chunk, 0) != pdTRUE) {
                heap_caps_free(chunk);
                return tl::unexpected{ConversationError::SendFailed};
            }
            note_tx_eviction();
        }
        return {};
    }

    tl::expected<void, ConversationError> commit_audio()
    {
        // Server-side VAD (listen mode=auto) detects turn boundaries.
        return {};
    }

    tl::expected<void, ConversationError> submit_tool_result(std::string_view call_id, std::string_view output_json)
    {
        // JSON-RPC result for a tools/call: MCP CallToolResult with one text
        // content block (the server hands content[0].text to its LLM).
        if (client_ == nullptr || !esp_websocket_client_is_connected(client_)) {
            return tl::unexpected{ConversationError::NotConnected};
        }
        auto doc = make_json_doc();
        doc["type"] = "mcp";
        if (!session_id_.empty()) doc["session_id"] = session_id_;
        JsonObject payload = doc["payload"].to<JsonObject>();
        payload["jsonrpc"] = "2.0";
        payload["id"] = std::strtod(std::string{call_id}.c_str(), nullptr);
        JsonObject result = payload["result"].to<JsonObject>();
        JsonObject block = result["content"].to<JsonArray>().add<JsonObject>();
        block["type"] = "text";
        block["text"] = output_json;
        result["isError"] = false;
        std::lock_guard lock{send_mutex_};
        return send_json_locked(doc) ? tl::expected<void, ConversationError>{}
                                     : tl::unexpected{ConversationError::SendFailed};
    }

    tl::expected<void, ConversationError> push_text(std::string_view text)
    {
        // listen/detect with text: the server treats it as a recognised user
        // utterance (no audio) and runs the LLM on it. Used to hand a
        // delegated agent's answer back into the conversation.
        if (client_ == nullptr || !esp_websocket_client_is_connected(client_) || !hello_received_) {
            return tl::unexpected{ConversationError::NotConnected};
        }
        auto doc = make_json_doc();
        doc["type"] = "listen";
        if (!session_id_.empty()) doc["session_id"] = session_id_;
        doc["state"] = "detect";
        doc["text"] = text;
        std::lock_guard lock{send_mutex_};
        return send_json_locked(doc) ? tl::expected<void, ConversationError>{}
                                     : tl::unexpected{ConversationError::SendFailed};
    }

    tl::expected<void, ConversationError> cancel_response()
    {
        // Barge-in: ask the server to abandon the in-progress reply.
        auto doc = make_json_doc();
        doc["type"] = "abort";
        doc["reason"] = "wake_word_detected";
        if (!session_id_.empty()) doc["session_id"] = session_id_;
        // Resync locally too: if the server never answers with `tts stop`
        // (e.g. its reply pipeline threw), push_audio() would keep dropping
        // mic chunks forever while the task believes it is listening.
        if (state_.load(std::memory_order_relaxed) == ConversationState::Speaking) {
            set_state(ConversationState::Listening);
        }
        std::lock_guard lock{send_mutex_};
        return send_json_locked(doc) ? tl::expected<void, ConversationError>{}
                                     : tl::unexpected{ConversationError::SendFailed};
    }

    ConversationState state() const { return state_.load(std::memory_order_relaxed); }

    std::uint32_t tx_evicted_chunks() const { return tx_evicted_total_.load(std::memory_order_relaxed); }

    void set_event_callback(EventCallback cb) { event_callback_ = std::move(cb); }

private:
    // ---- lifecycle ---------------------------------------------------------

    // Count a mic-uplink eviction and emit at most one warning per
    // kEvictLogIntervalMs — under sustained congestion push_audio evicts on
    // every chunk, and a per-chunk ESP_LOGW floods the serial log. The
    // cumulative total is published per turn via tx_evicted_chunks(). Runs
    // only on the push_audio caller's task, so last_evict_log_ms_ needs no
    // synchronisation; the total is atomic because it is read cross-task.
    void note_tx_eviction()
    {
        static constexpr std::uint32_t kEvictLogIntervalMs = 5000;
        const std::uint32_t total = tx_evicted_total_.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        if (last_evict_log_ms_ == 0 || now_ms - last_evict_log_ms_ >= kEvictLogIntervalMs) {
            ESP_LOGD(kTag, "audio tx queue full; evicting oldest chunks (%u evicted this session)",
                     static_cast<unsigned>(total));
            last_evict_log_ms_ = now_ms;
        }
    }

    void teardown()
    {
        if (decoder_task_ != nullptr) {
            decoder_should_exit_.store(true, std::memory_order_release);
            RxFrame* sentinel = nullptr;
            xQueueSend(audio_rx_queue_, &sentinel, 0);
            for (int i = 0; i < 200; ++i) {
                if (eTaskGetState(decoder_task_) == eDeleted) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            decoder_task_ = nullptr;
        }
        if (audio_rx_queue_ != nullptr) {
            RxFrame* f = nullptr;
            while (xQueueReceive(audio_rx_queue_, &f, 0) == pdTRUE) {
                if (f != nullptr) heap_caps_free(f);
            }
            vQueueDelete(audio_rx_queue_);
            audio_rx_queue_ = nullptr;
        }
        if (sender_task_ != nullptr) {
            sender_should_exit_.store(true, std::memory_order_release);
            AudioChunk* sentinel = nullptr;
            xQueueSend(audio_tx_queue_, &sentinel, 0);
            for (int i = 0; i < 200; ++i) {
                if (eTaskGetState(sender_task_) == eDeleted) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            sender_task_ = nullptr;
        }
        if (audio_tx_queue_ != nullptr) {
            AudioChunk* chunk = nullptr;
            while (xQueueReceive(audio_tx_queue_, &chunk, 0) == pdTRUE) {
                if (chunk != nullptr) heap_caps_free(chunk);
            }
            vQueueDelete(audio_tx_queue_);
            audio_tx_queue_ = nullptr;
        }

        if (client_ != nullptr) {
            esp_websocket_client_stop(client_);
            esp_websocket_client_destroy(client_);
            client_ = nullptr;
        }

        // The WS + sender tasks are gone now, so the codec handles have no
        // other users — safe to close.
        if (encoder_ != nullptr) {
            esp_opus_enc_close(encoder_);
            encoder_ = nullptr;
        }
        if (decoder_ != nullptr) {
            esp_opus_dec_close(decoder_);
            decoder_ = nullptr;
        }
        enc_accum_.clear();
        enc_accum_.shrink_to_fit();
        decode_out_.clear();
        decode_out_.shrink_to_fit();

        if (rx_buffer_ != nullptr) {
            heap_caps_free(rx_buffer_);
            rx_buffer_ = nullptr;
        }
        rx_capacity_ = 0;
        rx_len_ = 0;
        set_state(ConversationState::Idle);
    }

    void set_state(ConversationState s)
    {
        const ConversationState prev = state_.exchange(s, std::memory_order_relaxed);
        if (prev != s) {
            ConversationEvent ev{};
            ev.type = ConversationEventType::StateChanged;
            ev.state = s;
            emit(ev);
        }
    }

    void emit(const ConversationEvent& ev)
    {
        if (event_callback_) event_callback_(ev);
    }

    void emit_text(ConversationEventType type, const char* text)
    {
        ConversationEvent ev{};
        ev.type = type;
        if (text != nullptr) ev.text = text;
        emit(ev);
    }

    void emit_error(ConversationError code, std::string message)
    {
        ConversationEvent ev{};
        ev.type = ConversationEventType::Error;
        ev.error = code;
        ev.text = std::move(message);
        emit(ev);
    }

    // ---- headers / identity ------------------------------------------------

    void compose_device_headers()
    {
        std::uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char dev[18];
        std::snprintf(dev, sizeof(dev), "%02x:%02x:%02x:%02x:%02x:%02x",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        // Stable per-device UUID derived from the STA MAC, so the server sees
        // the same Client-Id across reboots.
        char cid[37];
        std::snprintf(cid, sizeof(cid), "00000000-0000-0000-0000-%02x%02x%02x%02x%02x%02x",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        headers_.clear();
        if (!token_.empty()) {
            headers_ += "Authorization: Bearer " + token_ + "\r\n";
        }
        headers_ += "Protocol-Version: 1\r\n";
        headers_ += std::string{"Device-Id: "} + dev + "\r\n";
        headers_ += std::string{"Client-Id: "} + cid + "\r\n";
        ESP_LOGI(kTag, "device-id=%s", dev);
    }

    // ---- codec -------------------------------------------------------------

    static void log_heap(const char* where)
    {
        ESP_LOGI(kTag, "heap@%s: INT free=%u largest=%u DMA largest=%u", where,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    }

    bool open_encoder()
    {
        log_heap("before opus enc");
        esp_opus_enc_config_t ecfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
        ecfg.sample_rate = kUplinkSampleRate;
        ecfg.channel = ESP_AUDIO_MONO;
        ecfg.bits_per_sample = ESP_AUDIO_BIT16;
        ecfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
        ecfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
        ecfg.bitrate = 24000; // speech-friendly for 16 kHz mono
        if (esp_opus_enc_open(&ecfg, sizeof(ecfg), &encoder_) != ESP_AUDIO_ERR_OK ||
            encoder_ == nullptr) {
            ESP_LOGE(kTag, "esp_opus_enc_open failed");
            return false;
        }
        log_heap("after opus enc");
        int in_size = 0, out_size = 0;
        if (esp_opus_enc_get_frame_size(encoder_, &in_size, &out_size) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(kTag, "esp_opus_enc_get_frame_size failed");
            return false;
        }
        enc_in_bytes_ = static_cast<std::size_t>(in_size);   // bytes per 60 ms frame
        enc_out_buf_.assign(static_cast<std::size_t>(out_size), 0);
        ESP_LOGI(kTag, "opus enc: in_frame=%dB out_frame=%dB (%u samples/frame)",
                 in_size, out_size, static_cast<unsigned>(enc_in_bytes_ / sizeof(std::int16_t)));
        return true;
    }

    // Opens (or reopens) the downlink decoder at `rate`. Called from the WS
    // event task once the server hello announces its TTS sample rate.
    bool open_decoder(std::uint32_t rate)
    {
        std::lock_guard lock{decoder_mutex_};
        if (decoder_ != nullptr) {
            esp_opus_dec_close(decoder_);
            decoder_ = nullptr;
        }
        esp_opus_dec_cfg_t dcfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
        dcfg.sample_rate = rate;
        dcfg.channel = ESP_AUDIO_MONO;
        dcfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        dcfg.self_delimited = false;
        log_heap("before opus dec");
        if (esp_opus_dec_open(&dcfg, sizeof(dcfg), &decoder_) != ESP_AUDIO_ERR_OK ||
            decoder_ == nullptr) {
            ESP_LOGE(kTag, "esp_opus_dec_open(%u) failed", static_cast<unsigned>(rate));
            return false;
        }
        server_out_rate_ = rate;
        ESP_LOGI(kTag, "opus dec opened @ %u Hz (resample -> %u Hz)",
                 static_cast<unsigned>(rate), static_cast<unsigned>(output_rate_));
        return true;
    }

    // ---- WebSocket transport ----------------------------------------------

    static void websocket_event_trampoline(void* arg, esp_event_base_t /*base*/,
                                           std::int32_t event_id, void* data)
    {
        static_cast<Impl*>(arg)->on_websocket_event(
            event_id, static_cast<esp_websocket_event_data_t*>(data));
    }

    void on_websocket_event(std::int32_t event_id, const esp_websocket_event_data_t* data)
    {
        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(kTag, "WEBSOCKET_EVENT_CONNECTED");
            send_hello();
            break;
        case WEBSOCKET_EVENT_DATA:
            on_websocket_data(data);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(kTag, "WEBSOCKET_EVENT_DISCONNECTED");
            set_state(ConversationState::Error);
            emit_error(ConversationError::NotConnected, "websocket disconnected");
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(kTag, "WEBSOCKET_EVENT_ERROR");
            set_state(ConversationState::Error);
            emit_error(ConversationError::TransportInit, "websocket error");
            break;
        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(kTag, "WEBSOCKET_EVENT_CLOSED");
            set_state(ConversationState::Error);
            emit_error(ConversationError::NotConnected, "websocket closed");
            break;
        default:
            break;
        }
    }

    void on_websocket_data(const esp_websocket_event_data_t* data)
    {
        if (data == nullptr || data->data_len < 0) return;
        if (data->payload_offset == 0) {
            rx_len_ = 0;
            rx_op_code_ = data->op_code;
        }
        // 0x01 text = JSON control, 0x02 binary = Opus audio. Anything else is
        // a WS control frame (log close codes, ignore the rest).
        if (rx_op_code_ != 0x01 && rx_op_code_ != 0x02) {
            if (rx_op_code_ == 0x08 && data->data_len >= 2) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(data->data_ptr);
                const std::uint16_t code = (bytes[0] << 8) | bytes[1];
                ESP_LOGW(kTag, "ws close: code=%u reason='%.*s'", code,
                         data->data_len - 2, reinterpret_cast<const char*>(bytes + 2));
            }
            return;
        }
        const std::size_t total = static_cast<std::size_t>(data->payload_len);
        const std::size_t chunk = static_cast<std::size_t>(data->data_len);
        if (rx_len_ + chunk > rx_capacity_) {
            ESP_LOGW(kTag, "rx overflow: total=%u capacity=%u",
                     static_cast<unsigned>(total), static_cast<unsigned>(rx_capacity_));
            rx_len_ = 0;
            return;
        }
        std::memcpy(rx_buffer_ + rx_len_, data->data_ptr, chunk);
        rx_len_ += chunk;
        // Require total > 0: a payload_len==0 fragment (with data_len>0) would
        // otherwise satisfy rx_len_ >= total immediately and dispatch an
        // incomplete frame. Wait for the framed total to be known.
        if (rx_len_ < total || total == 0) return; // wait for the rest of a fragmented frame

        if (rx_op_code_ == 0x02) {
            enqueue_audio_frame(rx_buffer_, rx_len_);
        } else {
            parse_control(reinterpret_cast<char*>(rx_buffer_), rx_len_);
        }
        rx_len_ = 0;
    }

    // ---- outbound ----------------------------------------------------------

    bool send_json_locked(const JsonDocument& doc)
    {
        // send_mutex_ must be held by the caller.
        if (client_ == nullptr || !esp_websocket_client_is_connected(client_)) return false;
        // Serialise straight into one PSRAM buffer (cJSON used to malloc the
        // whole text in internal RAM, which starved the TLS layer's DMA
        // allocations right after a reconnect).
        PsramBytes text = json_to_psram(doc);
        if (text.data == nullptr || text.len == 0) return false;
        const int rc = esp_websocket_client_send_text(client_, text.data, static_cast<int>(text.len), kSendTimeout);
        // rc==0 means the poll_write timed out (nothing sent). Serialized JSON
        // is always non-empty here, so only rc>0 is a real success — treating 0
        // as success would silently drop control messages and hang the turn.
        return rc > 0;
    }

    void send_hello()
    {
        auto doc = make_json_doc();
        doc["type"] = "hello";
        doc["version"] = 1;
        doc["transport"] = "websocket";
        // MCP: the device acts as an MCP server over this socket. The
        // xiaozhi server sends initialize / tools/list / tools/call as
        // {"type":"mcp","payload":<JSON-RPC>} and we answer in kind, so its
        // LLM can drive our tools (expression, head pose, time, memory,
        // timer, ask_agent) just like OpenAI / Gemini do natively.
        doc["features"]["mcp"] = true;
        // Without this the server pushes `llm.emotion = "happy"` at the start of
        // every reply (its default when the text has no emoji), which overrides
        // whatever set_expression just chose. We drive the face via MCP tools.
        doc["features"]["emoji"] = false;
        // stackchan-idf extension (not in the official protocol): carry the
        // firmware's system prompt so our forked server can use it as this
        // connection's persona instead of its config.yaml prompt. The
        // official server ignores unknown hello fields.
        if (!config_.instructions.empty()) doc["system_prompt"] = config_.instructions;
        JsonObject ap = doc["audio_params"].to<JsonObject>();
        ap["format"] = "opus";
        ap["sample_rate"] = kUplinkSampleRate;
        ap["channels"] = 1;
        ap["frame_duration"] = kUplinkFrameMs;
        {
            std::lock_guard lock{send_mutex_};
            (void)send_json_locked(doc);
        }
        ESP_LOGI(kTag, "hello sent");
    }

    // Open the server's mode=auto (server-VAD) listening window. Sent once
    // after the hello handshake; the server then handles turn boundaries
    // itself. NOTE: we never send `listen stop` — in mode=auto the server
    // treats a mid-session stop as a barge-in and aborts its own TTS.
    void send_listen_start()
    {
        auto doc = make_json_doc();
        if (!session_id_.empty()) doc["session_id"] = session_id_;
        doc["type"] = "listen";
        doc["state"] = "start";
        doc["mode"] = "auto"; // server-side VAD
        {
            std::lock_guard lock{send_mutex_};
            (void)send_json_locked(doc);
        }
        ESP_LOGI(kTag, "listen start (mode=auto)");
    }

    // ---- inbound: JSON control --------------------------------------------

    // `json` is parsed in place (zero-copy: strings are unescaped inside the
    // rx buffer and the document only points at them), so it must stay alive
    // and writable for the duration of the call. rx_buffer_ satisfies both.
    void parse_control(char* json, std::size_t len)
    {
        if (rx_log_count_ < 8) {
            const std::size_t snip = std::min<std::size_t>(len, 384);
            ESP_LOGI(kTag, "rx[%u/%uB]: %.*s%s", rx_log_count_, static_cast<unsigned>(len),
                     static_cast<int>(snip), json, snip < len ? "…" : "");
            ++rx_log_count_;
        }
        auto doc = make_json_doc();
        const DeserializationError err = deserializeJson(doc, json, len);
        if (err) {
            ESP_LOGW(kTag, "json parse failed: %s", err.c_str());
            return;
        }
        JsonObjectConst root = doc.as<JsonObjectConst>();
        const char* type = root["type"].as<const char*>();
        if (type == nullptr) return;

        if (std::strcmp(type, "hello") == 0) {
            handle_server_hello(root);
        } else if (std::strcmp(type, "stt") == 0) {
            // The server reuses the stt message to display "% <tool>" while
            // it runs a tool; that's not the user's words, so keep it out of
            // the transcript (the top-right badge already shows ツール実行中).
            const char* text = root["text"].as<const char*>();
            if (text != nullptr && text[0] == '%' && text[1] == ' ') {
                ESP_LOGI(kTag, "server tool marker: %s", text);
            } else {
                emit_text(ConversationEventType::UserTranscript, text);
            }
        } else if (std::strcmp(type, "llm") == 0) {
            emit_text(ConversationEventType::AssistantEmotion, root["emotion"].as<const char*>());
        } else if (std::strcmp(type, "tts") == 0) {
            handle_tts(root);
        } else if (std::strcmp(type, "mcp") == 0) {
            handle_mcp(root["payload"].as<JsonObjectConst>());
        } else if (std::strcmp(type, "system") == 0 || std::strcmp(type, "alert") == 0) {
            ESP_LOGI(kTag, "server %s: %s", type, root["message"] | "");
        }
    }

    // ---- MCP (device = MCP server) -----------------------------------------

    // Start an outbound {"type":"mcp","payload":{"jsonrpc":"2.0","id":<id>}}
    // and return the payload object for the caller to fill in.
    JsonObject begin_mcp_reply(JsonDocument& doc, JsonVariantConst id)
    {
        doc["type"] = "mcp";
        if (!session_id_.empty()) doc["session_id"] = session_id_;
        JsonObject payload = doc["payload"].to<JsonObject>();
        payload["jsonrpc"] = "2.0";
        payload["id"] = id;
        return payload;
    }

    void send_mcp(const JsonDocument& doc)
    {
        std::lock_guard lock{send_mutex_};
        (void)send_json_locked(doc);
    }

    void handle_mcp(JsonObjectConst payload)
    {
        if (payload.isNull()) return;
        const char* method = payload["method"].as<const char*>();
        JsonVariantConst id = payload["id"];
        if (method == nullptr) return;          // results / errors: nothing we wait for
        if (id.isNull()) {                      // notification (e.g. notifications/initialized)
            ESP_LOGD(kTag, "mcp notification %s", method);
            return;
        }
        if (std::strcmp(method, "initialize") == 0) {
            auto out = make_json_doc();
            JsonObject result = begin_mcp_reply(out, id)["result"].to<JsonObject>();
            result["protocolVersion"] = "2024-11-05";
            result["capabilities"]["tools"].to<JsonObject>();
            JsonObject info = result["serverInfo"].to<JsonObject>();
            info["name"] = "stackchan-idf";
            info["version"] = "1.0.0";
            send_mcp(out);
            ESP_LOGI(kTag, "mcp initialize answered (%u tools available)", static_cast<unsigned>(config_.tools.size()));
        } else if (std::strcmp(method, "tools/list") == 0) {
            auto out = make_json_doc();
            JsonArray tools = begin_mcp_reply(out, id)["result"]["tools"].to<JsonArray>();
            for (const auto& t : config_.tools) {
                JsonObject tool = tools.add<JsonObject>();
                tool["name"] = t.name;
                tool["description"] = t.description;
                // Parse the schema straight into the reply's own arena (the
                // schema literals are static, so no copy is made of them).
                JsonVariant schema = tool["inputSchema"].to<JsonVariant>();
                if (deserializeJson(schema, t.parameters_json.c_str()) || !schema.is<JsonObjectConst>()) {
                    JsonObject fallback = schema.to<JsonObject>();
                    fallback["type"] = "object";
                    fallback["properties"].to<JsonObject>();
                }
            }
            send_mcp(out);
            ESP_LOGI(kTag, "mcp tools/list answered (%u tools)", static_cast<unsigned>(config_.tools.size()));
        } else if (std::strcmp(method, "tools/call") == 0) {
            JsonObjectConst params = payload["params"].as<JsonObjectConst>();
            const char* name = params["name"].as<const char*>();
            if (name == nullptr) {
                send_mcp_error(id, -32602, "missing tool name");
                return;
            }
            JsonVariantConst args = params["arguments"];
            ConversationEvent ev{};
            ev.type = ConversationEventType::ToolCallRequested;
            ev.tool_call = ToolCall{
                .call_id = json_to_string(id),
                .name = name,
                .arguments_json = args.isNull() ? std::string{"{}"} : json_to_string(args),
            };
            emit(ev); // the conversation task answers via submit_tool_result
        } else if (std::strcmp(method, "ping") == 0) {
            auto out = make_json_doc();
            begin_mcp_reply(out, id)["result"].to<JsonObject>();
            send_mcp(out);
        } else {
            send_mcp_error(id, -32601, "method not found");
        }
    }

    void send_mcp_error(JsonVariantConst id, int code, const char* message)
    {
        auto out = make_json_doc();
        JsonObject err = begin_mcp_reply(out, id)["error"].to<JsonObject>();
        err["code"] = code;
        err["message"] = message;
        send_mcp(out);
    }

    void handle_server_hello(JsonObjectConst root)
    {
        if (const char* sid = root["session_id"].as<const char*>()) {
            session_id_ = sid;
        }
        std::uint32_t rate = output_rate_;
        if (const int sr = root["audio_params"]["sample_rate"] | 0; sr > 0) {
            rate = static_cast<std::uint32_t>(sr);
        }
        if (!open_decoder(rate)) {
            emit_error(ConversationError::ProtocolError, "decoder open failed");
            return;
        }
        hello_received_ = true;
        log_heap("session ready");
        send_listen_start();
        // Session is live — the conversation task starts listening.
        set_state(ConversationState::Listening);
    }

    void handle_tts(JsonObjectConst root)
    {
        const char* state = root["state"].as<const char*>();
        if (state == nullptr) return;

        if (std::strcmp(state, "start") == 0) {
            // The user's turn has ended and the assistant is about to speak.
            // Synthesise SpeechStopped so the conversation task leaves Listening
            // (where it would drop the incoming audio chunks). The device is
            // half-duplex, so the mic is off for the whole reply — no uplink
            // reaches the server while we speak.
            emit_text(ConversationEventType::SpeechStopped, nullptr);
            set_state(ConversationState::Speaking);
        } else if (std::strcmp(state, "sentence_start") == 0) {
            // Each sentence arrives as the server's TTS generates it. Append it
            // to the running transcript (Delta) and immediately commit (Done) so
            // the balloon shows the reply progressively as it's spoken — the
            // conversation task only refreshes the balloon on AssistantTextDone.
            const char* text = root["text"].as<const char*>();
            if (text != nullptr && text[0] != '\0') {
                emit_text(ConversationEventType::AssistantTextDelta, text);
                emit_text(ConversationEventType::AssistantTextDone, nullptr);
            }
        } else if (std::strcmp(state, "stop") == 0) {
            emit_text(ConversationEventType::AssistantAudioDone, nullptr);
            emit_text(ConversationEventType::ResponseDone, nullptr);
            set_state(ConversationState::Listening);
        }
    }

    // ---- inbound: Opus audio ----------------------------------------------

    void enqueue_audio_frame(const std::uint8_t* opus, std::size_t len)
    {
        if (audio_rx_queue_ == nullptr || len == 0 || len > 0xFFFF) return;
        auto* f = static_cast<RxFrame*>(heap_caps_malloc(sizeof(RxFrame) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (f == nullptr) return;
        f->len = static_cast<std::uint16_t>(len);
        std::memcpy(f->data, opus, len);
        if (xQueueSend(audio_rx_queue_, &f, 0) != pdTRUE) {
            // Decoder can't keep up — drop the oldest frame rather than stall
            // the WS task (which would also delay control messages).
            RxFrame* old = nullptr;
            if (xQueueReceive(audio_rx_queue_, &old, 0) == pdTRUE && old != nullptr) heap_caps_free(old);
            if (xQueueSend(audio_rx_queue_, &f, 0) != pdTRUE) heap_caps_free(f);
        }
    }

    static void decoder_trampoline(void* arg)
    {
        static_cast<Impl*>(arg)->decoder_loop();
        vTaskDeleteWithCaps(nullptr);
    }

    void decoder_loop()
    {
        RxFrame* f = nullptr;
        while (!decoder_should_exit_.load(std::memory_order_acquire)) {
            if (xQueueReceive(audio_rx_queue_, &f, portMAX_DELAY) != pdTRUE) continue;
            if (f == nullptr) break; // sentinel from teardown
            handle_audio_frame(f->data, f->len);
            heap_caps_free(f);
            f = nullptr;
        }
    }

    void handle_audio_frame(const std::uint8_t* opus, std::size_t len)
    {
        // Runs on xiaozhi_rx; the WS task may re-open the decoder on a new
        // server hello, hence the mutex.
        std::lock_guard lock{decoder_mutex_};
        if (decoder_ == nullptr || len == 0) return;

        esp_audio_dec_in_raw_t raw{};
        raw.buffer = const_cast<std::uint8_t*>(opus);
        raw.len = static_cast<std::uint32_t>(len);

        esp_audio_dec_out_frame_t out{};
        out.buffer = decode_out_.data();
        out.len = static_cast<std::uint32_t>(decode_out_.size());

        esp_audio_dec_info_t info{}; // required out-param; nullptr → INVALID_PARAMETER

        auto pcm = std::make_shared<std::vector<std::int16_t>>();
        while (raw.len) {
            raw.consumed = 0;
            out.decoded_size = 0;
            out.needed_size = 0;
            const auto ret = esp_opus_dec_decode(decoder_, &raw, &out, &info);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                decode_out_.assign(out.needed_size, 0);
                out.buffer = decode_out_.data();
                out.len = static_cast<std::uint32_t>(decode_out_.size());
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                // Throttle: a corrupt frame / packet loss can affect a long run
                // of frames, and one line each would drown the log.
                if ((decode_fail_count_++ % 50) == 0) {
                    ESP_LOGW(kTag, "opus decode failed: %d (count=%lu)",
                             static_cast<int>(ret),
                             static_cast<unsigned long>(decode_fail_count_));
                }
                break;
            }
            if (out.decoded_size > 0) {
                const auto* s16 = reinterpret_cast<const std::int16_t*>(decode_out_.data());
                resample_into(s16, out.decoded_size / sizeof(std::int16_t),
                              server_out_rate_, output_rate_, *pcm);
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (raw.consumed == 0) break;
        }
        if (pcm->empty()) return;

        ConversationEvent ev{};
        ev.type = ConversationEventType::AssistantAudioChunk;
        ev.audio = std::move(pcm);
        emit(ev);
    }

    // ---- audio sender task -------------------------------------------------

    static void sender_trampoline(void* arg)
    {
        static_cast<Impl*>(arg)->sender_loop();
        vTaskDeleteWithCaps(nullptr);
    }

    void sender_loop()
    {
        AudioChunk* chunk = nullptr;
        while (!sender_should_exit_.load(std::memory_order_acquire)) {
            if (xQueueReceive(audio_tx_queue_, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
                continue;
            }
            if (chunk == nullptr) break; // teardown sentinel
            // Append to the 60 ms accumulator and flush whole Opus frames.
            enc_accum_.insert(enc_accum_.end(), chunk->data, chunk->data + chunk->len_samples);
            heap_caps_free(chunk);
            chunk = nullptr;
            flush_encoded_frames();
        }
    }

    void flush_encoded_frames()
    {
        if (encoder_ == nullptr || enc_in_bytes_ == 0) return;
        while (enc_accum_.size() >= kUplinkFrameSamples) {
            if (!encode_and_send(enc_accum_.data())) {
                // Not connected / not listening — drop the buffered audio so we
                // don't replay stale mic samples when the link comes back.
                enc_accum_.clear();
                return;
            }
            enc_accum_.erase(enc_accum_.begin(), enc_accum_.begin() + kUplinkFrameSamples);
        }
    }

    // Encodes one 60 ms frame at `samples` and sends it as a binary WS frame.
    // Returns false if the audio was discarded (not connected / not listening).
    bool encode_and_send(std::int16_t* samples)
    {
        if (client_ == nullptr || !esp_websocket_client_is_connected(client_)) return false;
        if (!hello_received_) return false;
        if (state_.load(std::memory_order_relaxed) != ConversationState::Listening) return false;

        esp_audio_enc_in_frame_t in{};
        in.buffer = reinterpret_cast<std::uint8_t*>(samples);
        in.len = static_cast<std::uint32_t>(enc_in_bytes_);
        esp_audio_enc_out_frame_t out{};
        out.buffer = enc_out_buf_.data();
        out.len = static_cast<std::uint32_t>(enc_out_buf_.size());
        out.encoded_bytes = 0;
        if (esp_opus_enc_process(encoder_, &in, &out) != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(kTag, "opus encode failed");
            return true; // keep the link; just drop this frame
        }
        if (out.encoded_bytes == 0) return true;

        int rc;
        {
            std::lock_guard lock{send_mutex_};
            rc = esp_websocket_client_send_bin(
                client_, reinterpret_cast<const char*>(enc_out_buf_.data()),
                static_cast<int>(out.encoded_bytes), kSendTimeout);
        }
        ++audio_seq_;
        // rc==0 is a poll_write timeout (nothing sent); the Opus frame here is
        // always non-empty, so treat rc<=0 as a failed send — matching the
        // gemini/openai audio-send heartbeats.
        if (rc <= 0) {
            ESP_LOGW(kTag, "audio send failed seq=%lu rc=%d",
                     static_cast<unsigned long>(audio_seq_), rc);
        } else if ((audio_seq_ % 100) == 1) {
            // Debug-only heartbeat — see gemini/openai counterparts.
            ESP_LOGD(kTag, "audio send seq=%lu bytes=%u",
                     static_cast<unsigned long>(audio_seq_),
                     static_cast<unsigned>(out.encoded_bytes));
        }
        return true;
    }

    // ---- members -----------------------------------------------------------

    std::string url_;
    std::string token_;
    std::string headers_;
    std::string session_id_;
    ConversationConfig config_;
    EventCallback event_callback_;

    esp_websocket_client_handle_t client_{nullptr};
    std::atomic<ConversationState> state_{ConversationState::Idle};
    std::mutex send_mutex_;

    bool hello_received_{false};
    std::uint32_t audio_seq_{0};
    std::uint32_t decode_fail_count_{0};
    unsigned rx_log_count_{0};
    std::uint32_t output_rate_{24000};     // rate the conv-task replays at
    std::uint32_t server_out_rate_{24000}; // rate the server's Opus TTS uses

    // Opus codec.
    void* encoder_{nullptr};
    void* decoder_{nullptr};
    std::size_t enc_in_bytes_{0};
    std::vector<std::uint8_t> enc_out_buf_;
    std::vector<std::int16_t, PsramAllocator<std::int16_t>> enc_accum_;
    std::vector<std::uint8_t, PsramAllocator<std::uint8_t>> decode_out_;

    // WS RX reassembly.
    std::uint8_t* rx_buffer_{nullptr};
    std::size_t rx_capacity_{0};
    std::size_t rx_len_{0};
    std::uint8_t rx_op_code_{0};

    QueueHandle_t audio_tx_queue_{nullptr};
    struct RxFrame {
        std::uint16_t len;
        std::uint8_t data[];
    };
    static constexpr UBaseType_t kRxQueueLen = 32;           // ~2 s of 60 ms frames
    static constexpr std::size_t kDecoderTaskStack = 16384;  // libopus decode is stack-hungry; PSRAM
    QueueHandle_t audio_rx_queue_{nullptr};
    TaskHandle_t decoder_task_{nullptr};
    std::atomic<bool> decoder_should_exit_{false};
    std::mutex decoder_mutex_;
    // Session-cumulative eviction count, read cross-task by
    // tx_evicted_chunks() → conv-task metrics.
    std::atomic<std::uint32_t> tx_evicted_total_{0};
    std::uint32_t last_evict_log_ms_{0}; // note_tx_eviction rate limiter; 0 = never logged
    TaskHandle_t sender_task_{nullptr};
    std::atomic<bool> sender_should_exit_{false};
};

// ---- public class plumbing ------------------------------------------------

XiaoZhiClient::XiaoZhiClient(std::string url, std::string token)
    : impl_{std::make_unique<Impl>(std::move(url), std::move(token))}
{
}

XiaoZhiClient::~XiaoZhiClient() = default;

void XiaoZhiClient::set_event_callback(EventCallback cb)
{
    impl_->set_event_callback(std::move(cb));
}

std::uint32_t XiaoZhiClient::tx_evicted_chunks() const
{
    return impl_->tx_evicted_chunks();
}

tl::expected<void, ConversationError> XiaoZhiClient::start(const ConversationConfig& config)
{
    return impl_->start(config);
}

void XiaoZhiClient::stop() { impl_->stop(); }

tl::expected<void, ConversationError> XiaoZhiClient::push_audio(std::span<const std::int16_t> pcm)
{
    return impl_->push_audio(pcm);
}

tl::expected<void, ConversationError> XiaoZhiClient::commit_audio()
{
    return impl_->commit_audio();
}

tl::expected<void, ConversationError>
XiaoZhiClient::submit_tool_result(std::string_view call_id, std::string_view output_json)
{
    return impl_->submit_tool_result(call_id, output_json);
}

tl::expected<void, ConversationError> XiaoZhiClient::push_text(std::string_view text)
{
    return impl_->push_text(text);
}

tl::expected<void, ConversationError> XiaoZhiClient::cancel_response()
{
    return impl_->cancel_response();
}

ConversationState XiaoZhiClient::state() const { return impl_->state(); }

} // namespace stackchan::conversation
