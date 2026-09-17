#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <stdio.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class AsyncMftHandler : public IMFAsyncCallback {
public:
    AsyncMftHandler(IMFTransform* enc, IMFMediaEventGenerator* gen)
        : encoder_(enc), eventGen_(gen), refCount_(1), frameCount_(0), outCount_(0) {
        nv12Size_ = 1920 * 1080 * 3 / 2;
        dummyNv12_.resize(nv12Size_, 128);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&refCount_);
        if (count == 0) delete this;
        return count;
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

    STDMETHODIMP Invoke(IMFAsyncResult* pAsyncResult) override {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = eventGen_->EndGetEvent(pAsyncResult, ev.GetAddressOf());
        if (FAILED(hr)) return hr;

        MediaEventType met = MEUnknown;
        ev->GetType(&met);

        if (met == METransformNeedInput) {
            if (frameCount_ < 10) {
                ComPtr<IMFMediaBuffer> inBuf;
                MFCreateMemoryBuffer((DWORD)nv12Size_, inBuf.GetAddressOf());
                BYTE* pDst = nullptr;
                inBuf->Lock(&pDst, nullptr, nullptr);
                memcpy(pDst, dummyNv12_.data(), nv12Size_);
                inBuf->Unlock();
                inBuf->SetCurrentLength((DWORD)nv12Size_);

                ComPtr<IMFSample> inSample;
                MFCreateSample(inSample.GetAddressOf());
                inSample->AddBuffer(inBuf.Get());
                inSample->SetSampleTime(frameCount_ * 166666);
                inSample->SetSampleDuration(166666);

                hr = encoder_->ProcessInput(0, inSample.Get(), 0);
                printf("  [Callback] ProcessInput (frame %lld): hr=0x%08x\n", frameCount_, hr);
                frameCount_++;
            }
        } else if (met == METransformHaveOutput) {
            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            DWORD status = 0;
            hr = encoder_->ProcessOutput(0, 1, &outBuf, &status);
            if (SUCCEEDED(hr) && outBuf.pSample) {
                DWORD len = 0;
                outBuf.pSample->GetTotalLength(&len);
                printf("  >>> [Callback] HW OUTPUT %d: %u bytes!\n", outCount_, len);
                outBuf.pSample->Release();
                outCount_++;
            }
            if (outBuf.pEvents) outBuf.pEvents->Release();
        }

        // Request next event
        eventGen_->BeginGetEvent(this, nullptr);
        return S_OK;
    }

    int GetOutCount() const { return outCount_; }

private:
    IMFTransform* encoder_;
    IMFMediaEventGenerator* eventGen_;
    long refCount_;
    long long frameCount_;
    int outCount_;
    size_t nv12Size_;
    std::vector<uint8_t> dummyNv12_;
};

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inType, &outType, &activates, &count);

    ComPtr<IMFTransform> encoder;
    for (UINT32 i = 0; i < count; i++) {
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(encoder.ReleaseAndGetAddressOf())))) {
            ComPtr<IMFAttributes> attrs;
            encoder->GetAttributes(attrs.GetAddressOf());
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);

            ComPtr<IMFMediaType> outMediaType;
            MFCreateMediaType(outMediaType.GetAddressOf());
            outMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            outMediaType->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
            outMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(outMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
            MFSetAttributeRatio(outMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
            MFSetAttributeRatio(outMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            outMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
            if (FAILED(encoder->SetOutputType(0, outMediaType.Get(), 0))) continue;

            ComPtr<IMFMediaType> inMediaType;
            MFCreateMediaType(inMediaType.GetAddressOf());
            inMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(inMediaType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_FRAME_RATE, 60, 1);
            MFSetAttributeRatio(inMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (FAILED(encoder->SetInputType(0, inMediaType.Get(), 0))) continue;

            printf("Activated hardware encoder %u!\n", i);
            break;
        }
    }
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(encoder.As(&codecApi))) {
        VARIANT v; VariantInit(&v);
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
        codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.vt = VT_UI4; v.ulVal = 60;
        codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
    }

    ComPtr<IMFMediaEventGenerator> eventGen;
    encoder.As(&eventGen);

    AsyncMftHandler* handler = new AsyncMftHandler(encoder.Get(), eventGen.Get());
    eventGen->BeginGetEvent(handler, nullptr);

    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    for (int s = 0; s < 50; s++) {
        Sleep(20);
        if (handler->GetOutCount() >= 5) break;
    }

    printf("Done! OutCount = %d\n", handler->GetOutCount());
    handler->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
