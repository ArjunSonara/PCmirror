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

extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

using Microsoft::WRL::ComPtr;

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inType, &outType, &activates, &count);

    ComPtr<IMFTransform> encoder;
    activates[0]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    // Unlock async if needed
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(encoder->GetAttributes(attrs.GetAddressOf()))) {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }

    // Set output type
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
    HRESULT hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
    printf("NVIDIA SetOutputType: hr=0x%08x\n", hr);

    // Set input type
    ComPtr<IMFMediaType> inMediaType;
    MFCreateMediaType(inMediaType.GetAddressOf());
    inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = encoder->SetInputType(0, inMediaType.Get(), 0);
    printf("NVIDIA SetInputType: hr=0x%08x\n", hr);

    // CodecAPI low-latency tuning
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder.As(&codecApi))) {
        VARIANT v; VariantInit(&v);
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
        codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.vt = VT_UI4; v.ulVal = 60;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        v.vt = VT_UI4; v.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
        printf("Configured NVIDIA CodecAPI for ultra-low latency!\n");
    }

    MFT_OUTPUT_STREAM_INFO sInfo = {};
    encoder->GetOutputStreamInfo(0, &sInfo);
    printf("NVIDIA OutputStreamInfo: cbSize=%u, dwFlags=0x%08x\n", sInfo.cbSize, sInfo.dwFlags);

    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    size_t nv12Size = 1920 * 1080 * 3 / 2;
    std::vector<uint8_t> dummyNv12(nv12Size, 128);

    int totalOutputs = 0;
    for (int f = 0; f < 10; f++) {
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

        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        hr = encoder->ProcessInput(0, inSample.Get(), 0);
        QueryPerformanceCounter(&t1);
        double msInput = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        printf("Frame %d ProcessInput: hr=0x%08x (%.2f ms)\n", f, hr, msInput);

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
            if (FAILED(hr)) { printf("  ProcessOutput failed: 0x%08x\n", hr); break; }

            IMFSample* res = outBuf.pSample;
            if (res) {
                DWORD len = 0;
                res->GetTotalLength(&len);
                printf("  >>> NVIDIA NVENC OUTPUT: %u bytes in %.2f ms!\n", len, msOutput);
                totalOutputs++;
                res->Release();
            }
            if (outBuf.pEvents) outBuf.pEvents->Release();
        }
    }

    printf("Done! Total outputs: %d\n", totalOutputs);
    MFShutdown();
    CoUninitialize();
    return 0;
}
