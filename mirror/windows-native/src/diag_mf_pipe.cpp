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
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inType, &outType, &activates, &count);

    ComPtr<IMFTransform> encoder;
    for (UINT32 i = 0; i < count; i++) {
        HRESULT hr = activates[i]->ActivateObject(IID_PPV_ARGS(encoder.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(hr)) {
            // Unlock async
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(encoder->GetAttributes(attrs.GetAddressOf()))) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }

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
            if (FAILED(hr)) continue;

            ComPtr<IMFMediaType> inMediaType;
            MFCreateMediaType(inMediaType.GetAddressOf());
            inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            hr = encoder->SetInputType(0, inMediaType.Get(), 0);
            if (FAILED(hr)) continue;

            printf("Using encoder %u\n", i);
            break;
        }
    }
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    if (!encoder) {
        printf("No working encoder found!\n");
        return 1;
    }

    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder.As(&codecApi))) {
        VARIANT v; VariantInit(&v);
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
        codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.vt = VT_UI4; v.ulVal = 60;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
    }

    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // Create a dummy 1920x1080 NV12 frame (Y=128, UV=128)
    size_t nv12Size = 1920 * 1080 * 3 / 2;
    std::vector<uint8_t> dummyNv12(nv12Size, 128);

    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    encoder->GetOutputStreamInfo(0, &streamInfo);

    int totalNals = 0;
    for (int f = 0; f < 5; f++) {
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
        LONGLONG duration = (LONGLONG)(10000000.0 / 60.0);
        inSample->SetSampleTime(f * duration);
        inSample->SetSampleDuration(duration);

        HRESULT hr = encoder->ProcessInput(0, inSample.Get(), 0);
        printf("Frame %d ProcessInput: hr=0x%08x\n", f, hr);

        // Drain output
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            outBuf.dwStreamID = 0;
            DWORD status = 0;
            ComPtr<IMFSample> outSample;
            ComPtr<IMFMediaBuffer> outMemBuf;

            if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                MFCreateSample(outSample.GetAddressOf());
                MFCreateMemoryBuffer(streamInfo.cbSize ? streamInfo.cbSize : (1024 * 1024), outMemBuf.GetAddressOf());
                outSample->AddBuffer(outMemBuf.Get());
                outBuf.pSample = outSample.Get();
            }

            hr = encoder->ProcessOutput(0, 1, &outBuf, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (FAILED(hr)) {
                printf("  ProcessOutput: hr=0x%08x\n", hr);
                break;
            }

            IMFSample* resSample = outBuf.pSample;
            if (resSample) {
                ComPtr<IMFMediaBuffer> contiguous;
                resSample->ConvertToContiguousBuffer(contiguous.GetAddressOf());
                BYTE* pData = nullptr; DWORD len = 0;
                contiguous->Lock(&pData, nullptr, &len);
                printf("  Encoded frame ready! Bytes: %u. First 4 bytes: %02X %02X %02X %02X\n",
                    len, pData[0], pData[1], pData[2], pData[3]);
                totalNals++;
                contiguous->Unlock();
                resSample->Release();
            }
            if (outBuf.pEvents) outBuf.pEvents->Release();
        }
    }

    printf("Total encoded packets drained: %d\n", totalNals);
    MFShutdown();
    CoUninitialize();
    return 0;
}
