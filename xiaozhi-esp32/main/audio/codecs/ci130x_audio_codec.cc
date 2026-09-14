#include "ci130x_audio_codec.h"
#include "application.h"
#include "ci130x_protocol.h"
#include "settings.h"
#include "moinai_device_settings.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#include <algorithm>

#define TAG "Ci130xAudioCodec"

// Defaults used by the CI130X firmware currently flashed on the target.
#define CI_CMD_SET_VAD_FILTER_FRAME CI_CMD_SET_VAD_FILTER_FRAMES
#define CI_CMD_SET_WAKE_UP_CONTINUE_TIME CI_CMD_SET_WAKEUP_CONTINUE_TIME

static const uint8_t* HEADER = CIAS_HEADER;
static constexpr int kPlayGetWaitSliceMs = 200;
static constexpr int kPlayGetStallRecoveryMs = 1500;
static constexpr uint32_t kPlayGetWindowBytes = 4096;
// Cap on unspent PLAY_GET credit. Four PLAY_DATA frames are 4 * 1936 = 7744 wire bytes, so
// everything we may have in flight still fits in the CI130X 8212-byte UART stream buffer even
// while its receive task is blocked pushing PCM into a full player buffer. Unbounded credit is
// what used to overrun that buffer and silently drop bytes mid-frame.
static constexpr uint32_t kPlayGetMaxQuotaBytes = 4 * 1920;
static constexpr uint16_t kPowerOnPromptFirstId = 5000;
static constexpr int32_t kPowerOnPromptCount = 5;

Ci130xAudioCodec::Ci130xAudioCodec(uart_port_t uart_num, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud_rate,
                                   int input_sample_rate, int output_sample_rate)
    : uart_num_(uart_num), baud_rate_(baud_rate), tx_pin_(tx_pin), rx_pin_(rx_pin) {
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    pcm_rx_ringbuf_ = xRingbufferCreate(kPcmRxRingbufBytes, RINGBUF_TYPE_BYTEBUF);
    play_get_sem_ = xSemaphoreCreateCounting(50, 0);

    esp_timer_create_args_t timer_args = {
        .callback = OnPlaybackTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "playback_watchdog"
    };
    esp_timer_create(&timer_args, &playback_timer_);

    esp_timer_create_args_t local_play_timer_args = {
        .callback = OnLocalPlayFallbackTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "local_play_end"
    };
    esp_timer_create(&local_play_timer_args, &local_play_fallback_timer_);

    InitializeUart();
}

Ci130xAudioCodec::~Ci130xAudioCodec() {
    if (playback_timer_) {
        esp_timer_stop(playback_timer_);
        esp_timer_delete(playback_timer_);
    }
    if (local_play_fallback_timer_) {
        esp_timer_stop(local_play_fallback_timer_);
        esp_timer_delete(local_play_fallback_timer_);
    }
    if (pcm_rx_ringbuf_) {
        vRingbufferDelete(pcm_rx_ringbuf_);
    }
    if (play_get_sem_) {
        vSemaphoreDelete(play_get_sem_);
    }
    uart_driver_delete(uart_num_);
}

void Ci130xAudioCodec::InitializeUart() {
    uart_config_t uart_cfg = {
        .baud_rate = baud_rate_,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(uart_num_, 4096, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_num_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void Ci130xAudioCodec::Start() {
    StartRxTask();
    ApplyStoredSettings();

    AudioCodec::Start();
}

void Ci130xAudioCodec::ApplyStoredSettings() {
    const MoinaiDeviceSettings settings = GetMoinaiDeviceSettings();

    // The effective configuration, which the overlay can make differ from what
    // Ota::FetchDeviceSettings just logged as saved. Every value the CI130X is
    // about to be told is printed here, because the chip acknowledges the
    // commands without echoing their payloads: this line is the only record of
    // what it actually runs on.
    const char* const source =
        settings.debug_override_enabled ? "debug override" : "cloud";
    ESP_LOGI(TAG, "Applying %s settings: wakeup %ds, standby %ds, poll %ds",
             source, settings.wakeup_time_seconds, settings.standby_time_seconds,
             settings.topic_poll_interval_seconds);
    ESP_LOGI(TAG, "Applying %s VAD: end trigger %dms, sensitivity %d, "
             "filter %d frames, start max %ds",
             source, settings.vad_end_trigger_ms, settings.vad_sensitivity,
             settings.vad_filter_frames, settings.vad_start_max_timeout_seconds);
    ESP_LOGI(TAG, "Applying %s audio: volume %d, muted %d, denoise %d, "
             "multi-round %d, full duplex %d, stop play on VAD %d",
             source, settings.volume, settings.muted, settings.denoise_enabled,
             settings.multi_round, settings.full_duplex,
             settings.vad_stop_playback);

    SetVadSensitivity(settings.vad_sensitivity);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetPcmDenoiseEnabled(settings.denoise_enabled);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetVadFilterFrames(settings.vad_filter_frames);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetVadEndTrigger(settings.vad_end_trigger_ms);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetVadStartMaxTimeout(settings.vad_start_max_timeout_seconds);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetWakeupContinueTime(settings.wakeup_time_seconds);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetSwm221SleepTimeout(settings.standby_time_seconds);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetMultiRoundEnabled(settings.multi_round);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetFullDuplexEnabled(settings.full_duplex);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetCiVolume(settings.volume);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetMuted(settings.muted);
    vTaskDelay(pdMS_TO_TICKS(20));
    SetVadStopPlayback(settings.vad_stop_playback);
}

bool Ci130xAudioCodec::SetVadSensitivity(int sensitivity) {
    if (sensitivity < 45 || sensitivity > 60) {
        ESP_LOGW(TAG, "Invalid VAD sensitivity: %d", sensitivity);
        return false;
    }
    const uint8_t value = static_cast<uint8_t>(sensitivity);
    SendPacket(CI_CMD_SET_VAD_SENSITIVITY, &value, 1);
    return true;
}

bool Ci130xAudioCodec::SetPcmDenoiseEnabled(bool enabled) {
    const uint8_t value = enabled ? 1 : 0;
    SendPacket(CI_CMD_SET_PCM_DENOISE, &value, 1);
    return true;
}

bool Ci130xAudioCodec::SetVadFilterFrames(int frames) {
    if (frames < 15 || frames > 40) {
        ESP_LOGW(TAG, "Invalid VAD filter frames: %d", frames);
        return false;
    }
    const uint8_t payload[2] = {
        static_cast<uint8_t>(frames & 0xFF),
        static_cast<uint8_t>((frames >> 8) & 0xFF),
    };
    SendPacket(CI_CMD_SET_VAD_FILTER_FRAME, payload, sizeof(payload));
    return true;
}

bool Ci130xAudioCodec::SetVadEndTrigger(int milliseconds) {
    uint16_t preset = 0;
    switch (milliseconds) {
        case 300: preset = 6; break;
        case 400: preset = 15; break;
        case 500: preset = 20; break;
        case 700: preset = 30; break;
        case 1000: preset = 50; break;
        case 1500: preset = 80; break;
        case 2000: preset = 120; break;
        default:
            ESP_LOGW(TAG, "Invalid VAD end trigger: %d ms", milliseconds);
            return false;
    }
    const uint8_t payload[2] = {
        static_cast<uint8_t>(preset & 0xFF),
        static_cast<uint8_t>((preset >> 8) & 0xFF),
    };
    SendPacket(CI_CMD_SET_VAD_END_SILENCE, payload, sizeof(payload));
    return true;
}

bool Ci130xAudioCodec::SetVadStartMaxTimeout(int seconds) {
    if (seconds <= 0 || seconds > UINT16_MAX) {
        ESP_LOGW(TAG, "Invalid VAD start max timeout: %d", seconds);
        return false;
    }
    const uint8_t payload[2] = {
        static_cast<uint8_t>(seconds & 0xFF),
        static_cast<uint8_t>((seconds >> 8) & 0xFF),
    };
    SendPacket(CI_CMD_SET_VAD_START_MAX_TIMEOUT, payload, sizeof(payload));
    return true;
}

bool Ci130xAudioCodec::SetWakeupContinueTime(int seconds) {
    if (seconds <= 0 || seconds > UINT16_MAX) {
        ESP_LOGW(TAG, "Invalid wakeup continue time: %d", seconds);
        return false;
    }

    uint8_t payload[2] = {
        static_cast<uint8_t>(seconds & 0xFF),
        static_cast<uint8_t>((seconds >> 8) & 0xFF),
    };
    ESP_LOGI(TAG, "Setting CI130X wakeup continue time to %d seconds", seconds);
    SendPacket(CI_CMD_SET_WAKE_UP_CONTINUE_TIME, payload, sizeof(payload));
    return true;
}

bool Ci130xAudioCodec::SetSwm221SleepTimeout(int seconds) {
    if (seconds <= 0 || seconds > UINT16_MAX) {
        ESP_LOGW(TAG, "Invalid SWM221 sleep timeout: %d", seconds);
        return false;
    }

    const uint8_t payload[2] = {
        static_cast<uint8_t>(seconds & 0xFF),
        static_cast<uint8_t>((seconds >> 8) & 0xFF),
    };
    ESP_LOGI(TAG, "Setting SWM221 sleep timeout to %d seconds", seconds);
    SendPacket(CI_CMD_SET_SWM221_SLEEP_TIMEOUT, payload, sizeof(payload));
    return true;
}

void Ci130xAudioCodec::SetTopicPlayback(bool topic_playback) {
    const uint8_t value = topic_playback ? 1 : 0;
    SendPacket(CI_CMD_SET_TOPIC_PLAYBACK, &value, sizeof(value));
}

bool Ci130xAudioCodec::SetMultiRoundEnabled(bool enabled) {
    const uint8_t value = enabled ? 1 : 0;
    SendPacket(CI_CMD_SET_MULTI_ROUND, &value, 1);
    return true;
}

bool Ci130xAudioCodec::SetFullDuplexEnabled(bool enabled) {
    const uint8_t value = enabled ? 1 : 0;
    SendPacket(CI_CMD_SET_FULL_DUPLEX, &value, 1);
    return true;
}

bool Ci130xAudioCodec::SetCiVolume(int volume) {
    if (volume < 1 || volume > 7) {
        ESP_LOGW(TAG, "Invalid CI130X volume: %d", volume);
        return false;
    }
    const uint8_t value = static_cast<uint8_t>(volume);
    SendPacket(CI_CMD_SET_VOLUME, &value, 1);
    return true;
}

bool Ci130xAudioCodec::SetMuted(bool muted) {
    const uint8_t value = muted ? 1 : 0;
    SendPacket(CI_CMD_SET_MUTE, &value, 1);
    return true;
}

bool Ci130xAudioCodec::SetVadStopPlayback(bool enabled) {
    const uint8_t value = enabled ? 1 : 0;
    SendPacket(CI_CMD_SET_VAD_STOP_PLAY, &value, 1);
    return true;
}

void Ci130xAudioCodec::StartRxTask() {
    xTaskCreate(RxTask, "ci_rx_task", 4096, this, 10, NULL);
}

void Ci130xAudioCodec::RxTask(void* arg) {
    Ci130xAudioCodec* self = (Ci130xAudioCodec*)arg;
    uint8_t state = 0;
    uint8_t header_buf[16];
    int header_idx = 0;
    uint16_t current_cmd = 0;
    int payload_remain = 0;
    uint8_t chunk[512];
    uint8_t payload_buf[256];
    int payload_idx = 0;

    while (true) {
        int r = uart_read_bytes(self->uart_num_, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (r <= 0) continue;

        for (int i = 0; i < r; i++) {
            uint8_t byte = chunk[i];
            if (state < 4) {
                if (byte == HEADER[state]) state++;
                else state = (byte == HEADER[0]) ? 1 : 0;
                if (state == 4) header_idx = 4;
            } else if (state == 4) {
                header_buf[header_idx++] = byte;
                if (header_idx == 16) {
                    current_cmd = (uint16_t)(header_buf[6] | (header_buf[7] << 8));
                    uint16_t p_len = (uint16_t)(header_buf[8] | (header_buf[9] << 8));
                    payload_remain = (int)p_len;
                    payload_idx = 0;

                    if (current_cmd == CI_CMD_PCM_MIDDLE) {
                        self->HandlePacket(current_cmd, nullptr, 0); // Trigger immediate events
                    }

                    if (payload_remain > 0) {
                        state = 5;
                    } else {
                        if (current_cmd != CI_CMD_PCM_MIDDLE) {
                            self->HandlePacket(current_cmd, nullptr, 0);
                        }
                        state = 0;
                    }
                }
            } else if (state == 5) {
                if (current_cmd == CI_CMD_PCM_MIDDLE) {
                    int left = r - i;
                    int copy_len = (payload_remain < left) ? payload_remain : left;
                    // PCM payloads can span multiple UART reads. Only enqueue bytes while a
                    // live microphone session is accepting them; the ingress is closed
                    // between turns and while the CI130X is playing, and bytes arriving then
                    // are not part of any utterance.
                    if (self->accept_pcm_input_.load() &&
                        !self->cloud_playing_.load() && !self->local_playing_.load()) {
                        if (xRingbufferSend(self->pcm_rx_ringbuf_, &chunk[i], copy_len, 0) !=
                            pdTRUE) {
                            // The consumer could not keep up with the burst.
                            self->pcm_dropped_full_bytes_.fetch_add(copy_len);
                        }
                    } else {
                        self->pcm_dropped_closed_bytes_.fetch_add(copy_len);
                    }
                    payload_remain -= copy_len;
                    i += (copy_len - 1);
                } else {
                    if (payload_idx < (int)sizeof(payload_buf)) {
                        payload_buf[payload_idx++] = byte;
                    }
                    payload_remain--;
                }
                if (payload_remain <= 0) {
                    if (current_cmd != CI_CMD_PCM_MIDDLE) {
                        self->HandlePacket(current_cmd, payload_buf, payload_idx);
                    }
                    state = 0;
                }
            }
        }
    }
}

void Ci130xAudioCodec::HandlePacket(uint16_t cmd, const uint8_t* payload, size_t len) {
    switch (cmd) {
        case CI_CMD_ASR_RESULT: {
            uint16_t cmd_id = 0;
            if (payload != nullptr && len >= 2) {
                cmd_id = (uint16_t)(payload[0] | (payload[1] << 8));
            }
            ESP_LOGI(TAG, "CI130X ASR Result detected, cmd_id: %d", cmd_id);
            // Reset playing state on offline command to prevent driver lockup
            if (cmd_id == 100) {
                cloud_playing_.store(false);
            }
            if (on_asr_result_) {
                on_asr_result_(cmd_id);
            }
            break;
        }
        case CI_CMD_WAKEUP: {
            ESP_LOGI(TAG, "CI130X Wakeup detected");
            bool was_already_awake = is_awake_;
            is_awake_ = true;
            // The chip is capturing from here on and replays its roll-back the moment its VAD
            // fires, which can be well before the application reaches Listening and calls
            // EnableInput(). Open the ingress on this edge so the head of the utterance is
            // buffered across the handoff instead of being dropped in the parser. This is also
            // the start of a turn, and therefore the one point where stale PCM may be dropped.
            // Running on the RX task means the parser cannot refill behind the flush.
            FlushPcmRxBuffer();
            pcm_dropped_closed_bytes_.store(0);
            pcm_dropped_full_bytes_.store(0);
            accept_pcm_input_.store(true);
            mic_turn_armed_by_wakeup_.store(true);
            if (on_wakeup_) on_wakeup_(was_already_awake);
            break;
        }
        case CI_CMD_EXIT_WAKEUP:
            ESP_LOGI(TAG, "CI130X Exit Wakeup (timeout)");
            is_awake_ = false;
            // The window closed with no turn to hand the buffer to, so the next one must not
            // treat what is sitting there as its own.
            mic_turn_armed_by_wakeup_.store(false);
            if (on_exit_wakeup_) on_exit_wakeup_();
            if (on_vad_end_) on_vad_end_();
            break;
        case CI_CMD_LOCAL_PLAY_START:
            ESP_LOGI(TAG, "CI130X Local Play Start");
            local_playing_.store(true);
            if (on_local_play_start_) on_local_play_start_();
            break;
        case CI_CMD_LOCAL_PLAY_STOP:
            ESP_LOGD(TAG, "CI130X Local Play Stop");
            local_playing_.store(false);
            if (power_on_prompt_started_.load() && !power_on_prompt_finished_.load()) {
                // The greeting is sent only once the queue has drained, so the
                // first stop after it starts is its own.
                power_on_prompt_finished_.store(true);
                ESP_LOGI(TAG, "Power-on prompt completed");
            } else {
                // A status prompt: either one the greeting is still waiting on,
                // or a late one reported after it. Retire it either way so the
                // wakeup gate cannot stay stuck on a stale count.
                uint16_t pending = startup_status_pending_.load();
                while (pending > 0 &&
                       !startup_status_pending_.compare_exchange_weak(pending, pending - 1)) {
                }
                ESP_LOGI(TAG, "Startup status prompt completed, %u remaining",
                         startup_status_pending_.load());
                // Hold the greeting until the whole queue is done, otherwise it
                // preempts a status prompt that is still playing.
                if (startup_status_pending_.load() == 0 && power_on_prompt_armed_.load()) {
                    SendPowerOnPrompt();
                }
            }
            TryEnterWakeupAfterPowerOn();
            break;
        case CI_CMD_VAD_START:
            ESP_LOGI(TAG, "CI130X VAD Start");
            if (on_vad_start_) on_vad_start_();
            break;
        case CI_CMD_VAD_END:
        case CI_CMD_PCM_FINISH: {
            // 32 bytes of 16 kHz mono PCM is 1 ms. "closed" is microphone audio the
            // application was not ready to accept yet, which is the head of the utterance;
            // "full" means the RX ring was too small for the burst. Both should read 0.
            const uint32_t dropped_closed = pcm_dropped_closed_bytes_.load();
            const uint32_t dropped_full = pcm_dropped_full_bytes_.load();
            ESP_LOGI(TAG, "CI130X VAD End / PCM Finish (dropped: closed %lu ms, full %lu ms)",
                     (unsigned long)(dropped_closed / 32), (unsigned long)(dropped_full / 32));
            if (on_vad_end_) on_vad_end_();
            break;
        }
        case CI_CMD_PLAY_STOP_EVT:
            ESP_LOGI(TAG, "CI130X Play Stop Event");
            if (cloud_playing_.exchange(false)) {
                if (playback_timer_) esp_timer_stop(playback_timer_);
                play_get_quota_.store(0);
                if (play_get_sem_) xQueueReset(play_get_sem_);
                if (output_stream_active_.load()) {
                    // The cloud stream still has PCM pending. Treat this as a
                    // recoverable CI130X underrun, not the end of the response.
                    ESP_LOGW(TAG, "CI130X stopped while stream is active; preserving pending PCM");
                    if (play_get_sem_) xSemaphoreGive(play_get_sem_);
                } else {
                    ReportTtsPlaybackFinished();
                }
            }
            break;
        case CI_CMD_PLAY_GET: {
            // Every PLAY_DATA_GET grants one fixed 4096-byte window; its payload is not
            // interpreted as a byte count. Grants accumulate up to kPlayGetMaxQuotaBytes so the
            // link can stay a few frames ahead of the CI130X player. Honouring exactly one
            // window at a time instead makes each window cost the 20 ms delay the CI130X request
            // loop takes after asking, which drags playback below real time: the player then
            // underruns, and our own decode queue backs up until it drops audio.
            uint32_t quota = play_get_quota_.load();
            uint32_t granted;
            do {
                granted = std::min(quota + kPlayGetWindowBytes, kPlayGetMaxQuotaBytes);
            } while (!play_get_quota_.compare_exchange_weak(quota, granted));
            if (play_get_sem_) xSemaphoreGive(play_get_sem_);
            break;
        }
        case CI_CMD_EXEC_STATE:
            if (payload != nullptr && len >= 3) {
                const uint16_t executed_cmd = static_cast<uint16_t>(
                    payload[0] | (payload[1] << 8));
                if (payload[2] == 0x01) {
                    // PLAY_DATA is sent for every TTS PCM frame.  The CI130X
                    // acknowledges each frame, so logging successful results
                    // here would flood the serial console during playback.
                    if (executed_cmd != CI_CMD_PLAY_DATA) {
                        ESP_LOGI(TAG, "CIAS command 0x%04X applied", executed_cmd);
                    }
                } else {
                    ESP_LOGE(TAG, "CIAS command 0x%04X failed, result=0x%02X",
                             executed_cmd, payload[2]);
                }
            } else {
                ESP_LOGW(TAG, "Malformed CIAS execution result, length=%u",
                         static_cast<unsigned>(len));
            }
            break;
        default:
            break;
    }
}

void Ci130xAudioCodec::SendPacket(uint16_t cmd, const uint8_t* payload, size_t len) {
    std::lock_guard<std::mutex> lock(uart_mutex_);
    uint8_t header[16] = {0};
    memcpy(header, HEADER, 4);
    header[6] = cmd & 0xFF; header[7] = (cmd >> 8) & 0xFF;
    header[8] = len & 0xFF; header[9] = (len >> 8) & 0xFF;
    header[10] = 0x01;
    uint32_t tail = 0x12345678;
    memcpy(&header[12], &tail, 4);

    uart_write_bytes(uart_num_, (const char*)header, 16);
    if (len > 0 && payload != nullptr) {
        uart_write_bytes(uart_num_, (const char*)payload, len);
    }
}

int Ci130xAudioCodec::Read(int16_t* dest, int samples) {
    // CI130X only sends PCM data during VAD-active periods. UART delivery is
    // fragmented, so accumulate bytes until one complete caller-requested PCM
    // block is available. Always pace one call to the duration represented by
    // that block: buffered UART fragments must not turn into a burst of fake
    // real-time audio, and each fragment must not be padded as a separate block.
    size_t size_req = samples * sizeof(int16_t);
    const int64_t period_us = std::max<int64_t>(1000,
        static_cast<int64_t>(samples) * 1000000 / input_sample_rate_);
    std::unique_lock<std::mutex> read_lock(pcm_read_mutex_);

    int64_t now_us = esp_timer_get_time();
    if (next_pcm_read_deadline_us_ == 0 || now_us >= next_pcm_read_deadline_us_) {
        next_pcm_read_deadline_us_ = now_us + period_us;
    }
    const int64_t deadline_us = next_pcm_read_deadline_us_;
    next_pcm_read_deadline_us_ += period_us;

    size_t bytes_copied = 0;
    const bool accept_audio = accept_pcm_input_.load() &&
        !cloud_playing_.load() && !local_playing_.load();
    while (accept_audio && bytes_copied < size_req) {
        now_us = esp_timer_get_time();
        if (now_us >= deadline_us) {
            break;
        }

        const int wait_ms = static_cast<int>((deadline_us - now_us + 999) / 1000);
        const TickType_t wait_ticks = std::max<TickType_t>(1, pdMS_TO_TICKS(wait_ms));
        size_t received = 0;
        uint8_t* data = static_cast<uint8_t*>(xRingbufferReceiveUpTo(
            pcm_rx_ringbuf_, &received, wait_ticks, size_req - bytes_copied));
        if (data != nullptr) {
            memcpy(reinterpret_cast<uint8_t*>(dest) + bytes_copied, data, received);
            bytes_copied += received;
            vRingbufferReturnItem(pcm_rx_ringbuf_, data);
        }
    }

    // Silence is generated at one block per real-time period, never once per
    // UART fragment. This also keeps the AudioInputTask alive during VAD silence.
    if (bytes_copied < size_req) {
        memset(reinterpret_cast<uint8_t*>(dest) + bytes_copied, 0, size_req - bytes_copied);
    }

    now_us = esp_timer_get_time();
    if (now_us < deadline_us) {
        const int delay_ms = static_cast<int>((deadline_us - now_us + 999) / 1000);
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(delay_ms)));
    }
    return samples;  // Always return requested count to keep AudioInputTask alive
}

void Ci130xAudioCodec::StartPlaybackSession() {
    tts_session_++;
    play_get_quota_.store(0);
    if (play_get_sem_) xQueueReset(play_get_sem_);
    ESP_LOGI(TAG, "Starting CI130X playback (session=%lu)",
             (unsigned long)tts_session_);
    SendPacket(CI_CMD_PLAY_START, nullptr, 0);
    cloud_playing_.store(true);
}

int Ci130xAudioCodec::Write(const int16_t* data, int samples) {
    // TTS blocking: drop ALL audio packets while blocked.
    // Block is cleared explicitly by board code via ClearTtsBlock() before StartListening.
    if (tts_blocked_) {
        if (!tts_block_abort_sent_) {
            tts_block_abort_sent_ = true;
            ESP_LOGI(TAG, "TTS blocked: Aborting speaking session.");
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().AbortSpeaking(kAbortReasonWakeWordDetected);
            });
        }
        return samples;
    }

    if (!cloud_playing_.load()) {
        StartPlaybackSession();
    }

    // A Write call means the stream is alive even if CI130X is currently
    // applying backpressure. Do not let the idle watchdog end playback while
    // this task is waiting for PLAY_GET.
    if (playback_timer_) esp_timer_stop(playback_timer_);

    size_t bytes = samples * sizeof(int16_t);
    int64_t stall_start_us = esp_timer_get_time();

    while (true) {
        // Reserve enough credit for the complete PCM frame. PLAY_STOP may
        // concurrently reset the quota, so use CAS instead of load/subtract.
        uint32_t quota = play_get_quota_.load();
        while (quota >= bytes) {
            if (play_get_quota_.compare_exchange_weak(
                    quota, quota - static_cast<uint32_t>(bytes))) {
                if (cloud_playing_.load()) {
                    SendPacket(CI_CMD_PLAY_DATA,
                               reinterpret_cast<const uint8_t*>(data),
                               bytes);

                    // Non-stream sounds do not have an explicit end callback,
                    // so retain the original idle watchdog for that path.
                    if (playback_timer_ && !output_stream_active_.load()) {
                        esp_timer_start_once(playback_timer_, 800 * 1000);
                    }
                    return samples;
                }
                break;
            }
        }

        // A mid-stream PLAY_TTS_END revokes the old credit. Start a fresh
        // CI130X playback session but keep this PCM frame pending.
        if (!cloud_playing_.load()) {
            if (!output_stream_active_.load()) {
                return samples;
            }
            StartPlaybackSession();
            stall_start_us = esp_timer_get_time();
            continue;
        }

        if (xSemaphoreTake(play_get_sem_,
                           pdMS_TO_TICKS(kPlayGetWaitSliceMs)) == pdTRUE) {
            continue;
        }

        const int elapsed_ms =
            static_cast<int>((esp_timer_get_time() - stall_start_us) / 1000);
        if (elapsed_ms < kPlayGetStallRecoveryMs) {
            // This is normal CI130X backpressure. Keep the current frame;
            // logging every 200 ms adds load without providing new evidence.
            continue;
        }

        ESP_LOGW(TAG,
                 "PLAY_GET stalled %d ms, restarting playback without dropping PCM "
                 "(quota=%lu, session=%lu)",
                 elapsed_ms,
                 (unsigned long)play_get_quota_.load(),
                 (unsigned long)tts_session_);
        // Use the protocol's immediate stop for recovery and wait for its
        // PLAY_TTS_END acknowledgement before starting the new generation.
        // Starting after a fixed 20 ms delay can let the old stop event kill
        // the newly started session.
        play_get_quota_.store(0);
        if (play_get_sem_) xQueueReset(play_get_sem_);
        SendPacket(CI_CMD_PLAY_STOP, nullptr, 0);
        for (int wait_ms = 0;
             wait_ms < 500 && cloud_playing_.load();
             wait_ms += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        cloud_playing_.store(false);
        StartPlaybackSession();
        stall_start_us = esp_timer_get_time();
    }
}

int Ci130xAudioCodec::OutputBufferedMs() const {
    // PCM is sent directly under CI130X PLAY_GET flow control; there is no
    // additional host-side output ring buffer.
    return 0;
}

void Ci130xAudioCodec::NotifyOutputStreamStart() {
    output_stream_active_.store(true);
    tts_stream_completion_pending_.store(true);
    if (playback_timer_) esp_timer_stop(playback_timer_);
}

void Ci130xAudioCodec::NotifyOutputStreamEnd() {
    output_stream_active_.store(false);
    if (playback_timer_) esp_timer_stop(playback_timer_);
    if (cloud_playing_.load()) {
        ESP_LOGI(TAG, "TTS stream drained, sending PLAY_DATA_END (session=%lu)",
                 (unsigned long)tts_session_);
        SendPacket(CI_CMD_PLAY_DATA_END, nullptr, 0);
        // Normally CI130X answers with PLAY_TTS_END. Keep a bounded fallback
        // so a lost end event cannot leave the application stuck in playback.
        if (playback_timer_) {
            esp_timer_start_once(playback_timer_, 1500 * 1000);
        }
    } else if (tts_stream_completion_pending_.load()) {
        // The CI130X session is already gone (for example a mid-stream PLAY_TTS_END that was
        // never followed by another PCM frame), so no end event will ever arrive. Report the
        // response as finished here, otherwise the application waits forever in Speaking.
        ESP_LOGW(TAG, "TTS stream drained with no active CI130X session (session=%lu)",
                 (unsigned long)tts_session_);
        ReportTtsPlaybackFinished();
    }
}

void Ci130xAudioCodec::ReportTtsPlaybackFinished() {
    tts_stream_completion_pending_.store(false);
    if (on_tts_end_) {
        on_tts_end_();
    }
}

void Ci130xAudioCodec::OnPlaybackTimer(void* arg) {
    auto* self = static_cast<Ci130xAudioCodec*>(arg);
    if (self->cloud_playing_.exchange(false)) {
        ESP_LOGW(TAG, "Playback watchdog triggered, sending PLAY_DATA_END");
        self->SendPacket(CI_CMD_PLAY_DATA_END, nullptr, 0);
        self->play_get_quota_.store(0);
        if (self->play_get_sem_) xQueueReset(self->play_get_sem_);
        self->ReportTtsPlaybackFinished();
    }
}

void Ci130xAudioCodec::OnLocalPlayFallbackTimer(void* arg) {
    auto* self = static_cast<Ci130xAudioCodec*>(arg);
    if (self->power_on_prompt_armed_.load() && !self->power_on_prompt_started_.load()) {
        // A status prompt never reported completion. Drop the queue accounting
        // and play the greeting anyway; the timer restarts to cover it.
        ESP_LOGW(TAG, "Startup status prompts did not complete; playing power-on prompt");
        self->startup_status_pending_.store(0);
        self->SendPowerOnPrompt();
        return;
    }
    ESP_LOGW(TAG, "Startup audio completion timed out; enabling wakeup");
    self->power_on_prompt_finished_.store(true);
    self->startup_status_pending_.store(0);
    self->TryEnterWakeupAfterPowerOn();
}

void Ci130xAudioCodec::TryEnterWakeupAfterPowerOn() {
    if (!power_on_prompt_finished_.load() || !backend_ready_for_wakeup_.load() ||
        startup_status_pending_.load() != 0 || local_playing_.load()) {
        return;
    }
    if (!power_on_wakeup_entered_.exchange(true)) {
        if (local_play_fallback_timer_) esp_timer_stop(local_play_fallback_timer_);
        if (is_awake_) {
            // The wake word already opened a turn while the startup audio played.
            // Re-entering wakeup here would cancel that conversation.
            ESP_LOGI(TAG, "Startup audio done; keeping the wake-word turn already in progress");
            return;
        }
        ESP_LOGI(TAG, "Startup prompts and backend are ready; entering wakeup for listening");
        EnterWakeup();
    }
}

void Ci130xAudioCodec::SetOutputVolume(int volume) {
    AudioCodec::SetOutputVolume(volume);

    // Volume range for CI130X is typically 1-7 (0x0117 command)
    // Scale 0-100 to 1-7
    uint8_t ci_vol = (uint8_t)(1 + (volume * 6 / 100));
    if (ci_vol > 7) ci_vol = 7;

    ESP_LOGI(TAG, "Configuring CI130X volume to %d", ci_vol);
    SendPacket(CI_CMD_SET_VOLUME, &ci_vol, 1);
}

void Ci130xAudioCodec::FlushPcmRxBuffer() {
    size_t item_size;
    void* item;
    while ((item = xRingbufferReceive(pcm_rx_ringbuf_, &item_size, 0)) != nullptr) {
        vRingbufferReturnItem(pcm_rx_ringbuf_, item);
    }
}

size_t Ci130xAudioCodec::PendingInputBytes() const {
    if (pcm_rx_ringbuf_ == nullptr) {
        return 0;
    }
    const size_t free_bytes = xRingbufferGetCurFreeSize(pcm_rx_ringbuf_);
    return free_bytes >= kPcmRxRingbufBytes ? 0 : kPcmRxRingbufBytes - free_bytes;
}

void Ci130xAudioCodec::BeginMicTurn() {
    if (mic_turn_armed_by_wakeup_.exchange(false)) {
        // A wakeup already opened the buffer for this turn and the CI130X may have started
        // streaming into it before the application got here. Flushing now would throw away the
        // head of the utterance, which is exactly what arming early exists to keep.
        return;
    }

    // A turn that begins without a wakeup (continuous dialogue) inherits whatever the parser
    // buffered since the previous turn ended. That audio belongs to no utterance, so it must
    // not be prepended to this one. Close the ingress first so the RX task cannot refill
    // behind the flush; it runs on another task than this one.
    accept_pcm_input_.store(false);
    FlushPcmRxBuffer();
    pcm_dropped_closed_bytes_.store(0);
    pcm_dropped_full_bytes_.store(0);
    accept_pcm_input_.store(true);
}

void Ci130xAudioCodec::EnableInput(bool enable) {
    if (enable) {
        // Deliberately no flush here. CI_CMD_WAKEUP opened the ingress for this turn, so what
        // the parser has buffered since then is the beginning of the utterance: the CI130X
        // starts streaming on its own VAD, which happens before the application gets here.
        accept_pcm_input_.store(true);
    } else {
        // Closing the session is the only safe place to drop captured PCM. Stop the parser
        // first so bytes it is still handling cannot refill the buffer behind the flush.
        accept_pcm_input_.store(false);
        FlushPcmRxBuffer();
    }
    AudioCodec::EnableInput(enable);
    std::lock_guard<std::mutex> read_lock(pcm_read_mutex_);
    next_pcm_read_deadline_us_ = 0;
    // Ingress boundary: kept at INFO because a dropped utterance head is diagnosed
    // from when this path opened relative to the wakeup and VAD start.
    ESP_LOGI(TAG, "PCM RX path %s", enable ? "opened" : "closed and flushed");
}

void Ci130xAudioCodec::EnableOutput(bool enable) {
    AudioCodec::EnableOutput(enable);
    if (!enable) {
        output_stream_active_.store(false);
        if (playback_timer_) esp_timer_stop(playback_timer_);
        SendPacket(CI_CMD_PLAY_DATA_END, nullptr, 0);
        cloud_playing_.store(false);
        play_get_quota_.store(0);
        if (play_get_sem_) xQueueReset(play_get_sem_);
    }
}

void Ci130xAudioCodec::BlockNextTts() {
    tts_blocked_ = true;
    tts_block_abort_sent_ = false;
    output_stream_active_.store(false);
    // The caller ends this turn itself, so the discarded response must not also report a
    // playback completion once the blocked frames drain out of the pipeline.
    tts_stream_completion_pending_.store(false);

    // If audio is currently playing on the CI130X chip, immediately stop it
    if (cloud_playing_.load()) {
        ESP_LOGI(TAG, "BlockNextTts: Forcefully stopping active playback");
        if (playback_timer_) esp_timer_stop(playback_timer_);
        SendPacket(CI_CMD_PLAY_DATA_END, nullptr, 0);
        cloud_playing_.store(false);
        play_get_quota_.store(0);
        if (play_get_sem_) xQueueReset(play_get_sem_);
    }
    SendPacket(CI_CMD_SET_CLOUD_ANS_TIMEOUT_EXIT, nullptr, 0);
}

void Ci130xAudioCodec::ClearTtsBlock() {
    tts_blocked_ = false;
    tts_block_abort_sent_ = false;
}

void Ci130xAudioCodec::PlayOfflineVoice(uint16_t voice_id) {
    uint8_t payload[3];
    payload[0] = voice_id & 0xFF;
    payload[1] = (voice_id >> 8) & 0xFF;
    payload[2] = 0x01; // preemptive = true
    SendPacket(CI_CMD_SET_PLAY_VOICE_ID, payload, 3);
}

void Ci130xAudioCodec::NotifyStatus(Ci130xStatus status) {
    uint16_t status_id = static_cast<uint16_t>(status);
    if (last_status_notified_.exchange(status_id) == status_id) {
        ESP_LOGI(TAG, "Suppressing duplicate system status %u", status_id);
        return;
    }
    // A silent status produces no local playback, so the CI130X never reports
    // its completion. Counting it would leave the greeting waiting forever.
    const bool audible = Ci130xStatusIsAudible(status_id);
    if (audible && !power_on_wakeup_entered_.load()) {
        startup_status_pending_.fetch_add(1);
    }
    uint8_t payload[3] = {
        static_cast<uint8_t>(status_id & 0xFF),
        static_cast<uint8_t>((status_id >> 8) & 0xFF),
        static_cast<uint8_t>(audible ? 1 : 0),
    };
    ESP_LOGI(TAG, "Reporting system status %u to CI130X (audible=%d)", status_id,
             audible ? 1 : 0);
    SendPacket(CI_CMD_STATUS_NOTIFY, payload, sizeof(payload));
}

void Ci130xAudioCodec::StartPowerOnPrompt() {
    if (power_on_prompt_requested_.exchange(true)) {
        return;
    }

    // Pick the voice here rather than in SendPowerOnPrompt(): this runs on the
    // main loop, while the send can be driven from the RX task, whose stack is
    // too small for NVS access.
    {
        Settings settings("ci130x", true);
        int32_t last_index = settings.GetInt("pwr_prompt", kPowerOnPromptCount - 1);
        if (last_index < 0 || last_index >= kPowerOnPromptCount) {
            last_index = kPowerOnPromptCount - 1;
        }
        int32_t next_index = (last_index + 1) % kPowerOnPromptCount;
        settings.SetInt("pwr_prompt", next_index);
        power_on_prompt_voice_id_ = kPowerOnPromptFirstId + static_cast<uint16_t>(next_index);
    }

    if (local_play_fallback_timer_) {
        esp_timer_stop(local_play_fallback_timer_);
        esp_timer_start_once(local_play_fallback_timer_, 30ULL * 1000 * 1000);
    }

    // Publish only now: the RX task may send the prompt as soon as it sees this.
    power_on_prompt_armed_.store(true);

    uint16_t pending = startup_status_pending_.load();
    if (pending == 0 && !local_playing_.load()) {
        SendPowerOnPrompt();
    } else {
        ESP_LOGI(TAG, "Power-on prompt %u queued behind %u status prompt(s)",
                 power_on_prompt_voice_id_, pending);
    }
}

void Ci130xAudioCodec::SendPowerOnPrompt() {
    if (power_on_prompt_started_.exchange(true)) {
        return;
    }
    ESP_LOGI(TAG, "Starting power-on prompt %u", power_on_prompt_voice_id_);
    if (local_play_fallback_timer_) {
        esp_timer_stop(local_play_fallback_timer_);
        esp_timer_start_once(local_play_fallback_timer_, 30ULL * 1000 * 1000);
    }
    PlayOfflineVoice(power_on_prompt_voice_id_);
}

void Ci130xAudioCodec::NotifyBackendReadyForWakeup() {
    backend_ready_for_wakeup_.store(true);
    ESP_LOGI(TAG, "Backend ready for wakeup");
    TryEnterWakeupAfterPowerOn();
}

void Ci130xAudioCodec::EnterWakeup() {
    const uint8_t silent_enter_wakeup = 0;
    ESP_LOGI(TAG, "Entering CI130X wakeup state");
    SendPacket(CI_CMD_SET_ENTER_WAKE_UP, &silent_enter_wakeup, 1);
}

void Ci130xAudioCodec::ExitWakeup() {
    const uint8_t silent_exit_wakeup = 0;
    ESP_LOGI(TAG, "Exiting CI130X wakeup state");
    SendPacket(CI_CMD_SET_AUDIO_EXIT_WAKEUP, &silent_exit_wakeup, 1);
}
