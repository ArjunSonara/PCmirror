#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>

using Microsoft::WRL::ComPtr;

class HwEncoder {
public:
    bool Init(UINT width, UINT height, UINT fps, UINT bitrateBps, ID3D11Device* d3dDevice = nullptr, ID3D11DeviceContext* d3dContext = nullptr, UINT inWidth = 0, UINT inHeight = 0);
    bool EncodeFrameGpu(ID3D11Texture2D* bgraTexture);
    void EncodeFrame(const uint8_t* bgra, UINT stride);
    void EncodeFrame(const std::vector<uint8_t>& bgra, UINT stride);
    void RequestKeyframe();
    void SetBitrate(UINT bitrateBps);
    void SetFps(UINT fps);
    void Shutdown();

    bool IsGpuAccelerated() const { return isGpuAccelerated_; }

    std::function<void(const uint8_t* data, size_t len)> OnNal;

private:
    bool InitGpuPipeline(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext, UINT inWidth, UINT inHeight);
    void ConvertBgraToNv12(const uint8_t* bgra, UINT stride, uint8_t* dstNv12);
    void DrainOutput();
    void DrainAsyncOutput();
    friend class EncoderEventCallback;

    ComPtr<IMFTransform> encoder_;
    ComPtr<IMFSample> inSample_;
    ComPtr<IMFMediaBuffer> inBuffer_;
    ComPtr<IMFSample> outSampleHolder_;
    ComPtr<IMFMediaBuffer> outBufferHolder_;

    // Direct3D 11 GPU Zero-Copy Video Processor pipeline
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    UINT resetToken_ = 0;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> videoProcessor_;
    ComPtr<ID3D11VideoProcessorInputView> vpInputView_;
    ID3D11Texture2D* lastBgraTexture_ = nullptr;
    static const size_t GPU_RING_SIZE = 3;
    ComPtr<ID3D11VideoProcessorOutputView> vpOutputViews_[GPU_RING_SIZE];
    ComPtr<ID3D11Texture2D> nv12Textures_[GPU_RING_SIZE];
    ComPtr<IMFMediaBuffer> dxgiMediaBuffers_[GPU_RING_SIZE];
    ComPtr<IMFSample> gpuSamples_[GPU_RING_SIZE];
    size_t gpuIndex_ = 0;
    bool isGpuAccelerated_ = false;

    // Asynchronous MFT event generator pipeline
    ComPtr<IMFMediaEventGenerator> eventGen_;
    ComPtr<IMFAsyncCallback> eventCallback_;
    std::atomic<bool> canAcceptInput_{ false };
    std::mutex inputMutex_;
    std::condition_variable inputCv_;
    std::mutex outputMutex_;
    std::atomic<bool> isStreaming_{ false };

    UINT width_ = 0;
    UINT height_ = 0;
    UINT fps_ = 60;
    LONGLONG frameCount_ = 0;
    MFT_OUTPUT_STREAM_INFO streamInfo_ = {};
    bool isAsync_ = false;
};
