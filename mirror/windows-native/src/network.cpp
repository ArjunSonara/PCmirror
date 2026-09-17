#include "network.h"
#include <stdio.h>

bool NetworkServer::Init(int port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;

    // Use WSA_FLAG_NO_HANDLE_INHERIT to strictly prevent child processes (like adb.exe) from inheriting the socket
    listenSocket_ = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (listenSocket_ == INVALID_SOCKET) {
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }
    if (listenSocket_ == INVALID_SOCKET) return false;

    SetHandleInformation((HANDLE)listenSocket_, HANDLE_FLAG_INHERIT, 0);

    BOOL yes = TRUE;
    setsockopt(listenSocket_, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0
    addr.sin_port = htons((u_short)port);

    int bindResult = SOCKET_ERROR;
    for (int retry = 0; retry < 5; ++retry) {
        bindResult = bind(listenSocket_, (sockaddr*)&addr, sizeof(addr));
        if (bindResult != SOCKET_ERROR) break;
        int err = WSAGetLastError();
        if (err == WSAEADDRINUSE) {
            printf("[Notice] Port %d is temporarily busy, waiting for release (retry %d/5)...\n", port, retry + 1);
            Sleep(600);
        } else {
            break;
        }
    }

    if (bindResult == SOCKET_ERROR) {
        printf("\n[ERROR] Could not bind to port %d (Winsock error %d).\n", port, WSAGetLastError());
        printf("This means another instance or app is using port %d.\n", port);
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listenSocket_, 1) == SOCKET_ERROR) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    printf("Listening on 0.0.0.0:%d -- reachable over USB RNDIS and adb reverse.\n", port);
    return true;
}

bool NetworkServer::AcceptClient() {
    printf("Waiting for client connection...\n");
    clientSocket_ = accept(listenSocket_, nullptr, nullptr);
    if (clientSocket_ == INVALID_SOCKET) return false;

    BOOL yes = TRUE;
    setsockopt(clientSocket_, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));

    // Increase socket send buffer to 2MB to prevent TCP stall on keyframes
    int sndBuf = 2 * 1024 * 1024;
    setsockopt(clientSocket_, SOL_SOCKET, SO_SNDBUF, (const char*)&sndBuf, sizeof(sndBuf));

    printf("Client connected successfully!\n");
    return true;
}

bool NetworkServer::Send(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(clientSocket_, (const char*)data + sent, (int)(len - sent), 0);
        if (n <= 0) {
            printf("send() failed with error %d\n", WSAGetLastError());
            return false;
        }
        sent += n;
    }
    return true;
}

bool NetworkServer::SendPacket(const uint8_t* data, size_t len) {
    if (clientSocket_ == INVALID_SOCKET || len == 0) return false;

    uint32_t netLen = htonl((uint32_t)len);
    DWORD sent = 0;
    DWORD total = 4 + (DWORD)len;

    while (sent < total) {
        DWORD curSent = 0;
        WSABUF curBufs[2];
        DWORD bufCount = 0;

        if (sent < 4) {
            curBufs[0].len = 4 - sent;
            curBufs[0].buf = ((char*)&netLen) + sent;
            curBufs[1].len = (ULONG)len;
            curBufs[1].buf = (char*)data;
            bufCount = 2;
        } else {
            curBufs[0].len = total - sent;
            curBufs[0].buf = ((char*)data) + (sent - 4);
            bufCount = 1;
        }

        int res = WSASend(clientSocket_, curBufs, bufCount, &curSent, 0, nullptr, nullptr);
        if (res != 0 || curSent == 0) {
            printf("WSASend() failed with error %d\n", WSAGetLastError());
            return false;
        }
        sent += curSent;
    }
    return true;
}

void NetworkServer::DisconnectClient() {
    if (clientSocket_ != INVALID_SOCKET) {
        shutdown(clientSocket_, SD_BOTH);
        closesocket(clientSocket_);
        clientSocket_ = INVALID_SOCKET;
    }
}

void NetworkServer::Shutdown() {
    DisconnectClient();
    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }
    WSACleanup();
}
