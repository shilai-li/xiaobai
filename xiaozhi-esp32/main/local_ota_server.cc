#include "local_ota_server.h"
#include "ota_feature_config.h"

#if LOCAL_AP_OTA_ENABLED
#include "ota_image_writer.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr char kLogTag[] = "LocalOta";
constexpr char kApSsid[] = "Xiaobai-OTA";
constexpr char kApPassword[] = "xiaobai-ota";
constexpr char kApAddress[] = "192.168.4.1";
constexpr char kUploadPath[] = "/ota";
constexpr char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Xiaobai OTA</title>
<style>
body{font-family:system-ui,sans-serif;background:#f4f6f8;color:#17202a;margin:0;padding:24px}
main{max-width:520px;margin:8vh auto;background:#fff;padding:28px;border-radius:16px;box-shadow:0 8px 30px #0002}
h1{margin-top:0;font-size:1.7rem}p{line-height:1.5;color:#52606d}
input,button{box-sizing:border-box;width:100%;margin-top:14px}
input{padding:12px;border:1px solid #cbd5e1;border-radius:8px}
button{padding:13px;border:0;border-radius:8px;background:#1769e0;color:#fff;font-weight:700;cursor:pointer}
button:disabled{background:#94a3b8;cursor:not-allowed}
progress{width:100%;height:18px;margin-top:18px}
#status{min-height:24px;font-weight:600;color:#334155}
.warning{color:#b45309;font-size:.9rem}
</style>
</head>
<body>
<main>
<h1>Xiaobai firmware update</h1>
<p>Select the ESP32 application <code>.bin</code> file. Keep the device powered until it reboots.</p>
<input id="firmware" type="file" accept=".bin,application/octet-stream">
<button id="upload" type="button">Upload firmware</button>
<progress id="progress" value="0" max="100"></progress>
<p id="status">Ready.</p>
<p class="warning">Do not upload a merged flash image or firmware for another ESP32 target.</p>
</main>
<script>
const fileInput=document.getElementById('firmware');
const button=document.getElementById('upload');
const progress=document.getElementById('progress');
const status=document.getElementById('status');
button.addEventListener('click',()=>{
  const file=fileInput.files[0];
  if(!file){status.textContent='Select a firmware .bin file first.';return;}
  if(!file.name.toLowerCase().endsWith('.bin')){status.textContent='The selected file must use the .bin extension.';return;}
  button.disabled=true;fileInput.disabled=true;progress.value=0;
  status.textContent='Uploading '+file.name+'...';
  const request=new XMLHttpRequest();
  request.open('POST','/ota');
  request.setRequestHeader('Content-Type','application/octet-stream');
  request.upload.onprogress=(event)=>{
    if(event.lengthComputable){progress.value=Math.round(event.loaded*100/event.total);}
  };
  request.onload=()=>{
    if(request.status>=200&&request.status<300){progress.value=100;status.textContent=request.responseText||'Update accepted. Device is rebooting.';}
    else{status.textContent='Update failed: '+(request.responseText||('HTTP '+request.status));button.disabled=false;fileInput.disabled=false;}
  };
  request.onerror=()=>{status.textContent='Connection lost. Check whether the device rebooted.';button.disabled=false;fileInput.disabled=false;};
  request.send(file);
});
</script>
</body>
</html>)HTML";

httpd_handle_t s_server = nullptr;
SemaphoreHandle_t s_upload_mutex = nullptr;
esp_netif_t* s_ap_netif = nullptr;
esp_timer_handle_t s_connection_timer = nullptr;
esp_event_handler_instance_t s_wifi_event_handler = nullptr;
bool s_wifi_initialized = false;
bool s_wifi_started = false;

enum class ConnectionWindowState : uint8_t {
    kIdle,
    kWaiting,
    kConnected,
    kStopping,
};

enum class ShutdownReason : uint8_t {
    kConnectionTimeout,
    kStationDisconnected,
};

std::atomic<ConnectionWindowState> s_connection_state{ConnectionWindowState::kIdle};
std::atomic<ShutdownReason> s_shutdown_reason{ShutdownReason::kConnectionTimeout};

void StopLocalOta() {
    if (s_connection_timer != nullptr) {
        esp_timer_stop(s_connection_timer);
        esp_timer_delete(s_connection_timer);
        s_connection_timer = nullptr;
    }

    if (s_server != nullptr) {
        httpd_stop(s_server);
        s_server = nullptr;
    }

    if (s_wifi_event_handler != nullptr) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_handler);
        s_wifi_event_handler = nullptr;
    }

    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_ap_netif != nullptr) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
    }
    if (s_upload_mutex != nullptr) {
        vSemaphoreDelete(s_upload_mutex);
        s_upload_mutex = nullptr;
    }

    s_connection_state.store(ConnectionWindowState::kIdle);
}

void ShutdownTask(void*) {
    // Let the esp_timer callback return before deleting its timer handle.
    vTaskDelay(1);
    if (s_shutdown_reason.load() == ShutdownReason::kConnectionTimeout) {
        ESP_LOGI(kLogTag,
                 "No station connected within %d seconds; stopping local OTA",
                 LOCAL_AP_OTA_CONNECTION_WINDOW_SECONDS);
    } else {
        ESP_LOGI(kLogTag, "OTA station disconnected; stopping local OTA");
    }
    StopLocalOta();
    vTaskDelete(nullptr);
}

bool ScheduleShutdown(ShutdownReason reason) {
    s_shutdown_reason.store(reason);
    if (xTaskCreate(ShutdownTask, "local_ota_stop", 3072, nullptr, 3,
                    nullptr) == pdPASS) {
        return true;
    }

    ESP_LOGE(kLogTag, "Failed to create the local OTA shutdown task");
    return false;
}

void ConnectionTimeout(void*) {
    ConnectionWindowState expected = ConnectionWindowState::kWaiting;
    if (!s_connection_state.compare_exchange_strong(
            expected, ConnectionWindowState::kStopping)) {
        return;
    }

    if (!ScheduleShutdown(ShutdownReason::kConnectionTimeout)) {
        s_connection_state.store(ConnectionWindowState::kWaiting);
    }
}

void WifiEventHandler(void*, esp_event_base_t, int32_t event_id, void*) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        ConnectionWindowState expected = ConnectionWindowState::kWaiting;
        if (!s_connection_state.compare_exchange_strong(
                expected, ConnectionWindowState::kConnected)) {
            return;
        }

        if (s_connection_timer != nullptr) {
            esp_timer_stop(s_connection_timer);
        }
        ESP_LOGI(kLogTag, "Station connected; local OTA timeout cancelled");
        return;
    }

    if (event_id != WIFI_EVENT_AP_STADISCONNECTED) {
        return;
    }

    ConnectionWindowState expected = ConnectionWindowState::kConnected;
    if (!s_connection_state.compare_exchange_strong(
            expected, ConnectionWindowState::kStopping)) {
        return;
    }
    if (!ScheduleShutdown(ShutdownReason::kStationDisconnected)) {
        s_connection_state.store(ConnectionWindowState::kConnected);
    }
}

void SendError(httpd_req_t* request, const char* status, const char* message) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_sendstr(request, message);
}

esp_err_t IndexHandler(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t UploadHandler(httpd_req_t* request) {
    if (request->content_len == 0) {
        SendError(request, "400 Bad Request", "The request body is empty");
        return ESP_OK;
    }

    if (s_upload_mutex == nullptr || xSemaphoreTake(s_upload_mutex, 0) != pdTRUE) {
        httpd_resp_set_status(request, "409 Conflict");
        httpd_resp_set_type(request, "text/plain");
        httpd_resp_sendstr(request, "Another OTA upload is already in progress");
        return ESP_OK;
    }

    const auto release_mutex = [&]() { xSemaphoreGive(s_upload_mutex); };
    OtaImageWriter writer(request->content_len);
    esp_err_t err = writer.Begin();
    if (err != ESP_OK) {
        const char* status = "500 Internal Server Error";
        const char* message = "No OTA partition is available";
        if (err == ESP_ERR_INVALID_STATE) {
            status = "409 Conflict";
            message = "Another OTA transaction is already in progress";
        } else if (err == ESP_ERR_INVALID_SIZE) {
            status = "413 Payload Too Large";
            message = "The image does not fit the OTA partition";
        }
        release_mutex();
        SendError(request, status, message);
        return ESP_OK;
    }

    ESP_LOGI(kLogTag, "Receiving %u bytes into %s", request->content_len,
             writer.PartitionLabel());

    constexpr size_t kBufferSize = 4096;
    uint8_t buffer[kBufferSize];
    size_t received = 0;
    while (received < request->content_len) {
        const size_t remaining = request->content_len - received;
        const size_t to_read = std::min(remaining, sizeof(buffer));
        const int count = httpd_req_recv(request, reinterpret_cast<char*>(buffer), to_read);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (count <= 0) {
            ESP_LOGE(kLogTag, "Failed to receive OTA body: %d", count);
            release_mutex();
            SendError(request, "500 Internal Server Error",
                      "Failed to receive the firmware image");
            return ESP_OK;
        }

        err = writer.Write(buffer, static_cast<size_t>(count));
        if (err != ESP_OK) {
            ESP_LOGE(kLogTag, "OTA image write failed: %s", esp_err_to_name(err));
            release_mutex();
            SendError(request,
                      err == ESP_ERR_OTA_VALIDATE_FAILED
                          ? "400 Bad Request"
                          : "500 Internal Server Error",
                      err == ESP_ERR_OTA_VALIDATE_FAILED
                          ? "Upload an application image, not a merged flash image"
                          : "Failed to write the firmware image");
            return ESP_OK;
        }
        received += static_cast<size_t>(count);
    }

    err = writer.Finish();
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "Image validation failed: %s", esp_err_to_name(err));
        release_mutex();
        SendError(request, "400 Bad Request",
                  "The image failed ESP-IDF validation");
        return ESP_OK;
    }

    release_mutex();
    ESP_LOGI(kLogTag, "OTA image accepted; rebooting into %s",
             writer.PartitionLabel());
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_sendstr(request, "OTA accepted; rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

bool StartWifiAp() {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kLogTag, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kLogTag, "Default event loop failed: %s", esp_err_to_name(err));
        return false;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == nullptr) {
        ESP_LOGE(kLogTag, "Failed to create the SoftAP network interface");
        return false;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return false;
    }
    s_wifi_initialized = true;

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, WifiEventHandler, nullptr,
        &s_wifi_event_handler);
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "Failed to register the SoftAP connection handler: %s",
                 esp_err_to_name(err));
        return false;
    }

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), kApSsid,
                 sizeof(ap_config.ap.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.password), kApPassword,
                 sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len = std::strlen(kApSsid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "Failed to start SoftAP: %s", esp_err_to_name(err));
        return false;
    }
    s_wifi_started = true;

    ESP_LOGI(kLogTag, "SoftAP started: SSID=%s password=%s address=http://%s/",
             kApSsid, kApPassword, kApAddress);
    return true;
}

bool StartConnectionWindow() {
    if (s_connection_state.load() != ConnectionWindowState::kWaiting) {
        return true;
    }

    esp_timer_create_args_t timer_args = {
        .callback = ConnectionTimeout,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "local_ota_window",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_connection_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_once(
            s_connection_timer,
            static_cast<uint64_t>(LOCAL_AP_OTA_CONNECTION_WINDOW_SECONDS) *
                1000ULL * 1000ULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "Failed to start the local OTA connection timer: %s",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(kLogTag, "Waiting %d seconds for a station to connect",
             LOCAL_AP_OTA_CONNECTION_WINDOW_SECONDS);
    return true;
}

bool StartHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 2;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(kLogTag, "Failed to start the local OTA HTTP server");
        return false;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = IndexHandler,
        .user_ctx = nullptr,
    };
    httpd_uri_t upload_uri = {
        .uri = kUploadPath,
        .method = HTTP_POST,
        .handler = UploadHandler,
        .user_ctx = nullptr,
    };
    if (httpd_register_uri_handler(s_server, &index_uri) != ESP_OK ||
        httpd_register_uri_handler(s_server, &upload_uri) != ESP_OK) {
        ESP_LOGE(kLogTag, "Failed to register local OTA HTTP handlers");
        httpd_stop(s_server);
        s_server = nullptr;
        return false;
    }

    ESP_LOGI(kLogTag, "Local OTA endpoint ready at http://%s%s", kApAddress, kUploadPath);
    return true;
}

}  // namespace
#endif  // LOCAL_AP_OTA_ENABLED

bool LocalOtaServer::Start() {
#if LOCAL_AP_OTA_ENABLED
    if (s_server != nullptr) {
        return true;
    }
    s_connection_state.store(ConnectionWindowState::kWaiting);
    s_upload_mutex = xSemaphoreCreateMutex();
    if (s_upload_mutex == nullptr || !StartWifiAp() || !StartHttpServer() ||
        !StartConnectionWindow()) {
        ESP_LOGE(kLogTag, "Local OTA server startup failed");
        s_connection_state.store(ConnectionWindowState::kStopping);
        StopLocalOta();
        return false;
    }
    return true;
#else
    return false;
#endif  // LOCAL_AP_OTA_ENABLED
}
