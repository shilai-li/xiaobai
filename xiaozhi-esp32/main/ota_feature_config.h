#ifndef OTA_FEATURE_CONFIG_H
#define OTA_FEATURE_CONFIG_H

// Independent build-time switches. Set either value to 0 to remove that OTA
// entry point while keeping the shared image validation and writer unchanged.
#ifndef CLOUD_OTA_ENABLED
#define CLOUD_OTA_ENABLED 1
#endif

#ifndef LOCAL_AP_OTA_ENABLED
#define LOCAL_AP_OTA_ENABLED 1
#endif

#ifndef LOCAL_AP_OTA_CONNECTION_WINDOW_SECONDS
#define LOCAL_AP_OTA_CONNECTION_WINDOW_SECONDS 60
#endif

// The backend returns firmware metadata from this path followed by the device
// serial number. Override it at compile time if the deployed route differs.
#ifndef CLOUD_OTA_METADATA_PATH
#define CLOUD_OTA_METADATA_PATH "/api/v1/embeded/device/firmware/"
#endif

#endif  // OTA_FEATURE_CONFIG_H
