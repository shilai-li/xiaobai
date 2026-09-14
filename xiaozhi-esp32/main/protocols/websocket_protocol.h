#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstdint>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
#define WEBSOCKET_PROTOCOL_SEND_TASK_STOPPED_EVENT (1 << 1)
#define WEBSOCKET_PROTOCOL_AUTH_ERROR_EVENT (1 << 2)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool IsAuthenticationError() const override { return authentication_error_; }

private:
    EventGroupHandle_t event_group_handle_;
    QueueHandle_t deferred_send_queue_ = nullptr;
    TaskHandle_t deferred_send_task_handle_ = nullptr;
    std::unique_ptr<WebSocket> websocket_;
    bool expect_binary_audio_ = false;
    bool authentication_error_ = false;
    double pending_audio_timestamp_ = 0;
    int64_t last_audio_rx_us_ = 0;
    double last_audio_server_timestamp_ = 0;
    uint32_t audio_rx_count_ = 0;
    int64_t audio_rx_window_start_us_ = 0;
    double audio_rx_window_start_server_timestamp_ = 0;
    uint32_t audio_rx_window_frames_ = 0;
    uint32_t audio_rx_window_max_local_us_ = 0;
    uint32_t audio_rx_window_slow_intervals_ = 0;
    double audio_rx_window_max_server_ms_ = 0;
    double pipeline_asr_start_ms_ = 0;
    double pipeline_asr_done_ms_ = 0;
    double pipeline_llm_start_ms_ = 0;
    double pipeline_llm_done_ms_ = 0;
    double pipeline_tts_start_ms_ = 0;
    int version_ = 1;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    void HandleSocketIoText(const std::string& message);
    void ParseBotResponse(const cJSON* payload, bool is_binary_event);
    bool DeferTextSend(std::string message);
    void DeferredSendTask();
};

#endif
