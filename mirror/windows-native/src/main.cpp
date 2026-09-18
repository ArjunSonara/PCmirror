#include "network.h"
#include "capture.h"
#include "encoder.h"
#include "input_server.h"
#include "audio_server.h"
#include "autodetect.h"
#include <stdio.h>
#include <windows.h>
#include <csignal>
#include <atomic>

#include <timeapi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

static std::atomic<bool> g_running{ true };

static BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        printf("\nShutting down PC Mirror...\n");
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

static void DownscaleBgra(const uint8_t* src, UINT srcW, UINT srcH, UINT srcStride,
                          uint8_t* dst, UINT dstW, UINT dstH, UINT dstStride) {
    for (UINT y = 0; y < dstH; y++) {
        UINT srcY = (y * srcH) / dstH;
        const uint32_t* srcRow = (const uint32_t*)(src + (size_t)srcY * srcStride);
        uint32_t* dstRow = (uint32_t*)(dst + (size_t)y * dstStride);
        for (UINT x = 0; x < dstW; x++) {
            UINT srcX = (x * srcW) / dstW;
            dstRow[x] = srcRow[srcX];
        }
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    int port = argc > 1 ? atoi(argv[1]) : 8080;
    UINT customWidth = argc > 2 ? atoi(argv[2]) : 0;
    UINT customHeight = argc > 3 ? atoi(argv[3]) : 0;
    UINT fps = argc > 4 ? atoi(argv[4]) : 60;
    UINT bitrate = argc > 5 ? atoi(argv[5]) : 40000000;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[ERROR] Failed to initialize Winsock (WSAStartup).\n");
        return 1;
    }

    HANDLE hMutex = CreateMutexA(NULL, TRUE, "Global\\PCMirror_SingleInstance_Mutex_Arjun");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        printf("=================================================================\n");
        printf("  [!] PC Mirror is ALREADY RUNNING in the background!\n");
        printf("=================================================================\n");
        printf("Check your taskbar or existing window.\n");
        printf("If you want to restart it, close the existing PC Mirror first.\n\n");
        printf("Press Enter to close this window...");
        getchar();
        return 0;
    }

    DesktopCapture capture;
    if (!capture.Init()) {
        printf("\n[ERROR] Failed to initialize Desktop Duplication Capture.\n");
        printf("Please ensure your display driver is running normally.\n");
        printf("Press Enter to exit...");
        getchar();
        CoUninitialize();
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    UINT streamWidth = customWidth ? customWidth : capture.GetWidth();
    UINT streamHeight = customHeight ? customHeight : capture.GetHeight();
    printf("Target video resolution: %ux%u @ %u fps (%.1f Mbps)\n", streamWidth, streamHeight, fps, bitrate / 1000000.0f);

    InputServer inputServer;
    inputServer.Start(port + 1);

    AudioServer audioServer;
    audioServer.Start(port + 2);

    NetworkServer server;
    if (!server.Init(port)) {
        printf("\n[ERROR] Failed to start network server on port %d.\n", port);
        printf("Press Enter to exit...");
        getchar();
        audioServer.Stop();
        inputServer.Stop();
        capture.Shutdown();
        CoUninitialize();
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // Auto-detect optimal transport (RNDIS vs ADB reverse) and launch app on phone
    AutoDetectAndLaunch(port);

    while (g_running) {
        printf("\nWaiting for Android app to connect...\n");
        if (!server.AcceptClient()) {
            if (!g_running) break;
            Sleep(500);
            continue;
        }

        HwEncoder encoder;
        if (!encoder.Init(streamWidth, streamHeight, fps, bitrate)) {
            printf("[ERROR] Failed to initialize H.264 hardware encoder.\n");
            server.DisconnectClient();
            Sleep(1000);
            continue;
        }

        std::atomic<bool> resChangeRequested{ false };
        UINT reqWidth = streamWidth;
        UINT reqHeight = streamHeight;

        inputServer.OnBitrateChange = [&](uint32_t newBitrate) {
            bitrate = newBitrate;
            encoder.SetBitrate(newBitrate);
        };

        inputServer.OnResolutionChange = [&](uint32_t w, uint32_t h) {
            reqWidth = w ? w : capture.GetWidth();
            reqHeight = h ? h : capture.GetHeight();
            resChangeRequested = true;
        };

        inputServer.OnKeyframeRequest = [&]() {
            encoder.RequestKeyframe();
        };

        double targetFrameInterval = 1.0 / fps;

        inputServer.OnFpsChange = [&](uint32_t newFps) {
            if (newFps >= 30 && newFps <= 144) {
                fps = newFps;
                encoder.SetFps(newFps);
                targetFrameInterval = 1.0 / (double)fps;
                printf("[PC Mirror] Dynamic Frame Rate switched to %u FPS (target interval: %.2f ms)\n", fps, targetFrameInterval * 1000.0);
            }
        };

        inputServer.OnCursorToggle = [&](bool visible) {
            capture.SetCursorVisible(visible);
        };

        std::atomic<bool> clientActive{ true };
        size_t totalBytesSent = 0;
        size_t frameCount = 0;
        double totalCapSec = 0;
        double totalEncSec = 0;

        encoder.OnNal = [&](const uint8_t* data, size_t len) {
            if (!server.SendPacket(data, len)) {
                printf("\nPhone disconnected.\n");
                clientActive = false;
            }
            totalBytesSent += (len + 4);
        };

        printf(">>> STREAMING LIVE DESKTOP TO ANDROID. Press Ctrl+C to exit. <<<\n");

        std::vector<uint8_t> bgra;
        std::vector<uint8_t> scaledBgra;
        UINT w = 0, h = 0, stride = 0;
        LARGE_INTEGER freq, lastReport, lastFrameTime, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&lastReport);
        QueryPerformanceCounter(&lastFrameTime);

        double lastFrameSec = 0;

        // Force DWM to produce an initial desktop frame immediately upon connection
        mouse_event(MOUSEEVENTF_MOVE, 0, 0, 0, 0);
        int initTries = 0;
        while (g_running && clientActive && !capture.GrabFrame(bgra, w, h, stride, 50) && initTries < 20) {
            mouse_event(MOUSEEVENTF_MOVE, 0, 0, 0, 0);
            Sleep(10);
            initTries++;
        }
        if (bgra.size() > 0) {
            printf("Sending initial desktop keyframe to Android...\n");
            encoder.RequestKeyframe();
            if (streamWidth != w || streamHeight != h) {
                UINT scaledStride = streamWidth * 4;
                scaledBgra.resize((size_t)scaledStride * streamHeight);
                DownscaleBgra(bgra.data(), w, h, stride, scaledBgra.data(), streamWidth, streamHeight, scaledStride);
                encoder.EncodeFrame(scaledBgra, scaledStride);
            } else {
                encoder.EncodeFrame(bgra, stride);
            }
            QueryPerformanceCounter(&now);
            lastFrameSec = (double)now.QuadPart / (double)freq.QuadPart;
        }

        while (g_running && clientActive) {
            if (resChangeRequested) {
                resChangeRequested = false;
                streamWidth = reqWidth;
                streamHeight = reqHeight;
                printf("\n[PC Mirror] Dynamic Resolution Switching to %ux%u...\n", streamWidth, streamHeight);
                encoder.Shutdown();
                encoder.Init(streamWidth, streamHeight, fps, bitrate);
                encoder.RequestKeyframe();
            }

            LARGE_INTEGER tCap0, tCap1;
            QueryPerformanceCounter(&tCap0);

            // DXGI blocks efficiently until DWM presents a new frame, up to 16ms
            if (capture.GrabFrame(bgra, w, h, stride, 16)) {
                if (capture.CheckAndClearReinitialized()) {
                    printf("[PC Mirror] Game/display switch detected -- sending fresh keyframe to Android...\n");
                    encoder.RequestKeyframe();
                }
                QueryPerformanceCounter(&tCap1);
                QueryPerformanceCounter(&now);
                double currentSec = (double)now.QuadPart / (double)freq.QuadPart;
                double delta = currentSec - lastFrameSec;

                // Pace to target FPS (e.g. 60, 90, 120 FPS on a 144Hz screen)
                double leadMargin = targetFrameInterval > 0.010 ? 0.003 : 0.0015;
                if (delta >= (targetFrameInterval - leadMargin)) {
                    LARGE_INTEGER tEnc0, tEnc1;
                    QueryPerformanceCounter(&tEnc0);

                    if (streamWidth != w || streamHeight != h) {
                        UINT scaledStride = streamWidth * 4;
                        if (scaledBgra.size() != (size_t)scaledStride * streamHeight) {
                            scaledBgra.resize((size_t)scaledStride * streamHeight);
                        }
                        DownscaleBgra(bgra.data(), w, h, stride, scaledBgra.data(), streamWidth, streamHeight, scaledStride);
                        encoder.EncodeFrame(scaledBgra, scaledStride);
                    } else {
                        encoder.EncodeFrame(bgra, stride);
                    }

                    QueryPerformanceCounter(&tEnc1);

                    lastFrameSec = currentSec;
                    totalCapSec += (tCap1.QuadPart - tCap0.QuadPart) / (double)freq.QuadPart;
                    totalEncSec += (tEnc1.QuadPart - tEnc0.QuadPart) / (double)freq.QuadPart;
                    frameCount++;
                }
            }

            QueryPerformanceCounter(&now);
            double elapsedSec = (now.QuadPart - lastReport.QuadPart) / (double)freq.QuadPart;
            if (elapsedSec >= 2.0) {
                double currentFps = frameCount / elapsedSec;
                double mbps = (totalBytesSent * 8.0) / (elapsedSec * 1000000.0);
                double avgCapMs = frameCount > 0 ? (totalCapSec / frameCount) * 1000.0 : 0.0;
                double avgEncMs = frameCount > 0 ? (totalEncSec / frameCount) * 1000.0 : 0.0;
                double avgHostMs = avgCapMs + avgEncMs;
                printf("[PC Mirror] Streaming: %.1f FPS | Bitrate: %.2f Mbps | Host Latency: %.2f ms (Capture: %.2f ms, Encode: %.2f ms)\n",
                       currentFps, mbps, avgHostMs, avgCapMs, avgEncMs);
                frameCount = 0;
                totalBytesSent = 0;
                totalCapSec = 0;
                totalEncSec = 0;
                lastReport = now;
            }
        }

        encoder.Shutdown();
        server.DisconnectClient();
        if (g_running) {
            printf("[Notice] Client disconnected. Re-listening for connection...\n");
        }
    }

    audioServer.Stop();
    inputServer.Stop();
    capture.Shutdown();
    server.Shutdown();
    WSACleanup();
    timeEndPeriod(1);
    CoUninitialize();
    if (hMutex) CloseHandle(hMutex);
    printf("PC Mirror server stopped cleanly.\n");
    return 0;
}
