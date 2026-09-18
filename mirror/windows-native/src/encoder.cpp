#include "encoder.h"
#include <codecapi.h>
#include <icodecapi.h>
#include <mferror.h>
#include <immintrin.h>
#include <stdio.h>

static void ConfigureCodecAPI(IMFTransform* encoder, UINT fps, UINT bitrateBps) {
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder->QueryInterface(IID_PPV_ARGS(codecApi.GetAddressOf())))) {
        VARIANT v;
        VariantInit(&v);

        // 1. Mandatory low-latency mode (disables frame caching & lookahead)
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);

        // 2. Disable B-frames completely (0 B-pictures = strictly zero display reordering delay)
        v.vt = VT_UI4; v.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);

        // 3. Balanced preset (33) gives encoder deeper motion estimation during camera swipes without latency penalty
        v.vt = VT_UI4; v.ulVal = 33;
        if (FAILED(codecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v))) {
            v.ulVal = 0;
            codecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v);
        }

        // 4. Rate Control: Try Unconstrained VBR (eAVEncCommonRateControlMode_UnconstrainedVBR = 2)
        // This eliminates bit starvation and heavy quantization blur when swiping the mouse in games.
        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
        HRESULT hrRc = codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        if (FAILED(hrRc)) {
            // Fallback to Peak-Constrained VBR if unconstrained is rejected
            v.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
            hrRc = codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            if (FAILED(hrRc)) {
                // Final fallback to CBR
                v.ulVal = eAVEncCommonRateControlMode_CBR;
                codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            }
        }

        // 5. Target Bitrate & Peak Burst allowance (up to 2.5x target bitrate for sudden motion spikes)
        v.vt = VT_UI4; v.ulVal = bitrateBps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);

        UINT peakBps = (UINT)(bitrateBps * 2.5f);
        if (peakBps > 150000000) peakBps = 150000000;
        v.ulVal = peakBps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);

        // 6. Set VBV buffer window to 250ms worth of bits so sudden motion spikes can draw from buffer
        v.vt = VT_UI4; v.ulVal = bitrateBps / 4;
        codecApi->SetValue(&CODECAPI_AVEncCommonBufferSize, &v);

        // 7. GOP size (IDR keyframe cadence: 1 keyframe per second for quick recovery)
        v.vt = VT_UI4; v.ulVal = fps;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);

        // 8. Max QP = 28 to forbid encoder from degrading into pixel blocks during motion spikes
        v.vt = VT_UI4; v.ulVal = 28;
        codecApi->SetValue(&CODECAPI_AVEncVideoMaxQP, &v);

        // 9. Min QP = 12 for clean gradients
        v.vt = VT_UI4; v.ulVal = 12;
        codecApi->SetValue(&CODECAPI_AVEncVideoMinQP, &v);

        // 10. Enable CABAC entropy coding for 30%+ higher compression efficiency
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVEncH264CABACEnable, &v);

        VariantClear(&v);
    }
}

static bool ConfigureEncoderMediaTypes(IMFTransform* encoder, UINT width, UINT height, UINT fps, UINT bitrateBps) {
    // Output type (H.264 High Profile with CABAC)
    ComPtr<IMFMediaType> outMediaType;
    HRESULT hr = MFCreateMediaType(outMediaType.GetAddressOf());
    if (FAILED(hr)) return false;

    outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outMediaType->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
    outMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outMediaType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outMediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);

    hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
    if (FAILED(hr)) {
        printf("[HwEncoder] SetOutputType High Profile returned 0x%08x, trying Main...\n", hr);
        outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
        if (FAILED(hr)) {
            printf("[HwEncoder] SetOutputType Main Profile returned 0x%08x, trying Base...\n", hr);
            outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
            hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
            if (FAILED(hr)) {
                printf("[HwEncoder] SetOutputType Base Profile failed: 0x%08x\n", hr);
                return false;
            }
        }
    }

    // Input type (NV12)
    ComPtr<IMFMediaType> inMediaType;
    hr = MFCreateMediaType(inMediaType.GetAddressOf());
    if (FAILED(hr)) return false;

    inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = encoder->SetInputType(0, inMediaType.Get(), 0);
    if (FAILED(hr)) {
        printf("[HwEncoder] SetInputType NV12 failed: 0x%08x\n", hr);
        return false;
    }
    return true;
}

static bool FindAndActivateEncoder(ComPtr<IMFTransform>& outTransform, bool& outIsAsync, UINT width, UINT height, UINT fps, UINT bitrateBps) {
    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_ALL,
        &inType, &outType, &activates, &count);

    if (FAILED(hr) || count == 0) return false;

    // First pass: try synchronous encoders (immediate 1-in-1-out without async event loop)
    for (UINT32 pass = 0; pass < 2; pass++) {
        for (UINT32 i = 0; i < count; i++) {
            UINT32 isAsync = 0;
            activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            if (pass == 0 && isAsync != 0) continue; // Only sync on pass 0

            WCHAR name[256] = {};
            activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);

            ComPtr<IMFTransform> candidate;
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(candidate.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                if (isAsync) {
                    ComPtr<IMFAttributes> attrs;
                    if (SUCCEEDED(candidate->GetAttributes(attrs.GetAddressOf()))) {
                        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    }
                }

                // CRITICAL: Configure low latency CodecAPI BEFORE media type negotiation
                // so internal hardware buffers are initialized with 0 lookahead delay!
                ConfigureCodecAPI(candidate.Get(), fps, bitrateBps);

                if (ConfigureEncoderMediaTypes(candidate.Get(), width, height, fps, bitrateBps)) {
                    wprintf(L"Selected Zero-Latency Encoder: %s (Async=%u)\n", name, isAsync);
                    outTransform = candidate;
                    outIsAsync = (isAsync != 0);
                    for (UINT32 j = 0; j < count; j++) activates[j]->Release();
                    CoTaskMemFree(activates);
                    return true;
                }
            }
        }
    }

    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);
    return false;
}

bool HwEncoder::Init(UINT width, UINT height, UINT fps, UINT bitrateBps) {
    width_ = width;
    height_ = height;
    fps_ = fps;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        printf("MFStartup failed: 0x%08x\n", hr);
        return false;
    }

    if (!FindAndActivateEncoder(encoder_, isAsync_, width, height, fps, bitrateBps)) {
        printf("Could not find or configure any H.264 encoder MFT.\n");
        return false;
    }

    hr = encoder_->GetOutputStreamInfo(0, &streamInfo_);
    if (FAILED(hr)) {
        printf("GetOutputStreamInfo failed: 0x%08x\n", hr);
        return false;
    }

    encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // Pre-allocate reusable input sample & buffer to eliminate per-frame heap allocations
    DWORD inBufSize = (DWORD)((size_t)width * height * 3 / 2);
    hr = MFCreateMemoryBuffer(inBufSize, inBuffer_.GetAddressOf());
    if (SUCCEEDED(hr)) {
        MFCreateSample(inSample_.GetAddressOf());
        inSample_->AddBuffer(inBuffer_.Get());
    }

    // Pre-allocate reusable output sample & buffer (4MB to support 250 Mbps IDR bursts)
    DWORD outBufSize = (streamInfo_.cbSize > 4 * 1024 * 1024) ? streamInfo_.cbSize : (4 * 1024 * 1024);
    hr = MFCreateMemoryBuffer(outBufSize, outBufferHolder_.GetAddressOf());
    if (SUCCEEDED(hr)) {
        MFCreateSample(outSampleHolder_.GetAddressOf());
        outSampleHolder_->AddBuffer(outBufferHolder_.Get());
    }

    printf("H.264 Zero-Latency Parallel Encoder ready: %ux%u @ %ufps, %u bps\n", width, height, fps, bitrateBps);
    return true;
}

void HwEncoder::RequestKeyframe() {
    if (!encoder_) return;
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder_.As(&codecApi))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        VariantClear(&v);
    }
}

void HwEncoder::SetBitrate(UINT bitrateBps) {
    if (!encoder_) return;
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder_.As(&codecApi))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = bitrateBps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);

        // Allow peak burst up to 2.5x target bitrate during motion spikes
        UINT peakBps = (UINT)(bitrateBps * 2.5f);
        if (peakBps > 150000000) peakBps = 150000000;
        v.ulVal = peakBps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);

        // Update buffer size
        v.ulVal = bitrateBps / 4;
        codecApi->SetValue(&CODECAPI_AVEncCommonBufferSize, &v);

        RequestKeyframe();
        printf("[HwEncoder] Bitrate updated dynamically to %u bps (%.1f Mbps, peak: %.1f Mbps)\n",
               bitrateBps, bitrateBps / 1000000.0f, peakBps / 1000000.0f);
        fflush(stdout);
        VariantClear(&v);
    }
}

void HwEncoder::SetFps(UINT fps) {
    if (fps >= 30 && fps <= 144) {
        fps_ = fps;
        printf("[HwEncoder] Target frame rate set to %u FPS\n", fps_);
    }
}

// Mathematically exact ITU-R BT.709 integer BGRA -> NV12 color converter with multi-threaded parallel execution
void HwEncoder::ConvertBgraToNv12(const uint8_t* bgra, UINT stride, uint8_t* dstNv12) {
    uint8_t* yPlane = dstNv12;
    uint8_t* uvPlane = dstNv12 + (size_t)width_ * height_;

    #pragma omp parallel for schedule(static)
    for (int y = 0; y < (int)height_; y++) {
        const uint8_t* srcRow = bgra + (size_t)y * stride;
        const uint8_t* srcRowNext = (y + 1 < (int)height_) ? (bgra + (size_t)(y + 1) * stride) : srcRow;
        uint8_t* dstY = yPlane + (size_t)y * width_;
        uint8_t* dstUV = uvPlane + ((size_t)y / 2) * width_;
        bool doUV = (y % 2 == 0);

        UINT x = 0;
        if (doUV) {
            for (; x + 1 < width_; x += 2) {
                int b0 = srcRow[x * 4 + 0];
                int g0 = srcRow[x * 4 + 1];
                int r0 = srcRow[x * 4 + 2];

                int b1 = srcRow[(x + 1) * 4 + 0];
                int g1 = srcRow[(x + 1) * 4 + 1];
                int r1 = srcRow[(x + 1) * 4 + 2];

                // Exact ITU-R BT.709 limited-range Y: 16 + (47*R + 157*G + 16*B + 128) >> 8
                int y0 = ((47 * r0 + 157 * g0 + 16 * b0 + 128) >> 8) + 16;
                int y1 = ((47 * r1 + 157 * g1 + 16 * b1 + 128) >> 8) + 16;
                dstY[x]     = (uint8_t)(y0 < 0 ? 0 : (y0 > 255 ? 255 : y0));
                dstY[x + 1] = (uint8_t)(y1 < 0 ? 0 : (y1 > 255 ? 255 : y1));

                // 2x2 area box-filter (4-pixel average across rows y and y+1):
                // Eliminates vertical color aliasing and temporary saturation shifts during fast 3D rotation!
                int b0_n = srcRowNext[x * 4 + 0];
                int g0_n = srcRowNext[x * 4 + 1];
                int r0_n = srcRowNext[x * 4 + 2];

                int b1_n = srcRowNext[(x + 1) * 4 + 0];
                int g1_n = srcRowNext[(x + 1) * 4 + 1];
                int r1_n = srcRowNext[(x + 1) * 4 + 2];

                int rAvg = (r0 + r1 + r0_n + r1_n + 2) >> 2;
                int gAvg = (g0 + g1 + g0_n + g1_n + 2) >> 2;
                int bAvg = (b0 + b1 + b0_n + b1_n + 2) >> 2;

                // Exact ITU-R BT.709 limited-range Cb/Cr:
                // Cb: 128 + (-26*R - 87*G + 112*B + 128) >> 8
                // Cr: 128 + (112*R - 102*G - 10*B + 128) >> 8
                int uVal = ((-26 * rAvg - 87 * gAvg + 112 * bAvg + 128) >> 8) + 128;
                int vVal = ((112 * rAvg - 102 * gAvg - 10 * bAvg + 128) >> 8) + 128;
                dstUV[x]     = (uint8_t)(uVal < 0 ? 0 : (uVal > 255 ? 255 : uVal));
                dstUV[x + 1] = (uint8_t)(vVal < 0 ? 0 : (vVal > 255 ? 255 : vVal));
            }
        } else {
            for (; x < width_; x++) {
                int b = srcRow[x * 4 + 0];
                int g = srcRow[x * 4 + 1];
                int r = srcRow[x * 4 + 2];
                int yVal = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
                dstY[x] = (uint8_t)(yVal < 0 ? 0 : (yVal > 255 ? 255 : yVal));
            }
        }

        // Remainder
        for (; x < width_; x++) {
            int b = srcRow[x * 4 + 0];
            int g = srcRow[x * 4 + 1];
            int r = srcRow[x * 4 + 2];
            int yVal = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
            dstY[x] = (uint8_t)(yVal < 0 ? 0 : (yVal > 255 ? 255 : yVal));
            if (doUV && (x % 2 == 0)) {
                int b_n = srcRowNext[x * 4 + 0];
                int g_n = srcRowNext[x * 4 + 1];
                int r_n = srcRowNext[x * 4 + 2];

                int rAvg = (r + r_n + 1) >> 1;
                int gAvg = (g + g_n + 1) >> 1;
                int bAvg = (b + b_n + 1) >> 1;

                int uVal = ((-26 * rAvg - 87 * gAvg + 112 * bAvg + 128) >> 8) + 128;
                int vVal = ((112 * rAvg - 102 * gAvg - 10 * bAvg + 128) >> 8) + 128;
                dstUV[x]     = (uint8_t)(uVal < 0 ? 0 : (uVal > 255 ? 255 : uVal));
                dstUV[x + 1] = (uint8_t)(vVal < 0 ? 0 : (vVal > 255 ? 255 : vVal));
            }
        }
    }
}

void HwEncoder::EncodeFrame(const uint8_t* bgra, UINT stride) {
    if (!inBuffer_ || !inSample_) return;

    BYTE* dst = nullptr;
    HRESULT hr = inBuffer_->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr) || !dst) return;

    ConvertBgraToNv12(bgra, stride, dst);

    inBuffer_->Unlock();
    DWORD nv12Size = (DWORD)((size_t)width_ * height_ * 3 / 2);
    inBuffer_->SetCurrentLength(nv12Size);

    LONGLONG duration = (LONGLONG)(10000000.0 / fps_);
    inSample_->SetSampleTime(frameCount_ * duration);
    inSample_->SetSampleDuration(duration);
    frameCount_++;

    hr = encoder_->ProcessInput(0, inSample_.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        printf("ProcessInput failed: 0x%08x\n", hr);
    }

    DrainOutput();
}

void HwEncoder::EncodeFrame(const std::vector<uint8_t>& bgra, UINT stride) {
    EncodeFrame(bgra.data(), stride);
}

void HwEncoder::DrainOutput() {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER outBuf = {};
        outBuf.dwStreamID = 0;
        DWORD status = 0;

        if (!(streamInfo_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            outBuf.pSample = outSampleHolder_.Get();
        }

        HRESULT hr = encoder_->ProcessOutput(0, 1, &outBuf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) break;

        IMFSample* outSample = outBuf.pSample;
        if (outSample) {
            ComPtr<IMFMediaBuffer> contiguous;
            if (SUCCEEDED(outSample->ConvertToContiguousBuffer(contiguous.GetAddressOf()))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                contiguous->Lock(&data, nullptr, &len);
                if (OnNal && len > 0) {
                    OnNal(data, len);
                }
                contiguous->Unlock();
            }
            if (streamInfo_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
                outSample->Release();
            }
        }
        if (outBuf.pEvents) {
            outBuf.pEvents->Release();
        }
    }
}

void HwEncoder::Shutdown() {
    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    inSample_.Reset();
    inBuffer_.Reset();
    outSampleHolder_.Reset();
    outBufferHolder_.Reset();
    encoder_.Reset();
    MFShutdown();
}
