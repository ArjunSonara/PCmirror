#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <stdio.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_ALL,
        &inType, &outType, &activates, &count);

    for (UINT32 i = 0; i < count; i++) {
        WCHAR name[256] = {};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);
        UINT32 isAsync = 0;
        activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        wprintf(L"\n=== Testing [%u] %s (Async=%u) ===\n", i, name, isAsync);

        ComPtr<IMFTransform> encoder;
        HRESULT hr = activates[i]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
        if (FAILED(hr)) {
            printf("  ActivateObject failed: 0x%08x\n", hr);
            continue;
        }

        if (isAsync) {
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(encoder->GetAttributes(attrs.GetAddressOf()))) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
        }

        // Output type
        ComPtr<IMFMediaType> outMediaType;
        MFCreateMediaType(outMediaType.GetAddressOf());
        outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outMediaType->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
        outMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
        MFSetAttributeRatio(outMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
        MFSetAttributeRatio(outMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
        if (FAILED(hr)) { printf("  SetOutputType failed: 0x%08x\n", hr); continue; }

        // Input type
        ComPtr<IMFMediaType> inMediaType;
        MFCreateMediaType(inMediaType.GetAddressOf());
        inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
        MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
        MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = encoder->SetInputType(0, inMediaType.Get(), 0);
        if (FAILED(hr)) { printf("  SetInputType failed: 0x%08x\n", hr); continue; }

        printf("  Media types configured successfully!\n");

        if (isAsync) {
            ComPtr<IMFMediaEventGenerator> eventGen;
            hr = encoder.As(&eventGen);
            printf("  IMFMediaEventGenerator: hr=0x%08x\n", hr);

            encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

            // Wait for events
            for (int e = 0; e < 5; e++) {
                ComPtr<IMFMediaEvent> ev;
                hr = eventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, ev.GetAddressOf());
                if (SUCCEEDED(hr)) {
                    MediaEventType met = MEUnknown;
                    ev->GetType(&met);
                    printf("    Received event: %d\n", met);
                } else if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                    Sleep(10);
                } else {
                    printf("    GetEvent hr=0x%08x\n", hr);
                    break;
                }
            }
        } else {
            // Synchronous test
            encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

            size_t nv12Size = 1920 * 1080 * 3 / 2;
            std::vector<uint8_t> dummyNv12(nv12Size, 128);

            ComPtr<IMFMediaBuffer> inBuf;
            MFCreateMemoryBuffer((DWORD)nv12Size, inBuf.GetAddressOf());
            BYTE* pDst = nullptr;
            inBuf->Lock(&pDst, nullptr, nullptr);
            memcpy(pDst, dummyNv12.data(), nv12Size);
            inBuf->Unlock();
            inBuf->SetCurrentLength((DWORD)nv12Size);

            ComPtr<IMFSample> inSample;
            MFCreateSample(inSample.GetAddressOf());
            inSample->AddBuffer(inBuf.Get());
            inSample->SetSampleTime(0);
            inSample->SetSampleDuration(166666);

            hr = encoder->ProcessInput(0, inSample.Get(), 0);
            printf("  Sync ProcessInput: hr=0x%08x\n", hr);

            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            DWORD status = 0;
            MFT_OUTPUT_STREAM_INFO sInfo = {};
            encoder->GetOutputStreamInfo(0, &sInfo);
            ComPtr<IMFSample> s;
            ComPtr<IMFMediaBuffer> b;
            if (!(sInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                MFCreateSample(s.GetAddressOf());
                MFCreateMemoryBuffer(sInfo.cbSize ? sInfo.cbSize : 1048576, b.GetAddressOf());
                s->AddBuffer(b.Get());
                outBuf.pSample = s.Get();
            }
            hr = encoder->ProcessOutput(0, 1, &outBuf, &status);
            printf("  Sync ProcessOutput: hr=0x%08x\n", hr);
            if (SUCCEEDED(hr)) {
                IMFSample* res = outBuf.pSample;
                if (res) {
                    DWORD totalLen = 0;
                    res->GetTotalLength(&totalLen);
                    printf("  SUCCESS! Synchronous encoded output: %u bytes!\n", totalLen);
                    res->Release();
                }
            }
        }
    }

    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);
    MFShutdown();
    CoUninitialize();
    return 0;
}
