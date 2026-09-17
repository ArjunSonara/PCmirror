#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <stdio.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT, &inType, &outType, &activates, &count);

    if (count > 0) {
        ComPtr<IMFTransform> encoder;
        activates[0]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(encoder.As(&api))) {
            VARIANT v; VariantInit(&v);
            v.vt = VT_UI4; v.ulVal = 16000000;
            HRESULT hr = api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
            printf("Dynamic SetValue(AVEncCommonMeanBitRate, 16Mbps): hr=0x%08x\n", hr);
        }
        for (UINT32 i = 0; i < count; i++) activates[i]->Release();
        CoTaskMemFree(activates);
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}
