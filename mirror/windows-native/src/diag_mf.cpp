#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <stdio.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        printf("MFStartup failed: 0x%08x\n", hr);
        return 1;
    }

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inType, &outType, &activates, &count);

    printf("MFTEnumEx (HARDWARE): hr=0x%08x, count=%u\n", hr, count);

    for (UINT32 i = 0; i < count; i++) {
        WCHAR name[256] = {};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);
        wprintf(L"\n--- Encoder %u: %s ---\n", i, name);

        ComPtr<IMFTransform> encoder;
        hr = activates[i]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
        printf("  ActivateObject: hr=0x%08x\n", hr);
        if (SUCCEEDED(hr)) {
            // Unlock async MFT if needed
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(encoder->GetAttributes(attrs.GetAddressOf()))) {
                hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                printf("  Set MF_TRANSFORM_ASYNC_UNLOCK: hr=0x%08x\n", hr);
            }

            // Output media type
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
            printf("  SetOutputType (1920x1080@60): hr=0x%08x\n", hr);

            // Input media type
            ComPtr<IMFMediaType> inMediaType;
            MFCreateMediaType(inMediaType.GetAddressOf());
            inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            hr = encoder->SetInputType(0, inMediaType.Get(), 0);
            printf("  SetInputType (1920x1080 NV12): hr=0x%08x\n", hr);

            // Check output stream info
            MFT_OUTPUT_STREAM_INFO streamInfo = {};
            hr = encoder->GetOutputStreamInfo(0, &streamInfo);
            printf("  GetOutputStreamInfo: hr=0x%08x, cbSize=%u, dwFlags=0x%08x\n", hr, streamInfo.cbSize, streamInfo.dwFlags);
            printf("    Provides samples: %s\n", (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) ? "YES" : "NO");

            // CodecAPI
            ComPtr<ICodecAPI> codecApi;
            if (SUCCEEDED(encoder.As(&codecApi))) {
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
                hr = codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
                printf("    SetValue(AVLowLatencyMode): hr=0x%08x\n", hr);

                v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
                hr = codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
                printf("    SetValue(CBR): hr=0x%08x\n", hr);

                v.vt = VT_UI4; v.ulVal = 60;
                hr = codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
                printf("    SetValue(GOP 60): hr=0x%08x\n", hr);
            }

            // Test streaming messages
            encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            printf("  Stream initialized successfully!\n");
        }
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    MFShutdown();
    CoUninitialize();
    return 0;
}
