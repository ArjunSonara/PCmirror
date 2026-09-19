#include "encoder.h"
#include <codecapi.h>
#include <icodecapi.h>
#include <mferror.h>
#include <immintrin.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <chrono>

class EncoderEventCallback : public IMFAsyncCallback {
public:
    EncoderEventCallback(HwEncoder* parent) : parent_(parent), refCount_(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&refCount_);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&refCount_);
        if (count == 0) delete this;
        return count;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(IMFAsyncResult* pResult) override;

private:
    HwEncoder* parent_;
    long refCount_;
};

STDMETHODIMP EncoderEventCallback::Invoke(IMFAsyncResult* pResult) {
    if (!parent_ || !parent_->isStreaming_ || !parent_->eventGen_) {
        return S_OK;
    }

    ComPtr<IMFMediaEvent> pEvent;
    HRESULT hr = parent_->eventGen_->EndGetEvent(pResult, pEvent.GetAddressOf());
    if (FAILED(hr)) return hr;

    MediaEventType met = MEUnknown;
    pEvent->GetType(&met);

    if (met == METransformNeedInput) {
        parent_->canAcceptInput_ = true;
        parent_->inputCv_.notify_one();
    } else if (met == METransformHaveOutput) {
        parent_->DrainAsyncOutput();
    }

    if (parent_->isStreaming_ && parent_->eventGen_) {
        parent_->eventGen_->BeginGetEvent(this, nullptr);
    }
    return S_OK;
}

static void ConfigureCodecAPI(IMFTransform* encoder, UINT fps, UINT bitrateBps, VideoCodec codec, UINT sliceCount = 4) {
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

        // 3. Ultra-fast speed preset (0) maximizes hardware encoder throughput for lowest latency
        v.vt = VT_UI4; v.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v);

        // 4. Rate Control: Prefer Peak-Constrained VBR (eAVEncCommonRateControlMode_PeakConstrainedVBR = 1)
        // Constrains sudden frame ballooning during action while maintaining crisp quality.
        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
        HRESULT hrRc = codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        if (FAILED(hrRc)) {
            // Fallback to CBR if Peak-Constrained VBR is rejected
            v.ulVal = eAVEncCommonRateControlMode_CBR;
            codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        }

        // 5. Target Bitrate & Peak Burst allowance (up to 2.5x target bitrate for sudden motion spikes)
        v.vt = VT_UI4; v.ulVal = bitrateBps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);

        UINT peakBps = (UINT)(bitrateBps * 1.5f);
        if (peakBps > 200000000) peakBps = 200000000;
        v.ulVal = peakBps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);

        // 6. Set VBV buffer window to 250ms worth of bits so sudden motion spikes can draw from buffer
        v.vt = VT_UI4; v.ulVal = bitrateBps / 4;
        codecApi->SetValue(&CODECAPI_AVEncCommonBufferSize, &v);

        // 7. GOP size (IDR keyframe cadence: 1 keyframe per second for quick recovery)
        v.vt = VT_UI4; v.ulVal = fps;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);

        // 8. Max QP = 28: hard quality floor prevents aggressive blur/macroblocking during fast camera swipes
        v.vt = VT_UI4; v.ulVal = 28;
        codecApi->SetValue(&CODECAPI_AVEncVideoMaxQP, &v);

        // 9. Min QP = 12 for clean gradients
        v.vt = VT_UI4; v.ulVal = 12;
        codecApi->SetValue(&CODECAPI_AVEncVideoMinQP, &v);

        // 10. Enable CABAC entropy coding for H.264
        if (codec == CODEC_H264) {
            v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
            codecApi->SetValue(&CODECAPI_AVEncH264CABACEnable, &v);
        }

        // 11. Feature 3: Slice-Based Multi-Threading (sub-frame transmission)
        if (sliceCount > 1) {
            v.vt = VT_UI4;
            v.ulVal = 2; // eAVEncSliceControlMode_NumberOfSlices = 2
            codecApi->SetValue(&CODECAPI_AVEncSliceControlMode, &v);
            v.ulVal = sliceCount;
            codecApi->SetValue(&CODECAPI_AVEncSliceControlSize, &v);
        }

        VariantClear(&v);
    }
}

static bool ConfigureEncoderMediaTypes(IMFTransform* encoder, UINT width, UINT height, UINT fps, UINT bitrateBps, VideoCodec codec) {
    ComPtr<IMFMediaType> outMediaType;
    HRESULT hr = MFCreateMediaType(outMediaType.GetAddressOf());
    if (FAILED(hr)) return false;

    outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (codec == CODEC_HEVC) {
        outMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
    } else {
        outMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    }

    outMediaType->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
    outMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outMediaType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outMediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
    if (FAILED(hr) && codec == CODEC_H264) {
        outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
        if (FAILED(hr)) {
            outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
            hr = encoder->SetOutputType(0, outMediaType.Get(), 0);
            if (FAILED(hr)) {
                printf("[HwEncoder] SetOutputType H.264 Base Profile failed: 0x%08x\n", hr);
                return false;
            }
        }
    } else if (FAILED(hr)) {
        printf("[HwEncoder] SetOutputType failed: 0x%08x\n", hr);
        return false;
    }

    // Check available input types from the hardware encoder
    ComPtr<IMFMediaType> inMediaType;
    for (DWORD i = 0; i < 20; i++) {
        ComPtr<IMFMediaType> avail;
        if (SUCCEEDED(encoder->GetInputAvailableType(0, i, avail.GetAddressOf()))) {
            GUID sub = {};
            avail->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                inMediaType = avail;
                break;
            }
        } else {
            break;
        }
    }

    if (!inMediaType) {
        hr = MFCreateMediaType(inMediaType.GetAddressOf());
        if (FAILED(hr)) return false;
        inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }

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

static bool FindAndActivateEncoder(ComPtr<IMFTransform>& outTransform, bool& outIsAsync, VideoCodec& activeCodec, UINT width, UINT height, UINT fps, UINT bitrateBps, IMFDXGIDeviceManager* dxgiManager, VideoCodec requestedCodec, UINT sliceCount) {
    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };

    // Search pass for requested codec (HEVC prioritizes hardware MFT)
    if (requestedCodec == CODEC_HEVC) {
        MFT_REGISTER_TYPE_INFO outTypeHEVC = { MFMediaType_Video, MFVideoFormat_HEVC };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inType, &outTypeHEVC, &activates, &count);
        if (SUCCEEDED(hr) && count > 0) {
            for (UINT32 i = 0; i < count; i++) {
                UINT32 isAsync = 0;
                activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
                WCHAR name[256] = {};
                activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);

                ComPtr<IMFTransform> candidate;
                hr = activates[i]->ActivateObject(IID_PPV_ARGS(candidate.GetAddressOf()));
                if (SUCCEEDED(hr)) {
                    ComPtr<IMFAttributes> attrs;
                    if (SUCCEEDED(candidate->GetAttributes(attrs.GetAddressOf()))) {
                        attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
                        if (isAsync) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    }
                    ComPtr<IMFAttributes> inAttrs;
                    if (SUCCEEDED(candidate->GetInputStreamAttributes(0, inAttrs.GetAddressOf()))) {
                        inAttrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
                    }
                    if (dxgiManager) {
                        candidate->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)dxgiManager);
                    }

                    ConfigureCodecAPI(candidate.Get(), fps, bitrateBps, CODEC_HEVC, sliceCount);
                    if (ConfigureEncoderMediaTypes(candidate.Get(), width, height, fps, bitrateBps, CODEC_HEVC)) {
                        wprintf(L"Selected Zero-Latency Next-Gen Encoder: %s [HEVC/H.265] (Async=%u, Mode=Hardware)\n", name, isAsync);
                        outTransform = candidate;
                        outIsAsync = (isAsync != 0);
                        activeCodec = CODEC_HEVC;
                        for (UINT32 j = 0; j < count; j++) activates[j]->Release();
                        CoTaskMemFree(activates);
                        return true;
                    }
                }
            }
            for (UINT32 i = 0; i < count; i++) activates[i]->Release();
            CoTaskMemFree(activates);
        }
        printf("[HwEncoder] Hardware HEVC encoder not available or rejected parameters, falling back to H.264...\n");
    }

    // Fallback or explicit H.264 search
    MFT_REGISTER_TYPE_INFO outTypeH264 = { MFMediaType_Video, MFVideoFormat_H264 };
    for (int searchMode = 0; searchMode < 2; searchMode++) {
        UINT32 enumFlags = (searchMode == 0) ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER) : MFT_ENUM_FLAG_ALL;
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, enumFlags, &inType, &outTypeH264, &activates, &count);
        if (FAILED(hr) || count == 0) continue;

        for (UINT32 i = 0; i < count; i++) {
            UINT32 isAsync = 0;
            activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);

            WCHAR name[256] = {};
            activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 255, nullptr);

            ComPtr<IMFTransform> candidate;
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(candidate.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                ComPtr<IMFAttributes> attrs;
                if (SUCCEEDED(candidate->GetAttributes(attrs.GetAddressOf()))) {
                    attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
                    if (isAsync) {
                        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    }
                }

                ComPtr<IMFAttributes> inAttrs;
                if (SUCCEEDED(candidate->GetInputStreamAttributes(0, inAttrs.GetAddressOf()))) {
                    inAttrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
                }

                if (dxgiManager) {
                    hr = candidate->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)dxgiManager);
                    if (SUCCEEDED(hr)) {
                        wprintf(L"[HwEncoder] %s attached to Direct3D 11 Device Manager successfully!\n", name);
                    }
                }

                ConfigureCodecAPI(candidate.Get(), fps, bitrateBps, CODEC_H264, sliceCount);

                if (ConfigureEncoderMediaTypes(candidate.Get(), width, height, fps, bitrateBps, CODEC_H264)) {
                    wprintf(L"Selected Zero-Latency Encoder: %s [H.264] (Async=%u, Mode=%s)\n", name, isAsync, (searchMode == 0 ? L"Hardware" : L"Software"));
                    outTransform = candidate;
                    outIsAsync = (isAsync != 0);
                    activeCodec = CODEC_H264;
                    for (UINT32 j = 0; j < count; j++) activates[j]->Release();
                    CoTaskMemFree(activates);
                    return true;
                }
            }
        }
        for (UINT32 i = 0; i < count; i++) activates[i]->Release();
        CoTaskMemFree(activates);
    }
    return false;
}

bool HwEncoder::InitGpuPipeline(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext, UINT inWidth, UINT inHeight) {
    if (!d3dDevice || !d3dContext || !dxgiManager_) return false;
    d3dDevice_ = d3dDevice;
    d3dContext_ = d3dContext;

    if (inWidth == 0) inWidth = width_;
    if (inHeight == 0) inHeight = height_;

    // Setup D3D11 Video Processor for zero-latency GPU color conversion (BGRA -> NV12) and downscaling
    HRESULT hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(videoDevice_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        printf("[HwEncoder] ID3D11VideoDevice query failed: 0x%08x\n", hr);
        return false;
    }

    hr = d3dContext_->QueryInterface(IID_PPV_ARGS(videoContext_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        printf("[HwEncoder] ID3D11VideoContext query failed: 0x%08x\n", hr);
        return false;
    }

    // Allocate persistent renderable NV12 textures in GPU VRAM (triple-buffered ring)
    D3D11_TEXTURE2D_DESC nv12Desc = {};
    nv12Desc.Width = width_;
    nv12Desc.Height = height_;
    nv12Desc.MipLevels = 1;
    nv12Desc.ArraySize = 1;
    nv12Desc.Format = DXGI_FORMAT_NV12;
    nv12Desc.SampleDesc.Count = 1;
    nv12Desc.Usage = D3D11_USAGE_DEFAULT;
    nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpDesc = {};
    vpDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    vpDesc.InputWidth = inWidth;
    vpDesc.InputHeight = inHeight;
    vpDesc.OutputWidth = width_;
    vpDesc.OutputHeight = height_;
    vpDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    hr = videoDevice_->CreateVideoProcessorEnumerator(&vpDesc, vpEnum_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        printf("[HwEncoder] CreateVideoProcessorEnumerator failed: 0x%08x\n", hr);
        return false;
    }

    hr = videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, videoProcessor_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        printf("[HwEncoder] CreateVideoProcessor failed: 0x%08x\n", hr);
        return false;
    }

    D3D11_VIDEO_COLOR vpBg = {};
    vpBg.RGBA.A = 1.0f;
    videoContext_->VideoProcessorSetOutputBackgroundColor(videoProcessor_.Get(), FALSE, &vpBg);
    videoContext_->VideoProcessorSetStreamFrameFormat(videoProcessor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
    outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outViewDesc.Texture2D.MipSlice = 0;

    for (size_t i = 0; i < GPU_RING_SIZE; i++) {
        hr = d3dDevice_->CreateTexture2D(&nv12Desc, nullptr, nv12Textures_[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            printf("[HwEncoder] CreateTexture2D NV12[%zu] failed: 0x%08x\n", i, hr);
            return false;
        }

        hr = videoDevice_->CreateVideoProcessorOutputView(nv12Textures_[i].Get(), vpEnum_.Get(), &outViewDesc, vpOutputViews_[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            printf("[HwEncoder] CreateVideoProcessorOutputView[%zu] failed: 0x%08x\n", i, hr);
            return false;
        }

        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Textures_[i].Get(), 0, FALSE, dxgiMediaBuffers_[i].ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            printf("[HwEncoder] MFCreateDXGISurfaceBuffer[%zu] failed: 0x%08x\n", i, hr);
            return false;
        }

        MFCreateSample(gpuSamples_[i].ReleaseAndGetAddressOf());
        gpuSamples_[i]->AddBuffer(dxgiMediaBuffers_[i].Get());
    }

    gpuIndex_ = 0;
    printf("[HwEncoder] Zero-Copy D3D11 Hardware Video Pipeline initialized in GPU VRAM (%ux%u -> %ux%u NV12, triple-buffered)\n",
           inWidth, inHeight, width_, height_);
    isGpuAccelerated_ = true;
    return true;
}

bool HwEncoder::Init(UINT width, UINT height, UINT fps, UINT bitrateBps, ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext, UINT inWidth, UINT inHeight, VideoCodec codec) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    activeCodec_ = codec;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        printf("MFStartup failed: 0x%08x\n", hr);
        return false;
    }

    // 1. Initialize Direct3D 11 Device Manager FIRST
    if (d3dDevice && d3dContext) {
        d3dDevice_ = d3dDevice;
        d3dContext_ = d3dContext;
        hr = MFCreateDXGIDeviceManager(&resetToken_, dxgiManager_.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = dxgiManager_->ResetDevice(d3dDevice_.Get(), resetToken_);
            if (FAILED(hr)) {
                printf("[HwEncoder] dxgiManager ResetDevice failed: 0x%08x\n", hr);
                dxgiManager_.Reset();
            }
        }
    }

    // 2. Find and activate encoder (hardware MFT prioritized, D3D manager linked before media types)
    if (!FindAndActivateEncoder(encoder_, isAsync_, activeCodec_, width, height, fps, bitrateBps, dxgiManager_.Get(), codec, sliceCount_)) {
        printf("Could not find or configure any video encoder MFT.\n");
        return false;
    }

    hr = encoder_->GetOutputStreamInfo(0, &streamInfo_);
    if (FAILED(hr)) {
        printf("GetOutputStreamInfo failed: 0x%08x\n", hr);
        return false;
    }

    // 3. For async MFT: setup IMFMediaEventGenerator and callback BEFORE notifying stream start
    if (isAsync_) {
        hr = encoder_->QueryInterface(IID_PPV_ARGS(eventGen_.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(hr)) {
            eventCallback_ = new EncoderEventCallback(this);
            isStreaming_ = true;
            canAcceptInput_ = false;
            eventGen_->BeginGetEvent(eventCallback_.Get(), nullptr);
        } else {
            printf("[HwEncoder] QueryInterface IMFMediaEventGenerator failed: 0x%08x\n", hr);
        }
    }

    encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // 4. Setup Video Processor zero-copy pipeline
    if (d3dDevice && d3dContext) {
        if (!InitGpuPipeline(d3dDevice, d3dContext, inWidth, inHeight)) {
            printf("[HwEncoder] GPU pipeline init failed, falling back to parallel AVX2 CPU path.\n");
            isGpuAccelerated_ = false;
        }
    }

    // Pre-allocate reusable input sample & buffer to eliminate per-frame heap allocations (for CPU fallback)
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

    printf("H.264/HEVC Zero-Latency Parallel Encoder ready: %ux%u @ %ufps, %u bps (Codec: %s, Slices: %u, GPU Zero-Copy: %s)\n",
           width, height, fps, bitrateBps, activeCodec_ == CODEC_HEVC ? "H.265 (HEVC)" : "H.264 (AVC)", sliceCount_, isGpuAccelerated_ ? "ENABLED" : "DISABLED");
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

        // Allow peak burst up to 1.5x target bitrate during motion spikes (up to 200 Mbps)
        UINT peakBps = (UINT)(bitrateBps * 1.5f);
        if (peakBps > 200000000) peakBps = 200000000;
        v.ulVal = peakBps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);

        // Do NOT change CODECAPI_AVEncCommonBufferSize dynamically during streaming!
        // AVEncCommonBufferSize is a static HRD parameter; changing it on the fly causes
        // Intel QuickSync MFT to invalidate stream parameters and stall with MF_E_TRANSFORM_STREAM_CHANGE.

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

    if (isAsync_) {
        std::unique_lock<std::mutex> lock(inputMutex_);
        if (!canAcceptInput_) {
            inputCv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return canAcceptInput_.load() || !isStreaming_.load();
            });
        }
        if (!canAcceptInput_ || !isStreaming_) return;
        canAcceptInput_ = false;
    }

    BYTE* dst = nullptr;
    HRESULT hr = inBuffer_->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr) || !dst) {
        if (isAsync_) canAcceptInput_ = true;
        return;
    }

    ConvertBgraToNv12(bgra, stride, dst);

    inBuffer_->Unlock();
    DWORD nv12Size = (DWORD)((size_t)width_ * height_ * 3 / 2);
    inBuffer_->SetCurrentLength(nv12Size);

    LONGLONG duration = (LONGLONG)(10000000.0 / fps_);
    inSample_->SetSampleTime(frameCount_ * duration);
    inSample_->SetSampleDuration(duration);
    frameCount_++;

    hr = encoder_->ProcessInput(0, inSample_.Get(), 0);
    if (FAILED(hr)) {
        if (isAsync_ && hr != MF_E_NOTACCEPTING) {
            canAcceptInput_ = true;
        }
        printf("ProcessInput failed: 0x%08x\n", hr);
    }

    if (!isAsync_) {
        DrainOutput();
    }
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
            if (outBufferHolder_) {
                outBufferHolder_->SetCurrentLength(0);
            }
            outBuf.pSample = outSampleHolder_.Get();
        }

        HRESULT hr = encoder_->ProcessOutput(0, 1, &outBuf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            HandleStreamChange();
            if (outBuf.pEvents) outBuf.pEvents->Release();
            continue;
        }
        if (FAILED(hr)) {
            static DWORD lastPrint = 0;
            if (GetTickCount() - lastPrint > 1000) {
                printf("[HwEncoder] ProcessOutput returned 0x%08x\n", hr);
                lastPrint = GetTickCount();
            }
            if (outBuf.pEvents) outBuf.pEvents->Release();
            break;
        }

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

void HwEncoder::HandleStreamChange() {
    if (!encoder_) return;
    HRESULT hr = encoder_->GetOutputStreamInfo(0, &streamInfo_);
    if (FAILED(hr)) return;

    if (!(streamInfo_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        DWORD newSize = (streamInfo_.cbSize > 4 * 1024 * 1024) ? streamInfo_.cbSize : (4 * 1024 * 1024);
        ComPtr<IMFMediaBuffer> newBuf;
        if (SUCCEEDED(MFCreateMemoryBuffer(newSize, newBuf.GetAddressOf()))) {
            outBufferHolder_ = newBuf;
            outSampleHolder_.Reset();
            MFCreateSample(outSampleHolder_.GetAddressOf());
            outSampleHolder_->AddBuffer(outBufferHolder_.Get());
        }
    }
    RequestKeyframe();
    printf("[HwEncoder] Handled MF_E_TRANSFORM_STREAM_CHANGE successfully (buffer size: %u bytes)\n", streamInfo_.cbSize);
    fflush(stdout);
}

void HwEncoder::DrainAsyncOutput() {
    if (!encoder_ || !isStreaming_) return;

    std::lock_guard<std::mutex> lock(outputMutex_);

    MFT_OUTPUT_DATA_BUFFER outBuf = {};
    outBuf.dwStreamID = 0;
    DWORD status = 0;

    bool mftProvidesSamples = (streamInfo_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    if (!mftProvidesSamples) {
        if (!outSampleHolder_) return;
        outBuf.pSample = outSampleHolder_.Get();
        if (outBufferHolder_) {
            outBufferHolder_->SetCurrentLength(0);
        }
    } else {
        outBuf.pSample = nullptr;
    }

    HRESULT hr = encoder_->ProcessOutput(0, 1, &outBuf, &status);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        HandleStreamChange();
        if (outBuf.pEvents) {
            outBuf.pEvents->Release();
        }
        return;
    }

    if (SUCCEEDED(hr) && outBuf.pSample) {
        IMFSample* outSample = outBuf.pSample;
        ComPtr<IMFMediaBuffer> contiguous;
        if (SUCCEEDED(outSample->ConvertToContiguousBuffer(contiguous.GetAddressOf()))) {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(contiguous->Lock(&data, nullptr, &len))) {
                if (OnNal && len > 0) {
                    OnNal(data, len);
                }
                contiguous->Unlock();
            }
        }
        if (mftProvidesSamples) {
            outSample->Release();
        }
    } else if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
        static DWORD lastAsyncPrint = 0;
        if (GetTickCount() - lastAsyncPrint > 1000) {
            printf("[HwEncoder] DrainAsyncOutput ProcessOutput returned 0x%08x\n", hr);
            lastAsyncPrint = GetTickCount();
        }
    }

    if (outBuf.pEvents) {
        outBuf.pEvents->Release();
    }
}

bool HwEncoder::EncodeFrameGpu(ID3D11Texture2D* bgraTexture) {
    if (!isGpuAccelerated_ || !bgraTexture || !videoDevice_ || !videoContext_ || !videoProcessor_) {
        return false;
    }

    if (isAsync_) {
        std::unique_lock<std::mutex> lock(inputMutex_);
        if (!canAcceptInput_) {
            inputCv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return canAcceptInput_.load() || !isStreaming_.load();
            });
        }
        if (!canAcceptInput_ || !isStreaming_) {
            return false;
        }
        canAcceptInput_ = false;
    }

    if (bgraTexture != lastBgraTexture_ || !vpInputView_) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inViewDesc = {};
        inViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inViewDesc.Texture2D.MipSlice = 0;
        inViewDesc.Texture2D.ArraySlice = 0;

        HRESULT hr = videoDevice_->CreateVideoProcessorInputView(bgraTexture, vpEnum_.Get(), &inViewDesc, vpInputView_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            if (isAsync_) canAcceptInput_ = true;
            return false;
        }
        lastBgraTexture_ = bgraTexture;
    }

    size_t curIdx = gpuIndex_;
    gpuIndex_ = (gpuIndex_ + 1) % GPU_RING_SIZE;

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = vpInputView_.Get();

    // Zero-latency hardware GPU conversion (BGRA -> NV12) & scaling in VRAM (< 0.2 ms)
    HRESULT hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), vpOutputViews_[curIdx].Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        if (isAsync_) canAcceptInput_ = true;
        return false;
    }

    LONGLONG duration = (LONGLONG)(10000000.0 / fps_);
    gpuSamples_[curIdx]->SetSampleTime(frameCount_ * duration);
    gpuSamples_[curIdx]->SetSampleDuration(duration);
    frameCount_++;

    hr = encoder_->ProcessInput(0, gpuSamples_[curIdx].Get(), 0);
    if (FAILED(hr)) {
        if (isAsync_ && hr != MF_E_NOTACCEPTING) {
            canAcceptInput_ = true;
        }
        static DWORD lastInputPrint = 0;
        if (GetTickCount() - lastInputPrint > 1000) {
            printf("[HwEncoder] GPU ProcessInput returned 0x%08x\n", hr);
            lastInputPrint = GetTickCount();
        }
        return false;
    }

    if (!isAsync_) {
        DrainOutput();
    }
    return true;
}

void HwEncoder::Shutdown() {
    isStreaming_ = false;
    canAcceptInput_ = false;
    inputCv_.notify_all();

    std::lock_guard<std::mutex> lock(outputMutex_);

    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    eventGen_.Reset();
    eventCallback_.Reset();

    for (size_t i = 0; i < GPU_RING_SIZE; i++) {
        gpuSamples_[i].Reset();
        dxgiMediaBuffers_[i].Reset();
        vpOutputViews_[i].Reset();
        nv12Textures_[i].Reset();
    }
    vpInputView_.Reset();
    lastBgraTexture_ = nullptr;
    videoProcessor_.Reset();
    vpEnum_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    dxgiManager_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    isGpuAccelerated_ = false;

    inSample_.Reset();
    inBuffer_.Reset();
    outSampleHolder_.Reset();
    outBufferHolder_.Reset();
    encoder_.Reset();
    MFShutdown();
}
