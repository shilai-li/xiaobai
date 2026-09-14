#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <cstdio>
#include <cstring>

#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_console.h>

#include "application.h"

#define TAG "main"

#if CONFIG_BOARD_TYPE_ESP32C3_CI130X
namespace {

constexpr char kSerialNumberNamespace[] = "wifi";
constexpr char kSerialNumberKey[] = "serial_number";
constexpr size_t kMaxSerialNumberLength = 64;

void PrintNvsError(const char* operation, esp_err_t err) {
    printf("ERROR %s:%s\n", operation, esp_err_to_name(err));
}

bool IsValidSerialNumber(const char* serial_number) {
    if (serial_number == nullptr) {
        return false;
    }

    const size_t length = strnlen(serial_number, kMaxSerialNumberLength + 1);
    if (length == 0 || length > kMaxSerialNumberLength) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        // Keep the value safe for a whitespace-delimited console command and
        // for the line-oriented response format.
        if (serial_number[i] < 0x21 || serial_number[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

esp_err_t ReadSerialNumber(nvs_handle_t handle, char* output, size_t output_size) {
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, kSerialNumberKey, nullptr, &required_size);
    if (err != ESP_OK) {
        return err;
    }
    if (required_size == 0 || required_size > output_size) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    return nvs_get_str(handle, kSerialNumberKey, output, &required_size);
}

int SetSerialNumberCommand(int argc, char** argv) {
    if (argc != 2 || !IsValidSerialNumber(argc == 2 ? argv[1] : nullptr)) {
        printf("ERROR invalid_serial_number\n");
        return 1;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kSerialNumberNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        PrintNvsError("nvs_open", err);
        return 1;
    }

    err = nvs_set_str(handle, kSerialNumberKey, argv[1]);
    if (err != ESP_OK) {
        nvs_close(handle);
        PrintNvsError("nvs_set_str", err);
        return 1;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        nvs_close(handle);
        PrintNvsError("nvs_commit", err);
        return 1;
    }

    std::array<char, kMaxSerialNumberLength + 1> readback{};
    err = ReadSerialNumber(handle, readback.data(), readback.size());
    if (err != ESP_OK) {
        nvs_close(handle);
        PrintNvsError("verify", err);
        return 1;
    }

    const bool matches = strcmp(readback.data(), argv[1]) == 0;
    nvs_close(handle);
    if (!matches) {
        printf("ERROR verify_mismatch\n");
        return 1;
    }

    printf("OK SN=%s\n", readback.data());
    return 0;
}

int GetSerialNumberCommand(int argc, char** argv) {
    if (argc != 1) {
        printf("ERROR invalid_arguments\n");
        return 1;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kSerialNumberNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            printf("ERROR serial_number_not_found\n");
        } else {
            PrintNvsError("nvs_open", err);
        }
        return 1;
    }

    std::array<char, kMaxSerialNumberLength + 1> serial_number{};
    err = ReadSerialNumber(handle, serial_number.data(), serial_number.size());
    nvs_close(handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            printf("ERROR serial_number_not_found\n");
        } else {
            PrintNvsError("nvs_get_str", err);
        }
        return 1;
    }
    if (!IsValidSerialNumber(serial_number.data())) {
        printf("ERROR invalid_serial_number\n");
        return 1;
    }

    printf("SN=%s\n", serial_number.data());
    return 0;
}

constexpr size_t kMaxProvisioningLineLength = 96;

void RunProvisioningLine(char* line) {
    // Four slots so that a SET_SN call carrying extra arguments is reported as
    // argc 3 and rejected, instead of being truncated into a valid one.
    char* argv[4] = {};
    const size_t argc = esp_console_split_argv(line, argv, sizeof(argv) / sizeof(argv[0]));
    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "SET_SN") == 0) {
        SetSerialNumberCommand(static_cast<int>(argc), argv);
    } else if (strcmp(argv[0], "GET_SN") == 0) {
        GetSerialNumberCommand(static_cast<int>(argc), argv);
    } else {
        printf("ERROR unknown_command\n");
    }
    fflush(stdout);
}

void SerialNumberProvisioningTask(void* /*arg*/) {
    char line[kMaxProvisioningLineLength];
    size_t length = 0;
    bool overflow = false;

    while (true) {
        uint8_t byte = 0;
        // Blocks inside the driver until the host actually sends something, so
        // the task costs nothing while the USB port is unplugged.
        if (usb_serial_jtag_read_bytes(&byte, 1, portMAX_DELAY) != 1) {
            continue;
        }

        if (byte != '\r' && byte != '\n') {
            if (length + 1 < sizeof(line)) {
                line[length++] = static_cast<char>(byte);
            } else {
                overflow = true;
            }
            continue;
        }

        if (overflow) {
            printf("ERROR line_too_long\n");
            fflush(stdout);
        } else if (length > 0) {
            line[length] = '\0';
            RunProvisioningLine(line);
        }
        length = 0;
        overflow = false;
    }
}

void InitializeSerialNumberConsole() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t driver_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&driver_config));

    // Let stdio share the driver rather than writing to the FIFO behind its
    // back. Driver-backed writes are dropped after a bounded timeout when no
    // host is reading, so logging still cannot stall the application.
    usb_serial_jtag_vfs_use_driver();

    // Read the port directly instead of through esp_console: its REPL treats a
    // disconnected USB port as an endless stream of empty lines and spins at a
    // priority that starves the main task, so the device only ran while a host
    // was attached.
    xTaskCreate(SerialNumberProvisioningTask, "sn_provision", 4096, nullptr, 1, nullptr);
#else
    ESP_LOGW(TAG, "Serial number provisioning requires the USB-Serial-JTAG console");
#endif
}

}  // namespace
#else
static void InitializeSerialNumberConsole() {}
#endif

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    InitializeSerialNumberConsole();

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
