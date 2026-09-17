#include "autodetect.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

static std::string RunProcessAndCapture(const std::string& cmd, DWORD timeoutMs = 8000) {
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmdCopy = cmd;
    std::string output;

    if (CreateProcessA(NULL, &cmdCopy[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWrite);
        hWrite = NULL;

        DWORD startTime = GetTickCount();
        while (true) {
            DWORD bytesAvail = 0;
            if (PeekNamedPipe(hRead, NULL, 0, NULL, &bytesAvail, NULL) && bytesAvail > 0) {
                char buffer[512];
                DWORD bytesToRead = min((DWORD)(sizeof(buffer) - 1), bytesAvail);
                DWORD bytesRead = 0;
                if (ReadFile(hRead, buffer, bytesToRead, &bytesRead, NULL) && bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    output += buffer;
                }
            }

            DWORD waitRes = WaitForSingleObject(pi.hProcess, 50);
            if (waitRes == WAIT_OBJECT_0) {
                // Drain any final output
                while (PeekNamedPipe(hRead, NULL, 0, NULL, &bytesAvail, NULL) && bytesAvail > 0) {
                    char buffer[512];
                    DWORD bytesToRead = min((DWORD)(sizeof(buffer) - 1), bytesAvail);
                    DWORD bytesRead = 0;
                    if (ReadFile(hRead, buffer, bytesToRead, &bytesRead, NULL) && bytesRead > 0) {
                        buffer[bytesRead] = '\0';
                        output += buffer;
                    } else {
                        break;
                    }
                }
                break;
            }

            if (GetTickCount() - startTime > timeoutMs) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(hWrite);
    }
    CloseHandle(hRead);
    return output;
}

static bool FileExists(const std::string& path) {
    DWORD dwAttrib = GetFileAttributesA(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

static std::string FindAdbPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir = exePath;
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) {
        std::string localAdb = dir.substr(0, pos + 1) + "adb.exe";
        if (FileExists(localAdb)) return "\"" + localAdb + "\"";
    }

    const char* candidates[] = {
        "C:\\Users\\arjun\\Downloads\\platform-tools-latest-windows\\platform-tools\\adb.exe",
        "C:\\Users\\arjun\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe",
        "adb.exe"
    };

    for (const char* c : candidates) {
        if (FileExists(c)) {
            return std::string("\"") + c + "\"";
        }
    }
    return "adb.exe";
}

static std::string DetectRndisIp() {
    ULONG bufLen = 15000;
    std::vector<BYTE> buffer(bufLen);
    PIP_ADAPTER_ADDRESSES addresses = (PIP_ADAPTER_ADDRESSES)buffer.data();

    ULONG ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, addresses, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        addresses = (PIP_ADAPTER_ADDRESSES)buffer.data();
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, addresses, &bufLen);
    }

    if (ret != NO_ERROR) return "";

    std::string fallbackTetherIp;

    for (PIP_ADAPTER_ADDRESSES curr = addresses; curr != NULL; curr = curr->Next) {
        if (curr->OperStatus != IfOperStatusUp) continue;
        if (curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        std::wstring desc = curr->Description ? curr->Description : L"";
        std::wstring friendlyName = curr->FriendlyName ? curr->FriendlyName : L"";

        // Skip virtual network adapters
        if (desc.find(L"Virtual") != std::wstring::npos ||
            desc.find(L"Hyper-V") != std::wstring::npos ||
            desc.find(L"WSL") != std::wstring::npos ||
            desc.find(L"VMware") != std::wstring::npos) {
            continue;
        }

        bool isRndisMatch = (desc.find(L"Remote NDIS") != std::wstring::npos ||
                             desc.find(L"NDIS") != std::wstring::npos ||
                             desc.find(L"Tether") != std::wstring::npos ||
                             friendlyName.find(L"Ethernet 3") != std::wstring::npos);

        for (PIP_ADAPTER_UNICAST_ADDRESS u = curr->FirstUnicastAddress; u != NULL; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                sockaddr_in* sin = (sockaddr_in*)u->Address.lpSockaddr;
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));

                std::string ip = ipStr;
                if (isRndisMatch) {
                    return ip; // Direct strong match
                }

                // Subnet match for typical phone tethering (10.105.x.x, 192.168.42.x, etc.)
                if (curr->FirstGatewayAddress != NULL) {
                    if (ip.find("10.105.") == 0 || ip.find("192.168.42.") == 0) {
                        return ip;
                    }
                    if (friendlyName.find(L"Ethernet") != std::wstring::npos && ip.find("10.") == 0) {
                        fallbackTetherIp = ip;
                    }
                }
            }
        }
    }

    return fallbackTetherIp;
}

AutoConfig AutoDetectAndLaunch(int port) {
    AutoConfig cfg;
    cfg.adbPath = FindAdbPath();

    printf("\n=======================================================\n");
    printf("   PC Mirror -- Ultra-Low-Latency One-Click Launcher   \n");
    printf("=======================================================\n\n");

    // 1. Check for connected Android device via ADB
    printf("[1/3] Checking connected Android devices...\n");
    std::string devOut = RunProcessAndCapture(cfg.adbPath + " devices", 8000);
    if (devOut.find("offline") != std::string::npos) {
        printf("      [!] Phone reported offline. Attempting automatic ADB reconnect...\n");
        RunProcessAndCapture(cfg.adbPath + " reconnect offline", 4000);
        Sleep(500);
        devOut = RunProcessAndCapture(cfg.adbPath + " devices", 5000);
    }

    if (devOut.find("\tdevice") != std::string::npos) {
        cfg.deviceConnected = true;
        printf("      -> Found connected Android phone!\n");
    } else {
        printf("      [!] No authorized phone found via USB debugging.\n");
        if (!devOut.empty()) {
            printf("          ADB output: %s\n", devOut.c_str());
        }
    }

    // 2. Detect high-speed USB Tethering (RNDIS) vs ADB reverse fallback
    printf("[2/3] Detecting optimal USB transport...\n");
    if (cfg.deviceConnected) {
        // Guarantee ADB reverse is always active (bypasses Windows Firewall completely)
        RunProcessAndCapture(cfg.adbPath + " reverse tcp:" + std::to_string(port) + " tcp:" + std::to_string(port));
        RunProcessAndCapture(cfg.adbPath + " reverse tcp:" + std::to_string(port + 1) + " tcp:" + std::to_string(port + 1));
        RunProcessAndCapture(cfg.adbPath + " reverse tcp:" + std::to_string(port + 2) + " tcp:" + std::to_string(port + 2));
    }

    std::string rndisIp = DetectRndisIp();
    if (!rndisIp.empty()) {
        cfg.isRndis = true;
        cfg.serverIp = rndisIp;
        printf("      -> [OPTIMAL] Direct USB Tethering (RNDIS) Active!\n");
        printf("         PC Interface IP: %s (Targeting 15-25ms latency)\n", rndisIp.c_str());
    } else {
        cfg.isRndis = false;
        cfg.serverIp = "127.0.0.1";
        printf("      -> [FALLBACK] USB Tethering not detected.\n");
        printf("         Using ADB Reverse Tunnel (127.0.0.1:%d)...\n", port);
    }

    // 3. Ready state & start background ADB reverse watcher
    printf("[3/3] Ready! Open PC Mirror on your phone whenever you wish.\n");
    printf("      The app will automatically connect to %s:%d.\n\n", cfg.serverIp.c_str(), port);

    StartAdbWatcher(port);
    return cfg;
}

static std::atomic<bool> g_watcherRunning{ false };

void StartAdbWatcher(int port) {
    if (g_watcherRunning) return;
    g_watcherRunning = true;
    std::string adb = FindAdbPath();
    std::thread([adb, port]() {
        while (g_watcherRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
            if (!g_watcherRunning) break;
            std::string revList = RunProcessAndCapture(adb + " reverse --list", 5000);
            if (revList.find("tcp:" + std::to_string(port)) == std::string::npos) {
                RunProcessAndCapture(adb + " reverse tcp:" + std::to_string(port) + " tcp:" + std::to_string(port), 5000);
                RunProcessAndCapture(adb + " reverse tcp:" + std::to_string(port + 1) + " tcp:" + std::to_string(port + 1), 5000);
                RunProcessAndCapture(adb + " reverse tcp:" + std::to_string(port + 2) + " tcp:" + std::to_string(port + 2), 5000);
            }
        }
    }).detach();
}

void StopAdbWatcher() {
    g_watcherRunning = false;
}

