#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
#include "codecs/ci130x_audio_codec.h"
#endif
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "moinai_device_settings.h"
#include "latency_tracker.h"

#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

// The CI130X acknowledges an enter-wakeup command within a UART round trip. Past this point the
// acknowledgement is not coming, so the socket held open for the handoff is released.
static constexpr int kTopicWakeupHandoffTimeoutMs = 5000;
// The CI130X ends its own wakeup window and reports exit_wakeup. The application-side timer only
// recovers a window whose end is never reported, so it trails the configured window by this much.
static constexpr int kTopicListeningGraceSeconds = 10;
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
// Longest the microphone backlog held by the codec may delay vad_done. It covers the deepest
// backlog the RX ring can hold; past it the audio is lost either way and the turn has to close.
static constexpr int kCi130xInputDrainTimeoutMs = 800;
// The drain is complete once less than one 10 ms input read is left. Waiting for exactly zero
// would spend the whole timeout on the residue the reader picks up in the same tick anyway.
static constexpr size_t kCi130xInputDrainResidualBytes = 320;
#endif


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    esp_timer_create_args_t topic_listening_timer_args = {
        .callback = OnTopicListeningTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "topic_listen_timeout",
        .skip_unhandled_events = true
    };
    esp_timer_create(&topic_listening_timer_args, &topic_listening_timer_handle_);
}

void Application::ApplyMoinaiDeviceSettings() {
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
    auto* ci130x_codec = static_cast<Ci130xAudioCodec*>(
        Board::GetInstance().GetAudioCodec());
    ci130x_codec->ApplyStoredSettings();
#endif
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (topic_listening_timer_handle_ != nullptr) {
        esp_timer_stop(topic_listening_timer_handle_);
        esp_timer_delete(topic_listening_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::OnTopicListeningTimeout(void* arg) {
    auto* app = static_cast<Application*>(arg);
    app->Schedule([app]() {
        if (!app->topic_listening_timeout_active_.exchange(false) ||
            app->GetDeviceState() != kDeviceStateListening) {
            return;
        }
        ESP_LOGI(TAG, "Topic listening window timed out; returning to standby");
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())->ExitWakeup();
#endif
        app->SetDeviceState(kDeviceStateIdle);
    });
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);
    // Boot marker: confirms the running firmware carries the latency tracker.
    ESP_LOGI("Latency", "tracker v2 armed");

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                auto* ci130x_codec = static_cast<Ci130xAudioCodec*>(
                    Board::GetInstance().GetAudioCodec());
#endif
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                // First of the startup status prompts; the greeting follows them.
                ci130x_codec->NotifyStatus(CI_STATUS_REGISTERING);
#endif
                break;
            }
            case NetworkEvent::Connected: {
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())
                    ->NotifyStatus(CI_STATUS_REGISTER_SUCCESS);
#endif
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())
                    ->NotifyStatus(CI_STATUS_REGISTER_FAILED);
#endif
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())
                    ->NotifyStatus(CI_STATUS_REGISTER_FAILED);
#endif
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())
                    ->NotifyStatus(CI_STATUS_REGISTER_FAILED);
#endif
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())
                    ->NotifyStatus(CI_STATUS_REGISTER_FAILED);
#endif
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())
                    ->NotifyStatus(CI_STATUS_REGISTER_FAILED);
#endif
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously. The power-on greeting is deliberately not
    // started here: it is armed in HandleActivationDoneEvent() so it plays after
    // the registering/activating/connecting prompts rather than ahead of them.
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_DEVICE_REVOKED |
        MAIN_EVENT_DEVICE_REACTIVATE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_DEVICE_REVOKED) {
            HandleDeviceRevokedEvent();
        }

        if (bits & MAIN_EVENT_DEVICE_REACTIVATE) {
            HandleDeviceReactivationEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (true) {
                auto packet = audio_service_.PopPacketFromSendQueue();
                if (!packet) {
                    break;
                }
                LatencyTracker::Instance().RecordUplinkPacket(packet->mic_capture_time_us);
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready; the activation task also performs routine startup
        // checks, so it reports activation only when the server requests it.
        SetDeviceState(kDeviceStateActivating);
        StartActivationTask();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");
    device_reactivation_pending_.store(false);

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Keep the OTA/device API context alive for deferred settings and location
    // requests. It is also required if topic polling is enabled later.
    auto& board = Board::GetInstance();
    if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }

#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
    // 4G connection, activation, and protocol initialization are done, so
    // CONNECT_SUCCESS was the last startup status. Arm the local power-on
    // prompt (0x0112, the standard CI130X "play local voice" command; the codec
    // persists and rotates voice IDs 5000-5004). It plays once the status
    // prompts still queued on the CI130X have finished, and the silent wakeup
    // follows once the greeting itself ends.
    auto* ci130x_codec = static_cast<Ci130xAudioCodec*>(board.GetAudioCodec());
    ci130x_codec->StartPowerOnPrompt();
    ci130x_codec->NotifyBackendReadyForWakeup();
#endif
}

bool Application::HandleMoinaiApiResult(esp_err_t err) {
    if (err == Ota::kErrDeviceNotActivated) {
        ESP_LOGE(TAG, "Moinai reported DEVICE_NOT_ACTIVATED; requesting reactivation");
        device_reactivation_pending_.store(true);
        xEventGroupSetBits(event_group_, MAIN_EVENT_DEVICE_REACTIVATE);
        return true;
    }
    if (err != Ota::kErrNotReadyShipment) {
        return false;
    }
    ESP_LOGE(TAG, "Moinai reported NOT_READY_SHIPMENT; requesting device revocation");
    xEventGroupSetBits(event_group_, MAIN_EVENT_DEVICE_REVOKED);
    return true;
}

void Application::HandleDeviceRevokedEvent() {
    if (device_revoked_.exchange(true)) {
        return;
    }
    device_reactivation_pending_.store(false);

    ESP_LOGE(TAG, "Device access revoked; stopping cloud and voice activity");
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    while (audio_service_.PopPacketFromSendQueue()) {}
    audio_service_.ResetDecoder();

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel(false);
    }
    if (ota_) {
        ota_->ClearMoinaiCredentials();
    }

    SetDeviceState(kDeviceStateRevoked);
    Alert(Lang::Strings::ERROR, "Device Not Shipped", "circle_xmark",
          Lang::Sounds::OGG_EXCLAMATION);
}

void Application::HandleDeviceReactivationEvent() {
    if (device_revoked_.load()) {
        return;
    }
    device_reactivation_pending_.store(true);
    ESP_LOGW(TAG, "Resetting cached credentials and restarting device activation");

    // If the error came from the activation task itself, let that task unwind
    // before replacing its Ota instance.
    for (int i = 0; activation_task_handle_ != nullptr && i < 100; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (activation_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Activation task is still stopping; reactivation deferred");
        xEventGroupSetBits(event_group_, MAIN_EVENT_DEVICE_REACTIVATE);
        return;
    }

    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    while (audio_service_.PopPacketFromSendQueue()) {}
    audio_service_.ResetDecoder();

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel(false);
    }
    protocol_.reset();

    if (ota_) {
        ota_->PrepareForReactivation();
        ota_.reset();
    }

    SetDeviceState(kDeviceStateActivating);
    StartActivationTask();
}

bool Application::PrepareMoinaiSession() {
    if (!ota_) {
        return false;
    }

    // A cached token is only a transport credential. Validate its expiry and
    // then require one authenticated device API call before WebSocket startup,
    // so a returned device cannot converse during the former optimistic window.
    if (ota_->IsUsingCachedMoinaiToken()) {
        const esp_err_t validation_err = ota_->ValidateCachedMoinaiToken();
        if (HandleMoinaiApiResult(validation_err)) {
            return false;
        }
        if (validation_err != ESP_OK) {
            ESP_LOGE(TAG, "Cached Moinai token validation failed: %d", validation_err);
            return false;
        }
    }

    constexpr int kEligibilityAttempts = 3;
    for (int attempt = 1; attempt <= kEligibilityAttempts; ++attempt) {
        esp_err_t err = ota_->FetchDeviceSettings();
        if (HandleMoinaiApiResult(err)) {
            return false;
        }

        if (err == ESP_OK) {
            ApplyMoinaiDeviceSettings();
            ESP_LOGI(TAG, "Device eligibility confirmed before WebSocket startup");
            return true;
        } else if (err == Ota::kErrUnauthorized) {
            ESP_LOGW(TAG, "Eligibility check rejected the token; refreshing it");
            err = ota_->RefreshMoinaiToken();
            if (HandleMoinaiApiResult(err)) {
                return false;
            }
            if (err == ESP_OK) {
                continue;
            }
        }

        ESP_LOGW(TAG, "Device eligibility check failed (%d), attempt %d/%d",
                 err, attempt, kEligibilityAttempts);
        if (attempt < kEligibilityAttempts) {
            vTaskDelay(pdMS_TO_TICKS(attempt * 1000));
        }
    }

    return false;
}

void Application::StartActivationTask() {
    if (activation_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Activation task already running");
        return;
    }

    xTaskCreate([](void* arg) {
        Application* app = static_cast<Application*>(arg);
        app->ActivationTask();
        app->activation_task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    if (!ota_->HasWebsocketConfig()) {
        ESP_LOGE(TAG, "Activation failed, stopping activation task");
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())
            ->NotifyStatus(CI_STATUS_CONNECT_FAILED);
#endif
        SetDeviceState(kDeviceStateActivating);
        return;
    }

    if (!PrepareMoinaiSession()) {
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec())
            ->NotifyStatus(CI_STATUS_CONNECT_FAILED);
#endif
        if (!device_revoked_.load() && !device_reactivation_pending_.load()) {
            ESP_LOGE(TAG, "Device eligibility could not be confirmed; WebSocket startup blocked");
            Alert(Lang::Strings::ERROR, "Unable to verify device status",
                  "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }
        SetDeviceState(kDeviceStateActivating);
        return;
    }

    // Authentication and routine device eligibility checks are complete.
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
    auto* ci130x_codec = static_cast<Ci130xAudioCodec*>(Board::GetInstance().GetAudioCodec());
    ci130x_codec->NotifyStatus(CI_STATUS_CONNECTING);
#endif

    // Initialize the protocol
    InitializeProtocol();

    // Establish the Socket.IO channel as soon as authentication succeeds. This
    // moves bot_ready ahead of non-critical settings and location requests.
    // Startup transport failures are retried silently. Playing the generic
    // error sound here would overlap and preempt the CI130X power-on prompt.
    constexpr int kStartupConnectAttempts = 3;
    suppress_protocol_errors_.store(true);
    bool websocket_ready = false;
    for (int attempt = 1; attempt <= kStartupConnectAttempts && !websocket_ready; ++attempt) {
        websocket_ready = OpenAudioChannelWithTokenRefresh();
        if (!websocket_ready && attempt < kStartupConnectAttempts) {
            ESP_LOGW(TAG, "Startup WebSocket attempt %d/%d failed; retrying silently",
                     attempt, kStartupConnectAttempts);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    suppress_protocol_errors_.store(false);
    if (!websocket_ready) {
        ESP_LOGE(TAG, "WebSocket preconnect failed after %d attempts", kStartupConnectAttempts);
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        ci130x_codec->NotifyStatus(CI_STATUS_CONNECT_FAILED);
#endif
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        return;
    }

#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
    ci130x_codec->NotifyStatus(CI_STATUS_CONNECT_SUCCESS);
#endif

    // Report location immediately after the backend channel is ready. This
    // gives the service the current serving-cell data before topic polling or
    // the first conversation can begin.
    const esp_err_t location_err = ota_->ReportCurrentLocation();
    if (HandleMoinaiApiResult(location_err)) {
        return;
    }
    last_location_report_succeeded_.store(location_err == ESP_OK);

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);

    // Start the real polling path. Its first fetch waits one poll interval of
    // quiet, so startup topics enter the normal queue rather than being
    // consumed by a diagnostic-only read.
    StartTopicPolling();

}

void Application::StartTopicPolling() {
    if (topic_poll_task_handle_ != nullptr) {
        return;
    }
    xTaskCreate([](void* arg) {
        auto* app = static_cast<Application*>(arg);
        app->TopicPollTask();
        app->topic_poll_task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }, "moinai_topics", 6144, this, 3, &topic_poll_task_handle_);
}

void Application::TopicPollTask() {
    struct PendingTopic {
        Ota::DeviceTopic topic;
        int open_attempts = 0;
        TickType_t next_attempt = 0;
        bool awaiting_delivery = false;
        bool delivery_started = false;
        TickType_t delivery_deadline = 0;
    };

    constexpr int kTopicStartTimeoutMs = 30 * 1000;
    constexpr int kTopicCompletionTimeoutMs = 5 * 60 * 1000;
    std::deque<PendingTopic> pending;
    std::unordered_set<std::string> known_topic_ids;
    TickType_t next_poll = 0;
    bool topic_poll_armed = false;
    auto poll_interval_ticks = []() {
        return pdMS_TO_TICKS(
            GetMoinaiDeviceSettings().topic_poll_interval_seconds * 1000);
    };
    TickType_t next_token_check = 0;
    TickType_t next_settings_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(30 * 60 * 1000);
    TickType_t next_location_report = xTaskGetTickCount() + pdMS_TO_TICKS(
        last_location_report_succeeded_.load() ? 30 * 60 * 1000 : 60 * 1000);

    while (!device_revoked_.load()) {
        const TickType_t now = xTaskGetTickCount();
        const uint32_t handoff_deadline = topic_listening_handoff_deadline_.load();
        if (handoff_deadline != 0 &&
            static_cast<int32_t>(now - handoff_deadline) >= 0) {
            // The chip never acknowledged the wakeup, so nothing is going to turn the topic that
            // just played into listening. Drop the hold and let standby release the socket.
            ESP_LOGW(TAG, "Topic wakeup was not acknowledged; returning to standby");
            topic_listening_handoff_deadline_.store(0);
            topic_listening_after_playback_.store(false);
            Schedule([this]() { ReleaseIdleAudioChannel(); });
        }

        bool startup_audio_complete = true;
        bool ci130x_ready_for_topic = true;
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        auto* ci130x_codec = static_cast<Ci130xAudioCodec*>(
            Board::GetInstance().GetAudioCodec());
        startup_audio_complete = ci130x_codec->IsStartupAudioComplete();
        // A user wakeup owns the audio path until CI130X exits its wakeup window.
        // Local prompts also have priority over unattended topic delivery.
        ci130x_ready_for_topic = !ci130x_codec->IsAwake() &&
                                 !ci130x_codec->IsLocalPlaying();
#endif
        const DeviceState state = GetDeviceState();
        const bool idle = state == kDeviceStateIdle && startup_audio_complete;
        // Topics are fetched only while the device is idle with the wakeup window
        // closed, never during a conversation or a topic that is still playing.
        const bool poll_window_open = idle && ci130x_ready_for_topic &&
                                      !tts_audio_stream_active_.load();

        if (idle && static_cast<int32_t>(now - next_token_check) >= 0) {
            next_token_check = now + pdMS_TO_TICKS(60 * 1000);
            if (ota_ && ota_->ShouldRefreshMoinaiToken()) {
                const esp_err_t refresh_err = ota_->RefreshMoinaiToken();
                if (HandleMoinaiApiResult(refresh_err)) {
                    break;
                }
                if (refresh_err == ESP_OK && protocol_ && protocol_->IsAudioChannelOpened()) {
                    ESP_LOGI(TAG, "Token refreshed while idle; closing the stale conversation channel");
                    protocol_->CloseAudioChannel(false);
                }
            }
        }

        if (idle && ota_ && static_cast<int32_t>(now - next_settings_refresh) >= 0) {
            esp_err_t settings_err = ota_->FetchDeviceSettings();
            if (settings_err == Ota::kErrUnauthorized) {
                const esp_err_t refresh_err = ota_->RefreshMoinaiToken();
                if (HandleMoinaiApiResult(refresh_err)) {
                    break;
                }
                if (refresh_err == ESP_OK) {
                    settings_err = ota_->FetchDeviceSettings();
                }
            }
            if (HandleMoinaiApiResult(settings_err)) {
                break;
            }
            if (settings_err == ESP_OK) {
                ApplyMoinaiDeviceSettings();
            }
            const int retry_seconds = settings_err == ESP_OK ? 30 * 60 : 60;
            next_settings_refresh = now + pdMS_TO_TICKS(retry_seconds * 1000);
        }

        if (idle && ota_ && static_cast<int32_t>(now - next_location_report) >= 0) {
            esp_err_t location_err = ota_->ReportCurrentLocation();
            if (location_err == Ota::kErrUnauthorized) {
                const esp_err_t refresh_err = ota_->RefreshMoinaiToken();
                if (HandleMoinaiApiResult(refresh_err)) {
                    break;
                }
                if (refresh_err == ESP_OK) {
                    location_err = ota_->ReportCurrentLocation();
                }
            }
            if (HandleMoinaiApiResult(location_err)) {
                break;
            }
            last_location_report_succeeded_.store(location_err == ESP_OK);
            const int retry_seconds = location_err == ESP_OK ? 30 * 60 : 60;
            next_location_report = now + pdMS_TO_TICKS(retry_seconds * 1000);
        }

        // The interval runs from the moment the device becomes idle, so arm it on
        // that edge and leave it alone until the device is idle again. The cycle is
        // fetch -> play -> wakeup window -> quiet interval -> fetch.
        if (!poll_window_open) {
            topic_poll_armed = false;
        } else if (!topic_poll_armed) {
            topic_poll_armed = true;
            next_poll = now + poll_interval_ticks();
        }

        if (poll_window_open && static_cast<int32_t>(now - next_poll) >= 0) {
            // FetchTopics blocks for seconds. A wake word landing inside it outranks
            // whatever came back, so the result is dropped rather than queued: the user
            // owns the device from that point, and the backend still holds the topic
            // for a later poll.
            const uint32_t wakeup_seq_before = user_wakeup_seq_.load();
            std::vector<Ota::DeviceTopic> topics;
            esp_err_t fetch_err = ota_ ? ota_->FetchTopics(topics) : ESP_ERR_INVALID_STATE;
            if (fetch_err == Ota::kErrUnauthorized && ota_) {
                const esp_err_t refresh_err = ota_->RefreshMoinaiToken();
                if (HandleMoinaiApiResult(refresh_err)) {
                    break;
                }
                if (refresh_err == ESP_OK) {
                    fetch_err = ota_->FetchTopics(topics);
                }
            }
            if (HandleMoinaiApiResult(fetch_err)) {
                break;
            }
            if (user_wakeup_seq_.load() != wakeup_seq_before) {
                ESP_LOGI(TAG, "User wakeup during the topic fetch; dropping the topic");
            } else if (fetch_err == ESP_OK && !topics.empty()) {
                // The backend returns topics in chronological order. Keep only
                // the newest entry so stale notifications are never announced.
                auto latest = std::move(topics.back());
                if (known_topic_ids.find(latest.id) == known_topic_ids.end()) {
                    known_topic_ids.clear();
                    known_topic_ids.insert(latest.id);
                    pending.clear();
                    pending.push_back({std::move(latest), 0, now});
                }
            }

            next_poll = now + poll_interval_ticks();
        }

#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        // FetchTopics may block long enough for a button/wake-word event to arrive.
        // Re-read the codec state immediately before claiming the queued topic.
        ci130x_ready_for_topic = !ci130x_codec->IsAwake() &&
                                 !ci130x_codec->IsLocalPlaying();
#endif

        if (!pending.empty() && pending.front().awaiting_delivery) {
            auto& item = pending.front();
            if (topic_tts_preempted_.exchange(false)) {
                topic_tts_started_.store(false);
                topic_tts_done_.store(false);
                topic_tts_failed_.store(false);
                item.awaiting_delivery = false;
                item.delivery_started = false;
                item.next_attempt = now;
                ESP_LOGI(TAG, "Topic %s deferred by user wakeup",
                         item.topic.id.c_str());
            } else if (!item.delivery_started && topic_tts_started_.load()) {
                item.delivery_started = true;
                item.delivery_deadline = now + pdMS_TO_TICKS(kTopicCompletionTimeoutMs);
                ESP_LOGI(TAG, "Topic %s playback started", item.topic.id.c_str());
            }

            if (topic_tts_done_.load()) {
                ESP_LOGI(TAG, "Topic %s playback completed", item.topic.id.c_str());
                topic_tts_delivery_pending_.store(false);
                topic_tts_started_.store(false);
                topic_tts_done_.store(false);
                topic_tts_failed_.store(false);
                pending.pop_front();
            } else if (topic_tts_failed_.load() ||
                       static_cast<int32_t>(now - item.delivery_deadline) >= 0) {
                const bool timed_out = !topic_tts_failed_.load();
                const bool had_started = item.delivery_started;
                topic_tts_delivery_pending_.store(false);
                topic_tts_started_.store(false);
                topic_tts_done_.store(false);
                topic_tts_failed_.store(false);
                item.awaiting_delivery = false;
                item.delivery_started = false;
                item.open_attempts++;
                const int retry_seconds = std::min(60, 1 << std::min(item.open_attempts, 5));
                item.next_attempt = now + pdMS_TO_TICKS(retry_seconds * 1000);
                ESP_LOGW(TAG, "Topic %s playback %s%s; retrying in %d seconds",
                         item.topic.id.c_str(),
                         timed_out ? "timed out" : "connection failed",
                         had_started ? " after starting" : " before starting",
                         retry_seconds);
                if (protocol_ && protocol_->IsAudioChannelOpened()) {
                    protocol_->CloseAudioChannel(false);
                }
                SetDeviceState(kDeviceStateIdle);
            }
        } else if (!pending.empty() && GetDeviceState() == kDeviceStateIdle && protocol_ &&
                   !IsTopicListeningPending() && ci130x_ready_for_topic) {
            auto& item = pending.front();
            if (static_cast<int32_t>(now - item.next_attempt) >= 0) {
                // A topic TTS request needs the same short-lived conversation
                // channel as a wake-word session. Leave standby before opening it
                // so the idle-state handler cannot immediately close the socket.
                // A channel the turn that just ended left open is reused as it is:
                // the topic then plays without a second handshake.
                if (!SetDeviceState(kDeviceStateConnecting)) {
                    ESP_LOGI(TAG, "Device left standby; deferring queued topic %s",
                             item.topic.id.c_str());
                    item.next_attempt = now + pdMS_TO_TICKS(500);
                } else if (protocol_->IsAudioChannelOpened() ||
                           OpenAudioChannelWithTokenRefresh()) {
                    ESP_LOGI(TAG, "Playing queued topic %s", item.topic.id.c_str());
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                    // A cancelled turn leaves the codec dropping TTS audio. The topic is a new
                    // request, and the application layer already refuses audio from the turn
                    // that was cancelled, so let the codec play what arrives from here on.
                    static_cast<Ci130xAudioCodec*>(
                        Board::GetInstance().GetAudioCodec())->ClearTtsBlock();
#endif
                    topic_tts_started_.store(false);
                    topic_tts_done_.store(false);
                    topic_tts_failed_.store(false);
                    topic_tts_delivery_pending_.store(true);
                    item.awaiting_delivery = true;
                    item.delivery_started = false;
                    item.delivery_deadline = now + pdMS_TO_TICKS(kTopicStartTimeoutMs);
                    // The topic TTS is a server-side turn too; interrupting its
                    // playback (button/wake word) revokes it with cancel_turn.
                    cloud_turn_active_.store(true);
                    if (!protocol_->SendTts(item.topic.id, item.topic.text)) {
                        topic_tts_failed_.store(true);
                    }
                } else if (!device_revoked_.load()) {
                    SetDeviceState(kDeviceStateIdle);
                    item.open_attempts++;
                    const int retry_seconds = std::min(60, 1 << std::min(item.open_attempts, 5));
                    item.next_attempt = now + pdMS_TO_TICKS(retry_seconds * 1000);
                    ESP_LOGW(TAG, "Topic %s connection failed; retrying in %d seconds",
                             item.topic.id.c_str(), retry_seconds);
                }
            }
        } else if (!pending.empty() && GetDeviceState() != kDeviceStateIdle) {
            static TickType_t last_busy_log = 0;
            if (static_cast<int32_t>(now - last_busy_log) >= pdMS_TO_TICKS(10000)) {
                ESP_LOGI(TAG, "%u topic(s) waiting for the device to become idle",
                         static_cast<unsigned>(pending.size()));
                last_busy_log = now;
            }
        }

        topic_queue_pending_.store(!pending.empty());
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 2;
    int retry_count = 0;
    int retry_delay = 2; // Initial retry delay in seconds
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
    bool activation_prompt_active = false;
    bool activation_success_reported = false;
#endif

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            if (HandleMoinaiApiResult(err)) {
                ESP_LOGE(TAG, "Device business state requires activation flow to stop");
                return; // Stop further activation loop and server requests
            }
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        // 版本检查成功即确认运行镜像有效：
        // 1. 修复升级尝试先于 MarkCurrentVersionValid 执行导致的必现失败
        //    （esp_ota_begin 返回 ESP_ERR_OTA_ROLLBACK_INVALID_STATE）
        // 2. 将未确认窗口从 ~11s 缩短到 ~2s（版本检查成功点）
        ota_->MarkCurrentVersionValid();

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        if (!activation_prompt_active && !activation_success_reported) {
            static_cast<Ci130xAudioCodec*>(board.GetAudioCodec())
                ->NotifyStatus(CI_STATUS_ACTIVATING);
            activation_prompt_active = true;
        }
#endif
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        bool activation_succeeded = false;
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                activation_succeeded = true;
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        if (activation_succeeded && !activation_success_reported) {
            static_cast<Ci130xAudioCodec*>(board.GetAudioCodec())
                ->NotifyStatus(CI_STATUS_ACTIVATE_SUCCESS);
            activation_success_reported = true;
            activation_prompt_active = false;
        } else if (!activation_succeeded && activation_prompt_active) {
            static_cast<Ci130xAudioCodec*>(board.GetAudioCodec())
                ->NotifyStatus(CI_STATUS_ACTIVATE_FAILED);
            activation_prompt_active = false;
        }
#endif
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        if (topic_tts_delivery_pending_.load()) {
            topic_tts_failed_.store(true);
        }
        last_error_message_ = message;
        if (suppress_protocol_errors_.load()) {
            ESP_LOGW(TAG, "Suppressing retryable startup protocol alert: %s", message.c_str());
            return;
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (!tts_audio_stream_active_.load()) {
            return;
        }
        if (!audio_service_.PushPacketToDecodeQueue(std::move(packet))) {
            const uint32_t dropped = tts_audio_dropped_packets_.fetch_add(1) + 1;
            if (dropped == 1 || dropped % 25 == 0) {
                ESP_LOGW(TAG, "TTS decode queue full (%u ms buffered), dropped %lu packet(s)",
                         static_cast<unsigned>(audio_service_.BufferedPlaybackDurationMs()),
                         static_cast<unsigned long>(dropped));
            }
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        if (topic_tts_delivery_pending_.load() && !topic_tts_done_.load()) {
            topic_tts_failed_.store(true);
        }
        tts_audio_stream_active_.store(false);
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            if (!device_reactivation_pending_.load() && !device_revoked_.load()) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    
    protocol_->OnIncomingJson([this, display, codec](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                // Start buffering synchronously before returning to the
                // WebSocket receive loop so the first 60 ms packets cannot race
                // the scheduled speaking-state transition.
                LatencyTracker::Instance().Mark(LatencyStage::kTtsStart);
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                static_cast<Ci130xAudioCodec*>(
                    Board::GetInstance().GetAudioCodec())->SetTopicPlayback(
                        topic_tts_delivery_pending_.load());
#endif
                audio_service_.BeginStreamingPlayback(
                    topic_tts_delivery_pending_.load()
                        ? TOPIC_PLAYBACK_PREBUFFER_DURATION_MS
                        : STREAM_PLAYBACK_PREBUFFER_DURATION_MS);
                tts_audio_dropped_packets_.store(0);
                tts_audio_stream_active_.store(true);
                aborted_ = false;
                if (!SetDeviceState(kDeviceStateSpeaking)) {
                    tts_audio_stream_active_.store(false);
                    if (topic_tts_delivery_pending_.load()) {
                        topic_tts_failed_.store(true);
                    }
                } else if (topic_tts_delivery_pending_.load()) {
                    topic_tts_started_.store(true);
                }
            } else if (strcmp(state->valuestring, "stop") == 0) {
                LatencyTracker::Instance().Mark(LatencyStage::kTtsStop);
                const bool topic_delivery = topic_tts_delivery_pending_.load();
                if (topic_delivery) {
                    topic_tts_done_.store(true);
                }
                tts_audio_stream_active_.store(false);
                topic_listening_after_playback_.store(topic_delivery);
                speaking_turn_ends_idle_.store(false);
                audio_service_.FinishStreamingPlayback();
                const uint32_t dropped = tts_audio_dropped_packets_.exchange(0);
                if (dropped > 0) {
                    ESP_LOGW(TAG, "TTS stream ended with %lu dropped packet(s)",
                             static_cast<unsigned long>(dropped));
                }
                // tts_done only means the server finished sending. A codec that plays through a
                // second chip still holds seconds of PCM, so it ends the turn itself once that
                // audio has been heard; leaving Speaking here would let VAD start the next turn
                // on top of the tail that is still playing.
                if (!codec->PlaybackCompletionIsAsync()) {
                    FinishSpeakingTurn();
                }
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

bool Application::IsAsrTextEmpty(const char* text) {
    return text == nullptr || text[0] == '\0';
}

void Application::PlayAsrSuccessPrompt() {
    Schedule([this]() {
        // A turn revoked by a local command drops back to Idle before the
        // server's in-flight asr_done arrives (~RTT race window); the prompt
        // must not promise an answer for that dead turn.
        if (!protocol_ || !protocol_->IsAudioChannelOpened() ||
            GetDeviceState() != kDeviceStateConnecting) {
            return;
        }
        ESP_LOGI(TAG, "ASR recognized speech; playing success prompt");
        PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::RestartListeningAfterEmptyAsr() {
    Schedule([this]() {
        if (!protocol_ || !protocol_->IsAudioChannelOpened() ||
            GetDeviceState() != kDeviceStateConnecting) {
            ESP_LOGW(TAG, "Ignoring empty asr_done outside an active response wait");
            return;
        }

#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
        // Opening a turn here sends start_talk while the server may still owe a
        // reply for this one: an empty asr_done can be followed by a real one,
        // and the server abandons the answer once a newer turn begins. Go to
        // standby instead. The CI130X wakeup window is still open, so OnVadStart
        // opens the next turn once the user actually speaks, which also keeps
        // the microphone off the uplink until there is something to send. A late
        // reply still plays, because Idle accepts a transition to Speaking.
        ESP_LOGI(TAG, "Empty ASR result; waiting for speech before the next turn");
        SetDeviceState(kDeviceStateIdle);
#else
        ESP_LOGI(TAG, "Empty ASR result; starting the next conversation turn");
        SetListeningMode(listening_mode_);
#endif
    });
}

#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
void Application::DiscardCi130xPendingUplink() {
    // Speech streams to the server in real time; a local command revokes the turn
    // only after most of its audio has already gone out. Drop whatever packets are
    // still queued (at most a frame or two) so the tail cannot leak into the next
    // turn. CancelCloudSession() performs a second drain after the encoder idles.
    size_t discarded_packets = 0;
    while (audio_service_.PopPacketFromSendQueue()) {
        discarded_packets++;
    }
    ESP_LOGI(TAG, "Discarded %u pending uplink packet(s) for local command",
             static_cast<unsigned>(discarded_packets));
}
#endif

void Application::CancelActiveCloudTurn(const char* reason) {
    // A revoked turn never finishes playback, so close its latency report now;
    // otherwise its stale marks (VAD end without vad_done) corrupt the next
    // turn's report. Unconditional: stale marks may exist even when no
    // cancel_turn goes out (turn already torn down, double cancel).
    LatencyTracker::Instance().AbortTurn();
    // exchange(false) both tests and clears, so consecutive cancels of the same
    // turn (e.g. command 100 followed by ExitWakeup) send exactly one event.
    if (!cloud_turn_active_.exchange(false)) {
        return;
    }
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->SendCancelTurn(reason);
        ESP_LOGI(TAG, "Cancelled active cloud turn: %s", reason);
    }
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

bool Application::IsTopicListeningPending() const {
    const uint32_t deadline = topic_listening_handoff_deadline_.load();
    return deadline != 0 &&
           static_cast<int32_t>(xTaskGetTickCount() - deadline) < 0;
}

bool Application::PreemptTopicDeliveryForUserWakeup() {
    // The topic poll task compares this count across a fetch so a topic that
    // arrives after the user has taken over is never queued.
    user_wakeup_seq_.fetch_add(1);

    if (!topic_tts_delivery_pending_.load()) {
        // The server may already have sent tts_done while CI130X still has the
        // topic's PCM buffered. That audible tail is still user-preemptible.
        return topic_listening_after_playback_.exchange(false);
    }

    topic_listening_after_playback_.store(false);
    topic_tts_preempted_.store(true);
    if (!topic_tts_delivery_pending_.exchange(false)) {
        topic_tts_preempted_.store(false);
        return false;
    }
    return true;
}

void Application::ReleaseIdleAudioChannel() {
    if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
        return;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        // Still mid-turn; the Idle transition will close the channel instead.
        return;
    }
    ESP_LOGI(TAG, "Wakeup window closed; closing conversation WebSocket");
    protocol_->CloseAudioChannel(false);
}

bool Application::RestartListeningOnOpenAudioChannel() {
    if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
        return false;
    }

    const auto state = GetDeviceState();
    // Reuse is reserved for a true TTS barge-in. If the device is merely
    // listening or waiting for a response, the caller must close this channel
    // and establish a fresh wake-word conversation instead.
    if (state != kDeviceStateSpeaking || !tts_audio_stream_active_.load()) {
        return false;
    }

    ESP_LOGI(TAG, "Restarting listening on the existing conversation WebSocket");

    // start_talk starts a new turn for the embedded protocol, so do not send a
    // separate legacy abort frame that can race with it.
    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected, false);
        audio_service_.ResetDecoder();
        // BlockNextTts has already stopped the CI130X stream. Wait until the
        // output task releases any PCM frame it fetched before ResetDecoder so
        // ClearTtsBlock cannot let an old frame start a new playback session.
        audio_service_.WaitForPlaybackQueueEmpty();
    }

    while (audio_service_.PopPacketFromSendQueue()) {}

    if (state == kDeviceStateListening) {
        play_popup_on_listening_ = false;
        protocol_->SendStartListening(kListeningModeManualStop);
        audio_service_.ResetDecoder();
        audio_service_.EnableVoiceProcessing(true);
    } else {
        play_popup_on_listening_ = true;
        SetListeningMode(kListeningModeManualStop);
    }
    return true;
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

bool Application::OpenAudioChannelWithTokenRefresh() {
    if (!protocol_) {
        return false;
    }

    bool refresh_attempted = false;
    if (ota_ && ota_->ShouldRefreshMoinaiToken()) {
        refresh_attempted = true;
        ESP_LOGI(TAG, "Moinai token is within its one-hour refresh window");
        const esp_err_t refresh_err = ota_->RefreshMoinaiToken();
        if (HandleMoinaiApiResult(refresh_err)) {
            return false;
        }
        if (refresh_err != ESP_OK) {
            // The old token may still be valid. Attempt the connection so a
            // transient auth-service failure does not take the device offline.
            ESP_LOGW(TAG, "Proactive token refresh failed; trying the existing token");
        }
    }

    if (protocol_->OpenAudioChannel()) {
        return true;
    }
    if (!protocol_->IsAuthenticationError() || !ota_ || refresh_attempted) {
        return false;
    }

    // Handle both an HTTP Upgrade 401 and a Socket.IO connect_error. Replace
    // the token only after auth succeeds, then retry exactly once.
    ESP_LOGW(TAG, "WebSocket token rejected; refreshing token and retrying once");
    const esp_err_t refresh_err = ota_->RefreshMoinaiToken();
    if (HandleMoinaiApiResult(refresh_err)) {
        return false;
    }
    if (refresh_err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to refresh rejected WebSocket token");
        return false;
    }
    return protocol_->OpenAudioChannel();
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!OpenAudioChannelWithTokenRefresh()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state != kDeviceStateAudioTesting && state != kDeviceStateListening) {
        // Every other suppressed vad_done announces itself; this one used to fall
        // out silently, which reads on the wire exactly like a turn that never ended.
        ESP_LOGW(TAG, "Stop listening ignored in state %s; no vad_done sent",
                 DeviceStateMachine::GetStateName(state));
        return;
    }

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
            // The CI130X delivers its roll-back as a burst, so the codec can still carry
            // a tail of the utterance when the VAD ends. Stopping the processor with PCM
            // still in it would discard the end of the utterance, so let the input task
            // consume it first. The wait is short, because the burst read path drains the
            // backlog without real-time pacing.
            auto* ci130x_codec = static_cast<Ci130xAudioCodec*>(
                Board::GetInstance().GetAudioCodec());
            const int64_t drain_deadline_us =
                esp_timer_get_time() + kCi130xInputDrainTimeoutMs * 1000;
            while (ci130x_codec->PendingInputBytes() > kCi130xInputDrainResidualBytes) {
                if (esp_timer_get_time() >= drain_deadline_us) {
                    ESP_LOGW(TAG, "Microphone backlog not drained before vad_done, %u bytes lost",
                             static_cast<unsigned>(ci130x_codec->PendingInputBytes()));
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            LatencyTracker::Instance().Mark(LatencyStage::kMicDrainDone);
#endif
            // Stop producing audio first, then preserve wire order: every completed
            // Opus frame must reach the server before the vad_done marker.
            audio_service_.EnableVoiceProcessing(false);
            auto drain_send_queue = [this](const char* failure_message) {
                while (true) {
                    auto packet = audio_service_.PopPacketFromSendQueue();
                    if (!packet) {
                        return true;
                    }
                    LatencyTracker::Instance().RecordUplinkPacket(packet->mic_capture_time_us);
                    if (!protocol_->SendAudio(std::move(packet))) {
                        ESP_LOGW(TAG, "%s", failure_message);
                        return true;
                    }
                }
            };
            if (!drain_send_queue("Failed to drain audio before vad_done")) {
                return;
            }
            audio_service_.WaitForUplinkEncodeIdle();
            LatencyTracker::Instance().Mark(LatencyStage::kEncodeIdle);
            if (!drain_send_queue("Failed to send final audio frame before vad_done")) {
                return;
            }
            protocol_->SendStopListening();
            LatencyTracker::Instance().Mark(LatencyStage::kVadDoneSent);
        }
        // vad_done completes microphone upload, but the conversation is still
        // active while the server prepares its response. Do not enter standby
        // (which intentionally closes the socket) until the session really ends.
        SetDeviceState(kDeviceStateConnecting);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        // A following start_talk is the embedded API's interrupt signal. Stop
        // local TTS now, but do not send the legacy abort control message.
        AbortSpeaking(kAbortReasonWakeWordDetected, false);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!OpenAudioChannelWithTokenRefresh()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    if (new_state != kDeviceStateListening &&
        topic_listening_timeout_active_.exchange(false) &&
        topic_listening_timer_handle_ != nullptr) {
        esp_timer_stop(topic_listening_timer_handle_);
    }

    // Leaving standby ends the wakeup handoff, either because listening started or because
    // something else took the turn.
    if (new_state != kDeviceStateIdle) {
        topic_listening_handoff_deadline_.store(0);
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateIdle:
            // Every turn teardown path funnels through Idle (local-command
            // cancellation, wake-up revocation, finished response, empty ASR,
            // topic completion), so clear the active-turn guard here.
            cloud_turn_active_.store(false);
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            if (!IsTopicListeningPending()) {
                // Standby without a wakeup handoff means the topic turn is over, so the next
                // listening session is an ordinary one.
                topic_listening_after_playback_.store(false);
            }
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                if (board.KeepAudioChannelOnIdle()) {
                    ESP_LOGI(TAG, "Idle with the conversation WebSocket kept open for the next turn");
                } else {
                    ESP_LOGI(TAG, "Entering standby; closing conversation WebSocket");
                    protocol_->CloseAudioChannel(false);
                }
            }
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            LatencyTracker::Instance().Mark(LatencyStage::kListeningState);

            if (topic_listening_after_playback_.exchange(false) &&
                topic_listening_timer_handle_ != nullptr) {
                int window_seconds = 60;
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                window_seconds = GetMoinaiDeviceSettings().wakeup_time_seconds;
#endif
                topic_listening_timeout_active_.store(true);
                esp_timer_stop(topic_listening_timer_handle_);
                esp_timer_start_once(
                    topic_listening_timer_handle_,
                    (window_seconds + kTopicListeningGraceSeconds) * 1000000ULL);
                ESP_LOGI(TAG, "Topic playback completed; listening for %d seconds",
                         window_seconds);
            }

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                // Draw the line between this utterance and the previous one. A turn opened by
                // a wakeup keeps the audio buffered since that wakeup; a turn that follows the
                // previous one directly discards what accumulated in the gap.
                static_cast<Ci130xAudioCodec*>(board.GetAudioCodec())->BeginMicTurn();
#endif

                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                // start_talk marks a fresh server-side turn; a later local
                // command/wakeup revocation has something to cancel.
                cloud_turn_active_.store(true);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
                // The CI130X answers the wake word with its own local prompt. Streaming a
                // second acknowledgement here would open a cloud playback session, and the
                // chip stops any prompt still playing when that session starts, cutting the
                // wake prompt down to the fragment sap_stop leaves behind. The flag is still
                // cleared above because it also gates the start_talk block.
#else
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            break;
        case kDeviceStateRevoked:
            display->SetStatus(Lang::Strings::ERROR);
            display->SetEmotion("circle_xmark");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::FinishSpeakingTurn() {
    Schedule([this]() {
        if (GetDeviceState() != kDeviceStateSpeaking) {
            return;
        }
        if (topic_listening_after_playback_.load()) {
#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
            auto* ci130x_codec = static_cast<Ci130xAudioCodec*>(
                Board::GetInstance().GetAudioCodec());
            // The chip reports the wakeup only after this Idle transition has been handled, so
            // arm the handoff first: it holds the conversation socket open until listening
            // starts. The window itself stays at the backend-configured wakeupTime.
            topic_listening_handoff_deadline_.store(
                xTaskGetTickCount() + pdMS_TO_TICKS(kTopicWakeupHandoffTimeoutMs));
            SetDeviceState(kDeviceStateIdle);
            ci130x_codec->EnterWakeup();
#else
            SetListeningMode(kListeningModeAutoStop);
#endif
        } else if (speaking_turn_ends_idle_.load() || listening_mode_ == kListeningModeManualStop) {
            SetDeviceState(kDeviceStateIdle);
        } else {
            SetDeviceState(kDeviceStateListening);
        }
    });
}

void Application::AbortSpeaking(AbortReason reason, bool notify_server) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    tts_audio_stream_active_.store(false);
    if (notify_server && protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    auto* ci130x_codec = static_cast<Ci130xAudioCodec*>(
                Board::GetInstance().GetAudioCodec());
    ci130x_codec->NotifyStatus(CI_STATUS_UPGRADING);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        if (topic_poll_task_handle_ != nullptr) {
            vTaskDelete(topic_poll_task_handle_);
            topic_poll_task_handle_ = nullptr;
        }
        topic_tts_delivery_pending_.store(false);
        topic_tts_started_.store(false);
        topic_tts_done_.store(false);
        topic_tts_failed_.store(false);
        topic_tts_preempted_.store(false);
        topic_queue_pending_.store(false);
        topic_listening_handoff_deadline_.store(0);
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
