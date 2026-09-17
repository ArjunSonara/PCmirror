#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        printf("CreateDXGIFactory1 failed: 0x%08x\n", hr);
        return 1;
    }

    UINT adapterIdx = 0;
    ComPtr<IDXGIAdapter1> adapter;
    while (factory->EnumAdapters1(adapterIdx, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        wprintf(L"Adapter %u: %s (Vendor: 0x%04X, LUID: 0x%08X-%08X)\n",
            adapterIdx, desc.Description, desc.VendorId, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);

        UINT outputIdx = 0;
        ComPtr<IDXGIOutput> output;
        while (adapter->EnumOutputs(outputIdx, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC outDesc;
            output->GetDesc(&outDesc);
            wprintf(L"  Output %u: %s (AttachedToDesktop: %d, DesktopBounds: [%d,%d,%d,%d])\n",
                outputIdx, outDesc.DeviceName, outDesc.AttachedToDesktop,
                outDesc.DesktopCoordinates.left, outDesc.DesktopCoordinates.top,
                outDesc.DesktopCoordinates.right, outDesc.DesktopCoordinates.bottom);

            // Test DuplicateOutput
            D3D_FEATURE_LEVEL fl;
            ComPtr<ID3D11Device> dev;
            ComPtr<ID3D11DeviceContext> ctx;
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(), &fl, ctx.GetAddressOf());
            if (SUCCEEDED(hr)) {
                ComPtr<IDXGIOutput1> output1;
                if (SUCCEEDED(output.As(&output1))) {
                    ComPtr<IDXGIOutputDuplication> dupl;
                    hr = output1->DuplicateOutput(dev.Get(), dupl.GetAddressOf());
                    wprintf(L"    DuplicateOutput test: hr=0x%08x\n", hr);
                }
            } else {
                wprintf(L"    D3D11CreateDevice on adapter failed: hr=0x%08x\n", hr);
            }
            outputIdx++;
        }
        adapterIdx++;
    }
    return 0;
}
