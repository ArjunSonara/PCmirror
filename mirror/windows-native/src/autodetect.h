#pragma once
#include <string>

struct AutoConfig {
    std::string adbPath;
    std::string serverIp;
    bool isRndis = false;
    bool deviceConnected = false;
};

// Discovers ADB and detects USB RNDIS vs ADB reverse fallback (without force-launching app)
AutoConfig AutoDetectAndLaunch(int port = 8080);
void StartAdbWatcher(int port = 8080);
void StopAdbWatcher();
