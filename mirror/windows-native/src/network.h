#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <cstddef>

class NetworkServer {
public:
    bool Init(int port);
    bool AcceptClient();
    bool Send(const uint8_t* data, size_t len);
    bool SendPacket(const uint8_t* data, size_t len);
    void DisconnectClient();
    void Shutdown();

private:
    SOCKET listenSocket_ = INVALID_SOCKET;
    SOCKET clientSocket_ = INVALID_SOCKET;
};
