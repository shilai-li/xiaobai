#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "moinai_device_settings.h"
#include "ota_feature_config.h"
#include "ota_image_writer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif
#include <esp_random.h>
#include <mbedtls/md.h>
#include <sys/time.h>

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <initializer_list>

#define TAG "Ota"

// Helper function for HMAC-MD5 signature
static std::string HmacMd5(const std::string& key, const std::string& message) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info) {
        ESP_LOGE("HMAC", "MD5 not supported in mbedtls");
        mbedtls_md_free(&ctx);
        return "";
    }
    mbedtls_md_setup(&ctx, info, 1); // 1 for HMAC
    mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.data(), key.size());
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)message.data(), message.size());
    unsigned char out[16];
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    
    char hex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(hex + i * 2, "%02x", out[i]);
    }
    return std::string(hex, 32);
}

// Parse an RFC 1123 HTTP Date header (e.g. "Mon, 14 Jul 2026 08:20:00 GMT")
// into a UTC epoch. Returns 0 on failure. Avoids timegm()/TZ dependency by
// computing the epoch directly with the days-from-civil algorithm.
static time_t ParseHttpDate(const std::string& date) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // Skip the leading weekday name and comma, if present.
    const char* p = strchr(date.c_str(), ',');
    p = (p != nullptr) ? p + 1 : date.c_str();

    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char month_str[4] = {0};
    if (sscanf(p, " %d %3s %d %d:%d:%d", &day, month_str, &year, &hour, &minute, &second) != 6) {
        return 0;
    }

    int month = 0;
    for (int i = 0; i < 12; ++i) {
        if (strncmp(month_str, kMonths[i], 3) == 0) { month = i + 1; break; }
    }
    if (month == 0 || year < 2023) {
        return 0;
    }

    // Howard Hinnant's days-from-civil algorithm (proleptic Gregorian, UTC).
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (time_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}

// Normalize the backend expiresAt value to Unix milliseconds. Accept numeric
// seconds/milliseconds and ISO-8601 UTC/offset strings so firmware does not
// depend on one JSON representation of the same timestamp.
static int64_t NormalizeUnixTimestampMs(int64_t value) {
    constexpr int64_t kMillisecondsThreshold = 100000000000LL;
    return value > 0 && value < kMillisecondsThreshold ? value * 1000 : value;
}

static int64_t ParseIso8601TimestampMs(const std::string& value) {
    if (value.size() < 19 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != ' ') || value[13] != ':' || value[16] != ':') {
        return 0;
    }

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
               &year, &month, &day, &hour, &minute, &second) != 6 &&
        sscanf(value.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
               &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }
    if (year < 2023 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return 0;
    }

    size_t suffix = 19;
    int milliseconds = 0;
    if (suffix < value.size() && value[suffix] == '.') {
        ++suffix;
        int digits = 0;
        while (suffix < value.size() && value[suffix] >= '0' && value[suffix] <= '9') {
            if (digits < 3) {
                milliseconds = milliseconds * 10 + (value[suffix] - '0');
            }
            ++digits;
            ++suffix;
        }
        while (digits < 3) {
            milliseconds *= 10;
            ++digits;
        }
    }

    int timezone_offset_seconds = 0;
    if (suffix < value.size() && value[suffix] != 'Z' && value[suffix] != 'z') {
        const char sign = value[suffix];
        if (sign != '+' && sign != '-') {
            return 0;
        }
        int offset_hour = 0, offset_minute = 0;
        if (sscanf(value.c_str() + suffix + 1, "%2d:%2d", &offset_hour, &offset_minute) < 1 ||
            offset_hour > 23 || offset_minute > 59) {
            return 0;
        }
        timezone_offset_seconds = (offset_hour * 60 + offset_minute) * 60;
        if (sign == '-') {
            timezone_offset_seconds = -timezone_offset_seconds;
        }
    }

    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    return (days * 86400 + hour * 3600 + minute * 60 + second - timezone_offset_seconds) * 1000 +
           milliseconds;
}

static int64_t ParseTokenExpiresAtMs(const cJSON* item) {
    if (cJSON_IsNumber(item)) {
        return NormalizeUnixTimestampMs(static_cast<int64_t>(item->valuedouble));
    }
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return 0;
    }

    const std::string value(item->valuestring);
    errno = 0;
    char* end = nullptr;
    long long numeric = strtoll(value.c_str(), &end, 10);
    if (errno == 0 && end != value.c_str() && *end == '\0') {
        return NormalizeUnixTimestampMs(numeric);
    }
    return ParseIso8601TimestampMs(value);
}

static std::string GetMoinaiBusinessErrorCode(const std::string& response) {
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        return {};
    }

    std::string code;
    cJSON* code_item = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsString(code_item) && code_item->valuestring != nullptr) {
        code = code_item->valuestring;
    }
    cJSON_Delete(root);
    return code;
}

static std::string MaskMoinaiToken(const std::string& token) {
    if (token.empty()) {
        return "<empty>";
    }
    constexpr size_t kVisibleChars = 6;
    if (token.size() <= kVisibleChars * 2) {
        return std::string(token.size(), '*');
    }
    return token.substr(0, kVisibleChars) + "..." +
           token.substr(token.size() - kVisibleChars);
}

static esp_err_t MapMoinaiHttpError(int status, const std::string& response) {
    const std::string code = GetMoinaiBusinessErrorCode(response);
    esp_err_t mapped;
    if (code == "NOT_READY_SHIPMENT") {
        mapped = Ota::kErrNotReadyShipment;
    } else if (code == "ALREADY_ACTIVATED") {
        mapped = Ota::kErrAlreadyActivated;
    } else if (code == "DEVICE_NOT_ACTIVATED") {
        mapped = Ota::kErrDeviceNotActivated;
    } else if (status == 401 || status == 403) {
        // 403 is included because Moinai returns it for an expired token;
        // mapping it to kErrUnauthorized lets the caller trigger a refresh.
        mapped = Ota::kErrUnauthorized;
    } else {
        mapped = ESP_FAIL;
    }
    // Trace every mapping decision: which HTTP status, which business code the
    // server embedded, and what the caller will act on. kErrUnauthorized here
    // is the trigger for the token-refresh path in Application.
    ESP_LOGW(TAG, "Moinai HTTP error mapped: status=%d, business_code='%s', "
                  "esp_err=0x%x, action=%s",
             status, code.c_str(), mapped,
             mapped == Ota::kErrUnauthorized ? "refresh token" : "no token refresh");
    return mapped;
}

Ota::Ota() {
    // The production command stores the device SN in NVS. Read it first so
    // that a programmed value takes effect without rebuilding the firmware.
    Settings settings("wifi", false);
    serial_number_ = settings.GetString("serial_number");
    has_serial_number_ = !serial_number_.empty();

    // Fall back to the configured test serial number when NVS is empty.
#ifdef CONFIG_MOINAI_SERIAL_NUMBER
    if (!has_serial_number_) {
        serial_number_ = CONFIG_MOINAI_SERIAL_NUMBER;
        has_serial_number_ = !serial_number_.empty();
    }
#endif

    // Fallback to EFUSE if still empty.
    if (!has_serial_number_) {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
        // Read Serial Number from efuse user_data
        uint8_t efuse_sn[33] = {0};
        if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, efuse_sn, 32 * 8) == ESP_OK) {
            if (efuse_sn[0] != 0) {
                serial_number_ = std::string(reinterpret_cast<char*>(efuse_sn), 32);
                has_serial_number_ = true;
            }
        }
#endif
    }

    // Trim trailing spaces or null chars
    while (!serial_number_.empty() && (serial_number_.back() == '\0' || serial_number_.back() == ' ')) {
        serial_number_.pop_back();
    }
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp(const std::string& token) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");
    if (!token.empty()) {
        http->SetHeader("Authorization", ("Bearer " + token).c_str());
    }

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::GetMoinaiToken(std::string& token_out, int64_t& expires_at_ms_out) {
    std::string device_id = serial_number_;
    std::string access_key_id = CONFIG_MOINAI_ACCESS_KEY_ID;
    std::string access_key_secret = CONFIG_MOINAI_ACCESS_KEY_SECRET;
    
    std::string base_url = GetCheckVersionUrl();
    if (base_url.back() == '/') {
        base_url.pop_back();
    }
    std::string auth_url = base_url + "/api/v1/embeded/auth/device/" + device_id;
    ESP_LOGI(TAG, "Auth request to: %s", auth_url.c_str());

    // The auth API only accepts timestamps within 15 minutes of *server* time.
    // The device clock (SNTP) can be unreachable or wrong behind proxy networks,
    // and the server clock is not guaranteed to be true UTC, so if the first
    // attempt is rejected as an expired/invalid timestamp we re-sync the local
    // clock to the server's Date response header and sign again.
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Generate nonce
        char nonce_buf[33];
        snprintf(nonce_buf, sizeof(nonce_buf), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
        std::string nonce(nonce_buf);

        // Generate timestamp
        struct timeval tv;
        gettimeofday(&tv, NULL);
        long long time_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        std::string timestamp = std::to_string(time_ms);

        // Print the actual wall-clock value used for signing so cellular builds
        // can be diagnosed separately from the UART uptime counter.
        struct tm utc_tm = {};
        char utc_text[32] = "unknown";
        const bool clock_converted = gmtime_r(&tv.tv_sec, &utc_tm) != nullptr;
        if (clock_converted) {
            strftime(utc_text, sizeof(utc_text), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
        }
        // Pass the millisecond value as a string. This avoids 64-bit varargs
        // formatting issues on the ESP32-C3 newlib printf implementation.
        ESP_LOGI(TAG, "Auth clock: unix_seconds=%ld, unix_millis=%s, utc=%s",
                 (long)tv.tv_sec, timestamp.c_str(), utc_text);
        const int utc_year = utc_tm.tm_year + 1900;
        if (!clock_converted || utc_year < 2023 || utc_year > 2037) {
            ESP_LOGE(TAG, "Auth clock is not synchronized or plausible; refusing to sign the request");
            return ESP_ERR_INVALID_STATE;
        }

        std::string version = "V1";

        // Build canonical string
        std::string canonical = "accesskeyid=" + access_key_id +
                                "&accessnonce=" + nonce +
                                "&accesstimestamp=" + timestamp +
                                "&accessversion=" + version +
                                "&deviceid=" + device_id;

        // Compute HMAC-MD5 signature
        std::string signature = HmacMd5(access_key_secret, canonical);

        // Construct JSON payload
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "deviceId", device_id.c_str());
        cJSON_AddStringToObject(root, "accessKeyId", access_key_id.c_str());
        cJSON_AddStringToObject(root, "accessTimestamp", timestamp.c_str());
        cJSON_AddStringToObject(root, "accessNonce", nonce.c_str());
        cJSON_AddStringToObject(root, "accessVersion", version.c_str());
        cJSON_AddStringToObject(root, "accessSign", signature.c_str());

        char* json_str = cJSON_PrintUnformatted(root);
        std::string payload(json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);

        ESP_LOGI(TAG, "Requesting Moinai Token from: %s", auth_url.c_str());

        auto http = SetupHttp();
        http->SetContent(std::move(payload));

        if (!http->Open("POST", auth_url)) {
            ESP_LOGE(TAG, "Failed to connect to Moinai Auth server");
            return ESP_FAIL;
        }

        int status = http->GetStatusCode();
        std::string server_date = http->GetResponseHeader("Date");
        std::string response = http->ReadAll();
        http->Close();

        // The current auth endpoint returns 200 OK. Accept any 2xx response for
        // compatibility with equivalent successful HTTP status codes.
        if (status >= 200 && status < 300) {
            cJSON* res_root = cJSON_Parse(response.c_str());
            if (!res_root) {
                ESP_LOGE(TAG, "Failed to parse Auth response JSON");
                return ESP_FAIL;
            }
            cJSON* access_token = cJSON_GetObjectItem(res_root, "accessToken");
            cJSON* expires_at = cJSON_GetObjectItem(res_root, "expiresAt");
            if (cJSON_IsString(access_token)) {
                token_out = access_token->valuestring;
            }
            expires_at_ms_out = ParseTokenExpiresAtMs(expires_at);
            cJSON_Delete(res_root);
            if (token_out.empty() || expires_at_ms_out <= 0) {
                ESP_LOGE(TAG, "Auth response is missing a valid accessToken or expiresAt");
                token_out.clear();
                expires_at_ms_out = 0;
                return ESP_ERR_INVALID_RESPONSE;
            }
            return ESP_OK;
        }

        ESP_LOGE(TAG, "Auth failed with status %d: %s", status, response.c_str());

        const std::string business_error = GetMoinaiBusinessErrorCode(response);
        if (business_error == "NOT_READY_SHIPMENT") {
            return kErrNotReadyShipment;
        }
        const bool invalid_timestamp = business_error == "INVALID_TIMESTAMP";

        // The server rejected our timestamp. Align the local clock to the
        // server's own Date header and try once more before giving up.
        if (attempt == 0 && invalid_timestamp && !server_date.empty()) {
            time_t server_epoch = ParseHttpDate(server_date);
            if (server_epoch > 0) {
                struct timeval server_tv = { server_epoch, 0 };
                settimeofday(&server_tv, nullptr);
                ESP_LOGW(TAG, "Re-synced clock to server Date '%s' (epoch=%lld); retrying auth",
                         server_date.c_str(), (long long)server_epoch);
                continue;
            }
            ESP_LOGW(TAG, "Could not parse server Date header: '%s'", server_date.c_str());
        }
        return ESP_FAIL;
    }
    return ESP_FAIL;
}

esp_err_t Ota::CheckVersion() {
    auto app_desc = esp_app_get_description();

    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    Settings settings("websocket", true);

    // 1. A persisted token can authenticate Socket.IO without a locally valid
    // clock. Use it immediately and defer time/expiry validation until after
    // bot_ready. A missing cache still requires time sync and signed auth.
    if (moinai_token_.empty()) {
        has_websocket_config_ = false;

        const std::string cached_token = settings.GetString("token");
        const std::string cached_expiry = settings.GetString("token_expiry");
        int64_t cached_expires_at_ms = 0;
        if (!cached_expiry.empty()) {
            errno = 0;
            char* end = nullptr;
            cached_expires_at_ms = strtoll(cached_expiry.c_str(), &end, 10);
            if (errno != 0 || end == cached_expiry.c_str() || *end != '\0') {
                cached_expires_at_ms = 0;
            }
        }

        // Remove the placeholder credential left by the earlier token-cache test.
        // It is not a server-issued token and will always be rejected by protected
        // device APIs. This one-time migration preserves all unrelated NVS values.
        const bool is_legacy_test_token = cached_token == "test-token";
        if (is_legacy_test_token) {
            ESP_LOGW(TAG, "Discarding legacy test Moinai token and requesting a real token");
            settings.EraseKey("token");
            settings.EraseKey("token_expiry");
            settings.EraseKey("url");
            settings.SetBool("reactivate", true);
        }

        if (!is_legacy_test_token && !cached_token.empty() && cached_expires_at_ms > 0) {
            moinai_token_ = cached_token;
            moinai_token_expires_at_ms_ = cached_expires_at_ms;
            token_loaded_from_cache_ = true;
            const std::string masked_token = MaskMoinaiToken(moinai_token_);
            ESP_LOGI(TAG,
                     "Using cached Moinai token: %s (length=%u); expiry validation deferred until ready",
                     masked_token.c_str(), static_cast<unsigned int>(moinai_token_.size()));
#if CONFIG_MOINAI_LOG_TOKEN
            ESP_LOGW(TAG, "TEST ONLY - full cached Moinai token: %s", moinai_token_.c_str());
#endif
        } else {
            esp_err_t err = RefreshMoinaiToken();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get Moinai token: %d", err);
                settings.EraseKey("token");
                settings.EraseKey("token_expiry");
                settings.EraseKey("url");
                return err;
            }
        }
    } else {
        const std::string masked_token = MaskMoinaiToken(moinai_token_);
        ESP_LOGI(TAG, "Reusing current Moinai token: %s (length=%u)",
                 masked_token.c_str(), static_cast<unsigned int>(moinai_token_.size()));
#if CONFIG_MOINAI_LOG_TOKEN
        ESP_LOGW(TAG, "TEST ONLY - full current Moinai token: %s", moinai_token_.c_str());
#endif
    }

    // 2. Store token for activation / websocket to read.
    settings.SetString("token", moinai_token_);
    settings.SetString("url", "wss://robotic-test.moinai.com/api/v1/bot");

    // 3. Call activation when explicitly enabled, or when the backend has told
    // us that its activation record was removed. The latter must work even on
    // production builds where normal first-boot activation is disabled.
    bool activation_required = settings.GetBool("reactivate", false);
#if CONFIG_MOINAI_DEVICE_ACTIVATION
    activation_required = true;
#endif
    if (activation_required) {
        esp_err_t err = Activate();
        if (err == kErrUnauthorized && token_loaded_from_cache_) {
            ESP_LOGW(TAG, "Activation rejected cached token; refreshing and retrying once");
            if (RefreshMoinaiToken() == ESP_OK) {
                err = Activate();
            }
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to activate device: %d. Cleaning up config.", err);
            // If the server rejected the token itself (401), drop the cached token so
            // the next retry re-authenticates. Other failures (e.g. 500) keep the
            // token so retries only repeat the activation call.
            if (err == kErrUnauthorized) {
                ESP_LOGW(TAG, "Activation token rejected; clearing cached token to force re-auth.");
                moinai_token_.clear();
                moinai_token_expires_at_ms_ = 0;
                token_loaded_from_cache_ = false;
                settings.EraseKey("token");
                settings.EraseKey("token_expiry");
                settings.EraseKey("url");
            }
            has_websocket_config_ = false;
            return err;
        }
        settings.EraseKey("reactivate");
    } else {
        ESP_LOGI(TAG, "Device activation request skipped (already activated)");
    }

    // 4. OTA discovery is optional. A failed update server must not prevent the
    // device from starting its normal WebSocket service.
#if CLOUD_OTA_ENABLED
    esp_err_t firmware_err = FetchFirmwareMetadata();
    if (firmware_err == kErrUnauthorized) {
        ESP_LOGW(TAG, "Cloud OTA check rejected the token; refreshing and retrying once");
        if (RefreshMoinaiToken() == ESP_OK) {
            firmware_err = FetchFirmwareMetadata();
        }
    }
    if (firmware_err != ESP_OK) {
        ESP_LOGW(TAG, "Cloud OTA metadata check failed (%d); continuing startup",
                 firmware_err);
    }
#else
    has_new_version_ = false;
    firmware_version_.clear();
    firmware_url_.clear();
    ESP_LOGI(TAG, "Cloud OTA is disabled at compile time");
#endif

    // 5. Authentication completed (and activation, when enabled), so WebSocket
    // configuration is ready for use.
    has_websocket_config_ = true;
    return ESP_OK;
}

esp_err_t Ota::FetchFirmwareMetadata() {
#if !CLOUD_OTA_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    has_new_version_ = false;
    firmware_version_.clear();
    firmware_url_.clear();

    if (!has_serial_number_ || moinai_token_.empty()) {
        ESP_LOGW(TAG, "Skip cloud OTA check: device identity is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    std::string base_url = GetCheckVersionUrl();
    if (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    const std::string metadata_url =
        base_url + CLOUD_OTA_METADATA_PATH + serial_number_;
    auto http = SetupHttp(moinai_token_);
    http->SetHeader("Current-Version", current_version_);
    http->SetHeader("Board-Type", BOARD_TYPE);
    if (!http->Open("GET", metadata_url)) {
        ESP_LOGW(TAG, "Failed to open cloud OTA metadata request");
        return ESP_FAIL;
    }

    const int status = http->GetStatusCode();
    const std::string response = http->ReadAll();
    http->Close();
    if (status == 204 || status == 404) {
        ESP_LOGI(TAG, "No cloud firmware update is published");
        return ESP_OK;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "Cloud OTA metadata request failed with status %d: %s",
                 status, response.c_str());
        return MapMoinaiHttpError(status, response);
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!cJSON_IsObject(root)) {
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        ESP_LOGW(TAG, "Cloud OTA metadata is not a JSON object");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* envelope = root;
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        envelope = data;
    }
    cJSON* metadata = envelope;
    cJSON* firmware = cJSON_GetObjectItemCaseSensitive(envelope, "firmware");
    if (cJSON_IsObject(firmware)) {
        metadata = firmware;
    }

    const auto find_item = [](cJSON* object,
                              std::initializer_list<const char*> names) -> cJSON* {
        for (const char* name : names) {
            cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
            if (item != nullptr) {
                return item;
            }
        }
        return nullptr;
    };
    const auto read_string = [&find_item](
                                 cJSON* object,
                                 std::initializer_list<const char*> names) {
        cJSON* item = find_item(object, names);
        return cJSON_IsString(item) && item->valuestring != nullptr
                   ? std::string(item->valuestring)
                   : std::string();
    };

    cJSON* available = find_item(
        envelope, {"updateAvailable", "update_available", "available"});
    if (available == nullptr && envelope != root) {
        available = find_item(
            root, {"updateAvailable", "update_available", "available"});
    }
    if (available == nullptr && metadata != envelope) {
        available = find_item(
            metadata, {"updateAvailable", "update_available", "available"});
    }
    if (cJSON_IsBool(available) && cJSON_IsFalse(available)) {
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Backend reports that firmware is current");
        return ESP_OK;
    }

    firmware_version_ = read_string(
        metadata, {"version", "firmwareVersion", "firmware_version"});
    firmware_url_ = read_string(
        metadata, {"url", "firmwareUrl", "firmware_url", "downloadUrl",
                   "download_url"});

    const bool backend_requires_update =
        cJSON_IsBool(available) && cJSON_IsTrue(available);
    const bool valid_url = firmware_url_.rfind("https://", 0) == 0 ||
                           firmware_url_.rfind("http://", 0) == 0;
    if (firmware_version_.empty() || !valid_url) {
        ESP_LOGW(TAG, "Cloud OTA metadata lacks a valid version or download URL");
        firmware_version_.clear();
        firmware_url_.clear();
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_new_version_ = backend_requires_update ||
        IsNewVersionAvailable(current_version_, firmware_version_);
    if (!has_new_version_) {
        firmware_version_.clear();
        firmware_url_.clear();
        ESP_LOGI(TAG, "Firmware is current");
    } else {
        ESP_LOGI(TAG, "Cloud firmware %s is available",
                 firmware_version_.c_str());
    }
    cJSON_Delete(root);
    return ESP_OK;
#endif
}

// Fall back to the server's HTTP Date header when the modem clock (CCLK) is
// unavailable. Any response — including 401 — carries a Date header, so this
// works regardless of token validity. Returns true if the local clock was set.
bool Ota::SyncTimeFromHttpDate() {
    std::string base_url = GetCheckVersionUrl();
    if (base_url.back() == '/') {
        base_url.pop_back();
    }
    std::string settings_url = base_url + "/api/v1/embeded/device/settings/" + serial_number_;
    ESP_LOGI(TAG, "Clock sync via HTTP Date from: %s", settings_url.c_str());
    struct timeval before = {};
    gettimeofday(&before, nullptr);
    auto http = SetupHttp(moinai_token_);
    if (!http->Open("GET", settings_url)) {
        ESP_LOGW(TAG, "Clock sync via HTTP Date failed: could not reach server");
        return false;
    }
    // Open() returns as soon as AT+MHTTPREQUEST is accepted; response headers
    // arrive later via the +MHTTPURC "header" URC. GetStatusCode() blocks until
    // then (or times out with -1). Any status — including 401/403 — carries a
    // Date header, so proceed regardless of the status value.
    const int status = http->GetStatusCode();
    if (status < 0) {
        ESP_LOGW(TAG, "Clock sync via HTTP Date failed: no response headers received (status=%d)",
                 status);
        http->Close();
        return false;
    }
    ESP_LOGI(TAG, "Clock sync response received: status=%d", status);
    const std::string server_date = http->GetResponseHeader("Date");
    http->Close();
    if (server_date.empty()) {
        ESP_LOGW(TAG, "Clock sync via HTTP Date failed: response (status=%d) has no Date header",
                 status);
        return false;
    }

    const time_t server_epoch = ParseHttpDate(server_date);
    if (server_epoch <= 0) {
        ESP_LOGW(TAG, "Clock sync via HTTP Date failed: unparsable Date header '%s' (status=%d)",
                 server_date.c_str(), status);
        return false;
    }
    struct timeval server_tv = { server_epoch, 0 };
    settimeofday(&server_tv, nullptr);
    struct tm utc_tm = {};
    char utc_text[32] = "unknown";
    if (gmtime_r(&server_epoch, &utc_tm) != nullptr) {
        strftime(utc_text, sizeof(utc_text), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
    }
    const int64_t clock_adjust_s = static_cast<int64_t>(server_epoch) - before.tv_sec;
    ESP_LOGI(TAG, "System time synchronized from HTTP Date header: status=%d, date='%s', "
                  "utc=%s, epoch=%lld, clock_adjust=%llds",
             status, server_date.c_str(), utc_text,
             static_cast<long long>(server_epoch),
             static_cast<long long>(clock_adjust_s));
    return true;
}

esp_err_t Ota::RefreshMoinaiToken() {
    // Cached-token startup intentionally skips clock sync. Any path that really
    // needs to sign an auth request (cache miss or 401 recovery) must restore a
    // plausible wall clock first.
    struct timeval refresh_time = {};
    gettimeofday(&refresh_time, nullptr);
    struct tm utc_tm = {};
    const bool clock_valid = gmtime_r(&refresh_time.tv_sec, &utc_tm) != nullptr &&
                             utc_tm.tm_year + 1900 >= 2023 && utc_tm.tm_year + 1900 <= 2037;
    if (!clock_valid) {
        if (!SyncTimeFromHttpDate()) {
            ESP_LOGE(TAG, "Unable to synchronize system time before Moinai token refresh");
            return ESP_ERR_TIMEOUT;
        }
        gettimeofday(&refresh_time, nullptr);
    }
    last_token_refresh_attempt_ms_ = static_cast<int64_t>(refresh_time.tv_sec) * 1000 +
                                     refresh_time.tv_usec / 1000;

    std::string token;
    int64_t expires_at_ms = 0;
    esp_err_t err = GetMoinaiToken(token, expires_at_ms);
    if (err != ESP_OK) {
        return err;
    }

    moinai_token_ = std::move(token);
    moinai_token_expires_at_ms_ = expires_at_ms;
    token_loaded_from_cache_ = false;

    Settings settings("websocket", true);
    settings.SetString("token", moinai_token_);
    settings.SetString("token_expiry", std::to_string(moinai_token_expires_at_ms_));
    settings.SetString("url", "wss://robotic-test.moinai.com/api/v1/bot");
    const std::string expires_at_text = std::to_string(moinai_token_expires_at_ms_);
    const std::string masked_token = MaskMoinaiToken(moinai_token_);
    ESP_LOGI(TAG, "Stored refreshed Moinai token: %s (length=%u); expires_at_ms=%s",
             masked_token.c_str(), static_cast<unsigned int>(moinai_token_.size()),
             expires_at_text.c_str());
#if CONFIG_MOINAI_LOG_TOKEN
    ESP_LOGW(TAG, "TEST ONLY - full refreshed Moinai token: %s", moinai_token_.c_str());
#endif
    return ESP_OK;
}

esp_err_t Ota::ValidateCachedMoinaiToken() {
    if (!token_loaded_from_cache_) {
        return ESP_OK;
    }

    struct timeval now = {};
    gettimeofday(&now, nullptr);
    struct tm utc_tm = {};
    const bool clock_valid = gmtime_r(&now.tv_sec, &utc_tm) != nullptr &&
                             utc_tm.tm_year + 1900 >= 2023 && utc_tm.tm_year + 1900 <= 2037;
    if (!clock_valid) {
        if (!SyncTimeFromHttpDate()) {
            // Degrade to a warning instead of failing activation: with a
            // stale clock the expiry check below is merely optimistic (the
            // token looks far from expiry and is kept as-is), and the
            // subsequent device settings call only needs the Bearer token,
            // not a local timestamp signature. If the token has actually
            // expired, the server rejects it and the refresh path re-syncs
            // the clock from the server's Date header.
            ESP_LOGW(TAG, "Time sync failed; continuing token validation with unsynchronized clock");
        } else {
            gettimeofday(&now, nullptr);
        }
    }

    const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
    const std::string expires_in_seconds =
        std::to_string((moinai_token_expires_at_ms_ - now_ms) / 1000);
    ESP_LOGI(TAG, "Cached Moinai token validated; expires_in=%ss",
             expires_in_seconds.c_str());

    if (!ShouldRefreshMoinaiToken()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Cached Moinai token is within its one-hour refresh window");
    esp_err_t err = RefreshMoinaiToken();
    if (err != ESP_OK && moinai_token_expires_at_ms_ > now_ms) {
        ESP_LOGW(TAG, "Deferred token refresh failed (%d); keeping valid cached token", err);
        return ESP_OK;
    }
    return err;
}

bool Ota::ShouldRefreshMoinaiToken() const {
    if (moinai_token_.empty() || moinai_token_expires_at_ms_ <= 0) {
        return true;
    }
    struct timeval now = {};
    gettimeofday(&now, nullptr);
    const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
    constexpr int64_t kRefreshBeforeExpiryMs = 60LL * 60 * 1000;
    constexpr int64_t kMinimumRefreshIntervalMs = 5LL * 60 * 1000;
    if (last_token_refresh_attempt_ms_ > 0 &&
        now_ms - last_token_refresh_attempt_ms_ < kMinimumRefreshIntervalMs) {
        return false;
    }
    return moinai_token_expires_at_ms_ <= now_ms + kRefreshBeforeExpiryMs;
}

void Ota::ClearMoinaiCredentials() {
    moinai_token_.clear();
    moinai_token_expires_at_ms_ = 0;
    last_token_refresh_attempt_ms_ = 0;
    token_loaded_from_cache_ = false;
    has_websocket_config_ = false;

    Settings settings("websocket", true);
    settings.EraseKey("token");
    settings.EraseKey("token_expiry");
    settings.EraseKey("url");
    settings.EraseKey("activated");
    settings.EraseKey("activation_id");
    settings.EraseKey("reactivate");
    ESP_LOGW(TAG, "Cleared persisted Moinai credentials");
}

void Ota::PrepareForReactivation() {
    ClearMoinaiCredentials();
    Settings settings("websocket", true);
    settings.SetBool("reactivate", true);
    ESP_LOGW(TAG, "Marked device for forced reactivation");
}

esp_err_t Ota::FetchDeviceSettings() {
    if (moinai_token_.empty()) {
        ESP_LOGW(TAG, "Skip settings fetch: Moinai token is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    std::string base_url = GetCheckVersionUrl();
    if (base_url.back() == '/') {
        base_url.pop_back();
    }
    std::string settings_url = base_url + "/api/v1/embeded/device/settings/" + serial_number_;
    auto http = SetupHttp(moinai_token_);
    if (!http->Open("GET", settings_url)) {
        ESP_LOGW(TAG, "Failed to open settings request");
        return ESP_FAIL;
    }

    const int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGW(TAG, "Failed to fetch settings, status %d: %s", status, response.c_str());
        return MapMoinaiHttpError(status, response);
    }

    ESP_LOGI(TAG, "Settings fetched successfully: %s", response.c_str());
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Keep cloud state fresh even while the in-memory debug overlay is active,
    // so disabling it restores cloud values on the next boot.
    Settings cloud_settings("moinai", true);

    auto save_int = [](cJSON* parent, const char* json_key,
                       int min_value, int max_value,
                       Settings& settings, const char* nvs_key) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, json_key);
        if (item == nullptr) {
            return;
        }
        if (!cJSON_IsNumber(item) ||
            item->valuedouble < min_value ||
            item->valuedouble > max_value ||
            item->valuedouble != static_cast<double>(item->valueint)) {
            ESP_LOGW(TAG, "Ignoring invalid device setting: %s", json_key);
            return;
        }
        settings.SetInt(nvs_key, item->valueint);
        ESP_LOGI(TAG, "Saved device setting %s=%d", json_key, item->valueint);
    };

    auto save_bool = [](cJSON* parent, const char* json_key,
                        Settings& settings, const char* nvs_key) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, json_key);
        if (item == nullptr) {
            return;
        }
        if (!cJSON_IsBool(item)) {
            ESP_LOGW(TAG, "Ignoring invalid device setting: %s", json_key);
            return;
        }
        const bool value = cJSON_IsTrue(item);
        settings.SetBool(nvs_key, value);
        ESP_LOGI(TAG, "Saved device setting %s=%s",
                 json_key, value ? "true" : "false");
    };

    save_int(root, "topicPollInterval", 1, INT32_MAX,
             cloud_settings, "poll_interval");

    cJSON* wakeup_time = cJSON_GetObjectItemCaseSensitive(root, "wakeupTime");
    const char* wakeup_key = "wakeupTime";
    if (wakeup_time == nullptr) {
        wakeup_time = cJSON_GetObjectItemCaseSensitive(root, "wakeUpTime");
        wakeup_key = "wakeUpTime";
    }
    if (wakeup_time != nullptr) {
        if (cJSON_IsNumber(wakeup_time) &&
            wakeup_time->valuedouble >= 1 &&
            wakeup_time->valuedouble <= UINT16_MAX &&
            wakeup_time->valuedouble == static_cast<double>(wakeup_time->valueint)) {
            cloud_settings.SetInt("wakeup_time", wakeup_time->valueint);
            ESP_LOGI(TAG, "Saved device setting %s=%d",
                     wakeup_key, wakeup_time->valueint);
        } else {
            ESP_LOGW(TAG, "Ignoring invalid device setting: %s", wakeup_key);
        }
    }

    cJSON* sleep_time = cJSON_GetObjectItemCaseSensitive(root, "standbyTime");
    const char* sleep_key = "standbyTime";
    if (sleep_time == nullptr) {
        sleep_time = cJSON_GetObjectItemCaseSensitive(root, "sleepTime");
        sleep_key = "sleepTime";
    }
    if (sleep_time == nullptr) {
        sleep_time = cJSON_GetObjectItemCaseSensitive(root, "sleepTimeout");
        sleep_key = "sleepTimeout";
    }
    if (sleep_time != nullptr) {
        if (cJSON_IsNumber(sleep_time) &&
            sleep_time->valuedouble >= 1 &&
            sleep_time->valuedouble <= UINT16_MAX &&
            sleep_time->valuedouble == static_cast<double>(sleep_time->valueint)) {
            cloud_settings.SetInt("sleep_time", sleep_time->valueint);
            ESP_LOGI(TAG, "Saved device setting %s=%d",
                     sleep_key, sleep_time->valueint);
        } else {
            ESP_LOGW(TAG, "Ignoring invalid device setting: %s", sleep_key);
        }
    }

    cJSON* vad = cJSON_GetObjectItemCaseSensitive(root, "vad");
    if (vad != nullptr && !cJSON_IsObject(vad)) {
        ESP_LOGW(TAG, "Ignoring invalid device setting object: vad");
    } else if (vad != nullptr) {
        cJSON* end_trigger = cJSON_GetObjectItemCaseSensitive(vad, "endTrigger");
        if (end_trigger != nullptr) {
            const int value = end_trigger->valueint;
            const bool valid = cJSON_IsNumber(end_trigger) &&
                end_trigger->valuedouble == static_cast<double>(value) &&
                (value == 300 || value == 400 || value == 500 ||
                 value == 700 || value == 1000 || value == 1500 ||
                 value == 2000);
            if (valid) {
                cloud_settings.SetInt("vad_end_ms", value);
                ESP_LOGI(TAG, "Saved device setting vad.endTrigger=%d", value);
            } else {
                ESP_LOGW(TAG, "Ignoring invalid device setting: vad.endTrigger");
            }
        }
        save_int(vad, "sensitivity", 45, 60,
                 cloud_settings, "vad_sens");
        save_int(vad, "filterFrames", 15, 40,
                 cloud_settings, "vad_filter");
        // Validated against the protocol range only. The firmware ceiling is a
        // RAM limit, not a wire limit, so it is applied where the value is read
        // rather than dropping a backend setting the operator can still see.
        save_int(vad, "startMaxTimeout", 1, UINT16_MAX,
                 cloud_settings, "vad_max_time");
    }

    cJSON* audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    if (audio != nullptr && !cJSON_IsObject(audio)) {
        ESP_LOGW(TAG, "Ignoring invalid device setting object: audio");
    } else if (audio != nullptr) {
        save_bool(audio, "denoiseEnabled", cloud_settings, "denoise");
        save_int(audio, "volume", 1, 7, cloud_settings, "ci_volume");
        save_bool(audio, "muted", cloud_settings, "muted");
    }

    cJSON* conversation = cJSON_GetObjectItemCaseSensitive(root, "conversation");
    if (conversation != nullptr && !cJSON_IsObject(conversation)) {
        ESP_LOGW(TAG, "Ignoring invalid device setting object: conversation");
    } else if (conversation != nullptr) {
        save_bool(conversation, "multiRound", cloud_settings, "multi_round");
        save_bool(conversation, "fullDuplex", cloud_settings, "full_duplex");
    }

    cJSON* vad_playback = cJSON_GetObjectItemCaseSensitive(root, "vadPlayback");
    if (vad_playback != nullptr && !cJSON_IsObject(vad_playback)) {
        ESP_LOGW(TAG, "Ignoring invalid device setting object: vadPlayback");
    } else if (vad_playback != nullptr) {
        save_bool(vad_playback, "stopOnVadStart",
                  cloud_settings, "vad_stop_play");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
#if !CLOUD_OTA_ENABLED
    (void)firmware_url;
    (void)callback;
    ESP_LOGW(TAG, "Cloud OTA download is disabled at compile time");
    return false;
#else
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    // 应用 rollback 机制下，esp_ota_begin() 要求运行镜像已被确认（VALID），
    // 否则返回 ESP_ERR_OTA_ROLLBACK_INVALID_STATE。升级前必须先确认自身。
    // 确认后再开始写入还能保证：若下载中途断电，运行分区仍可正常引导，
    // 不会回滚到刚被擦除的旧分区（否则有变砖风险）。
    MarkCurrentVersionValid();
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        http->Close();
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        http->Close();
        return false;
    }

    OtaImageWriter writer(content_length);
    esp_err_t err = writer.Begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to prepare cloud OTA: %s", esp_err_to_name(err));
        http->Close();
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        http->Close();
        return false;
    }

    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (total_read < content_length) {
        const size_t remaining = content_length - total_read;
        const int ret = http->Read(buffer, std::min(PAGE_SIZE, remaining));
        if (ret <= 0) {
            ESP_LOGE(TAG, "Cloud OTA download ended after %u of %u bytes",
                     static_cast<unsigned int>(total_read),
                     static_cast<unsigned int>(content_length));
            http->Close();
            heap_caps_free(buffer);
            return false;
        }

        err = writer.Write(buffer, static_cast<size_t>(ret));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write cloud OTA image: %s",
                     esp_err_to_name(err));
            http->Close();
            heap_caps_free(buffer);
            return false;
        }

        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 ||
            total_read == content_length) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }
    }
    http->Close();
    heap_caps_free(buffer);

    err = writer.Finish();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cloud OTA image validation failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
#endif
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}


std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    size_t start = 0;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
        start = 1;
    }
    const size_t suffix = version.find_first_of("-+", start);
    std::stringstream ss(version.substr(start, suffix - start));
    std::string segment;

    while (std::getline(ss, segment, '.')) {
        if (segment.empty() ||
            !std::all_of(segment.begin(), segment.end(), [](unsigned char ch) {
                return ch >= '0' && ch <= '9';
            })) {
            return {};
        }
        errno = 0;
        char* end = nullptr;
        const long value = std::strtol(segment.c_str(), &end, 10);
        if (errno != 0 || end == segment.c_str() || *end != '\0' ||
            value < 0 || value > INT32_MAX) {
            return {};
        }
        versionNumbers.push_back(static_cast<int>(value));
    }

    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    if (current.empty() || newer.empty()) {
        ESP_LOGW(TAG, "Cannot compare firmware versions '%s' and '%s'",
                 currentVersion.c_str(), newVersion.c_str());
        return false;
    }

    const size_t width = std::max(current.size(), newer.size());
    for (size_t i = 0; i < width; ++i) {
        const int current_part = i < current.size() ? current[i] : 0;
        const int new_part = i < newer.size() ? newer[i] : 0;
        if (new_part > current_part) {
            return true;
        } else if (new_part < current_part) {
            return false;
        }
    }

    return false;
}

esp_err_t Ota::Activate() {
    Settings settings("websocket", true);
    std::string token = settings.GetString("token");
    if (token.empty()) {
        ESP_LOGE(TAG, "No token found for activation");
        return ESP_FAIL;
    }

    std::string base_url = GetCheckVersionUrl();
    if (base_url.back() == '/') {
        base_url.pop_back();
    }
    std::string act_url = base_url + "/api/v1/embeded/device/activation/" + serial_number_;

    ESP_LOGI(TAG, "Activating device: %s", act_url.c_str());

    auto http = SetupHttp(token);
    if (!http->Open("GET", act_url)) {
        ESP_LOGE(TAG, "Failed to open activation connection");
        return ESP_FAIL;
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Activation failed with status %d: %s", status, response.c_str());

        const esp_err_t err = MapMoinaiHttpError(status, response);
        if (err == kErrAlreadyActivated) {
            ESP_LOGI(TAG, "Device is already activated; treating activation as idempotent success");
            settings.SetBool("activated", true);
            return ESP_OK;
        }
        return err;
    }

    // Success!
    cJSON* root = cJSON_Parse(response.c_str());
    if (root) {
        cJSON* id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(id)) {
            ESP_LOGI(TAG, "Device activated, activation record ID: %s", id->valuestring);
            settings.SetString("activation_id", id->valuestring);
        }
        cJSON_Delete(root);
    }
    settings.SetBool("activated", true);

    return ESP_OK;
}

esp_err_t Ota::FetchTopics(std::vector<DeviceTopic>& topics) {
    topics.clear();
    if (moinai_token_.empty()) {
        ESP_LOGW(TAG, "Skip topic polling: Moinai token is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    std::string base_url = GetCheckVersionUrl();
    if (base_url.back() == '/') {
        base_url.pop_back();
    }
    std::string url = base_url + "/api/v1/embeded/device/topics/" + serial_number_;
    auto http = SetupHttp(moinai_token_);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open topic polling request");
        return ESP_FAIL;
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGW(TAG, "Topic polling failed with status %d: %s", status, response.c_str());
        return MapMoinaiHttpError(status, response);
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        ESP_LOGE(TAG, "Invalid topic response: %s", response.c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        cJSON* id = cJSON_GetObjectItem(item, "id");
        cJSON* text = cJSON_GetObjectItem(item, "text");
        if (cJSON_IsString(id) && cJSON_IsString(text) && text->valuestring[0] != '\0') {
            topics.push_back({id->valuestring, text->valuestring});
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Topic polling completed: %u topic(s)", static_cast<unsigned>(topics.size()));
    return ESP_OK;
}

esp_err_t Ota::ReportCurrentLocation() {
    if (moinai_token_.empty()) {
        ESP_LOGW(TAG, "Skip location report: Moinai token is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    ServingCellInfo cell;
    if (!Board::GetInstance().GetServingCellInfo(cell)) {
        ESP_LOGI(TAG, "Skip location report: serving-cell information is unavailable");
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceId", serial_number_.c_str());
    cJSON_AddStringToObject(root, "imei", cell.imei.c_str());
    struct timeval now;
    gettimeofday(&now, nullptr);
    const int64_t ctime_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
    cJSON_AddNumberToObject(root, "ctime", static_cast<double>(ctime_ms));
    cJSON* serving_cell = cJSON_CreateObject();
    cJSON_AddStringToObject(serving_cell, "tac", cell.tac.c_str());
    cJSON_AddStringToObject(serving_cell, "cellId", cell.cell_id.c_str());
    cJSON_AddStringToObject(serving_cell, "mcc", cell.mcc.c_str());
    cJSON_AddStringToObject(serving_cell, "mnc", cell.mnc.c_str());
    cJSON_AddNumberToObject(serving_cell, "signalQuality", cell.signal_quality);
    cJSON_AddStringToObject(serving_cell, "networkType", cell.network_type.c_str());
    cJSON_AddItemToObject(root, "servingCell", serving_cell);
    char* payload = cJSON_PrintUnformatted(root);
    std::string body(payload);
    cJSON_free(payload);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Location report payload: %s", body.c_str());

    std::string base_url = GetCheckVersionUrl();
    if (base_url.back() == '/') {
        base_url.pop_back();
    }
    std::string url = base_url + "/api/v1/embeded/device/location/" + serial_number_;
    auto http = SetupHttp(moinai_token_);
    http->SetContent(std::move(body));
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open location report request");
        return ESP_FAIL;
    }
    int status = http->GetStatusCode();
    // 204 No Content is complete after the response headers. ML307 does not
    // emit a content URC for it, so ReadAll() would wait for the full timeout.
    std::string response;
    if (status != 204) {
        response = http->ReadAll();
    }
    http->Close();
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Location report failed with status %d: %s", status, response.c_str());
        return MapMoinaiHttpError(status, response);
    }
    ESP_LOGI(TAG, "Location report completed with status %d", status);
    return ESP_OK;
}
