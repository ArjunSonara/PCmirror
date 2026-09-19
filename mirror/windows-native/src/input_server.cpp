#include "input_server.h"
#include <ws2tcpip.h>
#include <stdio.h>

#pragma pack(push, 1)
struct ControlPacket {
    uint8_t type; // 0 = DOWN, 1 = MOVE, 2 = UP, 10 = BITRATE, 11 = RESOLUTION, 12 = KEYFRAME
    float p1;     // x coord OR bitrate in Mbps OR width
    float p2;     // y coord OR height
};
#pragma pack(pop)

InputServer::InputServer() {}

InputServer::~InputServer() {
    Stop();
}

bool InputServer::Start(int port) {
    running_ = true;
    worker_ = std::thread(&InputServer::Run, this, port);
    return true;
}

void InputServer::Stop() {
    running_ = false;
    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void InputServer::Run(int port) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    listenSocket_ = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (listenSocket_ == INVALID_SOCKET) {
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }
    if (listenSocket_ == INVALID_SOCKET) return;

    SetHandleInformation((HANDLE)listenSocket_, HANDLE_FLAG_INHERIT, 0);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return;
    }

    if (listen(listenSocket_, 1) == SOCKET_ERROR) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return;
    }

    printf("[InputServer] Listening for touch and control input on port %d...\n", port);

    while (running_) {
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET client = accept(listenSocket_, (sockaddr*)&clientAddr, &addrLen);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }

        BOOL nodelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
        printf("[InputServer] Client connected for touch & control backchannel!\n");

        ControlPacket pkt;
        while (running_) {
            int received = 0;
            char* ptr = (char*)&pkt;
            while (received < sizeof(pkt)) {
                int r = recv(client, ptr + received, (int)(sizeof(pkt) - received), 0);
                if (r <= 0) goto client_done;
                received += r;
            }

            // Command 10: Dynamic Bitrate Change
            if (pkt.type == 10) {
                uint32_t bps = (pkt.p1 > 10000.0f) ? (uint32_t)pkt.p1 : (uint32_t)(pkt.p1 * 1000000.0f);
                printf("[InputServer] Received bitrate change request: %.1f Mbps (%u bps)\n", bps / 1000000.0f, bps);
                fflush(stdout);
                if (OnBitrateChange) OnBitrateChange(bps);
                continue;
            }

            // Command 11: Dynamic Resolution Change
            if (pkt.type == 11) {
                uint32_t w = (uint32_t)pkt.p1;
                uint32_t h = (uint32_t)pkt.p2;
                printf("[InputServer] Received resolution change request: %ux%u\n", w, h);
                fflush(stdout);
                if (OnResolutionChange) OnResolutionChange(w, h);
                continue;
            }

            // Command 12: Force IDR Keyframe Request
            if (pkt.type == 12) {
                printf("[InputServer] Received IDR keyframe request\n");
                fflush(stdout);
                if (OnKeyframeRequest) OnKeyframeRequest();
                continue;
            }

            // Command 13: Dynamic Frame Rate (FPS) Change
            if (pkt.type == 13) {
                uint32_t reqFps = (uint32_t)pkt.p1;
                printf("[InputServer] Received FPS change request: %u FPS\n", reqFps);
                fflush(stdout);
                if (OnFpsChange) OnFpsChange(reqFps);
                continue;
            }

            // Command 14: Dynamic Cursor Visibility Toggle (1.0f = Visible, 0.0f = Hidden)
            if (pkt.type == 14) {
                bool visible = (pkt.p1 > 0.5f);
                printf("[InputServer] Received cursor visibility toggle: %s\n", visible ? "VISIBLE" : "HIDDEN");
                fflush(stdout);
                if (OnCursorToggle) OnCursorToggle(visible);
                continue;
            }

            // Command 15: Adaptive Real-Time Congestion Feedback (scale factor)
            if (pkt.type == 15) {
                float scale = pkt.p1;
                if (scale > 0.4f && scale < 2.0f) {
                    if (OnCongestionScale) OnCongestionScale(scale);
                }
                continue;
            }

            // Command 16: Dynamic Video Codec Change (0 = H.264, 1 = H.265/HEVC)
            if (pkt.type == 16) {
                uint32_t codecId = (uint32_t)pkt.p1;
                printf("[InputServer] Received codec change request: %s\n", codecId == 1 ? "HEVC (H.265)" : "H.264 (AVC)");
                fflush(stdout);
                if (OnCodecChange) OnCodecChange(codecId);
                continue;
            }

            // Touch event handling (0 = DOWN, 1 = MOVE, 2 = UP)
            float clampedX = pkt.p1 < 0.0f ? 0.0f : (pkt.p1 > 1.0f ? 1.0f : pkt.p1);
            float clampedY = pkt.p2 < 0.0f ? 0.0f : (pkt.p2 > 1.0f ? 1.0f : pkt.p2);

            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dx = (LONG)(clampedX * 65535.0f);
            input.mi.dy = (LONG)(clampedY * 65535.0f);
            input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

            if (pkt.type == 0) { // DOWN
                input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
            } else if (pkt.type == 2) { // UP
                input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
            }

            SendInput(1, &input, sizeof(INPUT));
        }

    client_done:
        closesocket(client);
        printf("[InputServer] Control client disconnected.\n");
    }

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }
}
