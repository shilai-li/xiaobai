#ifndef _CI130X_PROTOCOL_H
#define _CI130X_PROTOCOL_H

#include <cstdint>

// CIAS V2.4 command identifiers. Multi-byte payloads use little-endian order.
#define CI_CMD_ASR_RESULT                 0x0101
#define CI_CMD_WAKEUP                     0x0102
#define CI_CMD_VAD_END                    0x0103
#define CI_CMD_SKIP_INVALID_SPEECH        0x0104
#define CI_CMD_PCM_MIDDLE                 0x0105
#define CI_CMD_PCM_FINISH                 0x0106
#define CI_CMD_PCM_IDLE                   0x0107
#define CI_CMD_VAD_START                  0x0108
#define CI_CMD_EXIT_WAKEUP                0x0109
#define CI_CMD_SET_VAD_SENSITIVITY        0x010A
#define CI_CMD_VAD_START_BY_KEY           0x010B
#define CI_CMD_VAD_END_BY_KEY             0x010C
#define CI_CMD_SET_AUDIO_EXIT_WAKEUP      0x010D
#define CI_CMD_SET_PCM_DENOISE            0x010E
#define CI_CMD_SET_VAD_FILTER_FRAMES      0x010F
#define CI_CMD_SET_VAD_END_SILENCE        0x0110
#define CI_CMD_SET_VAD_START_MAX_TIMEOUT  0x0111
#define CI_CMD_SET_PLAY_VOICE_ID          0x0112
#define CI_CMD_SET_WAKEUP_CONTINUE_TIME   0x0113
#define CI_CMD_ENTER_WAKEUP               0x0114
#define CI_CMD_SET_MULTI_ROUND            0x0115
#define CI_CMD_SET_FULL_DUPLEX            0x0116
#define CI_CMD_SET_VOLUME                 0x0117
#define CI_CMD_SET_MUTE                   0x0119
#define CI_CMD_START_RECORDING            0x011A
#define CI_CMD_STOP_RECORDING             0x011B
#define CI_CMD_SET_CLOUD_ANS_TIMEOUT_EXIT 0x011C
#define CI_CMD_FORCE_VAD_END              0x011D
#define CI_CMD_SET_SWM221_SLEEP_TIMEOUT   0x011E
#define CI_CMD_SET_TOPIC_PLAYBACK         0x011F

#define CI_CMD_PLAY_START                 0x0201
#define CI_CMD_PLAY_STOP                  0x0204
#define CI_CMD_PLAY_GET                   0x020A
#define CI_CMD_PLAY_DATA                  0x020B
#define CI_CMD_PLAY_END                   0x020C
#define CI_CMD_PLAY_STOP_EVT              0x020D
#define CI_CMD_SET_VAD_STOP_PLAY          0x0216
#define CI_CMD_LOCAL_PLAY_START           0x0217
#define CI_CMD_LOCAL_PLAY_STOP            0x0218

#define CI_CMD_EXEC_STATE                 0x0801
#define CI_CMD_STATUS_NOTIFY              0x4002

enum Ci130xStatus : uint16_t {
    CI_STATUS_REGISTERING = 2220,
    CI_STATUS_REGISTER_SUCCESS = 2221,
    CI_STATUS_REGISTER_FAILED = 2222,
    CI_STATUS_ACTIVATING = 2223,
    CI_STATUS_ACTIVATE_SUCCESS = 2224,
    CI_STATUS_ACTIVATE_FAILED = 2225,
    CI_STATUS_CONNECTING = 2226,
    CI_STATUS_CONNECT_SUCCESS = 2227,
    CI_STATUS_CONNECT_FAILED = 2228,
};

// Registration and final connection statuses are reported for display but
// stay silent because their prompts only delay the greeting. The CI130X obeys
// the audible flag in the 0x4002 payload; the ESP32 also needs the answer
// locally to know how many startup prompts to wait for.
inline bool Ci130xStatusIsAudible(uint16_t status_id) {
    return status_id != CI_STATUS_REGISTERING &&
           status_id != CI_STATUS_REGISTER_SUCCESS &&
           status_id != CI_STATUS_CONNECT_SUCCESS;
}

// Names retained for the current backend codec implementation.
#define CI_CMD_PLAY_DATA_END              CI_CMD_PLAY_END
#define CI_CMD_SET_ENTER_WAKE_UP          CI_CMD_ENTER_WAKEUP

// CI130X firmware defaults used by this product build.
#define CI130X_DEFAULT_VAD_SENSITIVITY  49
#define CI130X_DEFAULT_VAD_FILTER_FRAMES  40
#define CI130X_DEFAULT_VAD_END_SILENCE    30
#define CI130X_DEFAULT_VAD_END_TRIGGER_MS 700
// Bounded by the local-command gate, which holds the whole segment in RAM.
// kMaximumVadStartTimeoutSeconds is the ceiling; see moinai_device_settings.h.
#define CI130X_DEFAULT_VAD_START_MAX_TIMEOUT 5
#define CI130X_DEFAULT_WAKE_UP_CONTINUE_TIME 30
#define CI130X_DEFAULT_SWM221_SLEEP_TIMEOUT 300
#define CI130X_DEFAULT_PCM_DENOISE_ENABLED true
#define CI130X_DEFAULT_VOLUME 7
#define CI130X_DEFAULT_MUTED false
#define CI130X_DEFAULT_MULTI_ROUND true
#define CI130X_DEFAULT_FULL_DUPLEX false
// Only the wake word may interrupt a cloud response.
// The ESP32 state machine deliberately ignores VAD while speaking, so the
// CI130X must not stop playback on VAD start either; otherwise it emits
// PLAY_STOP and restarts the playback session under a still-streaming
// response, which produces gaps and repeated audio.
#define CI130X_DEFAULT_VAD_STOP_PLAY false

static const uint8_t CIAS_HEADER[] = {0xA5, 0xA5, 0x5A, 0x5A};

#endif // _CI130X_PROTOCOL_H
