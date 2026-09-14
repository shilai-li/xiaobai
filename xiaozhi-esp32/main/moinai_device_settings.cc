#include "moinai_device_settings.h"

#include "codecs/ci130x_protocol.h"
#include "settings.h"

#include <algorithm>

namespace {

// What the topic poll loop used before the settings moved into this module.
constexpr int kDefaultTopicPollIntervalSeconds = 300;

// The clamp below would hide a compiled default that outgrew the ceiling,
// leaving the two constants silently disagreeing about the same limit.
static_assert(CI130X_DEFAULT_VAD_START_MAX_TIMEOUT <= kMaximumVadStartTimeoutSeconds,
              "Default VAD timeout exceeds the local-command audio buffer");

}  // namespace

MoinaiDeviceSettings GetMoinaiDeviceSettings() {
    Settings cloud_settings("moinai", false);

    MoinaiDeviceSettings settings;
    settings.debug_override_enabled = MOINAI_DEBUG_SETTINGS_OVERRIDE;
    const int poll_interval_seconds = cloud_settings.GetInt(
        "poll_interval", kDefaultTopicPollIntervalSeconds);
    settings.topic_poll_interval_seconds = std::max(
        poll_interval_seconds, kMinimumTopicPollIntervalSeconds);
    settings.wakeup_time_seconds = cloud_settings.GetInt(
        "wakeup_time", CI130X_DEFAULT_WAKE_UP_CONTINUE_TIME);
    settings.standby_time_seconds = cloud_settings.GetInt(
        "sleep_time", CI130X_DEFAULT_SWM221_SLEEP_TIMEOUT);
    settings.vad_end_trigger_ms = cloud_settings.GetInt(
        "vad_end_ms", CI130X_DEFAULT_VAD_END_TRIGGER_MS);
    settings.vad_sensitivity = cloud_settings.GetInt(
        "vad_sens", CI130X_DEFAULT_VAD_SENSITIVITY);
    settings.vad_filter_frames = cloud_settings.GetInt(
        "vad_filter", CI130X_DEFAULT_VAD_FILTER_FRAMES);
    const int vad_max_timeout_seconds = cloud_settings.GetInt(
        "vad_max_time", CI130X_DEFAULT_VAD_START_MAX_TIMEOUT);
    settings.vad_start_max_timeout_seconds = std::clamp(
        vad_max_timeout_seconds, 1, kMaximumVadStartTimeoutSeconds);
    settings.denoise_enabled = cloud_settings.GetBool(
        "denoise", CI130X_DEFAULT_PCM_DENOISE_ENABLED);
    settings.volume = cloud_settings.GetInt("ci_volume", CI130X_DEFAULT_VOLUME);
    settings.muted = cloud_settings.GetBool("muted", CI130X_DEFAULT_MUTED);
    settings.multi_round = cloud_settings.GetBool(
        "multi_round", CI130X_DEFAULT_MULTI_ROUND);
    settings.full_duplex = cloud_settings.GetBool(
        "full_duplex", CI130X_DEFAULT_FULL_DUPLEX);
    settings.vad_stop_playback = cloud_settings.GetBool(
        "vad_stop_play", CI130X_DEFAULT_VAD_STOP_PLAY);
#if MOINAI_DEBUG_SETTINGS_OVERRIDE
    settings.topic_poll_interval_seconds = kDebugTopicPollIntervalSeconds;
    settings.wakeup_time_seconds = kDebugWakeupTimeSeconds;
    settings.standby_time_seconds = kDebugStandbyTimeSeconds;
    settings.vad_end_trigger_ms = kDebugVadEndTriggerMs;
    settings.vad_sensitivity = kDebugVadSensitivity;
    settings.vad_filter_frames = kDebugVadFilterFrames;
    settings.vad_start_max_timeout_seconds = kDebugVadStartMaxTimeoutSeconds;
    settings.denoise_enabled = kDebugDenoiseEnabled;
    settings.volume = kDebugVolume;
    settings.muted = kDebugMuted;
    settings.multi_round = kDebugMultiRound;
    settings.full_duplex = kDebugFullDuplex;
    settings.vad_stop_playback = kDebugVadStopPlayback;
#endif
    return settings;
}
