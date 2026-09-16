#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <esp_random.h>
#include <new>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    // Moinai's embedded WebSocket endpoint uses 16 kHz, 60 ms raw Opus TTS
    // packets. Keep these as the safe fallback until bot_ready arrives;
    // the server-provided values below still take precedence.
    server_sample_rate_ = 16000;
    server_frame_duration_ = 60;
    event_group_handle_ = xEventGroupCreate();
    deferred_send_queue_ = xQueueCreate(4, sizeof(std::string*));
    if (deferred_send_queue_ == nullptr ||
        xTaskCreate([](void* arg) {
            static_cast<WebsocketProtocol*>(arg)->DeferredSendTask();
        }, "ws_deferred_send", 4096, this, 2, &deferred_send_task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create deferred WebSocket send task");
        deferred_send_task_handle_ = nullptr;
    }
}

WebsocketProtocol::~WebsocketProtocol() {
    if (deferred_send_task_handle_ != nullptr) {
        std::string* stop = nullptr;
        xQueueSend(deferred_send_queue_, &stop, portMAX_DELAY);
        xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SEND_TASK_STOPPED_EVENT,
                            pdTRUE, pdFALSE, portMAX_DELAY);
    }
    if (deferred_send_queue_ != nullptr) {
        std::string* pending = nullptr;
        while (xQueueReceive(deferred_send_queue_, &pending, 0) == pdTRUE) {
            delete pending;
        }
        vQueueDelete(deferred_send_queue_);
    }
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::DeferTextSend(std::string message) {
    if (deferred_send_queue_ == nullptr || deferred_send_task_handle_ == nullptr) {
        ESP_LOGE(TAG, "Deferred WebSocket send task is unavailable");
        return false;
    }

    auto* pending = new (std::nothrow) std::string(std::move(message));
    if (pending == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate deferred WebSocket message");
        return false;
    }
    if (xQueueSend(deferred_send_queue_, &pending, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Deferred WebSocket send queue is full");
        delete pending;
        return false;
    }
    return true;
}

void WebsocketProtocol::DeferredSendTask() {
    while (true) {
        std::string* pending = nullptr;
        if (xQueueReceive(deferred_send_queue_, &pending, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (pending == nullptr) {
            break;
        }

        std::unique_ptr<std::string> message(pending);
        if (websocket_ == nullptr || !websocket_->IsConnected() || !websocket_->Send(*message)) {
            ESP_LOGE(TAG, "Failed to send deferred Socket.IO control frame");
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SEND_TASK_STOPPED_EVENT);
    vTaskDelete(nullptr);
}

bool WebsocketProtocol::Start() {
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    char msg_id[64];
    static uint32_t seq = 0;
    const uint32_t audio_seq = seq++;
    snprintf(msg_id, sizeof(msg_id), "audio-%08x-%08x", (unsigned int)esp_random(), (unsigned int)audio_seq);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long time_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    const int64_t socketio_pack_start_us = esp_timer_get_time();
    cJSON* root = cJSON_CreateArray();
    cJSON_AddItemToArray(root, cJSON_CreateString("bot_input"));

    cJSON* dto = cJSON_CreateObject();
    cJSON_AddStringToObject(dto, "id", msg_id);
    cJSON_AddStringToObject(dto, "type", "audio");
    cJSON_AddNumberToObject(dto, "timestamp", time_ms);

    cJSON* payload = cJSON_CreateObject();
    cJSON* audio_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(audio_obj, "_placeholder", true);
    cJSON_AddNumberToObject(audio_obj, "num", 0);
    cJSON_AddItemToObject(payload, "audio", audio_obj);
    cJSON_AddItemToObject(dto, "payload", payload);

    cJSON_AddItemToArray(root, dto);

    char* json_str = cJSON_PrintUnformatted(root);
    std::string text_frame = "451-";
    text_frame += json_str;
    cJSON_free(json_str);
    cJSON_Delete(root);
    const int64_t socketio_pack_end_us = esp_timer_get_time();

    const int64_t metadata_send_start_us = esp_timer_get_time();
    if (!websocket_->Send(text_frame)) {
        ESP_LOGE(TAG, "Failed to send audio metadata frame");
        return false;
    }

    const int64_t metadata_send_end_us = esp_timer_get_time();
    const int64_t audio_send_start_us = metadata_send_end_us;
    const bool audio_sent = websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    const int64_t audio_send_end_us = esp_timer_get_time();

    // websocket_->Send() blocks until the ML307C transport has accepted the
    // corresponding AT+MIPSEND write. Sample this every 25 Opus frames (about
    const int pcm_to_opus_ms = packet->mic_capture_time_us == 0 || packet->opus_ready_time_us == 0
        ? -1
        : static_cast<int>((packet->opus_ready_time_us - packet->mic_capture_time_us) / 1000);
    const int afe_to_encode_ms = packet->esp_timestamp_us == 0 || packet->opus_encode_start_time_us == 0
        ? -1
        : static_cast<int>((packet->opus_encode_start_time_us - packet->esp_timestamp_us) / 1000);
    const int opus_encode_ms = packet->opus_encode_time_us == 0
        ? -1
        : static_cast<int>(packet->opus_encode_time_us / 1000);
    const int opus_to_send_ms = packet->opus_ready_time_us == 0
        ? -1
        : static_cast<int>((metadata_send_start_us - packet->opus_ready_time_us) / 1000);
    const int socketio_pack_ms = static_cast<int>((socketio_pack_end_us - socketio_pack_start_us) / 1000);
    const int metadata_to_ml307_ms = static_cast<int>((metadata_send_end_us - metadata_send_start_us) / 1000);
    const int esp_to_ml307_ms = static_cast<int>((audio_send_end_us - audio_send_start_us) / 1000);
    const int total_ms = static_cast<int>((audio_send_end_us - metadata_send_start_us) / 1000);
    static int64_t last_audio_send_start_us = 0;
    const int interval_ms = last_audio_send_start_us == 0
        ? -1
        : static_cast<int>((metadata_send_start_us - last_audio_send_start_us) / 1000);
    last_audio_send_start_us = metadata_send_start_us;

    // Log every 25 frames (about 1.5 seconds for 60 ms frames), plus every
    // scheduling outlier, so diagnostics do not disrupt real-time audio.
    if (!audio_sent || audio_seq % 25 == 0 || afe_to_encode_ms > 30) {
        ESP_LOGI(TAG,
                 "Audio TX seq=%u opus=%uB interval=%dms pcm_to_opus=%dms afe_to_encode=%dms opus_encode=%dms "
                 "opus_to_send=%dms socketio_pack=%dms metadata_to_ml307=%dms "
                 "esp_to_ml307=%dms total=%dms ok=%d",
                 static_cast<unsigned>(audio_seq), static_cast<unsigned>(packet->payload.size()),
                 interval_ms, pcm_to_opus_ms, afe_to_encode_ms, opus_encode_ms, opus_to_send_ms, socketio_pack_ms,
                 metadata_to_ml307_ms, esp_to_ml307_ms, total_ms, audio_sent ? 1 : 0);
    }
    return audio_sent;
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        // The server cannot tell a marker the device never produced from one it
        // produced after the socket went away. Name the drop so the log can.
        ESP_LOGW(TAG, "Socket down, dropping outbound message: %s", text.c_str());
        return false;
    }

    if (text == "3" || text.rfind("40", 0) == 0) {
        return websocket_->Send(text);
    }

    cJSON* legacy = cJSON_Parse(text.c_str());
    if (!legacy) {
        return websocket_->Send(text);
    }

    cJSON* type = cJSON_GetObjectItem(legacy, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(legacy);
        return websocket_->Send(text);
    }

    std::string type_str = type->valuestring;
    cJSON* state = cJSON_GetObjectItem(legacy, "state");
    std::string state_str = state && cJSON_IsString(state) ? state->valuestring : "";

    cJSON* dto = cJSON_CreateObject();
    char msg_id[64];
    snprintf(msg_id, sizeof(msg_id), "msg-%08x-%08x", (unsigned int)esp_random(), (unsigned int)esp_random());
    cJSON_AddStringToObject(dto, "id", msg_id);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long time_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    cJSON_AddNumberToObject(dto, "timestamp", time_ms);

    bool should_send = false;

    if (type_str == "listen" && (state_str == "start" || state_str == "detect")) {
        cJSON_AddStringToObject(dto, "type", "event");
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "event", "start_talk");
        cJSON_AddItemToObject(dto, "payload", payload);
        should_send = true;
    } else if (type_str == "listen" && state_str == "stop") {
        cJSON_AddStringToObject(dto, "type", "event");
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "event", "vad_done");
        cJSON_AddItemToObject(dto, "payload", payload);
        should_send = true;
    } else if (type_str == "cancel_turn" || type_str == "abort") {
        // Both revoke the active server-side turn. "abort" used to have no mapping
        // here and was silently dropped, so barge-in never reached the backend and
        // it kept streaming the rest of the answer; map it to cancel_turn now.
        cJSON_AddStringToObject(dto, "type", "event");
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "event", "cancel_turn");
        cJSON* legacy_reason = cJSON_GetObjectItem(legacy, "reason");
        const char* reason = cJSON_IsString(legacy_reason) ? legacy_reason->valuestring : "user_wakeup";
        cJSON_AddStringToObject(payload, "reason", reason);
        cJSON_AddItemToObject(dto, "payload", payload);
        should_send = true;
    } else if (type_str == "mcp") {
        cJSON_AddStringToObject(dto, "type", "event");
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "event", "mcp");

        cJSON* legacy_payload = cJSON_GetObjectItem(legacy, "payload");
        if (legacy_payload) {
            cJSON_AddItemToObject(payload, "data", cJSON_Duplicate(legacy_payload, 1));
        }
        cJSON_AddItemToObject(dto, "payload", payload);
        should_send = true;
    } else if (type_str == "text") {
        cJSON_AddStringToObject(dto, "type", "text");
        cJSON* legacy_payload = cJSON_GetObjectItem(legacy, "payload");
        if (legacy_payload) {
            cJSON_AddItemToObject(dto, "payload", cJSON_Duplicate(legacy_payload, 1));
        }
        should_send = true;
    } else if (type_str == "tts") {
        cJSON_AddStringToObject(dto, "type", "tts");
        cJSON* legacy_payload = cJSON_GetObjectItem(legacy, "payload");
        if (legacy_payload) {
            // The embedded API puts the topic/message id on the TTS DTO itself.
            // Older local messages kept it inside payload, so move it when present.
            cJSON* payload_id = cJSON_GetObjectItem(legacy_payload, "id");
            if (payload_id != nullptr) {
                cJSON_ReplaceItemInObject(dto, "id", cJSON_Duplicate(payload_id, 1));
            }

            cJSON* outbound_payload = cJSON_Duplicate(legacy_payload, 1);
            if (outbound_payload != nullptr) {
                cJSON_DeleteItemFromObject(outbound_payload, "id");
                cJSON_AddItemToObject(dto, "payload", outbound_payload);
            }
        }
        should_send = true;
    }

    cJSON_Delete(legacy);

    if (should_send) {
        cJSON* root = cJSON_CreateArray();
        cJSON_AddItemToArray(root, cJSON_CreateString("bot_input"));
        cJSON_AddItemToArray(root, dto);

        char* json_str = cJSON_PrintUnformatted(root);
        std::string sio_msg = "42";
        sio_msg += json_str;

        cJSON_free(json_str);
        cJSON_Delete(root);

        ESP_LOGI(TAG, "Sending Socket.IO event: %s", sio_msg.c_str());
        if (!websocket_->Send(sio_msg)) {
            ESP_LOGW(TAG, "Socket.IO event was logged but not sent");
            return false;
        }
        return true;
    } else {
        cJSON_Delete(dto);
        return true;
    }
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");

    error_occurred_ = false;
    expect_binary_audio_ = false;
    authentication_error_ = false;
    xEventGroupClearBits(event_group_handle_,
                         WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT | WEBSOCKET_PROTOCOL_AUTH_ERROR_EVENT);

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (expect_binary_audio_ && on_incoming_audio_ != nullptr) {
                expect_binary_audio_ = false;
                const int64_t now_us = esp_timer_get_time();
                const double server_delta_ms =
                    last_audio_server_timestamp_ == 0 || pending_audio_timestamp_ == 0
                        ? 0
                        : pending_audio_timestamp_ - last_audio_server_timestamp_;

                if (audio_rx_window_frames_ == 0) {
                    audio_rx_window_start_us_ = now_us;
                    audio_rx_window_start_server_timestamp_ = pending_audio_timestamp_;
                }
                if (last_audio_rx_us_ != 0) {
                    const uint32_t local_delta_us =
                        static_cast<uint32_t>(now_us - last_audio_rx_us_);
                    if (local_delta_us > audio_rx_window_max_local_us_) {
                        audio_rx_window_max_local_us_ = local_delta_us;
                    }
                    if (server_delta_ms > audio_rx_window_max_server_ms_) {
                        audio_rx_window_max_server_ms_ = server_delta_ms;
                    }
                    // A single long hold followed by a burst looks the same as a uniform slowdown
                    // in the average alone; count the outliers so the two can be told apart.
                    if (local_delta_us > 200000) {
                        ++audio_rx_window_slow_intervals_;
                    }
                }

                last_audio_rx_us_ = now_us;
                last_audio_server_timestamp_ = pending_audio_timestamp_;
                ++audio_rx_count_;
                ++audio_rx_window_frames_;

                if (audio_rx_window_frames_ == 50) {
                    constexpr double kWindowIntervals = 49.0;
                    const double local_average_ms =
                        static_cast<double>(now_us - audio_rx_window_start_us_) /
                        kWindowIntervals / 1000.0;
                    const double server_average_ms =
                        audio_rx_window_start_server_timestamp_ == 0 ||
                                pending_audio_timestamp_ == 0
                            ? 0
                            : (pending_audio_timestamp_ -
                               audio_rx_window_start_server_timestamp_) /
                                  kWindowIntervals;
                    ESP_LOGI(
                        TAG,
                        "TTS_RX #%lu-%lu local avg=%.1fms max=%.1fms slow=%u, "
                        "server avg=%.1fms max=%.1fms",
                        static_cast<unsigned long>(audio_rx_count_ - 49),
                        static_cast<unsigned long>(audio_rx_count_),
                        local_average_ms,
                        static_cast<double>(audio_rx_window_max_local_us_) / 1000.0,
                        audio_rx_window_slow_intervals_,
                        server_average_ms,
                        audio_rx_window_max_server_ms_);
                    audio_rx_window_start_us_ = 0;
                    audio_rx_window_start_server_timestamp_ = 0;
                    audio_rx_window_frames_ = 0;
                    audio_rx_window_max_local_us_ = 0;
                    audio_rx_window_max_server_ms_ = 0;
                    audio_rx_window_slow_intervals_ = 0;
                }

                on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                    .sample_rate = server_sample_rate_,
                    .frame_duration = server_frame_duration_,
                    .timestamp = (uint32_t)pending_audio_timestamp_,
                    .esp_timestamp_us = now_us,
                    .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                }));
            }
        } else {
            HandleSocketIoText(std::string(data, len));
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    // Build Engine.IO connection URL
    std::string connect_url = url;
    if (connect_url.find("?") == std::string::npos) {
        connect_url += "/?EIO=4&transport=websocket";
    } else {
        connect_url += "&EIO=4&transport=websocket";
    }

    ESP_LOGI(TAG, "Connecting to Socket.IO: %s", connect_url.c_str());
    if (!websocket_->Connect(connect_url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Wait for server hello (bot_ready event)
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT | WEBSOCKET_PROTOCOL_AUTH_ERROR_EVENT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (bits & WEBSOCKET_PROTOCOL_AUTH_ERROR_EVENT) {
        authentication_error_ = true;
        ESP_LOGE(TAG, "Socket.IO authentication rejected");
        // The event is raised by the transport receive callback. Give it time
        // to return before a retry replaces and destroys the WebSocket object.
        vTaskDelay(pdMS_TO_TICKS(10));
        return false;
    }
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive bot_ready event");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

void WebsocketProtocol::HandleSocketIoText(const std::string& message) {
    if (message.empty()) return;

    char engine_type = message[0];
    if (engine_type == '2') { // Engine.IO Ping
        // On ML307 this callback runs on the AT UART event task. Sending here
        // synchronously would block the same task that must parse MIPSEND/OK.
        DeferTextSend("3"); // Engine.IO Pong
        return;
    }

    // Socket.IO CONNECT_ERROR. Backends commonly send this as a 44 packet
    // after the WebSocket upgrade when the bearer token is expired/revoked.
    // Wake OpenAudioChannel immediately so the application can refresh the
    // cached token instead of waiting for the full bot_ready timeout.
    if (message.rfind("44", 0) == 0) {
        ESP_LOGE(TAG, "Socket.IO connect_error: %s", message.substr(2).c_str());
        xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_AUTH_ERROR_EVENT);
        return;
    }

    if (engine_type == '0') { // Engine.IO Open
        Settings settings("websocket", false);
        std::string token = settings.GetString("token");

        cJSON* root = cJSON_CreateObject();
        // Socket.IO serializes the JavaScript client's `auth` option as the
        // CONNECT packet payload itself. Do not add another `auth` wrapper.
        cJSON_AddStringToObject(root, "Authorization", ("Bearer " + token).c_str());
        char* json_str = cJSON_PrintUnformatted(root);

        std::string connect_packet = "40";
        connect_packet += json_str;

        cJSON_free(json_str);
        cJSON_Delete(root);

        // Defer the CONNECT frame so the AT UART event task can return and
        // continue parsing the MIPSEND acknowledgement and server response.
        DeferTextSend(std::move(connect_packet));
        return;
    }

    if (message.rfind("40", 0) == 0) { // Socket.IO Connect ACK
        ESP_LOGI(TAG, "Socket.IO handshake complete");
        return;
    }

    bool is_binary_event = false;
    size_t json_start_idx = 0;

    if (message.rfind("42", 0) == 0) {
        json_start_idx = 2;
    } else if (message.rfind("451-", 0) == 0) {
        json_start_idx = 4;
        is_binary_event = true;
    } else {
        return;
    }

    std::string json_str = message.substr(json_start_idx);
    cJSON* array = cJSON_Parse(json_str.c_str());
    if (!array || !cJSON_IsArray(array) || cJSON_GetArraySize(array) < 2) {
        if (array) cJSON_Delete(array);
        return;
    }

    cJSON* event_name = cJSON_GetArrayItem(array, 0);
    cJSON* payload = cJSON_GetArrayItem(array, 1);

    if (cJSON_IsString(event_name)) {
        std::string event = event_name->valuestring;
        if (event == "bot_response") {
            ParseBotResponse(payload, is_binary_event);
        } else if (event == "exception") {
            cJSON* data = cJSON_GetObjectItem(payload, "data");
            if (data) {
                cJSON* msg = cJSON_GetObjectItem(data, "message");
                if (cJSON_IsString(msg)) {
                    ESP_LOGE(TAG, "Server exception: %s", msg->valuestring);
                }
            }
        }
    }
    cJSON_Delete(array);
}

void WebsocketProtocol::ParseBotResponse(const cJSON* payload, bool is_binary_event) {
    cJSON* type = cJSON_GetObjectItem(payload, "type");
    if (!cJSON_IsString(type)) return;

    std::string type_str = type->valuestring;
    cJSON* message = cJSON_GetObjectItem(payload, "message");

    if (type_str == "event") {
        cJSON* event = cJSON_GetObjectItem(message, "event");
        if (cJSON_IsString(event)) {
            std::string event_str = event->valuestring;
            cJSON* timestamp = cJSON_GetObjectItem(payload, "timestamp");
            const double server_timestamp_ms = cJSON_IsNumber(timestamp) ? timestamp->valuedouble : 0;
            struct timeval rx_tv;
            gettimeofday(&rx_tv, nullptr);
            const int64_t local_rx_ms = static_cast<int64_t>(rx_tv.tv_sec) * 1000 + rx_tv.tv_usec / 1000;

            // ESP-IDF's compact printf configuration on this target does not
            // reliably format long long integers. Epoch milliseconds remain
            // exact at this magnitude when represented as double.
            ESP_LOGI(TAG, "PIPELINE event=%s server_ts=%.0f local_rx=%.0f",
                     event_str.c_str(), server_timestamp_ms,
                     static_cast<double>(local_rx_ms));

            if (event_str == "asr_start") {
                pipeline_asr_start_ms_ = server_timestamp_ms;
                pipeline_asr_done_ms_ = 0;
                pipeline_llm_start_ms_ = 0;
                pipeline_llm_done_ms_ = 0;
                pipeline_tts_start_ms_ = 0;
            } else if (event_str == "asr_done") {
                pipeline_asr_done_ms_ = server_timestamp_ms;
                if (pipeline_asr_start_ms_ > 0 && server_timestamp_ms > 0) {
                    ESP_LOGI(TAG, "PIPELINE ASR duration=%.0fms",
                             server_timestamp_ms - pipeline_asr_start_ms_);
                }

                cJSON* data = cJSON_GetObjectItem(message, "data");
                cJSON* text = cJSON_GetObjectItem(data, "text");
                if (cJSON_IsString(text)) {
                    // What the server actually heard, so a clipped head or tail shows up here
                    // as a missing first or last word. Taken from asr_done rather than the
                    // user_text response because this is the earliest point the text is
                    // available and it does not depend on user_text also arriving.
                    ESP_LOGI(TAG, "ASR heard: \"%s\"", text->valuestring);
                    if (Application::IsAsrTextEmpty(text->valuestring)) {
                        Application::GetInstance().RestartListeningAfterEmptyAsr();
                    } else {
                        // Non-empty text guarantees downstream llm_*/tts events,
                        // so this is where the user learns an answer is coming.
                        // Empty results stay silent, and turns revoked before
                        // asr_done arrive outside the Connecting state, where
                        // the prompt is suppressed.
                        Application::GetInstance().PlayAsrSuccessPrompt();
                    }
                }
            } else if (event_str == "turn_cancelled") {
                // Ack for the cancel_turn event. The server nests the fields under
                // "data": status is "ok" or "no_active_turn"; cancelled_stage
                // (asr/llm/tts) records how far the revoked turn had progressed,
                // which measures how early the device revokes in practice.
                cJSON* data = cJSON_GetObjectItem(message, "data");
                cJSON* status = cJSON_GetObjectItem(data, "status");
                cJSON* cancelled_stage = cJSON_GetObjectItem(data, "cancelled_stage");
                ESP_LOGI(TAG, "PIPELINE turn_cancelled status=%s stage=%s",
                         cJSON_IsString(status) ? status->valuestring : "?",
                         cJSON_IsString(cancelled_stage) ? cancelled_stage->valuestring : "?");
            } else if (event_str == "llm_start") {
                pipeline_llm_start_ms_ = server_timestamp_ms;
                if (pipeline_asr_done_ms_ > 0 && server_timestamp_ms > 0) {
                    ESP_LOGI(TAG, "PIPELINE ASR_TO_LLM wait=%.0fms",
                             server_timestamp_ms - pipeline_asr_done_ms_);
                }
            } else if (event_str == "llm_done") {
                pipeline_llm_done_ms_ = server_timestamp_ms;
                if (pipeline_llm_start_ms_ > 0 && server_timestamp_ms > 0) {
                    ESP_LOGI(TAG, "PIPELINE LLM duration=%.0fms",
                             server_timestamp_ms - pipeline_llm_start_ms_);
                }
            } else if (event_str == "tts_start" || event_str == "tts_started") {
                pipeline_tts_start_ms_ = server_timestamp_ms;
                if (pipeline_llm_done_ms_ > 0 && server_timestamp_ms > 0) {
                    ESP_LOGI(TAG, "PIPELINE LLM_TO_TTS wait=%.0fms",
                             server_timestamp_ms - pipeline_llm_done_ms_);
                }
                if (pipeline_asr_start_ms_ > 0 && server_timestamp_ms > 0) {
                    ESP_LOGI(TAG, "PIPELINE ASR_START_TO_TTS_START total=%.0fms",
                             server_timestamp_ms - pipeline_asr_start_ms_);
                }
                if( pipeline_asr_done_ms_ > 0 && server_timestamp_ms > 0) {
                    ESP_LOGI(TAG, "PIPELINE ASR_DONE_TO_TTS_START wait=%.0fms",
                             server_timestamp_ms - pipeline_asr_done_ms_);
                }
            } else if (event_str == "tts_done") {
                if (pipeline_tts_start_ms_ > 0 && server_timestamp_ms > 0) {
                    ESP_LOGI(TAG, "PIPELINE TTS duration=%.0fms",
                             server_timestamp_ms - pipeline_tts_start_ms_);
                }
            }

            if (event_str == "bot_ready") {
                cJSON* data = cJSON_GetObjectItem(message, "data");
                if (data) {
                    cJSON* tts = cJSON_GetObjectItem(data, "tts");
                    if (tts) {
                        cJSON* sample_rate = cJSON_GetObjectItem(tts, "sampleRate");
                        if (cJSON_IsNumber(sample_rate)) {
                            server_sample_rate_ = sample_rate->valueint;
                        }
                        cJSON* frame_duration = cJSON_GetObjectItem(tts, "frameDurationMs");
                        if (cJSON_IsNumber(frame_duration) && frame_duration->valueint > 0) {
                            server_frame_duration_ = frame_duration->valueint;
                        }
                        ESP_LOGI(TAG, "TTS audio config: sample_rate=%d, frame_duration=%d ms",
                                 server_sample_rate_, server_frame_duration_);
                    }
                }
                xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
            } else if (event_str == "tts_started" || event_str == "tts_start") {
                last_audio_rx_us_ = 0;
                last_audio_server_timestamp_ = 0;
                audio_rx_count_ = 0;
                audio_rx_window_start_us_ = 0;
                audio_rx_window_start_server_timestamp_ = 0;
                audio_rx_window_frames_ = 0;
                audio_rx_window_max_local_us_ = 0;
                audio_rx_window_slow_intervals_ = 0;
                audio_rx_window_max_server_ms_ = 0;
                if (on_incoming_json_ != nullptr) {
                    cJSON* legacy = cJSON_CreateObject();
                    cJSON_AddStringToObject(legacy, "type", "tts");
                    cJSON_AddStringToObject(legacy, "state", "start");
                    on_incoming_json_(legacy);
                    cJSON_Delete(legacy);
                }
            } else if (event_str == "tts_done") {
                if (on_incoming_json_ != nullptr) {
                    cJSON* legacy = cJSON_CreateObject();
                    cJSON_AddStringToObject(legacy, "type", "tts");
                    cJSON_AddStringToObject(legacy, "state", "stop");
                    on_incoming_json_(legacy);
                    cJSON_Delete(legacy);
                }
            }
        }
    } else if (type_str == "audio") {
        if (is_binary_event) {
            expect_binary_audio_ = true;
            cJSON* timestamp = cJSON_GetObjectItem(payload, "timestamp");
            pending_audio_timestamp_ = cJSON_IsNumber(timestamp) ? timestamp->valuedouble : 0;
        }
    } else if (type_str == "bot_text") {
        cJSON* text = cJSON_GetObjectItem(message, "text");
        if (cJSON_IsString(text) && on_incoming_json_ != nullptr) {
            cJSON* legacy = cJSON_CreateObject();
            cJSON_AddStringToObject(legacy, "type", "tts");
            cJSON_AddStringToObject(legacy, "state", "sentence_start");
            cJSON_AddStringToObject(legacy, "text", text->valuestring);
            on_incoming_json_(legacy);
            cJSON_Delete(legacy);
        }
    } else if (type_str == "user_text") {
        cJSON* text = cJSON_GetObjectItem(message, "text");
        if (cJSON_IsString(text) && on_incoming_json_ != nullptr) {
            cJSON* legacy = cJSON_CreateObject();
            cJSON_AddStringToObject(legacy, "type", "stt");
            cJSON_AddStringToObject(legacy, "text", text->valuestring);
            on_incoming_json_(legacy);
            cJSON_Delete(legacy);
        }
    }
}
