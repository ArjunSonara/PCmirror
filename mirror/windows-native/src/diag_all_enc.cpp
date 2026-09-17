#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdio.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_ALL,
        &inType, &outType, &activates, &count);

    printf("Total H.264 encoders (ALL): count=%u\n", count);
    for (UINT32 i = 0; i < count; i++) {
        WCHAR name[256] = {};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);
        UINT32 isAsync = 0;
        activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        UINT32 isHw = 0;
        activates[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &isHw);

        wprintf(L"[%u] %s | Async=%u, Flags=0x%X\n", i, name, isAsync, isHw);
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    // Also check with inType = I420 or YUY2
    MFT_REGISTER_TYPE_INFO inTypeYUY2 = { MFMediaType_Video, MFVideoFormat_YUY2 };
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_ALL,
        &inTypeYUY2, &outType, &activates, &count);
    printf("\nTotal H.264 encoders with YUY2: count=%u\n", count);
    for (UINT32 i = 0; i < count; i++) {
        WCHAR name[256] = {};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);
        UINT32 isAsync = 0;
        activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        wprintf(L"[%u] %s | Async=%u\n", i, name, isAsync);
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    MFShutdown();
    CoUninitialize();
    return 0;
}
