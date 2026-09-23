#include "ota_image_writer.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <esp_log.h>

namespace {

constexpr char kLogTag[] = "OtaWriter";
std::atomic<bool> s_ota_session_active{false};

}  // namespace

OtaImageWriter::OtaImageWriter(size_t expected_size)
    : expected_size_(expected_size) {}

OtaImageWriter::~OtaImageWriter() {
    Abort();
    if (owns_session_ && !finished_) {
        s_ota_session_active.store(false);
    }
}

esp_err_t OtaImageWriter::Begin() {
    if (begun_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_size_ < kImageMetadataSize) {
        ESP_LOGE(kLogTag, "Application image is too small: %u bytes",
                 static_cast<unsigned int>(expected_size_));
        return ESP_ERR_INVALID_SIZE;
    }

    partition_ = esp_ota_get_next_update_partition(nullptr);
    if (partition_ == nullptr) {
        ESP_LOGE(kLogTag, "No inactive OTA partition is available");
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size_ > partition_->size) {
        ESP_LOGE(kLogTag, "Image is too large for %s: %u > %u bytes",
                 partition_->label, static_cast<unsigned int>(expected_size_),
                 static_cast<unsigned int>(partition_->size));
        return ESP_ERR_INVALID_SIZE;
    }

    bool expected = false;
    if (!s_ota_session_active.compare_exchange_strong(expected, true)) {
        ESP_LOGW(kLogTag, "Another OTA transaction is already active");
        return ESP_ERR_INVALID_STATE;
    }

    owns_session_ = true;
    begun_ = true;
    ESP_LOGI(kLogTag, "Preparing %u-byte image for partition %s",
             static_cast<unsigned int>(expected_size_), partition_->label);
    return ESP_OK;
}

esp_err_t OtaImageWriter::ValidateAndStartWriter() {
    esp_image_header_t image_header = {};
    esp_app_desc_t app_desc = {};
    std::memcpy(&image_header, metadata_.data(), sizeof(image_header));
    std::memcpy(&app_desc,
                metadata_.data() + sizeof(esp_image_header_t) +
                    sizeof(esp_image_segment_header_t),
                sizeof(app_desc));

    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC ||
        app_desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(kLogTag,
                 "Upload is not an ESP-IDF application image; merged images are not supported");
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }

    esp_err_t err = esp_ota_begin(partition_, OTA_WITH_SEQUENTIAL_WRITES,
                                  &handle_);
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }
    writer_started_ = true;

    err = esp_ota_write(handle_, metadata_.data(), metadata_size_);
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "Failed to write application metadata: %s",
                 esp_err_to_name(err));
        Abort();
        return err;
    }

    ESP_LOGI(kLogTag, "Validated application %.*s version %.*s",
             static_cast<int>(sizeof(app_desc.project_name)),
             app_desc.project_name,
             static_cast<int>(sizeof(app_desc.version)), app_desc.version);
    return ESP_OK;
}

esp_err_t OtaImageWriter::Write(const void* data, size_t size) {
    if (!begun_ || finished_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (size == 0) {
        return ESP_OK;
    }
    if (data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes_received_ > expected_size_ ||
        size > expected_size_ - bytes_received_) {
        ESP_LOGE(kLogTag, "Received more data than declared");
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    if (!writer_started_) {
        const size_t copy_size =
            std::min(remaining, kImageMetadataSize - metadata_size_);
        std::memcpy(metadata_.data() + metadata_size_, cursor, copy_size);
        metadata_size_ += copy_size;
        bytes_received_ += copy_size;
        cursor += copy_size;
        remaining -= copy_size;

        if (metadata_size_ == kImageMetadataSize) {
            const esp_err_t err = ValidateAndStartWriter();
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    if (remaining > 0) {
        const esp_err_t err = esp_ota_write(handle_, cursor, remaining);
        if (err != ESP_OK) {
            ESP_LOGE(kLogTag, "esp_ota_write failed: %s",
                     esp_err_to_name(err));
            Abort();
            return err;
        }
        bytes_received_ += remaining;
    }
    return ESP_OK;
}

esp_err_t OtaImageWriter::Finish() {
    if (!begun_ || finished_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!writer_started_ || bytes_received_ != expected_size_) {
        ESP_LOGE(kLogTag, "Incomplete image: received %u of %u bytes",
                 static_cast<unsigned int>(bytes_received_),
                 static_cast<unsigned int>(expected_size_));
        Abort();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_end(handle_);
    writer_started_ = false;
    handle_ = 0;
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "Application image validation failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(partition_);
    if (err != ESP_OK) {
        ESP_LOGE(kLogTag, "Failed to select %s for the next boot: %s",
                 partition_->label, esp_err_to_name(err));
        return err;
    }

    finished_ = true;
    ESP_LOGI(kLogTag, "Application image accepted in %s", partition_->label);
    return ESP_OK;
}

const char* OtaImageWriter::PartitionLabel() const {
    return partition_ == nullptr ? "<none>" : partition_->label;
}

void OtaImageWriter::Abort() {
    if (writer_started_) {
        esp_ota_abort(handle_);
        writer_started_ = false;
        handle_ = 0;
    }
}
