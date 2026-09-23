#ifndef OTA_IMAGE_WRITER_H
#define OTA_IMAGE_WRITER_H

#include <array>
#include <cstddef>
#include <cstdint>

#include <esp_app_format.h>
#include <esp_err.h>
#include <esp_ota_ops.h>

class OtaImageWriter {
public:
    explicit OtaImageWriter(size_t expected_size);
    ~OtaImageWriter();

    OtaImageWriter(const OtaImageWriter&) = delete;
    OtaImageWriter& operator=(const OtaImageWriter&) = delete;

    esp_err_t Begin();
    esp_err_t Write(const void* data, size_t size);
    esp_err_t Finish();

    const char* PartitionLabel() const;
    size_t BytesWritten() const { return bytes_received_; }

private:
    static constexpr size_t kImageMetadataSize =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
        sizeof(esp_app_desc_t);

    esp_err_t ValidateAndStartWriter();
    void Abort();

    size_t expected_size_ = 0;
    size_t bytes_received_ = 0;
    size_t metadata_size_ = 0;
    const esp_partition_t* partition_ = nullptr;
    esp_ota_handle_t handle_ = 0;
    bool begun_ = false;
    bool owns_session_ = false;
    bool writer_started_ = false;
    bool finished_ = false;
    std::array<uint8_t, kImageMetadataSize> metadata_{};
};

#endif  // OTA_IMAGE_WRITER_H
