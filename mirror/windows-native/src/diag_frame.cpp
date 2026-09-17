#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int main() {
    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (hDesk) SetThreadDesktop(hDesk);

    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));

    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(0, adapter.GetAddressOf());

    ComPtr<IDXGIOutput> output;
    adapter->EnumOutputs(0, output.GetAddressOf());

    D3D_FEATURE_LEVEL fl;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(), &fl, ctx.GetAddressOf());

    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);

    ComPtr<IDXGIOutputDuplication> dupl;
    HRESULT hr = output1->DuplicateOutput(dev.Get(), dupl.GetAddressOf());
    printf("DuplicateOutput: hr=0x%08x\n", hr);
    if (SUCCEEDED(hr)) {
        DXGI_OUTDUPL_DESC desc;
        dupl->GetDesc(&desc);
        printf("Screen mode: %ux%u, format: %d\n", desc.ModeDesc.Width, desc.ModeDesc.Height, desc.ModeDesc.Format);

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> res;
        hr = dupl->AcquireNextFrame(500, &frameInfo, res.GetAddressOf());
        printf("AcquireNextFrame: hr=0x%08x, TotalMetadataBufferSize=%u\n", hr, frameInfo.TotalMetadataBufferSize);
        if (SUCCEEDED(hr)) {
            printf("Acquired frame successfully!\n");
            dupl->ReleaseFrame();
        }
    }
    return 0;
}
