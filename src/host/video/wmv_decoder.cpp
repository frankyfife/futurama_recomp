// WMV2 decoder on the Windows WMVideo Decoder DMO (see wmv_decoder.h).
#include "video/wmv_decoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <dmo.h>
#include <amvideo.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace vid {
namespace {

// GUIDs (defined here instead of linking strmiids/dmoguids/wmcodecdspuuid).
constexpr GUID kClsidWmvDecoderDmo = {0x82D353DF, 0x90BD, 0x4382, {0x8B, 0xC2, 0x3F, 0x61, 0x92, 0xB7, 0x6E, 0x34}};
constexpr GUID kMediaTypeVideo = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kFormatVideoInfo = {0x05589F80, 0xC356, 0x11CE, {0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A}};
constexpr DWORD FourCC(char a, char b, char c, char d) {
    return static_cast<DWORD>(static_cast<unsigned char>(a)) | (static_cast<DWORD>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
}
// Media subtype of a FourCC: {FourCC-0000-0010-8000-00AA00389B71}.
GUID SubtypeOf(DWORD fourcc) {
    GUID g = {fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
    return g;
}
constexpr DWORD kFourccWmv2 = FourCC('W', 'M', 'V', '2');
constexpr DWORD kFourccI420 = FourCC('I', '4', '2', '0');
constexpr DWORD kFourccIyuv = FourCC('I', 'Y', 'U', 'V');
constexpr DWORD kFourccYv12 = FourCC('Y', 'V', '1', '2');

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);

std::string Hr(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

void FreeType(DMO_MEDIA_TYPE& t) noexcept {
    if (t.cbFormat != 0 && t.pbFormat != nullptr) {
        CoTaskMemFree(t.pbFormat);
    }
    t.cbFormat = 0;
    t.pbFormat = nullptr;
    if (t.pUnk != nullptr) {
        t.pUnk->Release();
        t.pUnk = nullptr;
    }
}

// IMediaBuffer over a std::vector; lives on our side (AddRef/Release only count, the owner frees it).
class Buffer final : public IMediaBuffer {
public:
    std::vector<BYTE> bytes;
    DWORD length = 0;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMediaBuffer)) {
            *ppv = static_cast<IMediaBuffer*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override { return static_cast<ULONG>(--refs_); }
    STDMETHODIMP SetLength(DWORD cbLength) override {
        if (cbLength > bytes.size()) {
            return E_INVALIDARG;
        }
        length = cbLength;
        return S_OK;
    }
    STDMETHODIMP GetMaxLength(DWORD* pcbMaxLength) override {
        if (pcbMaxLength == nullptr) {
            return E_POINTER;
        }
        *pcbMaxLength = static_cast<DWORD>(bytes.size());
        return S_OK;
    }
    STDMETHODIMP GetBufferAndLength(BYTE** ppBuffer, DWORD* pcbLength) override {
        if (ppBuffer != nullptr) {
            *ppBuffer = bytes.data();
        }
        if (pcbLength != nullptr) {
            *pcbLength = length;
        }
        return S_OK;
    }

private:
    std::atomic<long> refs_{1};
};

// Class factory of wmvdecod.dll (loaded once, never unloaded: the DMO may keep worker threads in it).
HRESULT CreateDmo(IMediaObject** out, std::string& error) {
    static HMODULE dll = nullptr;
    static DllGetClassObjectFn getClassObject = nullptr;
    if (dll == nullptr) {
        dll = LoadLibraryExW(L"wmvdecod.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll == nullptr) {
            error = "wmvdecod.dll (Windows WMV decoder) not found";
            return HRESULT_FROM_WIN32(GetLastError());
        }
        getClassObject = reinterpret_cast<DllGetClassObjectFn>(
            reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
    }
    if (getClassObject == nullptr) {
        error = "wmvdecod.dll has no DllGetClassObject";
        return E_NOINTERFACE;
    }
    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(kClsidWmvDecoderDmo, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        error = Hr("wmvdecod.dll DllGetClassObject(CLSID_CWMVDecMediaObject)", hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    hr = factory->CreateInstance(nullptr, __uuidof(IMediaObject), reinterpret_cast<void**>(out));
    factory->Release();
    if (FAILED(hr) || *out == nullptr) {
        error = Hr("WMVideo Decoder DMO CreateInstance", hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    return S_OK;
}

}  // namespace

struct WmvDecoder::Impl {
    IMediaObject* mo = nullptr;
    std::uint32_t width = 0, height = 0;
    bool swapUv = false;  // YV12 output
    std::uint32_t outStride = 0;
    Buffer in;
    Buffer out;
    std::string error;

    ~Impl() {
        if (mo != nullptr) {
            mo->FreeStreamingResources();
            mo->Release();
        }
    }

    // Copies the DMO output (I420/IYUV/YV12, stride outStride) into `frame`.
    void CopyOut(I420Frame& frame) const {
        const std::uint32_t w = width, h = height, cw = w / 2, ch = h / 2;
        frame.width = w;
        frame.height = h;
        frame.data.resize(static_cast<std::size_t>(w) * h + 2u * static_cast<std::size_t>(cw) * ch);
        const BYTE* src = out.bytes.data();
        const std::uint32_t stride = outStride;
        const std::uint32_t cstride = stride / 2;
        std::uint8_t* dst = frame.data.data();
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * w, src + static_cast<std::size_t>(y) * stride, w);
        }
        const BYTE* p1 = src + static_cast<std::size_t>(stride) * h;
        const BYTE* p2 = p1 + static_cast<std::size_t>(cstride) * ch;
        const BYTE* u = swapUv ? p2 : p1;
        const BYTE* v = swapUv ? p1 : p2;
        std::uint8_t* du = dst + static_cast<std::size_t>(w) * h;
        std::uint8_t* dv = du + static_cast<std::size_t>(cw) * ch;
        for (std::uint32_t y = 0; y < ch; ++y) {
            std::memcpy(du + static_cast<std::size_t>(y) * cw, u + static_cast<std::size_t>(y) * cstride, cw);
            std::memcpy(dv + static_cast<std::size_t>(y) * cw, v + static_cast<std::size_t>(y) * cstride, cw);
        }
    }

    // Pulls every pending output picture; keeps the last one.
    bool Pull(I420Frame& frame, bool& produced) {
        for (int guard = 0; guard < 8; ++guard) {
            DMO_OUTPUT_DATA_BUFFER odb{};
            out.length = 0;
            odb.pBuffer = &out;
            DWORD status = 0;
            const HRESULT hr = mo->ProcessOutput(0, 1, &odb, &status);
            if (odb.pBuffer != nullptr && odb.pBuffer != &out) {
                odb.pBuffer->Release();
            }
            if (hr == S_FALSE || (SUCCEEDED(hr) && out.length == 0)) {
                return true;
            }
            if (FAILED(hr)) {
                error = Hr("IMediaObject::ProcessOutput", hr);
                return false;
            }
            if (out.length < static_cast<DWORD>(outStride) * height * 3 / 2) {
                error = "IMediaObject::ProcessOutput: short picture";
                return false;
            }
            CopyOut(frame);
            produced = true;
            if ((odb.dwStatus & DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE) == 0) {
                return true;
            }
        }
        return true;
    }
};

WmvDecoder::WmvDecoder() = default;
WmvDecoder::~WmvDecoder() = default;

bool WmvDecoder::IsOpen() const noexcept {
    return impl_ != nullptr && impl_->mo != nullptr;
}

const std::string& WmvDecoder::LastError() const noexcept {
    static const std::string kNone = "decoder not open";
    return impl_ != nullptr ? impl_->error : kNone;
}

bool WmvDecoder::Open(std::uint32_t width, std::uint32_t height, const std::array<std::uint8_t, 4>& extradata,
                      std::string& error) {
    try {
        impl_ = std::make_unique<Impl>();
        Impl& d = *impl_;
        d.width = width;
        d.height = height;
        HRESULT hr = CreateDmo(&d.mo, error);
        if (FAILED(hr)) {
            impl_.reset();
            return false;
        }
        std::vector<BYTE> fmt(sizeof(VIDEOINFOHEADER) + extradata.size(), 0);
        auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(fmt.data());
        vih->rcSource = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        vih->rcTarget = vih->rcSource;
        vih->bmiHeader.biSize = static_cast<DWORD>(sizeof(BITMAPINFOHEADER) + extradata.size());
        vih->bmiHeader.biWidth = static_cast<LONG>(width);
        vih->bmiHeader.biHeight = static_cast<LONG>(height);
        vih->bmiHeader.biPlanes = 1;
        vih->bmiHeader.biBitCount = 24;
        vih->bmiHeader.biCompression = kFourccWmv2;
        std::memcpy(fmt.data() + sizeof(VIDEOINFOHEADER), extradata.data(), extradata.size());
        DMO_MEDIA_TYPE in{};
        in.majortype = kMediaTypeVideo;
        in.subtype = SubtypeOf(kFourccWmv2);
        in.bTemporalCompression = TRUE;
        in.formattype = kFormatVideoInfo;
        in.cbFormat = static_cast<ULONG>(fmt.size());
        in.pbFormat = fmt.data();
        hr = d.mo->SetInputType(0, &in, 0);
        if (FAILED(hr)) {
            error = Hr("WMVideo Decoder DMO SetInputType(WMV2)", hr);
            impl_.reset();
            return false;
        }
        bool typed = false;
        for (DWORD i = 0; !typed; ++i) {
            DMO_MEDIA_TYPE t{};
            if (d.mo->GetOutputType(0, i, &t) != S_OK) {
                break;
            }
            const DWORD fourcc = t.subtype.Data1;
            const bool planar = t.subtype == SubtypeOf(kFourccI420) || t.subtype == SubtypeOf(kFourccIyuv) ||
                                t.subtype == SubtypeOf(kFourccYv12);
            if (planar && SUCCEEDED(d.mo->SetOutputType(0, &t, 0))) {
                typed = true;
                d.swapUv = fourcc == kFourccYv12;
            }
            FreeType(t);
        }
        if (!typed) {
            error = "WMVideo Decoder DMO offers no planar 4:2:0 output (I420/IYUV/YV12)";
            impl_.reset();
            return false;
        }
        DWORD size = 0, align = 0;
        hr = d.mo->GetOutputSizeInfo(0, &size, &align);
        const DWORD minimum = width * height * 3 / 2;
        if (FAILED(hr) || size < minimum) {
            size = minimum;
        }
        d.outStride = width;  // packed planes (size info == w*h*3/2 for the game's 640x480)
        d.out.bytes.assign(size, 0);
        d.mo->AllocateStreamingResources();
        return true;
    } catch (...) {
        error = "out of memory";
        impl_.reset();
        return false;
    }
}

bool WmvDecoder::Decode(std::span<const std::uint8_t> frame, std::uint32_t ptsMs, I420Frame& out, bool& produced) {
    produced = false;
    if (!IsOpen()) {
        return false;
    }
    Impl& d = *impl_;
    try {
        d.in.bytes.assign(frame.begin(), frame.end());
        d.in.length = static_cast<DWORD>(frame.size());
    } catch (...) {
        d.error = "out of memory";
        return false;
    }
    HRESULT hr = d.mo->ProcessInput(0, &d.in, DMO_INPUT_DATA_BUFFERF_TIME,
                                    static_cast<REFERENCE_TIME>(ptsMs) * 10000, 0);
    if (hr == DMO_E_NOTACCEPTING) {
        if (!d.Pull(out, produced)) {
            return false;
        }
        hr = d.mo->ProcessInput(0, &d.in, DMO_INPUT_DATA_BUFFERF_TIME, static_cast<REFERENCE_TIME>(ptsMs) * 10000,
                                0);
    }
    if (FAILED(hr)) {
        d.error = Hr("IMediaObject::ProcessInput", hr);
        return false;
    }
    return d.Pull(out, produced);
}

bool WmvDecoder::Drain(I420Frame& out, bool& produced) {
    produced = false;
    if (!IsOpen()) {
        return false;
    }
    impl_->mo->Discontinuity(0);
    return impl_->Pull(out, produced);
}

bool WmvDecoderAvailable(std::string* error) {
    IMediaObject* mo = nullptr;
    std::string text;
    const HRESULT hr = CreateDmo(&mo, text);
    if (mo != nullptr) {
        mo->Release();
    }
    if (FAILED(hr) && error != nullptr) {
        *error = text;
    }
    return SUCCEEDED(hr);
}

}  // namespace vid
