# Cloud and local OTA

The ESP32 firmware exposes two independent compile-time switches in
`main/ota_feature_config.h`:

```c
#define CLOUD_OTA_ENABLED 1
#define LOCAL_AP_OTA_ENABLED 1
```

Set either switch to `0` to disable only that OTA entry point. Local OTA keeps
the `Xiaobai-OTA` access point and browser upload page. Cloud OTA performs an
authenticated metadata check during startup and downloads the returned
application image through the board network.

Both transports pass their bytes to `OtaImageWriter`. The writer selects the
inactive OTA slot, rejects oversized and non-application images, writes the
image, runs ESP-IDF validation, and selects the new boot partition. A merged
flash image is not accepted by either path.

Only one writer can own the inactive OTA partition. If local and cloud OTA are
requested at the same time, the first transaction continues and the other is
rejected without writing to flash.

## Backend contract

By default, the device sends this authenticated request:

```text
GET {CONFIG_OTA_URL}/api/v1/embeded/device/firmware/{serialNumber}
Authorization: Bearer {deviceToken}
Current-Version: 2.2.6
Board-Type: esp32c3-ci130x
```

The route prefix can be overridden at compile time with
`CLOUD_OTA_METADATA_PATH`. The request also carries the existing device,
client, serial-number, user-agent, and language headers.

Return `204` or `404` when no firmware is published. A successful response may
be flat or wrapped in `data` and `firmware` objects:

```json
{
  "updateAvailable": true,
  "firmware": {
    "version": "2.2.7",
    "url": "https://downloads.example.com/xiaozhi.bin"
  }
}
```

The download URL must resolve to the raw ESP-IDF application image and must
provide a nonzero `Content-Length`. Use a public or time-limited signed URL;
the metadata request is authenticated, but the binary download does not reuse
the device bearer token. The URL must not point to a merged flash image.
The device accepts `version`, `firmwareVersion`, or `firmware_version`, and
`url`, `firmwareUrl`, `firmware_url`, `downloadUrl`, or `download_url`.

If `updateAvailable` is omitted, the device compares dotted numeric versions.
An explicit `true` lets the backend request an update even when its version
format is not comparable. Metadata lookup errors are logged but do not block
normal device startup.
