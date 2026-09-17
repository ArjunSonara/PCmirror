#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <stdio.h>
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
    printf("Found %u hardware encoders\n", count);

    for (UINT32 i = 0; i < count; i++) {
        WCHAR name[256] = {};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);
        wprintf(L"Encoder %u: %s\n", i, name);

        ComPtr<IMFTransform> encoder;
        HRESULT hr = activates[i]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
        printf("  ActivateObject: hr=0x%08x\n", hr);
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    MFShutdown();
    CoUninitialize();
    return 0;
}
