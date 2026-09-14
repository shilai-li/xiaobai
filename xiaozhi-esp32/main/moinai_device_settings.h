#ifndef MOINAI_DEVICE_SETTINGS_H
#define MOINAI_DEVICE_SETTINGS_H

#include <cstdint>

// One resolved view of every setting the cloud can push. Cloud values remain in
// their own NVS namespace while the debug switch overlays constants in memory.

struct MoinaiDeviceSettings {
    bool debug_override_enabled = false;
    int topic_poll_interval_seconds = 0;
    int wakeup_time_seconds = 0;
    int standby_time_seconds = 0;
    int vad_end_trigger_ms = 0;
    int vad_sensitivity = 0;
    int vad_filter_frames = 0;
    int vad_start_max_timeout_seconds = 0;
    bool denoise_enabled = false;
    int volume = 0;
    bool muted = false;
    bool multi_round = false;
    bool full_duplex = false;
    bool vad_stop_playback = false;
};

// --- Debug override. true: overlay these values. false: use stored cloud values. ---
#define MOINAI_DEBUG_SETTINGS_OVERRIDE false

constexpr int kDebugTopicPollIntervalSeconds = 45;
constexpr int kDebugWakeupTimeSeconds = 20;
constexpr int kDebugStandbyTimeSeconds = 90;
// Cap on how long one upload may run after VAD start. Speech still ends on the
// end trigger below; this only bounds a turn that never falls silent.
constexpr int kDebugVadStartMaxTimeoutSeconds = 10;
constexpr int kDebugVadEndTriggerMs = 700;
constexpr int kDebugVadSensitivity = 49;
constexpr int kDebugVadFilterFrames = 40;
constexpr bool kDebugDenoiseEnabled = true;
constexpr int kDebugVolume = 7;
constexpr bool kDebugMuted = false;
constexpr bool kDebugMultiRound = true;
constexpr bool kDebugFullDuplex = false;
constexpr bool kDebugVadStopPlayback = false;
// --- End of the editable block. ---

// The CI130X setters drop out-of-range values with nothing but a console
// warning, which is easy to miss on a hand-edited constant. Catch it here.
constexpr int kMinimumTopicPollIntervalSeconds = 5;
// The local-command gate buffers the entire utterance. The uplink queue is
// derived from this limit, and stored cloud values are clamped when read.
constexpr int kMaximumVadStartTimeoutSeconds = 15;
// On top of the segment itself: 400 ms of CI130X roll-back (PCM_ALG_ROOLBACK_
// FRAME_LEN, 25 frames of 512 bytes) plus slack for encoder scheduling.
constexpr int kCi130xHeldAudioMarginMs = 1000;
static_assert(kMaximumVadStartTimeoutSeconds > 0 &&
              kMaximumVadStartTimeoutSeconds <= UINT16_MAX,
              "Maximum VAD timeout must fit the CI130X 16-bit payload");
static_assert(kDebugTopicPollIntervalSeconds >= kMinimumTopicPollIntervalSeconds,
              "Topic poll interval is clamped to the minimum below this");
static_assert(kDebugWakeupTimeSeconds > 0 && kDebugWakeupTimeSeconds <= UINT16_MAX,
              "Wakeup time must fit the CI130X 16-bit payload");
static_assert(kDebugStandbyTimeSeconds > 0 && kDebugStandbyTimeSeconds <= UINT16_MAX,
              "Standby time must fit the CI130X 16-bit payload");
static_assert(kDebugVadStartMaxTimeoutSeconds > 0 &&
              kDebugVadStartMaxTimeoutSeconds <= kMaximumVadStartTimeoutSeconds,
              "VAD timeout exceeds the local-command audio buffer");
static_assert(kDebugVadEndTriggerMs == 300 || kDebugVadEndTriggerMs == 400 ||
              kDebugVadEndTriggerMs == 500 || kDebugVadEndTriggerMs == 700 ||
              kDebugVadEndTriggerMs == 1000 || kDebugVadEndTriggerMs == 1500 ||
              kDebugVadEndTriggerMs == 2000,
              "VAD end trigger has no CI130X preset for this value");
static_assert(kDebugVadSensitivity >= 45 && kDebugVadSensitivity <= 60,
              "VAD sensitivity is 45-60");
static_assert(kDebugVadFilterFrames >= 15 && kDebugVadFilterFrames <= 40,
              "VAD filter frames is 15-40");
static_assert(kDebugVolume >= 1 && kDebugVolume <= 7, "CI130X volume is 1-7");

// Effective settings after applying the optional in-memory debug overlay.
MoinaiDeviceSettings GetMoinaiDeviceSettings();

#endif
