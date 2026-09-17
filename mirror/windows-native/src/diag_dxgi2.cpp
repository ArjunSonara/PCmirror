#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int main() {
    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (hDesk) {
        BOOL ok = SetThreadDesktop(hDesk);
        printf("SetThreadDesktop(hDesk) = %d (err=%u)\n", ok, GetLastError());
    } else {
        printf("OpenInputDesktop failed: %u\n", GetLastError());
    }

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));

    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(0, adapter.GetAddressOf());

    ComPtr<IDXGIOutput> output;
    adapter->EnumOutputs(0, output.GetAddressOf());

    D3D_FEATURE_LEVEL fl;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(), &fl, ctx.GetAddressOf());
    printf("D3D11CreateDevice: hr=0x%08x\n", hr);

    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);

    ComPtr<IDXGIOutputDuplication> dupl;
    hr = output1->DuplicateOutput(dev.Get(), dupl.GetAddressOf());
    printf("DuplicateOutput: hr=0x%08x\n", hr);

    if (FAILED(hr)) {
        // Also test with default adapter (D3D_DRIVER_TYPE_HARDWARE)
        ComPtr<ID3D11Device> dev2;
        ComPtr<ID3D11DeviceContext> ctx2;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev2.GetAddressOf(), &fl, ctx2.GetAddressOf());
        printf("D3D11CreateDevice (default): hr=0x%08x\n", hr);
        if (SUCCEEDED(hr)) {
            ComPtr<IDXGIDevice> dxgiDev;
            dev2.As(&dxgiDev);
            ComPtr<IDXGIAdapter> ad2;
            dxgiDev->GetAdapter(ad2.GetAddressOf());
            DXGI_ADAPTER_DESC desc2;
            ad2->GetDesc(&desc2);
            wprintf(L"Default adapter: %s\n", desc2.Description);
            ComPtr<IDXGIOutput> out2;
            hr = ad2->EnumOutputs(0, out2.GetAddressOf());
            printf("ad2->EnumOutputs(0): hr=0x%08x\n", hr);
            if (SUCCEEDED(hr)) {
                ComPtr<IDXGIOutput1> out1_2;
                out2.As(&out1_2);
                hr = out1_2->DuplicateOutput(dev2.Get(), dupl.GetAddressOf());
                printf("DuplicateOutput on default adapter: hr=0x%08x\n", hr);
            }
        }
    }

    return 0;
}
