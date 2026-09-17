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

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

void TestEncoder(IMFActivate* act, int encIndex) {
    WCHAR name[256] = {};
    act->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);
    wprintf(L"\n=======================================================\n");
    wprintf(L"Testing Encoder [%d]: %s\n", encIndex, name);
    wprintf(L"=======================================================\n");

    ComPtr<IMFTransform> encoder;
    HRESULT hr = act->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
    if (FAILED(hr)) {
        printf("ActivateObject failed: 0x%08x\n", hr);
        return;
    }

    // Check async
    UINT32 isAsync = 0;
    act->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
    if (isAsync) {
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(encoder->GetAttributes(attrs.GetAddressOf()))) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }
    }

    // Set CodecAPI properties BEFORE setting media types
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(encoder.As(&api))) {
        VARIANT v; VariantInit(&v);
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        hr = api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        printf("Set AVLowLatencyMode=TRUE: hr=0x%08x\n", hr);

        v.vt = VT_UI4; v.ulVal = 0;
        hr = api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
        printf("Set AVEncMPVDefaultBPictureCount=0: hr=0x%08x\n", hr);

        v.vt = VT_UI4; v.ulVal = 0; // Highest speed / lowest latency
        hr = api->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v);
        printf("Set AVEncCommonQualityVsSpeed=0: hr=0x%08x\n", hr);

        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
        hr = api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        printf("Set RateControl=CBR: hr=0x%08x\n", hr);

        v.vt = VT_UI4; v.ulVal = 120;
        hr = api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        printf("Set GOPSize=120: hr=0x%08x\n", hr);
    }

    // Output type
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(outType.GetAddressOf());
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    hr = encoder->SetOutputType(0, outType.Get(), 0);
    printf("SetOutputType: hr=0x%08x\n", hr);
    if (FAILED(hr)) return;

    // Input type
    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(inType.GetAddressOf());
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = encoder->SetInputType(0, inType.Get(), 0);
    printf("SetInputType: hr=0x%08x\n", hr);
    if (FAILED(hr)) return;

    MFT_OUTPUT_STREAM_INFO sInfo = {};
    encoder->GetOutputStreamInfo(0, &sInfo);

    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    size_t nv12Size = 1920 * 1080 * 3 / 2;
    std::vector<uint8_t> dummyNv12(nv12Size, 128);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    int totalOutputs = 0;
    int firstOutputFrame = -1;

    for (int f = 0; f < 20; f++) {
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
        inSample->SetSampleTime(f * 166666);
        inSample->SetSampleDuration(166666);

        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        hr = encoder->ProcessInput(0, inSample.Get(), 0);
        QueryPerformanceCounter(&t1);
        double msInput = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;

        // Drain
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            DWORD status = 0;
            ComPtr<IMFSample> s;
            ComPtr<IMFMediaBuffer> b;
            if (!(sInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                MFCreateSample(s.GetAddressOf());
                MFCreateMemoryBuffer(sInfo.cbSize ? sInfo.cbSize : 1048576, b.GetAddressOf());
                s->AddBuffer(b.Get());
                outBuf.pSample = s.Get();
            }

            QueryPerformanceCounter(&t0);
            hr = encoder->ProcessOutput(0, 1, &outBuf, &status);
            QueryPerformanceCounter(&t1);
            double msOutput = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (FAILED(hr)) break;

            IMFSample* outS = outBuf.pSample;
            if (outS) {
                DWORD len = 0;
                outS->GetTotalLength(&len);
                if (firstOutputFrame == -1) firstOutputFrame = f;
                printf("  [Input Frame %2d] -> Output %2d: %u bytes (Encode time: %.2f ms)\n",
                    f, totalOutputs, len, msInput + msOutput);
                totalOutputs++;
                if (sInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) outS->Release();
            }
            if (outBuf.pEvents) outBuf.pEvents->Release();
        }
    }

    printf("Result for [%s]:\n", name);
    printf("  First output produced at input frame: %d\n", firstOutputFrame);
    printf("  Pipeline buffer delay: %d frames (%.1f ms at 60fps)\n",
        firstOutputFrame, firstOutputFrame >= 0 ? firstOutputFrame * 16.666 : -1.0);
    printf("  Total outputs: %d / 20\n", totalOutputs);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL, &inType, &outType, &activates, &count);

    for (UINT32 i = 0; i < count; i++) {
        TestEncoder(activates[i], i);
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    MFShutdown();
    CoUninitialize();
    return 0;
}
