#include "ml307_board.h"
#include "codecs/ci130x_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <atomic>

#define TAG "Esp32c3Ci130xBoard"

// Flag to suppress cloud listening when a local wake word / offline command is being processed.
// Prevents OnVadStart's scheduled StartListening from racing with CancelCloudSession.
static std::atomic<bool> s_suppress_cloud_start{false};

// A CI130X wakeup can be followed by LOCAL_PLAY_START for the wake prompt.
// That prompt belongs to the same wakeup transaction and must not enqueue a
// second CancelCloudSession(false), otherwise the successful socket reuse is
// immediately overwritten by Listening -> Idle -> CloseAudioChannel.
static std::atomic<int64_t> s_ignore_wakeup_local_play_until_ms{0};

// VAD End can arrive before the cloud session reaches Listening, because the socket may still be
// connecting. Dropping it leaves the turn with no vad_done and nothing to end it, so latch it here
// and flush it as soon as Listening starts.
static std::atomic<bool> s_pending_vad_end{false};

// A VAD segment that opens while the device is speaking is ignored as a barge-in, but the CI130X
// still reports its VAD End once playback is over. By then the turn has auto-restarted, so that
// tail would end a fresh turn after a few hundred ms and upload almost no audio.
static std::atomic<bool> s_drop_next_vad_end{false};

// A turn whose VAD End never arrives keeps no record of needing to close: the device stays in
// Listening, no vad_done reaches the server, and the server waits out its own turn timeout
// instead of answering. Bound such a turn here and let the normal path emit the marker, so
// both ends agree on when it ended. Only a turn that has captured speech is watched, so a
// user who has not started talking yet is never cut off. The period clears the chip's own
// ceiling on one segment -- vad_start_max_timeout (5 s) plus vad_end_trigger (700 ms) plus
// the microphone drain margin -- and stays under the server's timeout, so it can only fire
// once the end marker is already lost.
static constexpr uint64_t kListeningWatchdogPeriodUs = 10ULL * 1000 * 1000;
static esp_timer_handle_t s_listening_watchdog_timer = nullptr;

static void ArmListeningWatchdog() {
    if (s_listening_watchdog_timer == nullptr) return;
    esp_timer_stop(s_listening_watchdog_timer);
    esp_timer_start_periodic(s_listening_watchdog_timer, kListeningWatchdogPeriodUs);
}

static void StopListeningWatchdog() {
    if (s_listening_watchdog_timer == nullptr) return;
    esp_timer_stop(s_listening_watchdog_timer);
}

class Esp32c3Ci130xBoardLed : public Led {
private:
    Ci130xAudioCodec& codec_;
    esp_timer_handle_t timer_;
    DeviceState last_state_ = kDeviceStateUnknown;

public:
    Esp32c3Ci130xBoardLed(Ci130xAudioCodec& codec, esp_timer_handle_t timer) 
        : codec_(codec), timer_(timer) {}

    void OnStateChanged() override {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        DeviceState old_state = last_state_;
        last_state_ = state;

        if (state == kDeviceStateSpeaking) {
            // The turn a deferred VadEnd belonged to has just been answered, so its
            // vad_done has nothing left to close. Dropping it here is what keeps the
            // flush below from ending the next listening turn before it has captured
            // any audio; OnVadStart clears the same latch, but only once the user
            // speaks, which is always after listening reopens.
            s_pending_vad_end.store(false);
        }

        if (state == kDeviceStateListening && s_pending_vad_end.exchange(false)) {
            // The switch below still sends start_talk for this state change; StopListening only
            // sets an event bit, so vad_done stays behind it on the wire.
            ESP_LOGI(TAG, "Flushing the VadEnd that arrived while the socket was connecting.");
            app.StopListening();
        }

        if (state == kDeviceStateListening) {
            // The utterance that opened while the device was speaking carries this turn's
            // only VAD End, and the latch above is about to consume it. OnVadStart cannot
            // arm the watchdog for that segment because it began outside the turn.
            if (s_drop_next_vad_end.load()) {
                ArmListeningWatchdog();
            }
        } else if (old_state == kDeviceStateListening) {
            StopListeningWatchdog();
        }

        if (state == kDeviceStateConnecting && old_state == kDeviceStateListening) {
            if (timer_) {
                esp_timer_stop(timer_);
                // vad_done has been sent and the socket must stay open while the
                // cloud prepares its response. Recover if no response arrives.
                esp_timer_start_once(timer_, 20ULL * 1000 * 1000); // 20 seconds
            }
        } else {
            if (timer_) esp_timer_stop(timer_);
            if (state == kDeviceStateIdle) {
                codec_.ClearTtsBlock();
            }
        }
    }
};

class Esp32c3Ci130xBoard : public Ml307Board {
private:
    Button boot_button_;
    esp_timer_handle_t silent_timeout_timer_ = nullptr;
    Esp32c3Ci130xBoardLed* board_led_ = nullptr;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                app.ToggleChatState();
                return;
            }
        });
        boot_button_.OnPressDown([this]() {
            auto& app = Application::GetInstance();
            if (app.PreemptTopicDeliveryForUserWakeup()) {
                ESP_LOGI(TAG, "Button interrupted topic delivery; prioritizing user listening.");
                CancelCloudSession(true);
                return;
            }
            auto codec = (Ci130xAudioCodec*)Board::GetInstance().GetAudioCodec();
            codec->ClearTtsBlock();
            app.StartListening();
        });
        boot_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });
    }

    // Cancel any active cloud session when a local event (wake word / offline command) takes priority.
    // Called from UART thread context - all APIs used here are thread-safe.
    static void CancelCloudSession(bool restart_listening = false) {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state != kDeviceStateIdle && state != kDeviceStateConnecting &&
            state != kDeviceStateListening && state != kDeviceStateSpeaking) {
            return;
        }

        // 1. Suppress any pending VadStart from starting a cloud session.
        s_suppress_cloud_start.store(true);
        s_pending_vad_end.store(false);
        app.DiscardCi130xLocalCommandAudioGate();

        // Block the next incoming TTS stream immediately. All Write() calls will be dropped
        // until ClearTtsBlock() is called (before the next StartListening).
        auto codec = (Ci130xAudioCodec*)Board::GetInstance().GetAudioCodec();
        codec->BlockNextTts();

        // 2. Drain audio send queue from UART thread to prevent leaked audio
        while (app.GetAudioService().PopPacketFromSendQueue()) {}

        // 3. Set cancellation timestamp to debounce VAD End
        s_last_cancel_time = esp_timer_get_time() / 1000;

        // 4. Handle state-specific cancellation in the main task
        app.Schedule([codec, restart_listening]() {
            auto& app = Application::GetInstance();
            auto current_state = app.GetDeviceState();
            // Reuse the healthy WebSocket only for an active cloud-TTS
            // barge-in. All other re-wakeups must go through Idle so the old
            // channel is closed before the next conversation starts.
            if (restart_listening && app.RestartListeningOnOpenAudioChannel()) {
                codec->ClearTtsBlock();
                s_suppress_cloud_start.store(false);
                return;
            }

            if (current_state == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonWakeWordDetected);
                app.GetAudioService().ResetDecoder();
                // End the turn here instead of waiting for the server's tts_done.
                // The abort never reaches this backend (WebsocketProtocol::SendText
                // has no mapping for type "abort"), so the response keeps streaming
                // and holds the conversation socket open for the rest of the answer
                // even though playback was already stopped locally. When
                // restart_listening is set this also forces the old channel through
                // Idle before a new one is opened.
                app.SetDeviceState(kDeviceStateIdle);
            } else if (current_state == kDeviceStateConnecting ||
                       current_state == kDeviceStateListening) {
                // Manually transition to Idle to cleanly abort the listening state
                // without queueing StopListening (which could race with subsequent StartListening)
                app.SetDeviceState(kDeviceStateIdle);
            }
            // Stop the producer and wait out an encoder frame that may already be in flight.
            // Otherwise that late packet could survive both drains and leak with the next turn.
            app.GetAudioService().EnableVoiceProcessing(false);
            app.GetAudioService().WaitForUplinkEncodeIdle();
            // Drain send queue once more to ensure clean state.
            while (app.GetAudioService().PopPacketFromSendQueue()) {}
            if (restart_listening) {
                codec->ClearTtsBlock();
                app.StartListening();
            }
            s_suppress_cloud_start.store(false);
        });
    }

    static uint32_t s_last_cancel_time;

public:
    Esp32c3Ci130xBoard() : Ml307Board(ML307_TX_PIN, ML307_RX_PIN, ML307_DTR_PIN),
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeButtons();
    }

    // While the CI130X stays awake the next turn starts on VAD, i.e. the user is already talking.
    // A fresh Socket.IO handshake takes 1-3 s there, longer than the whole utterance, so the
    // conversation socket is released only when the wakeup window closes.
    // The power-on sequence reaches Idle while the startup prompts are still playing and
    // enters wakeup only afterwards, so the socket must survive that gap too. A topic that
    // just finished playing waits for its own wakeup in Idle and needs the same treatment,
    // and a topic still queued when the window closes plays on this very socket.
    virtual bool KeepAudioChannelOnIdle() override {
        auto* codec = (Ci130xAudioCodec*)GetAudioCodec();
        auto& app = Application::GetInstance();
        return codec->IsAwake() || codec->IsPowerOnWakeupPending() ||
               app.IsTopicListeningPending() || app.HasQueuedTopic();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Ci130xAudioCodec audio_codec(
            CI130X_UART_NUM, 
            CI130X_UART_GPIO_TX, 
            CI130X_UART_GPIO_RX, 
            CI130X_UART_BAUD_RATE,
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE
        );

        static bool initialized = false;
        if (!initialized) {
            initialized = true;

            esp_timer_create_args_t watchdog_args = {
                .callback = [](void*) {
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() != kDeviceStateListening) return;
                    ESP_LOGW(TAG, "No VAD End closed this turn; ending it from the watchdog");
                    app.StopListening();
                },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "listening_watchdog"
            };
            esp_timer_create(&watchdog_args, &s_listening_watchdog_timer);

            // Wakeup handling: if already in a cloud session, cancel it first
            audio_codec.OnWakeup([](bool was_already_awake) {
                auto& app = Application::GetInstance();
                auto state = app.GetDeviceState();
                // FinishSpeakingTurn generates a silent wakeup through this same
                // callback. It acknowledges the topic handoff, not user input.
                if (!app.IsTopicListeningPending() &&
                    app.PreemptTopicDeliveryForUserWakeup()) {
                    // A physical/wake-word interaction has priority over an unattended topic.
                    // Cancel its delivery, suppress the prompt's matching LOCAL_PLAY_START,
                    // and reopen listening after the topic channel has been cleaned up.
                    ESP_LOGI(TAG, "Wakeup interrupted topic delivery; prioritizing user listening.");
                    s_ignore_wakeup_local_play_until_ms.store(
                        esp_timer_get_time() / 1000 + 1000);
                    CancelCloudSession(true);
                    return;
                }
                if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                    state == kDeviceStateSpeaking) {
                    if (was_already_awake) {
                        // Re-wakeup while already awake: cancel the previous session and restart listening directly
                        // to bypass the core application's buggy WakeWordInvoke handler.
                        ESP_LOGI(TAG, "Wakeup: Re-wakeup while already awake, restarting listening session directly.");
                        s_ignore_wakeup_local_play_until_ms.store(
                            esp_timer_get_time() / 1000 + 1000);
                        CancelCloudSession(true);
                    } else {
                        // First wakeup but cloud session was started by OnVadStart before CI130x
                        // recognized the wake word. Cancel fully - the audio was not a real query.
                        ESP_LOGI(TAG, "Wakeup: Already in cloud session, cancelling.");
                        CancelCloudSession();
                    }
                } else {
                    // No cloud session active - normal first wakeup
                    audio_codec.ClearTtsBlock();
                    app.StartListening();
                }
            });

            // Offline command: cancel active cloud session conditionally
            audio_codec.OnAsrResult([](uint16_t cmd_id) {
                ESP_LOGI(TAG, "ASR Result: Offline command detected, cmd_id: %d", cmd_id);
                if (cmd_id == 100) {
                    CancelCloudSession();
                } else if ((cmd_id >= 3 && cmd_id <= 6) || (cmd_id >= 9 && cmd_id <= 10) || (cmd_id >= 302 && cmd_id <= 314)) {
                    auto& app = Application::GetInstance();
                    auto state = app.GetDeviceState();
                    if (state != kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "ASR Result: Non-interrupt command %d detected while not speaking, cancelling cloud session.", cmd_id);
                        CancelCloudSession();
                    } else {
                        ESP_LOGI(TAG, "ASR Result: Non-interrupt command %d detected while speaking, ignoring to keep speaking.", cmd_id);
                    }
                } else {
                    ESP_LOGI(TAG, "ASR Result: Command %d is not a local control command, not cancelling cloud session.", cmd_id);
                }
            });

            // Exit wakeup (e.g. offline command 100 or timeout): cancel active cloud session and stop playback
            audio_codec.OnExitWakeup([]() {
                ESP_LOGI(TAG, "Exit Wakeup: Offline command 100 or timeout detected.");
                CancelCloudSession();
                // IsAwake() is already false, so Idle no longer keeps the socket. A device that was
                // Idle all along produces no transition, so release the channel explicitly.
                Application::GetInstance().Schedule([]() {
                    auto& app = Application::GetInstance();
                    if (app.HasQueuedTopic()) {
                        // The window closed without speech and a topic is waiting. It plays on
                        // this socket, so keep it instead of paying for a second handshake.
                        ESP_LOGI(TAG, "Exit Wakeup: keeping the socket for a queued topic.");
                        return;
                    }
                    app.ReleaseIdleAudioChannel();
                });
            });

            // Local play start (e.g. offline command voice broadcast starts): cancel active cloud session immediately
            // only if the assistant is currently speaking, to avoid cancelling normal wake up.
            audio_codec.OnLocalPlayStart([]() {
                const int64_t now_ms = esp_timer_get_time() / 1000;
                const int64_t ignore_until_ms =
                    s_ignore_wakeup_local_play_until_ms.exchange(0);
                if (ignore_until_ms != 0 && now_ms <= ignore_until_ms) {
                    ESP_LOGI(TAG, "Local Play Start belongs to re-wakeup; skipping duplicate cloud cancellation.");
                    return;
                }

                auto& app = Application::GetInstance();
                auto state = app.GetDeviceState();
                if (state == kDeviceStateSpeaking) {
                    ESP_LOGI(TAG, "Local Play Start: Cloud session active while speaking, cancelling immediately.");
                    CancelCloudSession();
                }
            });

            // Barge-in / Continuous dialogue: trigger listening on VAD if CI130x is still awake
            audio_codec.OnVadStart([]() {
                // A new utterance must never inherit the previous one's deferred VadEnd.
                s_pending_vad_end.store(false);
                auto& app = Application::GetInstance();
                const auto state = app.GetDeviceState();
                if (state != kDeviceStateSpeaking && audio_codec.IsAwake()) {
                    // CI130X reports VAD start before streaming this segment's PCM. Close the
                    // uplink gate here in the UART task so no speech packet can win the race.
                    app.BeginCi130xLocalCommandAudioGate();
                }
                if (state == kDeviceStateListening) {
                    // This turn now holds speech the server is waiting to have finalised.
                    // Re-arming per segment keeps the watchdog a backstop for a lost VAD
                    // End rather than a cap on how long the user may speak.
                    ArmListeningWatchdog();
                }
                Application::GetInstance().Schedule([]() {
                    // Check suppress flag - set by CancelCloudSession to prevent
                    // this callback from racing with the cancellation
                    if (s_suppress_cloud_start.load()) {
                        ESP_LOGI(TAG, "VadStart suppressed: local command active.");
                        return;
                    }

                    auto& app = Application::GetInstance();
                    auto state = app.GetDeviceState();
                    // Only a segment ignored as barge-in must have its end ignored too. Any other
                    // start means a real utterance is under way, so honour that one's end.
                    s_drop_next_vad_end.store(state == kDeviceStateSpeaking);
                    if (state == kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "Barge-in: VAD detected while speaking; ignoring it until a wake-word restart.");
                        // app.AbortSpeaking(kAbortReasonNone); // Disabled to avoid
                        // false VAD starts from AEC leakage interrupting playback.
                        // Do NOT manually set state = kDeviceStateIdle here.
                        // This prevents immediately starting a new listening session on false VAD 
                        // triggers caused by AEC leakage, breaking the infinite echo loop.
                        return;
                    }
                    
                    if (state == kDeviceStateIdle && audio_codec.IsAwake()) {
                        ESP_LOGI(TAG, "Continuous Dialogue: CI130x is awake, starting cloud listening on VAD.");
                        audio_codec.ClearTtsBlock();
                        app.StartListening();
                    }
                });
            });

            // VAD/Stop handling
            audio_codec.OnVadEnd([]() {
                uint32_t now = esp_timer_get_time() / 1000;
                if (now - s_last_cancel_time < 1000) {
                    ESP_LOGI(TAG, "VadEnd ignored due to recent cloud session cancellation.");
                    s_pending_vad_end.store(false);
                    return;
                }
                if (s_drop_next_vad_end.exchange(false)) {
                    ESP_LOGI(TAG, "VadEnd closes a barge-in ignored while speaking; keeping the turn open.");
                    return;
                }
                auto& app = Application::GetInstance();
                const auto state = app.GetDeviceState();
                // No local ASR result cancelled this segment, so it belongs to the cloud.
                // Release before StopListening() drains the queue and emits vad_done.
                app.ReleaseCi130xLocalCommandAudioGate();
                if (state == kDeviceStateListening) {
                    app.StopListening();
                } else if (state == kDeviceStateConnecting) {
                    ESP_LOGI(TAG, "VadEnd arrived while connecting; deferring vad_done.");
                    s_pending_vad_end.store(true);
                }
            });

            // PLAY_TTS_END is the only point at which the CI130x has really finished the
            // response: the server's tts_done arrives while ~1.5 s of PCM is still buffered on
            // the ESP32 and in the CI130x player. End the turn here, then continue the dialogue
            // if the chip is still in its wakeup window.
            audio_codec.OnTtsEnd([]() {
                auto& app = Application::GetInstance();
                app.FinishSpeakingTurn();
                if (!audio_codec.IsAwake()) return; // Not in chat mode, don't auto-listen
                // Queued behind FinishSpeakingTurn so it observes the state it settled on.
                app.Schedule([]() {
                    auto& scheduled_app = Application::GetInstance();
                    if (scheduled_app.IsTopicListeningPending()) {
                        // FinishSpeakingTurn has issued EnterWakeup for a topic. Do not send
                        // start_talk until CI130X acknowledges that it can capture audio; its
                        // OnWakeup callback performs that transition.
                        ESP_LOGI(TAG, "Topic playback ended; waiting for CI130X wakeup acknowledgement");
                        return;
                    }
                    if (scheduled_app.GetDeviceState() == kDeviceStateIdle) {
                        audio_codec.ClearTtsBlock();
                        scheduled_app.StartListening();
                    }
                });
            });
        }
        return &audio_codec;
    }

    virtual ~Esp32c3Ci130xBoard() {
        if (silent_timeout_timer_) {
            esp_timer_stop(silent_timeout_timer_);
            esp_timer_delete(silent_timeout_timer_);
        }
        delete board_led_;
    }

    virtual Led* GetLed() override {
        if (board_led_ == nullptr) {
            auto codec = (Ci130xAudioCodec*)GetAudioCodec();
            
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto c = (Ci130xAudioCodec*)arg;
                    ESP_LOGI(TAG, "Silent response watchdog triggered: resetting CI130X busy state");
                    auto& app = Application::GetInstance();
                    app.Schedule([c]() {
                        c->ClearTtsBlock();
                        auto& scheduled_app = Application::GetInstance();
                        if (scheduled_app.GetDeviceState() == kDeviceStateConnecting) {
                            scheduled_app.SetDeviceState(kDeviceStateIdle);
                        }
                    });
                },
                .arg = codec,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "silent_timeout"
            };
            esp_timer_create(&timer_args, &silent_timeout_timer_);
            
            board_led_ = new Esp32c3Ci130xBoardLed(*codec, silent_timeout_timer_);
        }
        return board_led_;
    }
};

uint32_t Esp32c3Ci130xBoard::s_last_cancel_time = 0;

DECLARE_BOARD(Esp32c3Ci130xBoard);
