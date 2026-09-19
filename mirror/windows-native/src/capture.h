#pragma once
#include <d3d11_1.h>
#include <d3d10_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

class DesktopCapture {
public:
    bool Init(UINT outputIndex = 0);
    bool GrabFrame(std::vector<uint8_t>& outBgra, UINT& outWidth, UINT& outHeight, UINT& outStride, UINT timeoutMs = 100);
    bool GrabFrameGpu(ID3D11Texture2D** ppTexture, UINT timeoutMs = 16);
    void Shutdown();

    ID3D11Device* GetDevice() const { return device_.Get(); }
    ID3D11DeviceContext* GetContext() const { return context_.Get(); }

    UINT GetWidth() const { return width_; }
    UINT GetHeight() const { return height_; }

    bool CheckAndClearReinitialized() {
        bool val = wasReinitialized_;
        wasReinitialized_ = false;
        return val;
    }

    void SetCursorVisible(bool visible) { enableCursor_ = visible; }
    bool IsCursorVisible() const { return enableCursor_; }

private:
    bool wasReinitialized_ = false;
    bool enableCursor_ = true;
    void DrawCursor(uint8_t* pFrame, UINT stride, const DXGI_OUTDUPL_FRAME_INFO& frameInfo);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> stagingTexture_;
    ComPtr<ID3D11Texture2D> gpuTexture_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT outputIndex_ = 0;

    RECT outputRect_ = {};

    // GDI cursor fallback
    HDC cursorHdc_ = NULL;
    HBITMAP cursorBmp_ = NULL;
    HBITMAP cursorOldBmp_ = NULL;
    uint32_t* cursorPixels_ = nullptr;
    const int cursorSize_ = 64;
    HCURSOR lastCursorHandle_ = NULL;
    int cursorHotX_ = 0;
    int cursorHotY_ = 0;
    bool cursorHasAlpha_ = false;

    // DXGI hardware cursor cache
    std::vector<uint8_t> pointerShapeBuf_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShapeInfo_ = {};
    int lastPointerX_ = 0;
    int lastPointerY_ = 0;
    bool pointerVisible_ = false;
};
