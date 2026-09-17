#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <cstdint>

using Microsoft::WRL::ComPtr;

class HwEncoder {
public:
    bool Init(UINT width, UINT height, UINT fps, UINT bitrateBps);
    void EncodeFrame(const uint8_t* bgra, UINT stride);
    void EncodeFrame(const std::vector<uint8_t>& bgra, UINT stride);
    void RequestKeyframe();
    void SetBitrate(UINT bitrateBps);
    void Shutdown();

    std::function<void(const uint8_t* data, size_t len)> OnNal;

private:
    void ConvertBgraToNv12(const uint8_t* bgra, UINT stride, uint8_t* dstNv12);
    void DrainOutput();

    ComPtr<IMFTransform> encoder_;
    ComPtr<IMFSample> inSample_;
    ComPtr<IMFMediaBuffer> inBuffer_;
    ComPtr<IMFSample> outSampleHolder_;
    ComPtr<IMFMediaBuffer> outBufferHolder_;

    UINT width_ = 0;
    UINT height_ = 0;
    UINT fps_ = 60;
    LONGLONG frameCount_ = 0;
    MFT_OUTPUT_STREAM_INFO streamInfo_ = {};
    bool isAsync_ = false;
};
