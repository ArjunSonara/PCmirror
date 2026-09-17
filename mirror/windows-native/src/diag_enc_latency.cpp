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
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT, &inType, &outType, &activates, &count);
    if (count == 0) return 1;

    ComPtr<IMFTransform> encoder;
    activates[0]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    // Output
    ComPtr<IMFMediaType> outTypeObj;
    MFCreateMediaType(outTypeObj.GetAddressOf());
    outTypeObj->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outTypeObj->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outTypeObj->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
    outTypeObj->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outTypeObj.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(outTypeObj.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(outTypeObj.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outTypeObj->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    encoder->SetOutputType(0, outTypeObj.Get(), 0);

    // Input
    ComPtr<IMFMediaType> inTypeObj;
    MFCreateMediaType(inTypeObj.GetAddressOf());
    inTypeObj->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inTypeObj->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inTypeObj->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inTypeObj.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(inTypeObj.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(inTypeObj.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    encoder->SetInputType(0, inTypeObj.Get(), 0);

    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder.As(&codecApi))) {
        VARIANT v; VariantInit(&v);
        // Low latency mode
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        // Real-time mode
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVEncCommonRealTime, &v);
        // B-frames = 0
        v.vt = VT_UI4; v.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
        // Quality vs speed = 0 (fastest / lowest latency)
        v.vt = VT_UI4; v.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v);
        // Rate control CBR
        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
        codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        // GOP size
        v.vt = VT_UI4; v.ulVal = 60;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
    }

    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    MFT_OUTPUT_STREAM_INFO sInfo = {};
    encoder->GetOutputStreamInfo(0, &sInfo);

    size_t nv12Size = 1920 * 1080 * 3 / 2;
    std::vector<uint8_t> dummyNv12(nv12Size, 128);

    int totalOutputs = 0;
    for (int f = 0; f < 30; f++) {
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

        encoder->ProcessInput(0, inSample.Get(), 0);

        for (;;) {
            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            DWORD status = 0;
            ComPtr<IMFSample> s;
            ComPtr<IMFMediaBuffer> b;
            MFCreateSample(s.GetAddressOf());
            MFCreateMemoryBuffer(sInfo.cbSize ? sInfo.cbSize : 1048576, b.GetAddressOf());
            s->AddBuffer(b.Get());
            outBuf.pSample = s.Get();

            HRESULT hr = encoder->ProcessOutput(0, 1, &outBuf, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (FAILED(hr)) { printf("ProcessOutput failed: 0x%08x\n", hr); break; }

            DWORD len = 0;
            outBuf.pSample->GetTotalLength(&len);
            printf("Input frame %2d -> Output %2d produced: %u bytes\n", f, totalOutputs, len);
            totalOutputs++;
        }
    }

    printf("Finished! Total outputs: %d / 30 inputs\n", totalOutputs);
    MFShutdown();
    CoUninitialize();
    return 0;
}
