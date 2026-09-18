#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_err.h>
#include "board.h"

class Ota {
public:
    // Component-specific business error. Keep this distinct from
    // ESP_ERR_INVALID_STATE, which is also used for local clock/configuration
    // failures and must never be mistaken for a shipment revocation.
    static constexpr esp_err_t kErrNotReadyShipment =
        static_cast<esp_err_t>(0x7F01);
    static constexpr esp_err_t kErrUnauthorized =
        static_cast<esp_err_t>(0x7F02);
    static constexpr esp_err_t kErrAlreadyActivated =
        static_cast<esp_err_t>(0x7F03);
    static constexpr esp_err_t kErrDeviceNotActivated =
        static_cast<esp_err_t>(0x7F04);

    struct DeviceTopic {
        std::string id;
        std::string text;
    };
    Ota();
    ~Ota();

    esp_err_t CheckVersion();
    esp_err_t Activate();
    esp_err_t RefreshMoinaiToken();
    esp_err_t ValidateCachedMoinaiToken();
    esp_err_t FetchDeviceSettings();
    esp_err_t FetchTopics(std::vector<DeviceTopic>& topics);
    esp_err_t ReportCurrentLocation();
    void ClearMoinaiCredentials();
    void PrepareForReactivation();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    bool IsUsingCachedMoinaiToken() const { return token_loaded_from_cache_; }
    bool ShouldRefreshMoinaiToken() const;
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    std::string GetCheckVersionUrl();

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    // Cached Moinai access token, reused across activation retries so a failing
    // (e.g. server-side 500) activation loop does not re-run the auth handshake
    // every time. Cleared when the server rejects it (401) to force re-auth.
    std::string moinai_token_;
    int64_t moinai_token_expires_at_ms_ = 0;
    int64_t last_token_refresh_attempt_ms_ = 0;
    bool token_loaded_from_cache_ = false;
    int activation_timeout_ms_ = 30000;

    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    esp_err_t FetchFirmwareMetadata();
    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp(const std::string& token = "");
    bool SyncTimeFromHttpDate();
    esp_err_t GetMoinaiToken(std::string& token_out, int64_t& expires_at_ms_out);
};

#endif // _OTA_H
