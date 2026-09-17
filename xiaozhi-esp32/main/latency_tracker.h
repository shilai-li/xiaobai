#ifndef LATENCY_TRACKER_H
#define LATENCY_TRACKER_H

// Conversation latency instrumentation (P0-0).
// Records one timestamp per pipeline stage of a conversation turn and prints a
// single summary line once the turn finishes, so a real-world log directly
// shows where the end-to-end delay is spent:
//
//   VAD start -> Listening -> VAD end -> mic drain -> encode idle -> vad_done sent
//   -> TTS start -> prebuffer ready -> first PCM write -> TTS stop -> TTS end
//
// All stages are stored in microseconds from esp_timer_get_time(). A stage that
// never fired keeps the value 0 and is omitted from the report.
//
// Turn boundaries: a new turn begins when the application enters Listening,
// except when a VAD end arrived moments ago without a vad_done (the deferred
// vad_done case: the socket was still connecting while the user spoke) -- that
// Listening entry belongs to the segment already in flight. A turn is closed
// when the output codec reports playback finished; a turn that never gets
// there (abort, watchdog) is reported at the next turn boundary instead.

#include <esp_log.h>
#include <esp_timer.h>
#include <atomic>
#include <cstdio>

enum class LatencyStage {
    kVadStart = 0,       // CI130X reported VAD start (user began speaking)
    kListeningState,     // Application entered Listening (start_talk sent)
    kVadEnd,             // CI130X reported VAD end (silence confirmed)
    kMicDrainDone,       // Microphone backlog drained before vad_done
    kEncodeIdle,         // Uplink Opus encoder queue drained
    kVadDoneSent,        // vad_done marker left the device
    kTtsStart,           // Server TTS stream started
    kPlaybackReady,      // Streaming prebuffer reached its target
    kFirstPcmWrite,      // First decoded PCM handed to the output codec
    kPlayStart,          // Output codec started its playback session
    kTtsStop,            // Server finished sending TTS
    kTtsEnd,             // Output codec really finished playing
    kStageCount,
};

class LatencyTracker {
public:
    static LatencyTracker& Instance() {
        static LatencyTracker instance;
        return instance;
    }

    void Mark(LatencyStage stage) {
        if (stage == LatencyStage::kListeningState && ShouldBeginTurn()) {
            // A VAD segment that opened just before this Listening entry
            // (continuous dialogue) is the head of the new turn, not the tail
            // of the old one, so keep it across the reset.
            const int64_t recent_vad_start = Get(LatencyStage::kVadStart);
            ReportIfPending();
            Reset();
            if (recent_vad_start != 0 &&
                esp_timer_get_time() - recent_vad_start < 3000 * 1000) {
                stages_[static_cast<size_t>(LatencyStage::kVadStart)].store(recent_vad_start);
            }
        }
        const size_t idx = static_cast<size_t>(stage);
        int64_t expected = stages_[idx].load();
        while (expected == 0 &&
               !stages_[idx].compare_exchange_weak(expected, esp_timer_get_time())) {
        }
    }

    // Uplink diagnostics: called for every audio packet handed to the protocol.
    // last_mic_capture is the timestamp carried by that packet, so
    // last_uplink_sent - last_mic_capture is the total device-side latency of
    // the utterance tail (microphone capture -> encode -> queue -> wire).
    void RecordUplinkPacket(int64_t mic_capture_time_us) {
        const int64_t now = esp_timer_get_time();
        int64_t expected = first_uplink_sent_.load();
        while (expected == 0 &&
               !first_uplink_sent_.compare_exchange_weak(expected, now)) {
        }
        last_uplink_sent_ = now;
        if (mic_capture_time_us > 0) {
            last_mic_capture_ = mic_capture_time_us;
        }
    }

    // Closes the current turn: reports it and clears every stage.
    void FinishTurn() {
        stages_[static_cast<size_t>(LatencyStage::kTtsEnd)].store(esp_timer_get_time());
        ReportIfPending();
        Reset();
    }

    // Abandons the current turn: a revoked turn never reaches playback
    // finished, so report whatever partial stages were collected and reset.
    // Without this, the cancelled turn's marks (e.g. a VAD end without its
    // vad_done) survive into the next turn's report and corrupt it, and the
    // stale vad_end makes ShouldBeginTurn() misclassify the next Listening
    // entry as the deferred-vad_done continuation.
    void AbortTurn() {
        ReportIfPending();
        Reset();
    }

    void ReportIfPending() {
        if (reported_.load()) {
            return;
        }
        reported_ = true;
        const int64_t vad_end = Get(LatencyStage::kVadEnd);
        const int64_t tts_start = Get(LatencyStage::kTtsStart);
        if (vad_end == 0 && tts_start == 0) {
            // Nothing reportable: wakeup without speech, or a local sound.
            return;
        }
        const int64_t vad_start = Get(LatencyStage::kVadStart);
        const int64_t listening = Get(LatencyStage::kListeningState);
        const int64_t drain_done = Get(LatencyStage::kMicDrainDone);
        const int64_t encode_idle = Get(LatencyStage::kEncodeIdle);
        const int64_t vad_done = Get(LatencyStage::kVadDoneSent);
        const int64_t ready = Get(LatencyStage::kPlaybackReady);
        const int64_t first_write = Get(LatencyStage::kFirstPcmWrite);
        const int64_t tts_stop = Get(LatencyStage::kTtsStop);
        const int64_t tts_end = Get(LatencyStage::kTtsEnd);

        char line[512];
        // NOTE: the firmware uses newlib nano formatting, which has no "ll"
        // length modifier -- print 32-bit milliseconds with %ld instead.
        int off = snprintf(line, sizeof(line), "turn latency:");
        if (vad_start && listening) {
            off += snprintf(line + off, sizeof(line) - off, " vad_start->listening=%ldms",
                            Ms(listening - vad_start));
        }
        if (vad_end) {
            if (drain_done) {
                off += snprintf(line + off, sizeof(line) - off, " vad_end->drain=%ldms", Ms(drain_done - vad_end));
            }
            if (drain_done && encode_idle) {
                off += snprintf(line + off, sizeof(line) - off, " drain->enc_idle=%ldms", Ms(encode_idle - drain_done));
            }
            if (encode_idle && vad_done) {
                off += snprintf(line + off, sizeof(line) - off, " enc_idle->vad_done=%ldms", Ms(vad_done - encode_idle));
            }
        }
        if (vad_done && tts_start) {
            off += snprintf(line + off, sizeof(line) - off, " vad_done->tts_start=%ldms(server)", Ms(tts_start - vad_done));
        } else if (vad_end && tts_start) {
            off += snprintf(line + off, sizeof(line) - off, " vad_end->tts_start=%ldms(server)", Ms(tts_start - vad_end));
        }
        if (tts_start && first_write) {
            off += snprintf(line + off, sizeof(line) - off, " tts_start->first_write=%ldms", Ms(first_write - tts_start));
            if (ready) {
                off += snprintf(line + off, sizeof(line) - off, " (prebuffer@%ldms)", Ms(ready - tts_start));
            }
        }
        if (first_write && tts_end) {
            off += snprintf(line + off, sizeof(line) - off, " first_write->tts_end=%ldms", Ms(tts_end - first_write));
            if (tts_stop) {
                off += snprintf(line + off, sizeof(line) - off, " (tts_stop@%ldms)", Ms(tts_stop - first_write));
            }
        }
        if (last_uplink_sent_ && last_mic_capture_) {
            off += snprintf(line + off, sizeof(line) - off, " | tail_mic->wire=%ldms", Ms(last_uplink_sent_ - last_mic_capture_));
        }
        if (vad_end && first_write) {
            off += snprintf(line + off, sizeof(line) - off, " | TOTAL vad_end->first_write=%ldms", Ms(first_write - vad_end));
        }
        ESP_LOGI("Latency", "%s", line);
    }

private:
    LatencyTracker() = default;

    static long Ms(int64_t us) { return static_cast<long>(us / 1000); }

    int64_t Get(LatencyStage stage) const {
        return stages_[static_cast<size_t>(stage)].load();
    }

    void Reset() {
        for (size_t i = 0; i < static_cast<size_t>(LatencyStage::kStageCount); ++i) {
            stages_[i].store(0);
        }
        first_uplink_sent_ = 0;
        last_uplink_sent_ = 0;
        last_mic_capture_ = 0;
        // The stages recorded after this point belong to a fresh turn that has
        // not been reported yet.
        reported_ = false;
    }

    // Entering Listening starts a new turn, except when a VAD end arrived
    // recently without its vad_done: that Listening entry belongs to the
    // deferred-vad_done turn whose socket was still connecting.
    bool ShouldBeginTurn() {
        if (reported_.load()) {
            return true;
        }
        const int64_t vad_end = Get(LatencyStage::kVadEnd);
        const int64_t vad_done = Get(LatencyStage::kVadDoneSent);
        if (vad_end != 0 && vad_done == 0 &&
            esp_timer_get_time() - vad_end < 3000 * 1000) {
            return false;
        }
        return true;
    }

    std::atomic<int64_t> stages_[static_cast<size_t>(LatencyStage::kStageCount)] = {};
    std::atomic<int64_t> first_uplink_sent_{0};
    std::atomic<int64_t> last_uplink_sent_{0};
    std::atomic<int64_t> last_mic_capture_{0};
    std::atomic<bool> reported_{true};
};

#endif  // LATENCY_TRACKER_H
