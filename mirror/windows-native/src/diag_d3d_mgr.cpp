#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    // Create D3D11 device on default or primary adapter
    D3D_FEATURE_LEVEL fl;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        device.GetAddressOf(), &fl, context.GetAddressOf());

    // Enable multithreaded protection on D3D11 device for MF
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    // Create DXGI Device Manager
    UINT resetToken = 0;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, dxgiManager.GetAddressOf());
    printf("MFCreateDXGIDeviceManager: hr=0x%08x\n", hr);
    hr = dxgiManager->ResetDevice(device.Get(), resetToken);
    printf("ResetDevice: hr=0x%08x\n", hr);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inType, &outType, &activates, &count);
    printf("Found %u hardware encoders\n", count);

    for (UINT32 i = 0; i < count; i++) {
        WCHAR name[256] = {};
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);
        wprintf(L"\nEncoder %u: %s\n", i, name);

        ComPtr<IMFTransform> encoder;
        hr = activates[i]->ActivateObject(IID_PPV_ARGS(encoder.GetAddressOf()));
        printf("  ActivateObject: hr=0x%08x\n", hr);
        if (SUCCEEDED(hr)) {
            // Unlock async
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(encoder->GetAttributes(attrs.GetAddressOf()))) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }

            // Set D3D Device Manager
            hr = encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)dxgiManager.Get());
            printf("  Set D3D Manager: hr=0x%08x\n", hr);

            // Output type
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
            printf("  SetOutputType: hr=0x%08x\n", hr);

            // Input type
            ComPtr<IMFMediaType> inMediaType;
            MFCreateMediaType(inMediaType.GetAddressOf());
            inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            hr = encoder->SetInputType(0, inMediaType.Get(), 0);
            printf("  SetInputType: hr=0x%08x\n", hr);
        }
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    MFShutdown();
    CoUninitialize();
    return 0;
}
