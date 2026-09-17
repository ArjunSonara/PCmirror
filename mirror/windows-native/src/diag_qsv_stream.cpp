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
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(encoder.ReleaseAndGetAddressOf())))) {
            ComPtr<IMFAttributes> attrs;
            encoder->GetAttributes(attrs.GetAddressOf());
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);

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
            if (FAILED(encoder->SetOutputType(0, outMediaType.Get(), 0))) continue;

            ComPtr<IMFMediaType> inMediaType;
            MFCreateMediaType(inMediaType.GetAddressOf());
            inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (FAILED(encoder->SetInputType(0, inMediaType.Get(), 0))) continue;

            printf("Successfully activated and configured Hardware Encoder %u!\n", i);
            break;
        }
    }
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

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

    ComPtr<IMFMediaEventGenerator> eventGen;
    encoder.As(&eventGen);

    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    size_t nv12Size = 1920 * 1080 * 3 / 2;
    std::vector<uint8_t> dummyNv12(nv12Size, 128);

    LONGLONG frameNum = 0;
    int encodedPackets = 0;

    for (int iter = 0; iter < 100 && encodedPackets < 5; iter++) {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = eventGen->GetEvent(0, ev.GetAddressOf()); // Wait for next event
        if (FAILED(hr)) {
            printf("GetEvent failed: 0x%08x\n", hr);
            break;
        }
        MediaEventType met = MEUnknown;
        ev->GetType(&met);

        if (met == METransformNeedInput) {
            // Provide a frame
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
            inSample->SetSampleTime(frameNum * 166666);
            inSample->SetSampleDuration(166666);
            frameNum++;

            hr = encoder->ProcessInput(0, inSample.Get(), 0);
            printf("ProcessInput (frame %lld): hr=0x%08x\n", frameNum - 1, hr);
        } else if (met == METransformHaveOutput) {
            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            DWORD status = 0;
            hr = encoder->ProcessOutput(0, 1, &outBuf, &status);
            printf("ProcessOutput: hr=0x%08x\n", hr);
            if (SUCCEEDED(hr) && outBuf.pSample) {
                ComPtr<IMFMediaBuffer> contiguous;
                outBuf.pSample->ConvertToContiguousBuffer(contiguous.GetAddressOf());
                BYTE* pData = nullptr; DWORD len = 0;
                contiguous->Lock(&pData, nullptr, &len);
                printf("  >>> HARDWARE ENCODED PACKET: %u bytes! Start bytes: %02X %02X %02X %02X %02X\n",
                    len, pData[0], pData[1], pData[2], pData[3], pData[4]);
                contiguous->Unlock();
                outBuf.pSample->Release();
                encodedPackets++;
            }
            if (outBuf.pEvents) outBuf.pEvents->Release();
        }
    }

    printf("Total hardware encoded packets received: %d\n", encodedPackets);
    MFShutdown();
    CoUninitialize();
    return 0;
}
