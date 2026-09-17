#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <stdio.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

void CheckProp(ICodecAPI* api, const GUID& guid, const char* name) {
    VARIANT v;
    VariantInit(&v);
    HRESULT hr = api->GetValue(&guid, &v);
    if (SUCCEEDED(hr)) {
        if (v.vt == VT_BOOL) printf("  %s = %s (S_OK)\n", name, v.boolVal ? "TRUE" : "FALSE");
        else if (v.vt == VT_UI4) printf("  %s = %u (S_OK)\n", name, v.ulVal);
        else printf("  %s: vt=%u (S_OK)\n", name, v.vt);
    } else {
        printf("  %s: GetValue failed hr=0x%08x\n", name, hr);
    }
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL, &inType, &outType, &activates, &count);

    for (UINT32 i = 0; i < count; i++) {
        WCHAR name[256] = {};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);
        UINT32 isAsync = 0;
        activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        wprintf(L"\n--- Encoder [%u]: %s (Async=%u) ---\n", i, name, isAsync);

        ComPtr<IMFTransform> encoder;
        HRESULT hr = activates[i]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
        if (FAILED(hr)) {
            printf("ActivateObject failed: 0x%08x\n", hr);
            continue;
        }

        ComPtr<ICodecAPI> api;
        hr = encoder.As(&api);
        if (SUCCEEDED(hr)) {
            printf("ICodecAPI supported:\n");
            CheckProp(api.Get(), CODECAPI_AVLowLatencyMode, "AVLowLatencyMode");
            CheckProp(api.Get(), CODECAPI_AVEncCommonRealTime, "AVEncCommonRealTime");
            CheckProp(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, "AVEncMPVDefaultBPictureCount");
            CheckProp(api.Get(), CODECAPI_AVEncCommonRateControlMode, "AVEncCommonRateControlMode");
            CheckProp(api.Get(), CODECAPI_AVEncCommonQualityVsSpeed, "AVEncCommonQualityVsSpeed");

            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
            HRESULT hrSet = api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            printf("  SetValue(AVLowLatencyMode, TRUE): hr=0x%08x\n", hrSet);

            v.vt = VT_UI4; v.ulVal = 0;
            hrSet = api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
            printf("  SetValue(AVEncMPVDefaultBPictureCount, 0): hr=0x%08x\n", hrSet);
        } else {
            printf("ICodecAPI NOT supported: hr=0x%08x\n", hr);
        }
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    MFShutdown();
    CoUninitialize();
    return 0;
}
