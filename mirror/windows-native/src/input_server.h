#pragma once
#include <winsock2.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>

class InputServer {
public:
    InputServer();
    ~InputServer();

    bool Start(int port = 8081);
    void Stop();

    std::function<void(uint32_t bitrateBps)> OnBitrateChange;
    std::function<void(uint32_t width, uint32_t height)> OnResolutionChange;
    std::function<void()> OnKeyframeRequest;

private:
    void Run(int port);
    std::thread worker_;
    std::atomic<bool> running_{ false };
    SOCKET listenSocket_ = INVALID_SOCKET;
};
