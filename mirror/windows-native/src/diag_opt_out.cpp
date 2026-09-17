#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <wrl/client.h>

extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

using Microsoft::WRL::ComPtr;

int main() {
    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (hDesk) SetThreadDesktop(hDesk);

    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));

    UINT aIdx = 0;
    ComPtr<IDXGIAdapter1> ad;
    while (factory->EnumAdapters1(aIdx, ad.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 adDesc;
        ad->GetDesc1(&adDesc);
        wprintf(L"Adapter %u: %s\n", aIdx, adDesc.Description);

        UINT oIdx = 0;
        ComPtr<IDXGIOutput> out;
        while (ad->EnumOutputs(oIdx, out.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC oDesc;
            out->GetDesc(&oDesc);
            wprintf(L"  Output %u: %s, Attached=%d\n", oIdx, oDesc.DeviceName, oDesc.AttachedToDesktop);

            D3D_FEATURE_LEVEL fl;
            ComPtr<ID3D11Device> dev;
            ComPtr<ID3D11DeviceContext> ctx;
            HRESULT hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(), &fl, ctx.GetAddressOf());
            if (SUCCEEDED(hr)) {
                ComPtr<IDXGIOutput1> out1;
                if (SUCCEEDED(out.As(&out1))) {
                    ComPtr<IDXGIOutputDuplication> dupl;
                    hr = out1->DuplicateOutput(dev.Get(), dupl.GetAddressOf());
                    printf("    DuplicateOutput: hr=0x%08x\n", hr);
                }
            }
            oIdx++;
        }
        aIdx++;
    }
    return 0;
}
