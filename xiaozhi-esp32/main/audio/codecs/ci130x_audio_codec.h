#ifndef _CI130X_AUDIO_CODEC_H
#define _CI130X_AUDIO_CODEC_H

#include "audio_codec.h"
#include "ci130x_protocol.h"
#include <driver/uart.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <atomic>
#include <mutex>
#include <functional>
#include <utility>

class Ci130xAudioCodec : public AudioCodec {
private:
    // The CI130X replays the roll-back it staged before VAD start as one UART burst, and the
    // consumer only starts once the application has reached Listening. Both the burst and the
    // audio arriving during that gap have to fit, or the parser drops the beginning of the
    // utterance. 24576 bytes is 768 ms of 16 kHz mono PCM.
    static constexpr size_t kPcmRxRingbufBytes = 24576;

    uart_port_t uart_num_;
    int baud_rate_;
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;

    RingbufHandle_t pcm_rx_ringbuf_ = nullptr;
    std::atomic<bool> accept_pcm_input_{false};
    // Captured PCM the parser threw away during the current turn, split by cause: the ingress
    // being closed (the head of an utterance the application was not ready for) against the
    // ring being full (the burst outrunning the buffer). They need different fixes, so they
    // are counted apart. 32 bytes is 1 ms of 16 kHz mono PCM.
    std::atomic<uint32_t> pcm_dropped_closed_bytes_{0};
    std::atomic<uint32_t> pcm_dropped_full_bytes_{0};
    // Set when a wakeup has already opened the buffer for the turn that is about to start, so
    // BeginMicTurn() knows the bytes waiting there are this utterance rather than stale ones.
    std::atomic<bool> mic_turn_armed_by_wakeup_{false};
    std::mutex pcm_read_mutex_;
    int64_t next_pcm_read_deadline_us_ = 0;
    SemaphoreHandle_t play_get_sem_ = nullptr;
    // Cloud PCM and CI130X-local prompts have independent lifecycles. Sharing
    // one flag lets LOCAL_PLAY_STOP make an active cloud Write() repeatedly
    // restart PLAY_START after a wake-word interruption.
    std::atomic<bool> cloud_playing_{false};
    std::atomic<bool> local_playing_{false};
    std::atomic<bool> output_stream_active_{false};
    // Set while a cloud TTS response is being streamed out, cleared once its end has been
    // reported to the application. Guards the fallback completion for a response whose
    // CI130X session was already gone when the last PCM frame drained.
    std::atomic<bool> tts_stream_completion_pending_{false};
    bool is_awake_ = false;
    uint32_t tts_session_ = 0;
    std::atomic<uint32_t> play_get_quota_{0};
    esp_timer_handle_t playback_timer_ = nullptr;
    esp_timer_handle_t local_play_fallback_timer_ = nullptr;
    // Set when the backend has finished emitting startup statuses. The voice is
    // chosen first and armed_ published last, so the RX task never sends an
    // unset id. The prompt itself goes out only after the queued status prompts
    // have drained, keeping the greeting the last piece of startup audio.
    std::atomic<bool> power_on_prompt_requested_{false};
    uint16_t power_on_prompt_voice_id_ = 0;
    std::atomic<bool> power_on_prompt_armed_{false};
    std::atomic<bool> power_on_prompt_started_{false};
    std::atomic<bool> power_on_prompt_finished_{false};
    std::atomic<bool> backend_ready_for_wakeup_{false};
    std::atomic<bool> power_on_wakeup_entered_{false};
    std::atomic<uint16_t> last_status_notified_{0};
    std::atomic<uint16_t> startup_status_pending_{0};
    std::mutex uart_mutex_;

    void InitializeUart();
    void StartRxTask();
    void StartPlaybackSession();
    static void RxTask(void* arg);
    static void OnPlaybackTimer(void* arg);
    static void OnLocalPlayFallbackTimer(void* arg);
    void SendPowerOnPrompt();
    void TryEnterWakeupAfterPowerOn();
    void ReportTtsPlaybackFinished();

    void SendPacket(uint16_t cmd, const uint8_t* payload, size_t len);
    void HandlePacket(uint16_t cmd, const uint8_t* payload, size_t len);
    void FlushPcmRxBuffer();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    Ci130xAudioCodec(uart_port_t uart_num, gpio_num_t tx_pin, gpio_num_t rx_pin, int baud_rate,
                    int input_sample_rate, int output_sample_rate);
    virtual ~Ci130xAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    virtual int OutputBufferedMs() const override;
    virtual void NotifyOutputStreamStart() override;
    virtual void NotifyOutputStreamEnd() override;
    // PLAY_TTS_END arrives only after the CI130X has played the PCM it still holds, which is
    // seconds after the server stopped sending it.
    virtual bool PlaybackCompletionIsAsync() const override { return true; }
    void OnWakeup(std::function<void(bool was_already_awake)> callback) { on_wakeup_ = std::move(callback); }
    void OnAsrResult(std::function<void(uint16_t cmd_id)> callback) { on_asr_result_ = std::move(callback); }
    void OnVadStart(std::function<void()> callback) { on_vad_start_ = std::move(callback); }
    void OnVadEnd(std::function<void()> callback) { on_vad_end_ = std::move(callback); }
    void OnTtsEnd(std::function<void()> callback) { on_tts_end_ = std::move(callback); }
    void OnExitWakeup(std::function<void()> callback) { on_exit_wakeup_ = std::move(callback); }
    void OnLocalPlayStart(std::function<void()> callback) { on_local_play_start_ = std::move(callback); }
    bool IsAwake() const { return is_awake_; }
    bool IsLocalPlaying() const { return local_playing_.load(); }
    // Captured PCM the CI130X has already delivered but Read() has not handed on yet. Read()
    // is paced to real time, so a burst leaves a backlog that survives until the end of the
    // turn; it has to be consumed before the uplink is torn down.
    size_t PendingInputBytes() const;

    // Call once per listening turn, before voice processing starts. Keeps the buffer a wakeup
    // already filled for this turn, and drops what accumulated in the gap when there was no
    // wakeup, so one turn's leftovers never prepend to the next.
    void BeginMicTurn();
    virtual void Start() override;

    // Block the next incoming TTS stream entirely.
    // All Write() calls will be silently dropped until ClearTtsBlock() is called.
    // If audio is currently playing, it will be forcefully stopped immediately.
    void BlockNextTts();

    // Clear the TTS block. Must be called from board code before every StartListening().
    // This is the ONLY reliable reset mechanism - we do NOT rely on EnableInput()
    // because AudioCodec::EnableInput() short-circuits when input is already enabled.
    void ClearTtsBlock();
    void PlayOfflineVoice(uint16_t voice_id);
    void NotifyStatus(Ci130xStatus status);
    // Arms the power-on greeting. It plays once every status prompt the CI130X
    // has queued (registering, activating, connecting, ...) has finished, and
    // wakeup follows once the greeting itself ends.
    void StartPowerOnPrompt();
    void NotifyBackendReadyForWakeup();
    bool IsStartupAudioComplete() const {
        return power_on_prompt_finished_.load() &&
               startup_status_pending_.load() == 0 && !local_playing_.load();
    }
    // True until the power-on sequence has sent its wakeup. The device reaches
    // Idle before that happens, so this keeps the conversation socket that was
    // preconnected during activation from being torn down and reopened.
    bool IsPowerOnWakeupPending() const { return !power_on_wakeup_entered_.load(); }
    void ApplyStoredSettings();
    bool SetVadSensitivity(int sensitivity);
    bool SetPcmDenoiseEnabled(bool enabled);
    bool SetVadFilterFrames(int frames);
    bool SetVadEndTrigger(int milliseconds);
    bool SetVadStartMaxTimeout(int seconds);
    bool SetWakeupContinueTime(int seconds);
    bool SetSwm221SleepTimeout(int seconds);
    void SetTopicPlayback(bool topic_playback);
    bool SetMultiRoundEnabled(bool enabled);
    bool SetFullDuplexEnabled(bool enabled);
    bool SetCiVolume(int volume);
    bool SetMuted(bool muted);
    bool SetVadStopPlayback(bool enabled);

    // Send SET_ENTER_WAKE_UP without a wakeup prompt.
    void EnterWakeup();
    void ExitWakeup();

private:
    std::function<void(bool was_already_awake)> on_wakeup_;
    std::function<void(uint16_t cmd_id)> on_asr_result_;
    std::function<void()> on_vad_start_;
    std::function<void()> on_vad_end_;
    std::function<void()> on_tts_end_;
    std::function<void()> on_exit_wakeup_;
    std::function<void()> on_local_play_start_;

    // TTS blocking: drops ALL Write() calls while active.
    // Reset explicitly via ClearTtsBlock() from board code before StartListening().
    bool tts_blocked_ = false;
    bool tts_block_abort_sent_ = false;
};

#endif // _CI130X_AUDIO_CODEC_H
