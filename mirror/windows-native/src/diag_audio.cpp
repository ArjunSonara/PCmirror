#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

#pragma comment(lib, "ole32.lib")

int main() {
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) return 1;

    IMMDeviceEnumerator* pEnumerator = NULL;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) { printf("Failed to create enumerator: 0x%08lx\n", hr); return 1; }

    IMMDevice* pDevice = NULL;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) { printf("Failed to get default endpoint: 0x%08lx\n", hr); return 1; }

    IAudioClient* pAudioClient = NULL;
    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
    if (FAILED(hr)) { printf("Failed to activate audio client: 0x%08lx\n", hr); return 1; }

    WAVEFORMATEX* pwfx = NULL;
    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) { printf("Failed to get mix format: 0x%08lx\n", hr); return 1; }

    printf("Default audio format: %lu Hz, %u channels, %u bits/sample, tag %u\n",
           pwfx->nSamplesPerSec, pwfx->nChannels, pwfx->wBitsPerSample, pwfx->wFormatTag);

    // Initialize loopback
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, pwfx, NULL);
    if (FAILED(hr)) { printf("Failed to init loopback: 0x%08lx\n", hr); return 1; }

    IAudioCaptureClient* pCaptureClient = NULL;
    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
    if (FAILED(hr)) { printf("Failed to get capture client: 0x%08lx\n", hr); return 1; }

    hr = pAudioClient->Start();
    if (FAILED(hr)) { printf("Failed to start audio client: 0x%08lx\n", hr); return 1; }

    printf("WASAPI Loopback capture initialized and started successfully!\n");

    pAudioClient->Stop();
    CoTaskMemFree(pwfx);
    pCaptureClient->Release();
    pAudioClient->Release();
    pDevice->Release();
    pEnumerator->Release();
    CoUninitialize();
    return 0;
}
