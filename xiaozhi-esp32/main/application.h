#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <atomic>
#include <mutex>
#include <deque>
#include <memory>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_DEVICE_REVOKED       (1 << 13)
#define MAIN_EVENT_DEVICE_REACTIVATE    (1 << 14)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason, bool notify_server = true);

    // Leave Speaking now that the response has really been heard. Boards whose codec reports
    // playback completion asynchronously call this from that report; everything else is driven
    // straight from the server's tts_done.
    void FinishSpeakingTurn();

    // Start a new turn on an already healthy conversation channel, but only
    // while cloud TTS is actively playing. Non-TTS wakeups use a fresh socket.
    bool RestartListeningOnOpenAudioChannel();

    // Release a channel that KeepAudioChannelOnIdle() held open, once the board's wakeup window
    // has closed. Needed because a device that is already Idle produces no state change to hook.
    void ReleaseIdleAudioChannel();

    // True while a played topic waits for the CI130X to acknowledge the wakeup that turns it into
    // listening. The acknowledgement only arrives after the Idle transition, so the conversation
    // socket has to survive that gap instead of being released as standby.
    bool IsTopicListeningPending() const;

    // True while the poll task still holds a topic that has not been played.
    bool HasQueuedTopic() const { return topic_queue_pending_.load(); }

    // Give an explicit user wakeup priority over an in-flight unattended topic.
    // Returns true when a topic was marked for a deferred retry.
    bool PreemptTopicDeliveryForUserWakeup();

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    // Resume continuous conversation after the backend finishes ASR without
    // recognizing any text. Safe to call from the protocol receive task.
    void RestartListeningAfterEmptyAsr();

    // Single source of truth for "the asr_done text carries no recognized
    // speech". Both the empty-turn restart path and the success-prompt path
    // must agree on this, or a turn could ring the prompt and then be
    // treated as empty (or the reverse).
    static bool IsAsrTextEmpty(const char* text);

    // Endpoint confirmation prompt: played when asr_done carries text, which
    // guarantees downstream llm_*/tts events. Safe to call from the protocol
    // receive task.
    void PlayAsrSuccessPrompt();

#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
    // Drop uplink packets still queued when a local command revokes the turn.
    // Speech streams out in real time, so only the final frame or two remain.
    // Thread-safe.
    void DiscardCi130xPendingUplink();
#endif

    // Revoke the active cloud turn by sending cancel_turn to the server, which
    // aborts its ASR/LLM/TTS pipeline and suppresses further messages of that
    // turn. No-op (nothing sent) when no turn is active, which also makes
    // consecutive cancels idempotent. Call from the main task so the send is
    // serialized with SEND_AUDIO processing.
    void CancelActiveCloudTurn(const char* reason);

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t topic_listening_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    std::atomic<bool> tts_audio_stream_active_{false};
    // Where the current speaking turn goes when it ends, decided when the server stops sending
    // audio but applied later if the codec reports playback completion asynchronously.
    std::atomic<bool> speaking_turn_ends_idle_{false};
    std::atomic<uint32_t> tts_audio_dropped_packets_{0};
    std::atomic<bool> suppress_protocol_errors_{false};
    std::atomic<bool> device_revoked_{false};
    std::atomic<bool> device_reactivation_pending_{false};
    std::atomic<bool> last_location_report_succeeded_{false};
    // A polled topic remains queued until the server confirms that its TTS
    // stream completed. These flags bridge the protocol callbacks and the
    // background polling task without moving queue ownership across tasks.
    std::atomic<bool> topic_tts_delivery_pending_{false};
    std::atomic<bool> topic_tts_started_{false};
    std::atomic<bool> topic_tts_done_{false};
    std::atomic<bool> topic_tts_failed_{false};
    std::atomic<bool> topic_tts_preempted_{false};
    std::atomic<bool> topic_listening_after_playback_{false};
    std::atomic<bool> topic_listening_timeout_active_{false};
    // Tick at which the wakeup handoff stops holding the conversation socket open, or zero when
    // no handoff is in flight.
    std::atomic<uint32_t> topic_listening_handoff_deadline_{0};
    std::atomic<bool> topic_queue_pending_{false};
    // Bumped by every user wake word or button press that is not the topic's
    // own playback handoff. The poll task compares it across a fetch.
    std::atomic<uint32_t> user_wakeup_seq_{0};
    // True while a cloud turn is active (start_talk sent or topic TTS delivery
    // in flight), i.e. a turn the server could still be processing. Guards
    // cancel_turn so it is only sent when there is something to revoke.
    std::atomic<bool> cloud_turn_active_{false};
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    TaskHandle_t topic_poll_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleDeviceRevokedEvent();
    void HandleDeviceReactivationEvent();
    void HandleWakeWordDetectedEvent();
    bool HandleMoinaiApiResult(esp_err_t err);
    bool OpenAudioChannelWithTokenRefresh();
    bool PrepareMoinaiSession();
    void ApplyMoinaiDeviceSettings();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);

    // Activation task (runs in background)
    void StartActivationTask();
    void ActivationTask();
    void StartTopicPolling();
    void TopicPollTask();
    static void OnTopicListeningTimeout(void* arg);

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
