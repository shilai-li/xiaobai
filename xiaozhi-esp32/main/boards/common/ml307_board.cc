#include "ml307_board.h"

#include "audio_codec.h"
#include "display.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <font_awesome.h>
#include <utility>
#include <sys/time.h>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <cctype>

static const char *TAG = "Ml307Board";

// Maximum retry count for modem detection
static constexpr int MODEM_DETECT_MAX_RETRIES = 30;
// Maximum retry count for network registration
static constexpr int NETWORK_REG_MAX_RETRIES = 6;
// +MIPCALL can announce an IP shortly before ML307's socket service is ready.
static constexpr int PDP_SOCKET_SETTLE_MS = 300;

// 3GPP TS 27.007 <AcT> values returned by +CREG/+CGREG/+CEREG. Keep this
// decoder in diagnostics so logs identify the actual radio technology instead
// of requiring a reader to translate numeric values manually.
static const char* CellularAccessTechnologyName(int act) {
    switch (act) {
        case 0: return "GSM/GPRS";
        case 1: return "GSM Compact";
        case 2: return "WCDMA/UTRAN";
        case 3: return "EDGE/EGPRS";
        case 4: return "HSDPA";
        case 5: return "HSUPA";
        case 6: return "HSDPA+HSUPA";
        case 7: return "LTE";
        case 8: return "EC-GSM-IoT";
        case 9: return "LTE-M/NB-IoT";
        case 10: return "LTE connected to 5GC";
        case 11: return "NR (5G)";
        case 12: return "NG-RAN";
        default: return "unknown";
    }
}

static int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

void Ml307Board::RunLbsAtTest() {
#if CONFIG_ML307_LBS_AT_TEST
    if (modem_ == nullptr) {
        ESP_LOGE(TAG, "LBS AT test skipped: modem is not initialized");
        return;
    }

    auto at_uart = modem_->GetAtUart();
    if (at_uart == nullptr) {
        ESP_LOGE(TAG, "LBS AT test skipped: AT UART is not available");
        return;
    }

    ESP_LOGI(TAG, "Starting ML307 network/LBS AT test");

    // Query every registration domain. +CREG is the source of a 2G/3G LAC,
    // +CGREG is the packet-domain counterpart, and +CEREG reports LTE/5G
    // TAC. Their raw responses are retained because field availability varies
    // with the registered radio technology and modem firmware.
    const char* commands[] = {
        "AT+GMM",
        "AT+GMR",
        "AT+CSQ",
        "AT+CREG=?",
        "AT+CREG?",
        "AT+CGREG=?",
        "AT+CGREG?",
        "AT+CEREG=?",
        "AT+CEREG?",
        "AT+COPS?",
        // ML307-specific UE statistics: serving cell and neighbor cells.
        "AT+MUESTATS=?",
        "AT+MUESTATS=\"cell\"",
        // Optional engineering/neighbor-cell commands. Support and output
        // format are firmware-dependent, so keep the raw response in logs.
        "AT+CENG=?",
        "AT+CENG?",
        "AT+CENG=2",
        "AT+MLBSCFG=?",
        // Use the OneOS LBS backend. These settings are persistent in the
        // modem and are applied only when the diagnostic test is enabled.
        "AT+MLBSCFG=\"method\",40",
        "AT+MLBSCFG=\"nearbtsen\",1",
        "AT+MLBSCFG=\"pid\"",
        "AT+MLBSLOC",
    };

    for (const char* command : commands) {
        const size_t timeout_ms = std::string(command) == "AT+MLBSLOC" ? 30000 : 3000;
        bool ok = at_uart->SendCommand(command, timeout_ms);
        std::string response = at_uart->GetResponse();
        ESP_LOGI(TAG, "AT test: %s -> %s\n%s", command, ok ? "OK" : "FAILED", response.c_str());
    }

    // +COPS=? scans visible networks and can take several minutes. It is
    // intentionally opt-in so normal LBS diagnostics never interrupt an
    // active data session. The result identifies technologies visible at the
    // test location; it does not prove every technology is usable by the SIM.
#if CONFIG_ML307_NETWORK_SCAN_TEST
    constexpr size_t kNetworkScanTimeoutMs = 180000;
    const bool scan_ok = at_uart->SendCommand("AT+COPS=?", kNetworkScanTimeoutMs);
    ESP_LOGI(TAG, "Network scan: AT+COPS=? -> %s\n%s", scan_ok ? "OK" : "FAILED",
             at_uart->GetResponse().c_str());
#endif

    const auto registration = modem_->GetRegistrationState();
    ESP_LOGI(TAG,
             "Network registration summary: CEREG stat=%d, AcT=%d (%s), TAC=%s, cellId=%s",
             registration.stat, registration.AcT,
             CellularAccessTechnologyName(registration.AcT),
             registration.tac.empty() ? "unavailable" : registration.tac.c_str(),
             registration.ci.empty() ? "unavailable" : registration.ci.c_str());
    ESP_LOGI(TAG,
             "Location-area guidance: use LAC only when +CREG?/+CGREG? reports one on 2G/3G; "
             "use TAC for LTE/5G. TAC and LAC must not be converted between each other.");

    // The ML307 TCP/IP manual defines AT+MPING as an asynchronous command:
    // the command returns OK first, then one or more +MPING URCs contain the
    // DNS result, RTT, TTL, and final statistics. Capture those URCs while
    // testing the real cellular data path to the Moinai backend.
    bool ping_running = false;
    auto ping_urc = at_uart->RegisterUrcCallback([&ping_running](const std::string& command,
                                                                  const std::vector<AtArgumentValue>& arguments) {
        if (command != "MPING" || !ping_running) {
            return;
        }

        std::string values;
        for (const auto& argument : arguments) {
            if (!values.empty()) {
                values += ",";
            }
            values += argument.ToString();
        }
        ESP_LOGI(TAG, "4G ping URC: +MPING: %s", values.c_str());
    });

    bool ping_supported = at_uart->SendCommand("AT+MPING=?", 3000);
    ESP_LOGI(TAG, "4G ping capability: %s\n%s", ping_supported ? "SUPPORTED" : "NOT SUPPORTED",
             at_uart->GetResponse().c_str());

    if (ping_supported) {
        const char* ping_host = "robotic-chat.moinai.com";
        const char* ping_command = "AT+MPING=\"robotic-chat.moinai.com\",10,1,16,1";
        bool ping_started = at_uart->SendCommand(ping_command, 3000);
        ESP_LOGI(TAG, "4G ping start: host=%s, result=%s\n%s", ping_host,
                 ping_started ? "STARTED" : "FAILED", at_uart->GetResponse().c_str());

        if (ping_started) {
            ping_running = true;
            // Wait for the asynchronous +MPING result before unregistering
            // the callback. The modem timeout is 10 seconds.
            vTaskDelay(pdMS_TO_TICKS(12000));
            ESP_LOGI(TAG, "4G ping test completed: host=%s", ping_host);
        }
    }

    at_uart->UnregisterUrcCallback(ping_urc);

    ESP_LOGI(TAG, "ML307 LBS AT test completed");
#else
    // Keep this method available in normal builds without adding any runtime
    // work unless the diagnostic option is explicitly enabled.
#endif
}

Ml307Board::Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin) : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin) {
}

std::string Ml307Board::GetBoardType() {
    return "ml307";
}

void Ml307Board::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void Ml307Board::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::ModemDetecting:
            ESP_LOGI(TAG, "Detecting modem...");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "Registering network...");
            break;
        case NetworkEvent::Connected:
            ESP_LOGI(TAG, "Network connected");
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "Network disconnected");
            break;
        case NetworkEvent::ModemErrorNoSim:
            ESP_LOGE(TAG, "No SIM card detected");
            break;
        case NetworkEvent::ModemErrorRegDenied:
            ESP_LOGE(TAG, "Network registration denied");
            break;
        case NetworkEvent::ModemErrorInitFailed:
            ESP_LOGE(TAG, "Modem initialization failed");
            break;
        case NetworkEvent::ModemErrorTimeout:
            ESP_LOGE(TAG, "Operation timeout");
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void Ml307Board::NetworkTask() {
    // Notify modem detection started
    OnNetworkEvent(NetworkEvent::ModemDetecting);

    // Try to detect modem with retry limit
    int detect_retries = 0;
    while (detect_retries < MODEM_DETECT_MAX_RETRIES) {
        modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, 921600);
        if (modem_ != nullptr) {
            break;
        }
        detect_retries++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (modem_ == nullptr) {
        ESP_LOGE(TAG, "Failed to detect modem after %d retries", MODEM_DETECT_MAX_RETRIES);
        OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
        return;
    }

    ESP_LOGI(TAG, "Modem detected successfully");

    // Configure SSL context 0 right after modem detection, before network
    // registration. AT+MSSLCFG does not require an active PDP context, and
    // doing this early avoids a race where CEREG registration fires
    // NetworkEvent::Connected (which triggers the first HTTPS request, e.g.
    // Moinai token auth) before this config would otherwise run further
    // below. If the HTTPS request runs before SNI/auth are configured on
    // SSL context 0, the server can abruptly close the connection
    // (+MHTTPURC: "err",0,5) before returning headers.
    auto at_uart = modem_->GetAtUart();
    if (at_uart != nullptr) {
        at_uart->SetDebug(false); // Keep verbose AT transaction logging disabled
        ESP_LOGI(TAG, "Configuring SSL context 0: disabling cert verification & enabling SNI");
        bool auth_ok = at_uart->SendCommand("AT+MSSLCFG=\"auth\",0,0");

        // Extract host from CONFIG_OTA_URL
        std::string ota_url = CONFIG_OTA_URL;
        std::string host = "robotic-chat.moinai.com"; // default fallback
        size_t proto_end = ota_url.find("://");
        if (proto_end != std::string::npos) {
            size_t host_start = proto_end + 3;
            size_t path_start = ota_url.find("/", host_start);
            if (path_start != std::string::npos) {
                host = ota_url.substr(host_start, path_start - host_start);
            } else {
                host = ota_url.substr(host_start);
            }
        }
        size_t port_colon = host.find(":");
        if (port_colon != std::string::npos) {
            host = host.substr(0, port_colon);
        }

        // NOTE: AT+MSSLCFG=? on this firmware (ML307C ...MBRH0S00) reports only
        // "auth","cert","encoding","negotime" as supported sub-options. There is
        // no "sni"/"version"/"ciphersuite" option: the modem returns OK for those
        // unknown keywords but silently ignores them, so the sni command above is
        // effectively a no-op. Testing confirmed the server accepts the device's
        // exact request without SNI (standard TLS 1.2/AES128-GCM), so the token
        // failure is not an SNI or TLS-profile problem.
        std::string sni_cmd = "AT+MSSLCFG=\"sni\",0,\"" + host + "\"";
        bool sni_ok = at_uart->SendCommand(sni_cmd);
        ESP_LOGI(TAG, "SSL context 0 config results -> auth: %s, sni (%s): %s",
                 auth_ok ? "SUCCESS" : "FAILED", host.c_str(), sni_ok ? "SUCCESS" : "FAILED");
    }

    // Notify network registration started
    OnNetworkEvent(NetworkEvent::Connecting);

    // Wait for network ready with retry limit
    int reg_retries = 0;
    while (reg_retries < NETWORK_REG_MAX_RETRIES) {
        auto result = modem_->WaitForNetworkReady();
        if (result == NetworkStatus::Ready) {
            break;
        } else if (result == NetworkStatus::ErrorInsertPin) {
            OnNetworkEvent(NetworkEvent::ModemErrorNoSim);
        } else if (result == NetworkStatus::ErrorRegistrationDenied) {
            OnNetworkEvent(NetworkEvent::ModemErrorRegDenied);
        } else if (result == NetworkStatus::ErrorTimeout) {
            OnNetworkEvent(NetworkEvent::ModemErrorTimeout);
        }
        reg_retries++;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    if (!modem_->network_ready()) {
        ESP_LOGE(TAG, "Failed to register network after %d retries", NETWORK_REG_MAX_RETRIES);
        return;
    }

    // Ml307AtModem::WaitForNetworkReady waits for +MIPCALL after cellular
    // registration. The module can report its PDP IP slightly before its TLS
    // socket service accepts connections, so retain a short settling window to
    // avoid a deterministic first-attempt ML307 error 753.
    vTaskDelay(pdMS_TO_TICKS(PDP_SOCKET_SETTLE_MS));
    OnNetworkEvent(NetworkEvent::Connected);

    // Future reconnects also wait for PDP readiness outside the UART callback.
    // AT commands must never be issued directly from that callback task.
    modem_->OnNetworkStateChanged([this](bool network_ready) {
        if (!network_ready) {
            OnNetworkEvent(NetworkEvent::Disconnected);
            return;
        }
        xTaskCreate([](void* arg) {
            auto* board = static_cast<Ml307Board*>(arg);
            if (board->modem_ != nullptr &&
                board->modem_->WaitForNetworkReady(10000) == NetworkStatus::Ready) {
                vTaskDelay(pdMS_TO_TICKS(PDP_SOCKET_SETTLE_MS));
                board->OnNetworkEvent(NetworkEvent::Connected);
            } else {
                ESP_LOGW(TAG, "Cellular registration recovered without a ready PDP context");
            }
            vTaskDelete(nullptr);
        }, "ml307_reconn", 4096, this, 5, nullptr);
    });

    // Print information that is already cached or responds immediately. Do not
    // query ICCID here: NetworkEvent::Connected has already started clock sync
    // and authentication, and a missing/unsupported AT+ICCID response can hold
    // the shared AT UART for one second. GetBoardJson() still retrieves ICCID
    // lazily when full diagnostic information is explicitly requested.
    std::string module_revision = modem_->GetModuleRevision();
    std::string imei = modem_->GetImei();
    ESP_LOGI(TAG, "ML307 Revision: %s", module_revision.c_str());
    ESP_LOGI(TAG, "ML307 IMEI: %s", imei.c_str());

    RunLbsAtTest();
}

void Ml307Board::StartNetwork() {
    // Create network initialization task and return immediately
    xTaskCreate([](void* arg) {
        Ml307Board* board = static_cast<Ml307Board*>(arg);
        board->NetworkTask();
        vTaskDelete(NULL);
    }, "ml307_net", 4096, this, 5, NULL);
}

bool Ml307Board::SyncSystemTime() {
    if (modem_ == nullptr) {
        ESP_LOGW(TAG, "Cannot sync system time: modem is not initialized");
        return false;
    }

    auto at_uart = modem_->GetAtUart();
    if (at_uart == nullptr) {
        ESP_LOGW(TAG, "Cannot sync system time: AT UART is not available");
        return false;
    }

    // ML307 may initially report its unsynchronized placeholder clock as
    // 2070-01-01. Poll until the network has supplied a plausible time instead
    // of letting the first TLS handshake fail with that placeholder value.
    // Network time commonly becomes valid shortly after registration. Poll at
    // a short cadence so authentication does not sit through a full one-second
    // delay after the clock is ready, while retaining roughly the original
    // 30-second tolerance for slow network synchronization.
    constexpr int kTimeSyncAttempts = 150;
    constexpr int kTimeSyncRetryMs = 200;
    constexpr int kMinValidYear = 2023;
    constexpr int kMaxValidYear = 2037;
    // A parse failure is deterministic: AtUart splits URC arguments on commas
    // without quote awareness, so a quoted +CCLK value ("26/09/18,08:15:30+32")
    // can never yield the 8 fields sscanf expects. Retrying cannot fix it, so
    // bail out quickly and let the caller fall back to HTTP Date sync.
    constexpr int kMaxParseFailures = 2;
    int parse_failures = 0;

    for (int attempt = 1; attempt <= kTimeSyncAttempts; ++attempt) {
        // AtUart classifies +CCLK as a URC, so it is not retained in
        // GetResponse(). Capture this command response through the URC path.
        std::string clock_value;
        auto cclk_urc = at_uart->RegisterUrcCallback(
            [&clock_value](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
                if (command == "CCLK" && !arguments.empty()) {
                    clock_value = arguments[0].string_value;
                }
            });
        const bool command_ok = at_uart->SendCommand("AT+CCLK?", 3000);
        const std::string response = at_uart->GetResponse();
        at_uart->UnregisterUrcCallback(cclk_urc);

        if (!command_ok) {
            ESP_LOGW(TAG, "AT+CCLK? failed, attempt=%d/%d", attempt, kTimeSyncAttempts);
            vTaskDelay(pdMS_TO_TICKS(kTimeSyncRetryMs));
            continue;
        }

        if (clock_value.empty()) {
            ESP_LOGW(TAG, "AT+CCLK? returned no clock value: %s", response.c_str());
            vTaskDelay(pdMS_TO_TICKS(kTimeSyncRetryMs));
            continue;
        }

        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        int second = 0;
        int timezone_quarters = 0;
        char timezone_sign = '+';
        const int fields = std::sscanf(clock_value.c_str(),
                                       "%d/%d/%d,%d:%d:%d%c%d",
                                       &year, &month, &day, &hour, &minute, &second,
                                       &timezone_sign, &timezone_quarters);
        if (fields != 8 || (timezone_sign != '+' && timezone_sign != '-')) {
            ESP_LOGW(TAG, "Unable to parse ML307 clock: %s", clock_value.c_str());
            if (++parse_failures >= kMaxParseFailures) {
                ESP_LOGW(TAG,
                         "ML307 clock parsing keeps failing (URC argument truncation); "
                         "giving up after %d attempts", parse_failures);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(kTimeSyncRetryMs));
            continue;
        }

        year += 2000;
        if (year < kMinValidYear || year > kMaxValidYear ||
            month < 1 || month > 12 || day < 1 || day > 31 ||
            hour > 23 || minute > 59 || second > 59) {
            ESP_LOGW(TAG, "ML307 clock is not synchronized yet: %s, attempt=%d/%d",
                     clock_value.c_str(), attempt, kTimeSyncAttempts);
            vTaskDelay(pdMS_TO_TICKS(kTimeSyncRetryMs));
            continue;
        }

        int64_t epoch = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400;
        epoch += hour * 3600 + minute * 60 + second;

        // ML307C-DC-CN-MBRH0S00 has been verified to return UTC in the date/time
        // fields while still appending the network timezone suffix (for example
        // +32 for UTC+08:00). Applying that suffix shifts the Unix timestamp
        // eight hours into the past, so keep it for diagnostics only.

        struct timeval time_value = {
            static_cast<time_t>(epoch),
            0,
        };
        if (settimeofday(&time_value, nullptr) != 0) {
            ESP_LOGE(TAG, "Failed to set ESP32 system time from ML307");
            return false;
        }

        struct tm utc_tm;
        char utc_text[32] = "unknown";
        if (gmtime_r(&time_value.tv_sec, &utc_tm) != nullptr) {
            strftime(utc_text, sizeof(utc_text), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
        }
        const int timezone_minutes = timezone_quarters * 15;
        ESP_LOGI(TAG,
                 "System time synchronized from ML307: modem_utc=%04d-%02d-%02d %02d:%02d:%02d, reported_zone=UTC%c%02d:%02d, utc=%s",
                 year, month, day, hour, minute, second, timezone_sign,
                 timezone_minutes / 60, timezone_minutes % 60, utc_text);
        return true;
    }

    return false;
}

NetworkInterface* Ml307Board::GetNetwork() {
    return modem_.get();
}

const char* Ml307Board::GetNetworkStateIcon() {
    if (modem_ == nullptr || !modem_->network_ready()) {
        return FONT_AWESOME_SIGNAL_OFF;
    }
    int csq = modem_->GetCsq();
    if (csq == -1) {
        return FONT_AWESOME_SIGNAL_OFF;
    } else if (csq >= 0 && csq <= 9) {
        return FONT_AWESOME_SIGNAL_WEAK;
    } else if (csq >= 10 && csq <= 14) {
        return FONT_AWESOME_SIGNAL_FAIR;
    } else if (csq >= 15 && csq <= 19) {
        return FONT_AWESOME_SIGNAL_GOOD;
    } else if (csq >= 20 && csq <= 31) {
        return FONT_AWESOME_SIGNAL_STRONG;
    }

    ESP_LOGW(TAG, "Invalid CSQ: %d", csq);
    return FONT_AWESOME_SIGNAL_OFF;
}

std::string Ml307Board::GetBoardJson() {
    // Set the board type for OTA
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    board_json += "\"revision\":\"" + modem_->GetModuleRevision() + "\",";
    board_json += "\"carrier\":\"" + modem_->GetCarrierName() + "\",";
    board_json += "\"csq\":\"" + std::to_string(modem_->GetCsq()) + "\",";
    board_json += "\"imei\":\"" + modem_->GetImei() + "\",";
    board_json += "\"iccid\":\"" + modem_->GetIccid() + "\",";
    board_json += "\"cereg\":" + modem_->GetRegistrationState().ToString() + "}";
    return board_json;
}

void Ml307Board::SetPowerSaveLevel(PowerSaveLevel level) {
    // TODO: Implement power save level for ML307
    (void)level;
}

bool Ml307Board::GetServingCellInfo(ServingCellInfo& info) {
    if (modem_ == nullptr || !modem_->network_ready()) {
        return false;
    }

    auto registration = modem_->GetRegistrationState();
    int csq = modem_->GetCsq();
    std::string imei = modem_->GetImei();

    // CEREG provides TAC and cell ID but not the public land mobile network
    // identity. Ask COPS for its numeric operator representation (MCC + MNC),
    // then restore the usual long alphanumeric representation.
    auto at_uart = modem_->GetAtUart();
    std::string plmn;
    if (at_uart != nullptr && at_uart->SendCommand("AT+COPS=3,2", 3000)) {
        auto cops_urc = at_uart->RegisterUrcCallback(
            [&plmn](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
                if (command == "COPS" && arguments.size() >= 3) {
                    plmn = arguments[2].string_value;
                }
            });
        const bool cops_ok = at_uart->SendCommand("AT+COPS?", 3000);
        at_uart->UnregisterUrcCallback(cops_urc);
        if (!cops_ok) {
            plmn.clear();
        }
        if (!at_uart->SendCommand("AT+COPS=3,0", 3000)) {
            ESP_LOGW(TAG, "Failed to restore alphanumeric COPS operator format");
        }
    }

    bool imei_is_valid = imei.size() == 15;
    for (char ch : imei) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            imei_is_valid = false;
            break;
        }
    }

    bool plmn_is_valid = plmn.size() == 5 || plmn.size() == 6;
    for (char ch : plmn) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            plmn_is_valid = false;
            break;
        }
    }

    if (!imei_is_valid || registration.tac.empty() || registration.ci.empty() ||
        !plmn_is_valid || csq < 0) {
        ESP_LOGW(TAG,
                 "Serving-cell information is incomplete: imei=%s, tac=%s, cell=%s, plmn=%s, csq=%d",
                 imei.c_str(), registration.tac.c_str(), registration.ci.c_str(), plmn.c_str(), csq);
        return false;
    }

    info.imei = std::move(imei);
    info.tac = registration.tac;
    info.cell_id = registration.ci;
    info.mcc = plmn.substr(0, 3);
    info.mnc = plmn.substr(3);
    info.signal_quality = csq;
    switch (registration.AcT) {
        case 0: info.network_type = "GSM"; break;
        case 2: info.network_type = "WCDMA"; break;
        case 7: info.network_type = "LTE"; break;
        case 11: info.network_type = "NR"; break;
        default:
            ESP_LOGW(TAG, "Unsupported CEREG AcT=%d for location reporting", registration.AcT);
            return false;
    }
    return true;
}

std::string Ml307Board::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "cellular",
     *         "carrier": "CHINA MOBILE",
     *         "csq": 10
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "cellular");
    cJSON_AddStringToObject(network, "carrier", modem_->GetCarrierName().c_str());
    int csq = modem_->GetCsq();
    if (csq == -1) {
        cJSON_AddStringToObject(network, "signal", "unknown");
    } else if (csq >= 0 && csq <= 14) {
        cJSON_AddStringToObject(network, "signal", "very weak");
    } else if (csq >= 15 && csq <= 19) {
        cJSON_AddStringToObject(network, "signal", "weak");
    } else if (csq >= 20 && csq <= 24) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else if (csq >= 25 && csq <= 31) {
        cJSON_AddStringToObject(network, "signal", "strong");
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
