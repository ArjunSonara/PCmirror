#include "audio_server.h"
#include <ws2tcpip.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "ole32.lib")

static inline int16_t FloatToPcm16(float v) {
    float scaled = v * 32767.0f;
    if (scaled > 32767.0f) return 32767;
    if (scaled < -32768.0f) return -32768;
    return (int16_t)scaled;
}

AudioServer::AudioServer() {}

AudioServer::~AudioServer() {
    Stop();
}

bool AudioServer::Start(int port) {
    running_ = true;
    worker_ = std::thread(&AudioServer::Run, this, port);
    return true;
}

void AudioServer::Stop() {
    running_ = false;
    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AudioServer::Run(int port) {
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

    printf("[AudioServer] Listening for Android audio connection on port %d...\n", port);

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
        int sndBuf = 64 * 1024;
        setsockopt(client, SOL_SOCKET, SO_SNDBUF, (char*)&sndBuf, sizeof(sndBuf));

        printf("[AudioServer] Android client connected for real-time audio streaming!\n");

        HRESULT hrCo = CoInitialize(NULL);

        IMMDeviceEnumerator* pEnumerator = NULL;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
        if (FAILED(hr) || !pEnumerator) {
            printf("[AudioServer] Failed to create MMDeviceEnumerator (0x%08lx)\n", hr);
            closesocket(client);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            continue;
        }

        IMMDevice* pDevice = NULL;
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (FAILED(hr) || !pDevice) {
            printf("[AudioServer] Failed to get default audio endpoint (0x%08lx)\n", hr);
            pEnumerator->Release();
            closesocket(client);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            continue;
        }

        IAudioClient* pAudioClient = NULL;
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
        if (FAILED(hr) || !pAudioClient) {
            printf("[AudioServer] Failed to activate IAudioClient (0x%08lx)\n", hr);
            pDevice->Release();
            pEnumerator->Release();
            closesocket(client);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            continue;
        }

        WAVEFORMATEX* pwfx = NULL;
        hr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr) || !pwfx) {
            printf("[AudioServer] Failed to get mix format (0x%08lx)\n", hr);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            closesocket(client);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            continue;
        }

        uint32_t sampleRate = (uint32_t)pwfx->nSamplesPerSec;
        uint16_t channels = (uint16_t)pwfx->nChannels;
        printf("[AudioServer] Native audio format: %u Hz, %u channels\n", sampleRate, channels);

        // Request 20ms buffer duration (200,000 x 100ns)
        REFERENCE_TIME hnsRequestedDuration = 200000;
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, hnsRequestedDuration, 0, pwfx, NULL);
        if (FAILED(hr)) {
            printf("[AudioServer] Failed to initialize audio client in loopback mode (0x%08lx)\n", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            closesocket(client);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            continue;
        }

        IAudioCaptureClient* pCaptureClient = NULL;
        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        if (FAILED(hr) || !pCaptureClient) {
            printf("[AudioServer] Failed to get IAudioCaptureClient (0x%08lx)\n", hr);
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            closesocket(client);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            continue;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            printf("[AudioServer] Failed to start audio client (0x%08lx)\n", hr);
            pCaptureClient->Release();
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            pDevice->Release();
            pEnumerator->Release();
            closesocket(client);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            continue;
        }

        // Send 8-byte handshake: [0x50434D41 ("PCMA"), sampleRate]
        uint32_t handshake[2];
        handshake[0] = 0x50434D41;
        handshake[1] = sampleRate;
        send(client, (const char*)handshake, 8, 0);

        std::vector<int16_t> pcm;

        while (running_) {
            UINT32 packetLength = 0;
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            if (packetLength == 0) {
                Sleep(5);
                continue;
            }

            BYTE* pData = NULL;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            pcm.resize((size_t)numFramesAvailable * 2);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                memset(pcm.data(), 0, pcm.size() * sizeof(int16_t));
            } else if (pData) {
                const float* fData = (const float*)pData;
                for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                    float l = fData[i * channels + 0];
                    float r = (channels > 1) ? fData[i * channels + 1] : l;
                    pcm[i * 2 + 0] = FloatToPcm16(l);
                    pcm[i * 2 + 1] = FloatToPcm16(r);
                }
            }

            pCaptureClient->ReleaseBuffer(numFramesAvailable);

            // Send converted 16-bit stereo PCM chunk
            int toSend = (int)(pcm.size() * sizeof(int16_t));
            int sent = 0;
            const char* buf = (const char*)pcm.data();
            bool sendFailed = false;

            while (sent < toSend) {
                int s = send(client, buf + sent, toSend - sent, 0);
                if (s <= 0) {
                    sendFailed = true;
                    break;
                }
                sent += s;
            }

            if (sendFailed) {
                printf("[AudioServer] Client disconnected from audio stream.\n");
                break;
            }
        }

        pAudioClient->Stop();
        pCaptureClient->Release();
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        closesocket(client);
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }
}
