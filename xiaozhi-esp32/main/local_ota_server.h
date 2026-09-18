#ifndef LOCAL_OTA_SERVER_H
#define LOCAL_OTA_SERVER_H

class LocalOtaServer {
public:
    // Starts the SoftAP and raw application-image HTTP upload endpoint.
    // LOCAL_AP_OTA_ENABLED controls whether this entry point is available.
    static bool Start();
};

#endif  // LOCAL_OTA_SERVER_H
