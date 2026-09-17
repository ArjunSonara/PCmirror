#include "capture.h"
#include "encoder.h"
#include <stdio.h>
#include <windows.h>
#include <timeapi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "winmm.lib")

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);

    printf("============================================================\n");
    printf("   PC Mirror: Zero-Latency Pipeline Verification Benchmark   \n");
    printf("============================================================\n");

    DesktopCapture capture;
    if (!capture.Init()) {
        printf("Desktop duplication Init failed.\n");
        return 1;
    }

    UINT w = capture.GetWidth();
    UINT h = capture.GetHeight();
    printf("Desktop Resolution: %ux%u\n", w, h);

    HwEncoder encoder;
    if (!encoder.Init(w, h, 60, 8000000)) {
        printf("HwEncoder Init failed.\n");
        return 1;
    }

    int nalsReceived = 0;
    size_t totalBytesReceived = 0;
    encoder.OnNal = [&](const uint8_t* data, size_t len) {
        nalsReceived++;
        totalBytesReceived += len;
    };

    std::vector<uint8_t> bgra;
    UINT stride = 0;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    // Grab first frame to ensure DWM is active
    mouse_event(MOUSEEVENTF_MOVE, 0, 0, 0, 0);
    while (!capture.GrabFrame(bgra, w, h, stride, 50)) {
        mouse_event(MOUSEEVENTF_MOVE, 0, 0, 0, 0);
        Sleep(10);
    }

    printf("First frame captured: %ux%u (stride=%u, size=%zu bytes)\n", w, h, stride, bgra.size());

    // Test 10 consecutive frames through capture + encode
    double totalCapMs = 0;
    double totalEncMs = 0;

    for (int i = 0; i < 10; i++) {
        mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);

        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        bool got = capture.GrabFrame(bgra, w, h, stride, 50);
        QueryPerformanceCounter(&t1);

        if (!got) {
            printf("Frame %d grab timeout.\n", i);
            continue;
        }

        int prevNals = nalsReceived;
        encoder.EncodeFrame(bgra, stride);
        QueryPerformanceCounter(&t2);

        double capMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        double encMs = (t2.QuadPart - t1.QuadPart) * 1000.0 / freq.QuadPart;
        double hostMs = capMs + encMs;

        totalCapMs += capMs;
        totalEncMs += encMs;

        printf("Frame %d: Capture = %.2f ms | Encode = %.2f ms | Host Total = %.2f ms | NALs Out = %d\n",
            i, capMs, encMs, hostMs, nalsReceived - prevNals);
    }

    printf("------------------------------------------------------------\n");
    printf("Summary across 10 frames:\n");
    printf("  Average Capture Latency: %.2f ms\n", totalCapMs / 10.0);
    printf("  Average Encode Latency:  %.2f ms (including SIMD AVX2)\n", totalEncMs / 10.0);
    printf("  Average Host Latency:    %.2f ms\n", (totalCapMs + totalEncMs) / 10.0);
    printf("  Total NALs generated:    %d\n", nalsReceived);
    printf("============================================================\n");

    encoder.Shutdown();
    capture.Shutdown();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
