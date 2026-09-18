#include "capture.h"
#include <stdio.h>
#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

bool DesktopCapture::Init(UINT outputIndex) {
    outputIndex_ = outputIndex;

    // Attach current thread to the interactive input desktop
    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (hDesk) {
        SetThreadDesktop(hDesk);
        CloseDesktop(hDesk);
    }

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        printf("CreateDXGIFactory1 failed: 0x%08x\n", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    UINT adapterIdx = 0;
    bool found = false;

    while (factory->EnumAdapters1(adapterIdx, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
        if (adapter->EnumOutputs(outputIndex, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC outDesc;
            output->GetDesc(&outDesc);
            if (outDesc.AttachedToDesktop) {
                DXGI_ADAPTER_DESC1 adDesc;
                adapter->GetDesc1(&adDesc);

                D3D_FEATURE_LEVEL featureLevel;
                hr = D3D11CreateDevice(
                    adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                    nullptr, 0, D3D11_SDK_VERSION,
                    device_.ReleaseAndGetAddressOf(), &featureLevel, context_.ReleaseAndGetAddressOf());

                if (SUCCEEDED(hr)) {
                    ComPtr<IDXGIOutput1> output1;
                    if (SUCCEEDED(output.As(&output1))) {
                        hr = output1->DuplicateOutput(device_.Get(), duplication_.ReleaseAndGetAddressOf());
                        if (SUCCEEDED(hr)) {
                            wprintf(L"Desktop Duplication active on adapter: %s (Output: %s)\n", adDesc.Description, outDesc.DeviceName);
                            outputRect_ = outDesc.DesktopCoordinates;
                            found = true;
                            break;
                        } else {
                            wprintf(L"Adapter %s DuplicateOutput returned 0x%08x, trying next adapter...\n", adDesc.Description, hr);
                        }
                    }
                }
            }
        }
        adapterIdx++;
    }

    if (!found) {
        device_.Reset();
        context_.Reset();
        return false;
    }

    DXGI_OUTDUPL_DESC desc;
    duplication_->GetDesc(&desc);
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = width_;
    stagingDesc.Height = height_;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&stagingDesc, nullptr, stagingTexture_.GetAddressOf());
    if (FAILED(hr)) {
        printf("CreateTexture2D (staging) failed: 0x%08x\n", hr);
        return false;
    }

    // Initialize cursor rendering DC and 32-bit DIB
    if (!cursorHdc_) {
        HDC hdcScreen = GetDC(NULL);
        cursorHdc_ = CreateCompatibleDC(hdcScreen);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = cursorSize_;
        bmi.bmiHeader.biHeight = -cursorSize_; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        cursorBmp_ = CreateDIBSection(cursorHdc_, &bmi, DIB_RGB_COLORS, (void**)&cursorPixels_, NULL, 0);
        cursorOldBmp_ = (HBITMAP)SelectObject(cursorHdc_, cursorBmp_);
        ReleaseDC(NULL, hdcScreen);
    }

    printf("Desktop capture initialized: %ux%u\n", width_, height_);
    return true;
}

void DesktopCapture::DrawCursor(uint8_t* pFrame, UINT stride, const DXGI_OUTDUPL_FRAME_INFO& frameInfo) {
    // 1. Check DXGI hardware pointer shape update
    if (frameInfo.PointerShapeBufferSize > 0) {
        pointerShapeBuf_.resize(frameInfo.PointerShapeBufferSize);
        UINT dummySize = 0;
        HRESULT hr = duplication_->GetFramePointerShape(
            frameInfo.PointerShapeBufferSize,
            pointerShapeBuf_.data(),
            &dummySize,
            &pointerShapeInfo_
        );
        if (FAILED(hr)) {
            pointerShapeBuf_.clear();
        }
    }

    if (frameInfo.PointerPosition.Visible) {
        lastPointerX_ = frameInfo.PointerPosition.Position.x;
        lastPointerY_ = frameInfo.PointerPosition.Position.y;
        pointerVisible_ = true;
    } else if (frameInfo.LastPresentTime.QuadPart > 0 && !frameInfo.PointerPosition.Visible) {
        // Only update visibility if DXGI explicitly reported it in this frame
        if (frameInfo.PointerPosition.Position.x == 0 && frameInfo.PointerPosition.Position.y == 0 && !frameInfo.PointerPosition.Visible) {
            // Keep last visibility state unless GDI confirms hidden
        }
    }

    // Attempt DXGI native pointer render if we have valid color shape
    if (pointerVisible_ && !pointerShapeBuf_.empty() && pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        int drawX = lastPointerX_ - pointerShapeInfo_.HotSpot.x;
        int drawY = lastPointerY_ - pointerShapeInfo_.HotSpot.y;
        UINT cW = pointerShapeInfo_.Width;
        UINT cH = pointerShapeInfo_.Height;
        UINT cPitch = pointerShapeInfo_.Pitch;
        const uint8_t* shapePixels = pointerShapeBuf_.data();

        for (UINT y = 0; y < cH; ++y) {
            int dstY = drawY + (int)y;
            if (dstY < 0 || dstY >= (int)height_) continue;
            uint8_t* dstRow = pFrame + dstY * stride;
            const uint32_t* srcRow = (const uint32_t*)(shapePixels + y * cPitch);

            for (UINT x = 0; x < cW; ++x) {
                int dstX = drawX + (int)x;
                if (dstX < 0 || dstX >= (int)width_) continue;

                uint32_t pix = srcRow[x];
                uint8_t a = (pix >> 24) & 0xFF;
                if (a == 0) continue;

                uint8_t r = (pix >> 16) & 0xFF;
                uint8_t g = (pix >> 8) & 0xFF;
                uint8_t b = pix & 0xFF;

                uint8_t* dstPix = dstRow + dstX * 4;
                if (a == 255) {
                    dstPix[0] = b;
                    dstPix[1] = g;
                    dstPix[2] = r;
                } else {
                    float alpha = a / 255.0f;
                    float invA = 1.0f - alpha;
                    dstPix[0] = (uint8_t)(b * alpha + dstPix[0] * invA);
                    dstPix[1] = (uint8_t)(g * alpha + dstPix[1] * invA);
                    dstPix[2] = (uint8_t)(r * alpha + dstPix[2] * invA);
                }
            }
        }
        return;
    }

    // Fallback: GDI cursor compositing
    if (!cursorPixels_ || !cursorHdc_) return;

    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci)) return;
    if (!(ci.flags & CURSOR_SHOWING)) return;

    int localX = ci.ptScreenPos.x - outputRect_.left;
    int localY = ci.ptScreenPos.y - outputRect_.top;

    if (localX < 0 || localX >= (int)width_ || localY < 0 || localY >= (int)height_) {
        return;
    }

    if (ci.hCursor != lastCursorHandle_) {
        lastCursorHandle_ = ci.hCursor;
        ICONINFO ii = {};
        if (GetIconInfo(ci.hCursor, &ii)) {
            cursorHotX_ = ii.xHotspot;
            cursorHotY_ = ii.yHotspot;
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        } else {
            cursorHotX_ = 0;
            cursorHotY_ = 0;
        }

        memset(cursorPixels_, 0, cursorSize_ * cursorSize_ * 4);
        DrawIconEx(cursorHdc_, 0, 0, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);

        cursorHasAlpha_ = false;
        for (int i = 0; i < cursorSize_ * cursorSize_; ++i) {
            if ((cursorPixels_[i] >> 24) != 0) {
                cursorHasAlpha_ = true;
                break;
            }
        }
    }

    int drawX = localX - cursorHotX_;
    int drawY = localY - cursorHotY_;

    for (int y = 0; y < cursorSize_; ++y) {
        int dstY = drawY + y;
        if (dstY < 0 || dstY >= (int)height_) continue;
        uint8_t* dstRow = pFrame + dstY * stride;

        for (int x = 0; x < cursorSize_; ++x) {
            int dstX = drawX + x;
            if (dstX < 0 || dstX >= (int)width_) continue;

            uint32_t srcPixel = cursorPixels_[y * cursorSize_ + x];
            if (srcPixel == 0) continue;

            uint8_t srcA = (srcPixel >> 24) & 0xFF;
            uint8_t srcR = (srcPixel >> 16) & 0xFF;
            uint8_t srcG = (srcPixel >> 8) & 0xFF;
            uint8_t srcB = srcPixel & 0xFF;

            if (!cursorHasAlpha_) {
                srcA = 255;
            }

            uint8_t* dstPix = dstRow + dstX * 4;
            if (srcA == 255) {
                dstPix[0] = srcB;
                dstPix[1] = srcG;
                dstPix[2] = srcR;
            } else if (srcA > 0) {
                float a = srcA / 255.0f;
                float invA = 1.0f - a;
                dstPix[0] = (uint8_t)(srcB * a + dstPix[0] * invA);
                dstPix[1] = (uint8_t)(srcG * a + dstPix[1] * invA);
                dstPix[2] = (uint8_t)(srcR * a + dstPix[2] * invA);
            }
        }
    }
}

bool DesktopCapture::GrabFrame(std::vector<uint8_t>& outBgra, UINT& outWidth, UINT& outHeight, UINT& outStride, UINT timeoutMs) {
    if (!duplication_) {
        static DWORD lastRetryTick = 0;
        DWORD nowTick = GetTickCount();
        if (nowTick - lastRetryTick < 250) {
            Sleep(10);
            return false;
        }
        lastRetryTick = nowTick;
        printf("[DesktopCapture] Re-initializing capture after game/display mode change...\n");
        if (!Init(outputIndex_)) {
            return false;
        }
        wasReinitialized_ = true;
        printf("[DesktopCapture] Successfully re-hooked desktop capture for game!\n");
    }

    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &frameInfo, desktopResource.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    if (FAILED(hr)) {
        // Any failure (DXGI_ERROR_ACCESS_LOST 0x887a0026, DXGI_ERROR_INVALID_CALL 0x887a0001, etc.)
        // indicates that a game launched, changed resolution/fullscreen mode, or reset the adapter.
        // We must cleanly shut down and allow the next frame to re-hook.
        printf("[DesktopCapture] AcquireNextFrame failed (0x%08x) -- resetting for game launch/mode switch...\n", hr);
        Shutdown();
        return false;
    }

    ComPtr<ID3D11Texture2D> frameTexture;
    hr = desktopResource.As(&frameTexture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }

    context_->CopyResource(stagingTexture_.Get(), frameTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }

    outWidth = width_;
    outHeight = height_;
    outStride = mapped.RowPitch;
    size_t totalBytes = (size_t)mapped.RowPitch * height_;
    if (outBgra.size() != totalBytes) {
        outBgra.resize(totalBytes);
    }
    memcpy(outBgra.data(), mapped.pData, totalBytes);

    context_->Unmap(stagingTexture_.Get(), 0);
    duplication_->ReleaseFrame();

    // Composite mouse cursor directly onto the captured frame if enabled
    if (enableCursor_) {
        DrawCursor(outBgra.data(), outStride, frameInfo);
    }

    return true;
}

void DesktopCapture::Shutdown() {
    if (cursorHdc_) {
        SelectObject(cursorHdc_, cursorOldBmp_);
        DeleteObject(cursorBmp_);
        DeleteDC(cursorHdc_);
        cursorHdc_ = NULL;
        cursorBmp_ = NULL;
        cursorPixels_ = nullptr;
        lastCursorHandle_ = NULL;
    }
    if (duplication_) {
        duplication_->ReleaseFrame();
        duplication_.Reset();
    }
    stagingTexture_.Reset();
    context_.Reset();
    device_.Reset();
    pointerShapeBuf_.clear();
}
