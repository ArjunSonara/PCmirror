#pragma once
#include <winsock2.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <cstdint>

class AudioServer {
public:
    AudioServer();
    ~AudioServer();

    bool Start(int port = 8082);
    void Stop();

private:
    void Run(int port);
    std::thread worker_;
    std::atomic<bool> running_{ false };
    SOCKET listenSocket_ = INVALID_SOCKET;
};
