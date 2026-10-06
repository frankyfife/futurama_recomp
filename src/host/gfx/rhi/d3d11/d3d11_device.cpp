// D3D11 backend: device, resources, caches, Present, read-backs, debug layer (d3d11_device.h).
#include "gfx/rhi/d3d11/d3d11_internal.h"

#include "core/host_main.h"
#include "core/log.h"

#include "d3d11_clear_vs.h"  // gen_shaders/ (fut_add_shader, block "backend agent" in src/host/CMakeLists.txt)
#include "d3d11_diag_color_ps.h"
#include "d3d11_diag_texture_ps.h"
#include "d3d11_diag_tint_ps.h"
#include "d3d11_diag_vs.h"
#include "d3d11_present_ps.h"
#include "d3d11_present_vs.h"
#include "d3d11_resample_ps.h"  // M3c: supersampling downsample / present downscale

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// WKPDID_D3DDebugObjectName, DXGI_DEBUG_DXGI (DEFINE_GUID without INITGUID -> dxguid.lib).
#pragma comment(lib, "dxguid.lib")

namespace gfx::rhi::d3d11 {
namespace {

// ---- process-wide debug-layer state (exit criterion 2) ----------------------------------------------------

struct GlobalDebug {
    std::mutex mutex;  // guards layer / haveWindowLayer / windowDevice
    DebugLayerInfo layer;
    bool haveWindowLayer = false;
    // The device of the window and the thread of its last Present: the exit line pumps that device's
    // queues once more when the exit runs on that thread (M3 review findings 5/10).
    D3D11Device* windowDevice = nullptr;
    std::atomic<DWORD> presentThread{0};
    std::atomic<std::uint32_t> devices{0};
    std::atomic<std::uint32_t> errors{0};
    std::atomic<std::uint32_t> warnings{0};
    std::atomic<std::uint32_t> corruption{0};
    std::atomic<std::uint32_t> other{0};
    std::atomic<int> lastLive{-1};
};

GlobalDebug& Global() noexcept {
    static GlobalDebug g;
    return g;
}

constexpr std::size_t kMaxPendingMessages = 256;
constexpr std::uint32_t kMaxLinesPerMessageId = 10;

const char* SeverityName(DebugSeverity s) noexcept {
    switch (s) {
    case DebugSeverity::Corruption:
        return "corruption";
    case DebugSeverity::Error:
        return "error";
    case DebugSeverity::Warning:
        return "warning";
    case DebugSeverity::Info:
        return "info";
    case DebugSeverity::Message:
        return "message";
    }
    return "?";
}

// D3D11_MESSAGE_SEVERITY and DXGI_INFO_QUEUE_MESSAGE_SEVERITY share the values 0 corruption .. 4 message.
DebugSeverity SeverityOf(int s) noexcept {
    switch (s) {
    case 0:
        return DebugSeverity::Corruption;
    case 1:
        return DebugSeverity::Error;
    case 2:
        return DebugSeverity::Warning;
    case 3:
        return DebugSeverity::Info;
    default:
        return DebugSeverity::Message;
    }
}

std::string Narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

const char* FeatureLevelName(D3D_FEATURE_LEVEL level) noexcept {
    switch (level) {
    case D3D_FEATURE_LEVEL_11_1:
        return "D3D11 feature level 11_1";
    case D3D_FEATURE_LEVEL_11_0:
        return "D3D11 feature level 11_0";
    case D3D_FEATURE_LEVEL_10_1:
        return "D3D11 feature level 10_1";
    case D3D_FEATURE_LEVEL_10_0:
        return "D3D11 feature level 10_0";
    default:
        return "D3D11 feature level ?";
    }
}

std::uint32_t MaxMipLevels(std::uint32_t w, std::uint32_t h) noexcept {
    std::uint32_t n = 1;
    std::uint32_t m = w > h ? w : h;
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

std::uint32_t MipSize(std::uint32_t size, std::uint32_t level) noexcept {
    const std::uint32_t s = level < 32 ? size >> level : 0;
    return s != 0 ? s : 1;
}

}  // namespace

// ---- public helpers ----------------------------------------------------------------------------------------

ShaderBytecode BuiltinShaderCode(BuiltinShader shader) noexcept {
    switch (shader) {
    case BuiltinShader::ClearVS:
        return {ShaderFormat::Dxbc, g_d3d11_clear_vs, sizeof(g_d3d11_clear_vs)};
    case BuiltinShader::DiagVS:
        return {ShaderFormat::Dxbc, g_d3d11_diag_vs, sizeof(g_d3d11_diag_vs)};
    case BuiltinShader::DiagColorPS:
        return {ShaderFormat::Dxbc, g_d3d11_diag_color_ps, sizeof(g_d3d11_diag_color_ps)};
    case BuiltinShader::DiagTexturePS:
        return {ShaderFormat::Dxbc, g_d3d11_diag_texture_ps, sizeof(g_d3d11_diag_texture_ps)};
    case BuiltinShader::DiagTintPS:
        return {ShaderFormat::Dxbc, g_d3d11_diag_tint_ps, sizeof(g_d3d11_diag_tint_ps)};
    }
    return {};
}

VertexLayoutDesc DiagVertexLayout() noexcept {
    VertexLayoutDesc l;
    l.attributes[0] = {VertexSemantic::Position, 0, VertexFormat::Float3, 0, 0};
    l.attributes[1] = {VertexSemantic::Color, 0, VertexFormat::Float4, 0, 12};
    l.attributes[2] = {VertexSemantic::TexCoord, 0, VertexFormat::Float2, 0, 28};
    l.attributeCount = 3;
    l.streamStrides[0] = static_cast<std::uint16_t>(kDiagVertexStride);
    return l;
}

DebugLayerTotals GetDebugLayerTotals() noexcept {
    GlobalDebug& g = Global();
    DebugLayerTotals t;
    try {
        std::lock_guard<std::mutex> lock(g.mutex);
        t.layer = g.layer;
    } catch (...) {
    }
    t.devices = g.devices.load();
    t.errors = g.errors.load();
    t.warnings = g.warnings.load();
    t.corruption = g.corruption.load();
    t.other = g.other.load();
    return t;
}

const char* DebugLayerStateName(const DebugLayerInfo& layer) noexcept {
    if (layer.enabled) {
        return "enabled";
    }
    if (!layer.requested) {
        return "off";
    }
    return layer.available ? "unavailable" : "not installed";
}

void LogDebugLayerSummary() noexcept {
    try {
        // Collect what the layer queued since the last Present (only on the render thread: the device and
        // its info queue belong to it; the window device cannot be destroyed while that thread is here).
        GlobalDebug& g = Global();
        D3D11Device* device = nullptr;
        bool otherThread = false;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (g.windowDevice != nullptr) {
                if (g.presentThread.load() == GetCurrentThreadId()) {
                    device = g.windowDevice;
                } else {
                    otherThread = true;
                }
            }
        }
        if (device != nullptr) {
            device->PumpDebugMessages();
        }
        const DebugLayerTotals t = GetDebugLayerTotals();
        FUT_LOGI(Gfx, "D3D11 debug layer: %s - %u errors, %u warnings, %u corruption (%u device%s%s%s%s)",
                 DebugLayerStateName(t.layer), t.errors, t.warnings, t.corruption, t.devices,
                 t.devices == 1 ? "" : "s", t.layer.note.empty() ? "" : "; ", t.layer.note.c_str(),
                 otherThread ? "; messages since the last Present not collected: the exit runs on another thread"
                             : "");
    } catch (...) {
    }
}

int LastTeardownLiveObjects() noexcept {
    return Global().lastLive.load();
}

// ---- device ------------------------------------------------------------------------------------------------

D3D11Device::D3D11Device() noexcept : context_(*this) {}

D3D11Device::~D3D11Device() {
    try {
        {
            GlobalDebug& g = Global();
            std::lock_guard<std::mutex> lock(g.mutex);
            if (g.windowDevice == this) {
                g.windowDevice = nullptr;
            }
        }
        PumpDebugMessages();
        context_.Release();
        pipelines_.clear();
        pipelineCache_.clear();
        samplers_.clear();
        samplerCache_.clear();
        layouts_.clear();
        buffers_.Clear();
        textures_.Clear();
        shaders_.Clear();
        captureStaging_.Reset();
        outputStaging_.Reset();
        targets_ = Targets{};
        presentVs_.Reset();
        presentPs_.Reset();
        resamplePs_.Reset();
        resampleConstants_.Reset();
        for (GpuTimer& t : gpuTimers_) {
            t = GpuTimer{};
        }
        presentPoint_.Reset();
        presentLinear_.Reset();
        presentRaster_.Reset();
        presentDepth_.Reset();
        presentBlend_.Reset();
        swapRtv_.Reset();
        swapSrgbRtv_.Reset();
        if (latencyWaitable_ != nullptr) {
            CloseHandle(latencyWaitable_);  // GetFrameLatencyWaitableObject: the caller closes the handle
            latencyWaitable_ = nullptr;
        }
        swapChain_.Reset();
        if (immediate_ != nullptr) {
            immediate_->ClearState();
            immediate_->Flush();
        }
        PumpDebugMessages();
        immediate_.Reset();
        ReportLiveObjects();
        if (dxgiInfoQueue_ != nullptr) {
            dxgiInfoQueue_->PopStorageFilter(DXGI_DEBUG_DXGI);  // the queue is process-wide
        }
        dxgiInfoQueue_.Reset();
        infoQueue_.Reset();
        factory_.Reset();
        device_.Reset();
    } catch (...) {
    }
}

bool D3D11Device::Init(const DeviceDesc& desc, std::string* error) noexcept {
    try {
        if (desc.width == 0 || desc.height == 0 || desc.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            *error = "invalid back buffer size";
            return false;
        }
        if (desc.backBufferFormat != Format::BGRA8_UNorm && desc.backBufferFormat != Format::RGBA8_UNorm) {
            *error = std::string("unsupported back buffer format ") + GetFormatInfo(desc.backBufferFormat).name;
            return false;
        }
        if (desc.depthFormat != Format::Unknown && desc.depthFormat != Format::D24_UNorm_S8_UInt &&
            desc.depthFormat != Format::D32_Float) {
            *error = std::string("unsupported depth format ") + GetFormatInfo(desc.depthFormat).name;
            return false;
        }
        // M3c: output size (0 x 0 = the back buffer size).
        if ((desc.outputWidth == 0) != (desc.outputHeight == 0) ||
            desc.outputWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.outputHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            *error = "invalid output size";
            return false;
        }
        debugLayer_.requested = desc.debugLayer;
        if (!CreateFactoryAndDevice(desc, error)) {
            return false;
        }
        SetUpDebugLayer();
        info_.backBufferWidth = desc.width;
        info_.backBufferHeight = desc.height;
        info_.backBufferFormat = desc.backBufferFormat;
        info_.depthFormat = desc.depthFormat;
        backBufferCount_ = std::clamp<std::uint32_t>(desc.bufferCount, 1, kMaxBackBuffers);
        currentBackBuffer_ = 0;
        // Review M3c finding 3: both supported back buffer formats have an sRGB variant.
        linearResolve_ = desc.linearResolve;
        // M3c: the sample count the device supports for the back buffer and depth formats (<= requested).
        const std::uint32_t samples = SupportedSampleCount(desc.backBufferFormat, desc.depthFormat, desc.sampleCount);
        if (!CreateTargets(desc.width, desc.height, samples, desc.outputWidth, desc.outputHeight, &targets_, error)) {
            return false;
        }
        if (!CreatePresentPass(error)) {
            return false;
        }
        if (desc.nativeWindow != nullptr && !CreateSwapChain(desc, error)) {
            return false;
        }
        info_.backend = Backend::D3D11;
        info_.headless = desc.nativeWindow == nullptr;
        info_.backBufferWidth = desc.width;
        info_.backBufferHeight = desc.height;
        info_.sampleCount = targets_.samples;
        info_.outputWidth = targets_.outputWidth;
        info_.outputHeight = targets_.outputHeight;
        info_.linearResolve = linearResolve_;
        info_.tearingSupported = swapChain_ != nullptr && tearingSupported_;
        context_.Init(immediate_.Get());
        context_.BindDefaultTargets();

        GlobalDebug& g = Global();
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (desc.nativeWindow != nullptr || !g.haveWindowLayer) {
                g.layer = debugLayer_;
                g.haveWindowLayer = g.haveWindowLayer || desc.nativeWindow != nullptr;
            }
            if (desc.nativeWindow != nullptr) {
                g.windowDevice = this;
                g.presentThread.store(GetCurrentThreadId());
            }
        }
        g.devices.fetch_add(1);
        if (desc.nativeWindow != nullptr) {
            static std::once_flag reporter;
            std::call_once(reporter, [] { core::AddExitReporter("d3d11", &LogDebugLayerSummary); });
        }
        if (desc.debugLayer && !debugLayer_.enabled) {
            FUT_LOGW(Gfx, "D3D11 debug layer requested but %s: %s", DebugLayerStateName(debugLayer_),
                     debugLayer_.note.c_str());
        }
        char target[96];
        if (info_.headless) {
            std::snprintf(target, sizeof(target), "headless");
        } else {
            std::snprintf(target, sizeof(target), "swap chain %ux%u, scaling %s", swapWidth_, swapHeight_,
                          PresentScalingName(scaling_));
        }
        FUT_LOGI(Gfx, "d3d11: device on \"%s\" (%s%s), %s, %u back buffers %ux%u %s (%u sample%s), depth %s, "
                 "output %ux%u, resolve %s, debug layer %s",
                 info_.adapterName.c_str(), info_.apiVersion.c_str(), info_.softwareAdapter ? ", software" : "",
                 target, backBufferCount_, desc.width, desc.height, GetFormatInfo(desc.backBufferFormat).name,
                 targets_.samples, targets_.samples == 1 ? "" : "s", GetFormatInfo(desc.depthFormat).name,
                 targets_.outputWidth, targets_.outputHeight, linearResolve_ ? "linear light" : "gamma",
                 DebugLayerStateName(debugLayer_));
        PumpDebugMessages();
        return true;
    } catch (...) {
        try {
            *error = "out of memory while creating the D3D11 device";
        } catch (...) {
        }
        return false;
    }
}

bool D3D11Device::CreateFactoryAndDevice(const DeviceDesc& desc, std::string* error) noexcept {
    try {
        ComPtr<IDXGIFactory2> factory;
        HRESULT hr = CreateDXGIFactory2(desc.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0u, IID_PPV_ARGS(&factory));
        bool dxgiDebugMissing = false;
        if (FAILED(hr) && desc.debugLayer) {
            dxgiDebugMissing = true;  // dxgidebug.dll (Graphics Tools) missing
            hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        }
        if (FAILED(hr)) {
            *error = "CreateDXGIFactory2 failed: " + HresultText(hr);
            return false;
        }
        ComPtr<IDXGIAdapter1> adapter;
        if (desc.softwareAdapter) {
            ComPtr<IDXGIFactory4> f4;
            if (SUCCEEDED(factory.As(&f4))) {
                f4->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
            }
        } else {
            ComPtr<IDXGIFactory6> f6;
            if (SUCCEEDED(factory.As(&f6))) {
                f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            }
            if (adapter == nullptr) {
                factory->EnumAdapters1(0, &adapter);
            }
        }
        const D3D_DRIVER_TYPE type = adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN
                                     : desc.softwareAdapter ? D3D_DRIVER_TYPE_WARP
                                                            : D3D_DRIVER_TYPE_HARDWARE;
        static constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (desc.debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0u);
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        auto create = [&]() {
            HRESULT r = D3D11CreateDevice(adapter.Get(), type, nullptr, flags, kLevels, 4, D3D11_SDK_VERSION, &device,
                                          &level, &context);
            if (r == E_INVALIDARG) {  // runtime without 11_1
                r = D3D11CreateDevice(adapter.Get(), type, nullptr, flags, kLevels + 1, 3, D3D11_SDK_VERSION,
                                      &device, &level, &context);
            }
            return r;
        };
        hr = create();
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
            debugLayer_.available = false;
            debugLayer_.note = "D3D11 SDK layers not installed (DXGI_ERROR_SDK_COMPONENT_MISSING; Windows optional "
                               "feature \"Graphics Tools\")";
            flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
            hr = create();
        } else if (desc.debugLayer) {
            debugLayer_.available = true;
        }
        if (FAILED(hr)) {
            *error = "D3D11CreateDevice failed: " + HresultText(hr);
            return false;
        }
        if (level < D3D_FEATURE_LEVEL_10_0) {
            *error = "feature level below 10_0";
            return false;
        }
        if (FAILED(device.As(&device_)) || FAILED(context.As(&immediate_))) {
            *error = "the D3D11.1 runtime is required (ID3D11Device1 / ID3D11DeviceContext1)";
            return false;
        }
        debugLayer_.enabled = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
        if (debugLayer_.enabled && dxgiDebugMissing) {
            debugLayer_.note = "DXGI debug layer (dxgidebug.dll) missing: D3D11 messages only";
        }
        info_.apiVersion = FeatureLevelName(level);

        // The swap chain must come from the factory of the device's adapter.
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> deviceAdapter;
        if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&deviceAdapter)) ||
            FAILED(deviceAdapter->GetParent(IID_PPV_ARGS(&factory_)))) {
            *error = "cannot get the DXGI factory of the D3D11 device";
            return false;
        }
        ComPtr<IDXGIAdapter1> adapter1;
        DXGI_ADAPTER_DESC1 ad{};
        if (SUCCEEDED(deviceAdapter.As(&adapter1)) && SUCCEEDED(adapter1->GetDesc1(&ad))) {
            info_.adapterName = Narrow(ad.Description);
            info_.softwareAdapter = desc.softwareAdapter || (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        } else {
            info_.adapterName = "?";
            info_.softwareAdapter = desc.softwareAdapter;
        }
        // Keep the CPU at most two frames ahead of the GPU. A swap chain with a frame-latency waitable object
        // (DeviceDesc::frameLatencyWaitable, M3b pacing) has its own latency instead (CreateSwapChain).
        ComPtr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(device_.As(&dxgiDevice1))) {
            dxgiDevice1->SetMaximumFrameLatency(2);
        }
        return true;
    } catch (...) {
        *error = "out of memory while creating the D3D11 device";
        return false;
    }
}

bool D3D11Device::SetUpDebugLayer() noexcept {
    if (!debugLayer_.enabled) {
        return false;
    }
    if (SUCCEEDED(device_.As(&infoQueue_))) {
        infoQueue_->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoQueue_->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
        infoQueue_->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
        infoQueue_->SetMessageCountLimit(4096);
        D3D11_MESSAGE_SEVERITY deny[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
        D3D11_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = 2;
        filter.DenyList.pSeverityList = deny;
        infoQueue_->PushStorageFilter(&filter);
    }
    ComPtr<IDXGIInfoQueue> q;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&q)))) {
        q->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        q->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, FALSE);
        q->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, FALSE);
        DXGI_INFO_QUEUE_MESSAGE_SEVERITY deny[] = {DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO,
                                                   DXGI_INFO_QUEUE_MESSAGE_SEVERITY_MESSAGE};
        DXGI_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = 2;
        filter.DenyList.pSeverityList = deny;
        q->PushStorageFilter(DXGI_DEBUG_DXGI, &filter);
        // The DXGI queue is process-wide: drop what other (earlier) devices left behind.
        q->ClearStoredMessages(DXGI_DEBUG_DXGI);
        dxgiInfoQueue_ = q;
    }
    return infoQueue_ != nullptr;
}

namespace {

// Client size of the swap chain's window (the 4:3 view of platform/window.h) in physical pixels; 0 x 0 while
// minimized. The view is per-monitor DPI aware (V2), the calling (guest main) thread is not: GetClientRect
// would return DPI-virtualized sizes (e.g. 1169 x 876 for 2631 x 1973 at 225 %), so the query runs in the
// window's DPI awareness context.
void ClientSize(void* window, std::uint32_t* w, std::uint32_t* h) noexcept {
    RECT rc{};
    const HWND hwnd = static_cast<HWND>(window);
    DPI_AWARENESS_CONTEXT previous = nullptr;
    if (hwnd != nullptr) {
        previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd));
    }
    const BOOL ok = hwnd != nullptr && GetClientRect(hwnd, &rc);
    if (previous != nullptr) {
        SetThreadDpiAwarenessContext(previous);
    }
    if (ok) {
        *w = static_cast<std::uint32_t>(rc.right > rc.left ? rc.right - rc.left : 0);
        *h = static_cast<std::uint32_t>(rc.bottom > rc.top ? rc.bottom - rc.top : 0);
    } else {
        *w = 0;
        *h = 0;
    }
}

}  // namespace

bool D3D11Device::CreateSwapChain(const DeviceDesc& desc, std::string* error) noexcept {
    try {
        const HWND hwnd = static_cast<HWND>(desc.nativeWindow);
        window_ = desc.nativeWindow;
        scaling_ = desc.scaling;
        ComPtr<IDXGIFactory5> f5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory_.As(&f5)) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
            tearingSupported_ = tearing != FALSE;
        }
        // The swap chain has the window's size: the present pass scales the back buffer into it (review M3
        // finding 11). It is redrawn completely every Present, so FLIP_DISCARD is fine - the persistent back
        // buffers (CreateTargets) keep the Xbox contents (review M3 findings 2 and 8).
        ClientSize(window_, &swapWidth_, &swapHeight_);
        if (swapWidth_ == 0 || swapHeight_ == 0) {
            swapWidth_ = desc.width;
            swapHeight_ = desc.height;
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = swapWidth_;
        sd.Height = swapHeight_;
        sd.Format = static_cast<DXGI_FORMAT>(DxgiFormatOf(desc.backBufferFormat));
        sd.Stereo = FALSE;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;  // only between a window resize and the next Present
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        // M3b pacing: frame-latency waitable object (DXGI 1.3, Windows 8.1) when requested. Both flags stay for
        // the swap chain's lifetime (ResizeBuffers passes swapFlags_).
        const UINT waitableFlag =
            desc.frameLatencyWaitable != 0 ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) : 0u;
        sd.Flags = (tearingSupported_ ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u) | waitableFlag;
        HRESULT hr = factory_->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swapChain_);
        const char* effect = "flip discard";
        if (FAILED(hr) && waitableFlag != 0) {  // no DXGI 1.3: without the waitable object
            sd.Flags = tearingSupported_ ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
            hr = factory_->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swapChain_);
        }
        if (FAILED(hr)) {  // FLIP_DISCARD needs Windows 10
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            sd.Flags = 0;
            tearingSupported_ = false;
            effect = "flip sequential";
            hr = factory_->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swapChain_);
        }
        if (FAILED(hr)) {
            *error = "CreateSwapChainForHwnd failed: " + HresultText(hr);
            PumpDebugMessages();
            return false;
        }
        swapFlags_ = sd.Flags;
        if ((sd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
            ComPtr<IDXGISwapChain2> chain2;
            const UINT latency = std::clamp<UINT>(desc.frameLatencyWaitable, 1u, 3u);
            if (SUCCEEDED(swapChain_.As(&chain2)) && SUCCEEDED(chain2->SetMaximumFrameLatency(latency))) {
                latencyWaitable_ = chain2->GetFrameLatencyWaitableObject();
            }
            if (latencyWaitable_ != nullptr) {
                info_.frameLatency = latency;
            }
        }
        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3D11Texture2D> buffer;
        hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
        if (FAILED(hr) || !CreateSwapViews(buffer.Get())) {
            *error = "swap chain buffer: " + (FAILED(hr) ? HresultText(hr) : lastError_);
            return false;
        }
        FUT_LOGD(Gfx, "d3d11: swap chain for window %p: %ux%u, %u buffers, %s, scaling %s, tearing %s, frame-latency "
                 "waitable %s (maximum latency %u)", desc.nativeWindow, sd.Width, sd.Height, sd.BufferCount, effect,
                 PresentScalingName(scaling_), tearingSupported_ ? "supported" : "no",
                 latencyWaitable_ != nullptr ? "yes" : "no", info_.frameLatency);
        return true;
    } catch (...) {
        *error = "out of memory while creating the swap chain";
        return false;
    }
}

// Render target views of swap chain buffer 0: the plain one and, with linearResolve_, an sRGB one (allowed on the
// UNORM buffer of a flip-model swap chain) for a downsample in the present pass.
bool D3D11Device::CreateSwapViews(ID3D11Texture2D* buffer) noexcept {
    swapRtv_.Reset();
    swapSrgbRtv_.Reset();
    HRESULT hr = device_->CreateRenderTargetView(buffer, nullptr, &swapRtv_);
    if (SUCCEEDED(hr) && linearResolve_) {
        D3D11_TEXTURE2D_DESC bd{};
        buffer->GetDesc(&bd);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                           : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = device_->CreateRenderTargetView(buffer, &rd, &swapSrgbRtv_);
    }
    if (FAILED(hr)) {
        swapRtv_.Reset();
        swapSrgbRtv_.Reset();
        return Fail("swap chain render target view", hr);
    }
    return true;
}

namespace {

// Review M3c finding 3: the TYPELESS / UNORM / sRGB members of the back buffer format's family.
struct ColorFormats {
    DXGI_FORMAT resource, unorm, srgb;
};
ColorFormats ColorFormatsOf(DXGI_FORMAT unorm, bool linear) noexcept {
    if (!linear) {
        return {unorm, unorm, unorm};
    }
    if (unorm == DXGI_FORMAT_R8G8B8A8_UNORM) {
        return {DXGI_FORMAT_R8G8B8A8_TYPELESS, unorm, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    }
    return {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
}

D3D11_SHADER_RESOURCE_VIEW_DESC SrvDesc(DXGI_FORMAT format) noexcept {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    return d;
}

D3D11_RENDER_TARGET_VIEW_DESC RtvDesc(DXGI_FORMAT format, bool multisampled) noexcept {
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = multisampled ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    return d;
}

}  // namespace

// M3c: the persistent back buffers (bufferCount, `samples` per pixel), their depth buffer, the resolve texture
// (samples > 1) and the output image (output size != back buffer size) of one configuration.
bool D3D11Device::CreateTargets(std::uint32_t width, std::uint32_t height, std::uint32_t samples,
                                std::uint32_t outputWidth, std::uint32_t outputHeight, Targets* out,
                                std::string* error) noexcept {
    try {
        Targets t;
        t.width = width;
        t.height = height;
        t.samples = samples < 1 ? 1 : samples;
        t.outputWidth = outputWidth != 0 ? outputWidth : width;
        t.outputHeight = outputHeight != 0 ? outputHeight : height;
        const ColorFormats color =
            ColorFormatsOf(static_cast<DXGI_FORMAT>(DxgiFormatOf(info_.backBufferFormat)), linearResolve_);
        const D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = RtvDesc(color.unorm, t.samples > 1);
        const D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = SrvDesc(color.unorm);
        const D3D11_SHADER_RESOURCE_VIEW_DESC srgbSrvDesc = SrvDesc(color.srgb);
        const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = color.resource;
        td.SampleDesc.Count = t.samples;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        // A multisampled back buffer is never sampled directly: Present resolves it into t.resolve.
        td.BindFlags = D3D11_BIND_RENDER_TARGET | (t.samples == 1 ? D3D11_BIND_SHADER_RESOURCE : 0u);
        static constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (std::uint32_t i = 0; i < backBufferCount_; ++i) {
            HRESULT hr = device_->CreateTexture2D(&td, nullptr, &t.backBuffers[i]);
            if (SUCCEEDED(hr)) {
                hr = device_->CreateRenderTargetView(t.backBuffers[i].Get(), &rtvDesc, &t.backBufferRtvs[i]);
            }
            if (SUCCEEDED(hr) && t.samples == 1) {
                hr = device_->CreateShaderResourceView(t.backBuffers[i].Get(), &srvDesc, &t.backBufferSrvs[i]);
            }
            if (SUCCEEDED(hr) && t.samples == 1 && linearResolve_) {
                hr = device_->CreateShaderResourceView(t.backBuffers[i].Get(), &srgbSrvDesc,
                                                       &t.backBufferSrgbSrvs[i]);
            }
            if (FAILED(hr)) {
                *error = "back buffer " + std::to_string(width) + "x" + std::to_string(height) + " (" +
                         std::to_string(t.samples) + " samples): " + HresultText(hr);
                return false;
            }
            char name[48];
            std::snprintf(name, sizeof(name), "rhi back buffer %u", i);
            SetName(t.backBuffers[i].Get(), name);
            // Defined start contents (black; the game clears every buffer itself, FUN_0002A660).
            immediate_->ClearRenderTargetView(t.backBufferRtvs[i].Get(), kBlack);
            t.bytes += pixels * 4 * t.samples;
        }
        if (info_.depthFormat != Format::Unknown) {
            D3D11_TEXTURE2D_DESC dd = td;
            dd.Format = TextureFormatsOf(info_.depthFormat, kTextureDepthStencil).dsv;
            dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            HRESULT hr = device_->CreateTexture2D(&dd, nullptr, &t.depth);
            if (SUCCEEDED(hr)) {
                hr = device_->CreateDepthStencilView(t.depth.Get(), nullptr, &t.depthDsv);
            }
            if (FAILED(hr)) {
                *error = "depth buffer: " + HresultText(hr);
                return false;
            }
            SetName(t.depth.Get(), "rhi back buffer depth");
            t.bytes += pixels * 4 * t.samples;
        }
        if (t.samples > 1) {
            D3D11_TEXTURE2D_DESC rd = td;
            rd.SampleDesc.Count = 1;
            rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = device_->CreateTexture2D(&rd, nullptr, &t.resolve);
            if (SUCCEEDED(hr)) {
                hr = device_->CreateShaderResourceView(t.resolve.Get(), &srvDesc, &t.resolveSrv);
            }
            if (SUCCEEDED(hr) && linearResolve_) {
                hr = device_->CreateShaderResourceView(t.resolve.Get(), &srgbSrvDesc, &t.resolveSrgbSrv);
            }
            if (FAILED(hr)) {
                *error = "resolve texture: " + HresultText(hr);
                return false;
            }
            SetName(t.resolve.Get(), "rhi resolved back buffer");
            t.bytes += pixels * 4;
        }
        if (t.outputWidth != width || t.outputHeight != height) {
            D3D11_TEXTURE2D_DESC od = td;
            od.Width = t.outputWidth;
            od.Height = t.outputHeight;
            od.SampleDesc.Count = 1;
            od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = device_->CreateTexture2D(&od, nullptr, &t.output);
            const D3D11_RENDER_TARGET_VIEW_DESC outRtv = RtvDesc(color.unorm, false);
            if (SUCCEEDED(hr)) {
                hr = device_->CreateRenderTargetView(t.output.Get(), &outRtv, &t.outputRtv);
            }
            if (SUCCEEDED(hr)) {
                hr = device_->CreateShaderResourceView(t.output.Get(), &srvDesc, &t.outputSrv);
            }
            if (SUCCEEDED(hr) && linearResolve_) {
                const D3D11_RENDER_TARGET_VIEW_DESC srgbRtv = RtvDesc(color.srgb, false);
                hr = device_->CreateRenderTargetView(t.output.Get(), &srgbRtv, &t.outputSrgbRtv);
                if (SUCCEEDED(hr)) {
                    hr = device_->CreateShaderResourceView(t.output.Get(), &srgbSrvDesc, &t.outputSrgbSrv);
                }
            }
            if (FAILED(hr)) {
                *error = "output image: " + HresultText(hr);
                return false;
            }
            SetName(t.output.Get(), "rhi output image");
            immediate_->ClearRenderTargetView(t.outputRtv.Get(), kBlack);
            t.bytes += static_cast<std::uint64_t>(t.outputWidth) * t.outputHeight * 4;
        }
        *out = std::move(t);
        return true;
    } catch (...) {
        *error = "out of memory";
        return false;
    }
}

bool D3D11Device::CreatePresentPass(std::string* error) noexcept {
    try {
        HRESULT hr = device_->CreateVertexShader(g_d3d11_present_vs, sizeof(g_d3d11_present_vs), nullptr, &presentVs_);
        if (SUCCEEDED(hr)) {
            hr = device_->CreatePixelShader(g_d3d11_present_ps, sizeof(g_d3d11_present_ps), nullptr, &presentPs_);
        }
        if (SUCCEEDED(hr)) {  // M3c: downsample pass (ResamplePS + its constants, d3d11_builtin.hlsl)
            hr = device_->CreatePixelShader(g_d3d11_resample_ps, sizeof(g_d3d11_resample_ps), nullptr, &resamplePs_);
        }
        if (SUCCEEDED(hr)) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 48;  // 3 x float4 (ResampleConstants)
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            hr = device_->CreateBuffer(&bd, nullptr, &resampleConstants_);
            if (SUCCEEDED(hr)) {
                SetName(resampleConstants_.Get(), "rhi resample constants");
            }
        }
        for (int linear = 0; linear < 2 && SUCCEEDED(hr); ++linear) {
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = linear != 0 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            hr = device_->CreateSamplerState(&sd, linear != 0 ? &presentLinear_ : &presentPoint_);
        }
        if (SUCCEEDED(hr)) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            hr = device_->CreateRasterizerState(&rd, &presentRaster_);
        }
        if (SUCCEEDED(hr)) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = FALSE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            dd.StencilEnable = FALSE;
            hr = device_->CreateDepthStencilState(&dd, &presentDepth_);
        }
        if (SUCCEEDED(hr)) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            hr = device_->CreateBlendState(&bd, &presentBlend_);
        }
        if (FAILED(hr)) {
            *error = "present pass: " + HresultText(hr);
            return false;
        }
        return true;
    } catch (...) {
        *error = "out of memory";
        return false;
    }
}

// ResizeBuffers when the window's client size changed (the UI thread lays the view out; GetClientRect is a
// plain read of the window state). Minimized (0 x 0): the old buffers stay. Only swapRtv_ references them.
bool D3D11Device::UpdateSwapChainSize() noexcept {
    std::uint32_t w = 0, h = 0;
    ClientSize(window_, &w, &h);
    w = std::min<std::uint32_t>(w, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    h = std::min<std::uint32_t>(h, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    if (w == 0 || h == 0 || (w == swapWidth_ && h == swapHeight_ && swapRtv_ != nullptr)) {
        return true;
    }
    immediate_->OMSetRenderTargets(0, nullptr, nullptr);
    swapRtv_.Reset();
    swapSrgbRtv_.Reset();
    const HRESULT hr = swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags_);
    if (FAILED(hr)) {
        Fail("IDXGISwapChain::ResizeBuffers", hr);  // keeps the old size and buffers
    } else {
        swapWidth_ = w;
        swapHeight_ = h;
        ++swapResizes_;
    }
    ComPtr<ID3D11Texture2D> buffer;
    const HRESULT hr2 = swapChain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
    if (FAILED(hr2)) {
        return Fail("swap chain buffer after ResizeBuffers", hr2);
    }
    if (!CreateSwapViews(buffer.Get())) {
        return false;
    }
    FUT_LOGD(Gfx, "d3d11: swap chain resized to %ux%u (scaling %s)", swapWidth_, swapHeight_,
             PresentScalingName(scaling_));
    return SUCCEEDED(hr);
}

namespace {

// Destination rectangle of the back buffer (srcW x srcH) in the swap chain buffer (dstW x dstH).
D3D11_VIEWPORT PresentRect(PresentScaling scaling, std::uint32_t srcW, std::uint32_t srcH, std::uint32_t dstW,
                           std::uint32_t dstH) noexcept {
    double w = dstW;
    double h = dstH;
    const std::uint32_t factor = srcW != 0 && srcH != 0 ? std::min(dstW / srcW, dstH / srcH) : 0;
    if (scaling == PresentScaling::Integer && factor >= 1) {
        w = static_cast<double>(srcW) * factor;
        h = static_cast<double>(srcH) * factor;
    } else if (static_cast<double>(dstW) * srcH > static_cast<double>(dstH) * srcW) {
        w = std::floor(static_cast<double>(dstH) * srcW / srcH + 0.5);  // pillarbox
    } else if (srcW != 0) {
        h = std::floor(static_cast<double>(dstW) * srcH / srcW + 0.5);  // letterbox
    }
    D3D11_VIEWPORT v{};
    v.TopLeftX = static_cast<float>(std::floor((dstW - w) / 2.0));
    v.TopLeftY = static_cast<float>(std::floor((dstH - h) / 2.0));
    v.Width = static_cast<float>(w);
    v.Height = static_cast<float>(h);
    v.MinDepth = 0.0f;
    v.MaxDepth = 1.0f;
    return v;
}

}  // namespace

// M3c: the picture of persistent back buffer `buffer` of `t` at the internal size as a shader resource: the
// buffer itself (single-sampled) or, multisampled, resolved into t.resolve (box average of the samples; with
// linearResolve_ an sRGB resolve, i.e. the average of linear light). `srgb`: the sRGB view of it.
ID3D11ShaderResourceView* D3D11Device::ResolvedPicture(const Targets& t, std::uint32_t buffer, bool srgb) noexcept {
    if (buffer >= backBufferCount_ || t.backBuffers[buffer] == nullptr) {
        return nullptr;
    }
    srgb = srgb && linearResolve_;
    if (t.samples <= 1) {
        return srgb ? t.backBufferSrgbSrvs[buffer].Get() : t.backBufferSrvs[buffer].Get();
    }
    const ColorFormats color =
        ColorFormatsOf(static_cast<DXGI_FORMAT>(DxgiFormatOf(info_.backBufferFormat)), linearResolve_);
    immediate_->ResolveSubresource(t.resolve.Get(), 0, t.backBuffers[buffer].Get(), 0, color.srgb);
    return srgb ? t.resolveSrgbSrv.Get() : t.resolveSrv.Get();
}

// M3c: the image Present shows for the current back buffer: resolved, then (output size != back buffer size)
// downsampled into targets_.output - exact box filter for an integer ratio (render_scale 2), Catmull-Rom with
// anti-ringing for a fractional one. With linearResolve_ the downsample reads and writes sRGB views (linear light);
// `srgb` receives the sRGB view of the image (nullptr without linearResolve_).
ID3D11ShaderResourceView* D3D11Device::PrepareOutput(ID3D11ShaderResourceView** srgb) noexcept {
    if (srgb != nullptr) {
        *srgb = nullptr;
    }
    ID3D11ShaderResourceView* picture = ResolvedPicture(targets_, currentBackBuffer_);
    if (picture == nullptr || targets_.output == nullptr) {
        if (srgb != nullptr && picture != nullptr && linearResolve_) {
            *srgb = targets_.samples > 1 ? targets_.resolveSrgbSrv.Get()
                                         : targets_.backBufferSrgbSrvs[currentBackBuffer_].Get();
        }
        return picture;
    }
    D3D11_VIEWPORT v{};
    v.Width = static_cast<float>(targets_.outputWidth);
    v.Height = static_cast<float>(targets_.outputHeight);
    v.MaxDepth = 1.0f;
    if (linearResolve_) {
        ID3D11ShaderResourceView* linear = targets_.samples > 1
                                               ? targets_.resolveSrgbSrv.Get()
                                               : targets_.backBufferSrgbSrvs[currentBackBuffer_].Get();
        DrawBlit(linear, targets_.width, targets_.height, targets_.outputSrgbRtv.Get(), v, BlitFilter::Resample);
        if (srgb != nullptr) {
            *srgb = targets_.outputSrgbSrv.Get();
        }
    } else {
        DrawBlit(picture, targets_.width, targets_.height, targets_.outputRtv.Get(), v, BlitFilter::Resample);
    }
    return targets_.outputSrv.Get();
}

void D3D11Device::DrawBlit(ID3D11ShaderResourceView* source, std::uint32_t srcW, std::uint32_t srcH,
                           ID3D11RenderTargetView* target, const D3D11_VIEWPORT& v, BlitFilter filter) noexcept {
    if (source == nullptr || target == nullptr || presentVs_ == nullptr || !(v.Width > 0.0f) || !(v.Height > 0.0f)) {
        return;
    }
    const auto dstW = static_cast<std::uint32_t>(v.Width);
    const auto dstH = static_cast<std::uint32_t>(v.Height);
    if (filter == BlitFilter::Resample && dstW >= srcW && dstH >= srcH) {
        filter = BlitFilter::Linear;  // nothing to shrink: a plain (bilinear) stretch
    }
    immediate_->OMSetRenderTargets(1, &target, nullptr);  // unbinds a back buffer before it is sampled
    immediate_->RSSetViewports(1, &v);
    immediate_->RSSetState(presentRaster_.Get());
    immediate_->OMSetBlendState(presentBlend_.Get(), nullptr, 0xFFFFFFFFu);
    immediate_->OMSetDepthStencilState(presentDepth_.Get(), 0);
    immediate_->IASetInputLayout(nullptr);
    immediate_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    immediate_->VSSetShader(presentVs_.Get(), nullptr, 0);
    immediate_->PSSetShaderResources(0, 1, &source);
    ComPtr<ID3D11Buffer> savedConstants;
    if (filter == BlitFilter::Resample) {
        // ResampleConstants (d3d11_builtin.hlsl): source size, source pixels per destination pixel + mode,
        // destination origin. Box only when both ratios are whole numbers (each destination pixel = k x k
        // source pixels exactly).
        const bool box = dstW != 0 && dstH != 0 && srcW % dstW == 0 && srcH % dstH == 0 &&
                         static_cast<float>(dstW) == v.Width && static_cast<float>(dstH) == v.Height;
        const float constants[12] = {static_cast<float>(srcW), static_cast<float>(srcH), 0.0f, 0.0f,
                                     static_cast<float>(srcW) / v.Width, static_cast<float>(srcH) / v.Height,
                                     box ? 0.0f : 1.0f, 0.0f,
                                     v.TopLeftX, v.TopLeftY, 0.0f, 0.0f};
        immediate_->UpdateSubresource(resampleConstants_.Get(), 0, nullptr, constants, 0, 0);
        immediate_->PSGetConstantBuffers(0, 1, savedConstants.ReleaseAndGetAddressOf());
        ID3D11Buffer* cb = resampleConstants_.Get();
        immediate_->PSSetConstantBuffers(0, 1, &cb);
        immediate_->PSSetShader(resamplePs_.Get(), nullptr, 0);
    } else {
        immediate_->PSSetShader(presentPs_.Get(), nullptr, 0);
        ID3D11SamplerState* sampler = filter == BlitFilter::Linear ? presentLinear_.Get() : presentPoint_.Get();
        immediate_->PSSetSamplers(0, 1, &sampler);
    }
    immediate_->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    immediate_->PSSetShaderResources(0, 1, &none);  // the source may become a render target again
    if (filter == BlitFilter::Resample) {
        ID3D11Buffer* cb = savedConstants.Get();
        immediate_->PSSetConstantBuffers(0, 1, &cb);
    }
}

// The output image -> swap chain buffer 0 (black borders, scaling_). Leaves the immediate context with the
// present pass state; D3D11Context::OnPresent restores the context's own bindings afterwards. M3c: an image
// of exactly the destination size is copied 1:1 (point); a larger one is downsampled (box / Catmull-Rom, on
// linear light with linearResolve_) instead of bilinear taps; a smaller one is scaled with [render] scaling as
// in M3.
void D3D11Device::DrawPresentPass() noexcept {
    if (swapRtv_ == nullptr) {
        return;
    }
    ID3D11ShaderResourceView* imageSrgb = nullptr;
    ID3D11ShaderResourceView* image = PrepareOutput(&imageSrgb);
    static constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ID3D11RenderTargetView* rtv = swapRtv_.Get();
    immediate_->OMSetRenderTargets(1, &rtv, nullptr);
    immediate_->ClearRenderTargetView(rtv, kBlack);
    if (image == nullptr) {
        return;
    }
    const std::uint32_t w = targets_.outputWidth;
    const std::uint32_t h = targets_.outputHeight;
    const D3D11_VIEWPORT v = PresentRect(scaling_, w, h, swapWidth_, swapHeight_);
    BlitFilter filter = scaling_ == PresentScaling::Linear ? BlitFilter::Linear : BlitFilter::Point;
    if (v.Width == static_cast<float>(w) && v.Height == static_cast<float>(h)) {
        filter = BlitFilter::Point;  // 1:1
    } else if (v.Width < static_cast<float>(w) || v.Height < static_cast<float>(h)) {
        filter = BlitFilter::Resample;  // the image is larger than the window
        if (imageSrgb != nullptr && swapSrgbRtv_ != nullptr) {
            DrawBlit(imageSrgb, w, h, swapSrgbRtv_.Get(), v, filter);
            return;
        }
    }
    DrawBlit(image, w, h, rtv, v, filter);
}

void D3D11Device::SetName(ID3D11DeviceChild* object, const char* name) noexcept {
    if (object != nullptr && name != nullptr && name[0] != 0) {
        object->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
    }
}

bool D3D11Device::FailText(const char* text) noexcept {
    try {
        lastError_ = text;
    } catch (...) {
    }
    FUT_LOGD(Gfx, "d3d11: %s", text);
    return false;
}

bool D3D11Device::Fail(const char* what, HRESULT hr) noexcept {
    try {
        lastError_ = std::string(what) + " failed: " + HresultText(hr);
        if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && device_ != nullptr) {
            lastError_ += " (device removed reason " + HresultText(device_->GetDeviceRemovedReason()) + ")";
        }
    } catch (...) {
    }
    FUT_LOGD(Gfx, "d3d11: %s", lastError_.c_str());
    PumpDebugMessages();
    return false;
}

// ---- debug messages ------------------------------------------------------------------------------------------

void D3D11Device::HandleDebugMessage(DebugSeverity severity, std::int32_t id, bool dxgi, const char* text) noexcept {
    GlobalDebug& g = Global();
    switch (severity) {
    case DebugSeverity::Corruption:
        ++stats_.debugErrors;
        g.corruption.fetch_add(1);
        break;
    case DebugSeverity::Error:
        ++stats_.debugErrors;
        g.errors.fetch_add(1);
        break;
    case DebugSeverity::Warning:
        ++stats_.debugWarnings;
        g.warnings.fetch_add(1);
        break;
    case DebugSeverity::Info:
    case DebugSeverity::Message:
        g.other.fetch_add(1);
        break;
    }
    const char* prefix = dxgi ? "DXGI " : "";
    try {
        const std::int64_t key = (dxgi ? (std::int64_t{1} << 32) : 0) | static_cast<std::uint32_t>(id);
        const std::uint32_t n = ++messageCounts_[key];
        if (n <= kMaxLinesPerMessageId) {
            const core::Level level = severity == DebugSeverity::Corruption || severity == DebugSeverity::Error
                                          ? core::Level::Error
                                      : severity == DebugSeverity::Warning ? core::Level::Warn
                                                                           : core::Level::Debug;
            if (core::LogEnabled(core::Channel::Gfx, level)) {
                core::LogWrite(core::Channel::Gfx, level, "D3D11 debug: %s #%d: %s%s", SeverityName(severity), id,
                               prefix, text != nullptr ? text : "");
                if (n == kMaxLinesPerMessageId) {
                    core::LogWrite(core::Channel::Gfx, level,
                                   "D3D11 debug: further %s#%d messages are counted but not logged", prefix, id);
                }
            }
        }
        if (pendingMessages_.size() >= kMaxPendingMessages) {
            pendingMessages_.erase(pendingMessages_.begin());
        }
        DebugMessage m;
        m.severity = severity;
        m.id = id;
        m.text = std::string(prefix) + (text != nullptr ? text : "");
        pendingMessages_.push_back(std::move(m));
    } catch (...) {
    }
}

void D3D11Device::PumpDebugMessages() noexcept {
    try {
        std::vector<std::uint8_t> buffer;
        if (infoQueue_ != nullptr) {
            const UINT64 n = infoQueue_->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                if (FAILED(infoQueue_->GetMessage(i, nullptr, &len)) || len < sizeof(D3D11_MESSAGE)) {
                    continue;
                }
                buffer.resize(len);
                auto* m = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
                if (SUCCEEDED(infoQueue_->GetMessage(i, m, &len))) {
                    HandleDebugMessage(SeverityOf(m->Severity), static_cast<std::int32_t>(m->ID), false,
                                       m->pDescription);
                }
            }
            infoQueue_->ClearStoredMessages();
        }
        if (dxgiInfoQueue_ != nullptr) {
            const UINT64 n = dxgiInfoQueue_->GetNumStoredMessagesAllowedByRetrievalFilters(DXGI_DEBUG_DXGI);
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T len = 0;
                if (FAILED(dxgiInfoQueue_->GetMessage(DXGI_DEBUG_DXGI, i, nullptr, &len)) ||
                    len < sizeof(DXGI_INFO_QUEUE_MESSAGE)) {
                    continue;
                }
                buffer.resize(len);
                auto* m = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buffer.data());
                if (SUCCEEDED(dxgiInfoQueue_->GetMessage(DXGI_DEBUG_DXGI, i, m, &len))) {
                    HandleDebugMessage(SeverityOf(m->Severity), static_cast<std::int32_t>(m->ID), true,
                                       m->pDescription);
                }
            }
            dxgiInfoQueue_->ClearStoredMessages(DXGI_DEBUG_DXGI);
        }
    } catch (...) {
    }
}

std::size_t D3D11Device::DrainDebugMessages(std::vector<DebugMessage>& out) noexcept {
    PumpDebugMessages();
    const std::size_t n = pendingMessages_.size();
    try {
        for (DebugMessage& m : pendingMessages_) {
            out.push_back(std::move(m));
        }
    } catch (...) {
    }
    pendingMessages_.clear();
    return n;
}

void D3D11Device::ReportLiveObjects() noexcept {
    if (!debugLayer_.enabled || infoQueue_ == nullptr || device_ == nullptr) {
        return;
    }
    try {
        infoQueue_->ClearStoredMessages();
        ComPtr<ID3D11Debug> debug;
        if (FAILED(device_.As(&debug))) {
            return;
        }
        infoQueue_->PushEmptyStorageFilter();
        debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
        debug.Reset();
        int live = 0;
        std::vector<std::uint8_t> buffer;
        const UINT64 n = infoQueue_->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            if (FAILED(infoQueue_->GetMessage(i, nullptr, &len)) || len < sizeof(D3D11_MESSAGE)) {
                continue;
            }
            buffer.resize(len);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
            if (FAILED(infoQueue_->GetMessage(i, m, &len)) || m->pDescription == nullptr) {
                continue;
            }
            // Measured format (Windows 11 SDK layers): "Live ID3D11Device at 0x..., Refcount: 4" for the device,
            // then one "\tLive ID3D11Buffer at 0x..., [Name: ..., ]Refcount: 1, IntRef: 0" per object. Count the
            // objects the caller still references (Refcount > 0); the device is alive by definition.
            const char* text = m->pDescription;
            while (*text == '\t' || *text == ' ') {
                ++text;
            }
            if (std::strncmp(text, "Live ", 5) != 0 || std::strncmp(text, "Live ID3D11Device ", 18) == 0) {
                continue;
            }
            const char* rc = std::strstr(text, "Refcount: ");
            if (rc != nullptr && std::atoi(rc + 10) > 0) {
                ++live;
                FUT_LOGD(Gfx, "d3d11 teardown: %s", text);
            }
        }
        infoQueue_->ClearStoredMessages();
        infoQueue_->PopStorageFilter();
        Global().lastLive.store(live);
        if (live > 0) {
            FUT_LOGW(Gfx, "d3d11 teardown: %d objects still referenced after the device was released", live);
        }
    } catch (...) {
    }
}

// ---- resources -----------------------------------------------------------------------------------------------

bool D3D11Device::IsFormatSupported(Format format, std::uint8_t usage) const noexcept {
    const FormatInfo& info = GetFormatInfo(format);
    const TextureFormats f = TextureFormatsOf(format, usage);
    if (f.resource == DXGI_FORMAT_UNKNOWN || usage == 0 || device_ == nullptr) {
        return false;
    }
    if (info.depth && (usage & kTextureRenderTarget) != 0) {
        return false;
    }
    if (!info.depth && (usage & kTextureDepthStencil) != 0) {
        return false;
    }
    auto has = [&](DXGI_FORMAT fmt, UINT bits) {
        UINT support = 0;
        return fmt != DXGI_FORMAT_UNKNOWN && SUCCEEDED(device_->CheckFormatSupport(fmt, &support)) &&
               (support & bits) == bits;
    };
    if ((usage & kTextureSampled) != 0 &&
        !has(f.srv, D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
        return false;
    }
    if ((usage & kTextureRenderTarget) != 0 && !has(f.rtv, D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
        return false;
    }
    if ((usage & kTextureDepthStencil) != 0 && !has(f.dsv, D3D11_FORMAT_SUPPORT_DEPTH_STENCIL)) {
        return false;
    }
    return true;
}

BufferHandle D3D11Device::CreateBuffer(const BufferDesc& desc) noexcept {
    try {
        const bool constant = (desc.bind & kBindConstant) != 0;
        if (desc.size == 0 || (desc.bind & (kBindVertex | kBindIndex | kBindConstant)) == 0) {
            FailText("CreateBuffer: size 0 or no bind flag");
            return {};
        }
        if (constant && ((desc.bind & ~kBindConstant) != 0 || desc.size % 16 != 0 ||
                         desc.size > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16)) {
            FailText("CreateBuffer: a constant buffer must be bound alone and be a multiple of 16 bytes (<= 64 KiB)");
            return {};
        }
        const bool dynamic = constant || desc.usage == BufferUsage::Dynamic;
        if (!dynamic && desc.initialData == nullptr) {
            FailText("CreateBuffer: a static buffer needs initial data");
            return {};
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = desc.size;
        bd.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = ((desc.bind & kBindVertex) != 0 ? D3D11_BIND_VERTEX_BUFFER : 0u) |
                       ((desc.bind & kBindIndex) != 0 ? D3D11_BIND_INDEX_BUFFER : 0u) |
                       (constant ? D3D11_BIND_CONSTANT_BUFFER : 0u);
        bd.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0u;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = desc.initialData;
        BufferRecord rec;
        const HRESULT hr = device_->CreateBuffer(&bd, desc.initialData != nullptr ? &init : nullptr, &rec.buffer);
        if (FAILED(hr)) {
            Fail("CreateBuffer", hr);
            return {};
        }
        SetName(rec.buffer.Get(), desc.debugName);
        rec.size = desc.size;
        rec.bind = desc.bind;
        rec.usage = dynamic ? BufferUsage::Dynamic : BufferUsage::Static;
        const std::uint32_t id = buffers_.Add(std::move(rec));
        if (id == 0) {
            FailText("CreateBuffer: too many buffers");
        }
        return BufferHandle{id};
    } catch (...) {
        FailText("CreateBuffer: out of memory");
        return {};
    }
}

std::uint32_t D3D11Device::AddTransientBuffer(ComPtr<ID3D11Buffer> buffer, std::uint32_t size,
                                              std::uint8_t bind) noexcept {
    try {
        BufferRecord rec;
        rec.buffer = std::move(buffer);
        rec.size = size;
        rec.bind = bind;
        rec.usage = BufferUsage::Dynamic;
        rec.transient = true;
        return buffers_.Add(std::move(rec));
    } catch (...) {
        return 0;
    }
}

void D3D11Device::RemoveTransientBuffer(std::uint32_t id) noexcept {
    try {
        buffers_.Remove(id);
    } catch (...) {
    }
}

TextureHandle D3D11Device::CreateTexture(const TextureDesc& desc, const SubresourceData* levels) noexcept {
    try {
        const FormatInfo& info = GetFormatInfo(desc.format);
        const TextureFormats f = TextureFormatsOf(desc.format, desc.usage);
        if (f.resource == DXGI_FORMAT_UNKNOWN) {
            FailText("CreateTexture: unknown format");
            return {};
        }
        if (desc.width == 0 || desc.height == 0 || desc.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || desc.mipLevels == 0 ||
            desc.mipLevels > MaxMipLevels(desc.width, desc.height)) {
            FailText("CreateTexture: invalid size or mip level count");
            return {};
        }
        if ((desc.usage & (kTextureSampled | kTextureRenderTarget | kTextureDepthStencil)) == 0 ||
            (info.depth && (desc.usage & kTextureRenderTarget) != 0) ||
            (!info.depth && (desc.usage & kTextureDepthStencil) != 0) ||
            (info.compressed && (desc.usage & ~kTextureSampled) != 0)) {
            FailText("CreateTexture: usage does not fit the format");
            return {};
        }
        if (info.compressed && (desc.width % 4 != 0 || desc.height % 4 != 0)) {
            FailText("CreateTexture: block-compressed level 0 must be a multiple of 4 in both dimensions");
            return {};
        }
        if (info.depth && levels != nullptr) {
            FailText("CreateTexture: depth textures take no initial data");
            return {};
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = desc.width;
        td.Height = desc.height;
        td.MipLevels = desc.mipLevels;
        td.ArraySize = 1;
        td.Format = f.resource;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = ((desc.usage & kTextureSampled) != 0 ? D3D11_BIND_SHADER_RESOURCE : 0u) |
                       ((desc.usage & kTextureRenderTarget) != 0 ? D3D11_BIND_RENDER_TARGET : 0u) |
                       ((desc.usage & kTextureDepthStencil) != 0 ? D3D11_BIND_DEPTH_STENCIL : 0u);
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        std::uint64_t bytes = 0;
        for (std::uint32_t l = 0; l < desc.mipLevels; ++l) {
            bytes += LevelSize(desc.format, MipSize(desc.width, l), MipSize(desc.height, l));
        }
        if (levels != nullptr) {
            init.resize(desc.mipLevels);
            for (std::uint32_t l = 0; l < desc.mipLevels; ++l) {
                if (levels[l].data == nullptr) {
                    FailText("CreateTexture: a mip level has no data");
                    return {};
                }
                const std::uint32_t w = MipSize(desc.width, l);
                const std::uint32_t h = MipSize(desc.height, l);
                init[l].pSysMem = levels[l].data;
                init[l].SysMemPitch = levels[l].rowPitch != 0 ? levels[l].rowPitch : RowPitch(desc.format, w);
                init[l].SysMemSlicePitch = levels[l].slicePitch != 0 ? levels[l].slicePitch : LevelSize(desc.format, w, h);
            }
        }
        TextureRecord rec;
        HRESULT hr = device_->CreateTexture2D(&td, levels != nullptr ? init.data() : nullptr, &rec.texture);
        if (FAILED(hr)) {
            Fail("CreateTexture2D", hr);
            return {};
        }
        SetName(rec.texture.Get(), desc.debugName);
        if ((desc.usage & kTextureSampled) != 0) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = f.srv;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MostDetailedMip = 0;
            sv.Texture2D.MipLevels = desc.mipLevels;
            hr = device_->CreateShaderResourceView(rec.texture.Get(), &sv, &rec.srv);
            if (FAILED(hr)) {
                Fail("CreateShaderResourceView", hr);
                return {};
            }
        }
        if ((desc.usage & kTextureRenderTarget) != 0) {
            D3D11_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = f.rtv;
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            hr = device_->CreateRenderTargetView(rec.texture.Get(), &rv, &rec.rtv);
            if (FAILED(hr)) {
                Fail("CreateRenderTargetView", hr);
                return {};
            }
        }
        if ((desc.usage & kTextureDepthStencil) != 0) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = f.dsv;
            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            hr = device_->CreateDepthStencilView(rec.texture.Get(), &dv, &rec.dsv);
            if (FAILED(hr)) {
                Fail("CreateDepthStencilView", hr);
                return {};
            }
        }
        rec.width = desc.width;
        rec.height = desc.height;
        rec.mipLevels = desc.mipLevels;
        rec.format = desc.format;
        rec.usage = desc.usage;
        rec.bytes = bytes;
        const std::uint32_t id = textures_.Add(std::move(rec));
        if (id == 0) {
            FailText("CreateTexture: too many textures");
            return {};
        }
        stats_.textureBytes += bytes;
        return TextureHandle{id};
    } catch (...) {
        FailText("CreateTexture: out of memory");
        return {};
    }
}

bool D3D11Device::UpdateTexture(TextureHandle texture, std::uint32_t level, const SubresourceData& data) noexcept {
    TextureRecord* rec = textures_.Get(texture.id);
    if (rec == nullptr) {
        return FailText("UpdateTexture: invalid texture handle");
    }
    if (level >= rec->mipLevels || data.data == nullptr || GetFormatInfo(rec->format).depth) {
        return FailText("UpdateTexture: invalid level/data or a depth texture");
    }
    const std::uint32_t w = MipSize(rec->width, level);
    const std::uint32_t h = MipSize(rec->height, level);
    immediate_->UpdateSubresource(rec->texture.Get(), D3D11CalcSubresource(level, 0, rec->mipLevels), nullptr,
                                  data.data, data.rowPitch != 0 ? data.rowPitch : RowPitch(rec->format, w),
                                  data.slicePitch != 0 ? data.slicePitch : LevelSize(rec->format, w, h));
    return true;
}

ShaderHandle D3D11Device::CreateShader(ShaderStage stage, const ShaderBytecode& code, const char* debugName) noexcept {
    try {
        if (code.format != ShaderFormat::Dxbc || code.data == nullptr || code.size < 4 ||
            std::memcmp(code.data, "DXBC", 4) != 0) {
            FailText("CreateShader: the D3D11 backend takes DXBC bytecode only");
            return {};
        }
        ShaderRecord rec;
        rec.stage = stage;
        HRESULT hr;
        if (stage == ShaderStage::Vertex) {
            hr = device_->CreateVertexShader(code.data, code.size, nullptr, &rec.vs);
            if (SUCCEEDED(hr)) {
                SetName(rec.vs.Get(), debugName);
                const auto* p = static_cast<const std::uint8_t*>(code.data);
                rec.code.assign(p, p + code.size);
            }
        } else {
            hr = device_->CreatePixelShader(code.data, code.size, nullptr, &rec.ps);
            if (SUCCEEDED(hr)) {
                SetName(rec.ps.Get(), debugName);
            }
        }
        if (FAILED(hr)) {
            Fail(stage == ShaderStage::Vertex ? "CreateVertexShader" : "CreatePixelShader", hr);
            return {};
        }
        return ShaderHandle{shaders_.Add(std::move(rec))};
    } catch (...) {
        FailText("CreateShader: out of memory");
        return {};
    }
}

void D3D11Device::Destroy(BufferHandle buffer) noexcept {
    BufferRecord* rec = buffers_.Get(buffer.id);
    if (rec == nullptr || rec->transient) {
        FailText("Destroy: invalid buffer handle (or a transient ring buffer)");
        return;
    }
    try {
        buffers_.Remove(buffer.id);
    } catch (...) {
    }
}

void D3D11Device::Destroy(TextureHandle texture) noexcept {
    TextureRecord* rec = textures_.Get(texture.id);
    if (rec == nullptr) {
        FailText("Destroy: invalid texture handle");
        return;
    }
    context_.OnDestroyTexture(texture.id);
    stats_.textureBytes -= rec->bytes;
    try {
        textures_.Remove(texture.id);
    } catch (...) {
    }
}

void D3D11Device::Destroy(ShaderHandle shader) noexcept {
    try {
        if (!shaders_.Remove(shader.id)) {
            FailText("Destroy: invalid shader handle");
        }
    } catch (...) {
    }
}

SamplerHandle D3D11Device::GetSampler(const SamplerDesc& desc) noexcept {
    try {
        std::vector<std::uint32_t>& bucket = samplerCache_[HashOf(desc)];
        for (std::uint32_t id : bucket) {
            if (samplers_[id - 1].first == desc) {
                return SamplerHandle{id};
            }
        }
        const D3D11_SAMPLER_DESC sd = SamplerDescOf(desc);
        ComPtr<ID3D11SamplerState> state;
        const HRESULT hr = device_->CreateSamplerState(&sd, &state);
        if (FAILED(hr)) {
            Fail("CreateSamplerState", hr);
            return {};
        }
        samplers_.emplace_back(desc, std::move(state));
        const auto id = static_cast<std::uint32_t>(samplers_.size());
        bucket.push_back(id);
        return SamplerHandle{id};
    } catch (...) {
        FailText("GetSampler: out of memory");
        return {};
    }
}

ID3D11SamplerState* D3D11Device::GetSamplerState(std::uint32_t id) const noexcept {
    return id != 0 && id <= samplers_.size() ? samplers_[id - 1].second.Get() : nullptr;
}

const PipelineRecord* D3D11Device::GetPipelineRecord(std::uint32_t id) const noexcept {
    return id != 0 && id <= pipelines_.size() ? pipelines_[id - 1].get() : nullptr;
}

PipelineHandle D3D11Device::GetPipeline(const PipelineDesc& desc, const char* debugName) noexcept {
    try {
        std::vector<std::uint32_t>& bucket = pipelineCache_[HashOf(desc)];
        for (std::uint32_t id : bucket) {
            if (pipelines_[id - 1]->desc == desc) {
                return PipelineHandle{id};
            }
        }
        ShaderRecord* vs = shaders_.Get(desc.vertexShader.id);
        if (vs == nullptr || vs->stage != ShaderStage::Vertex) {
            FailText("GetPipeline: invalid vertex shader handle");
            return {};
        }
        ShaderRecord* ps = nullptr;
        if (desc.pixelShader) {
            ps = shaders_.Get(desc.pixelShader.id);
            if (ps == nullptr || ps->stage != ShaderStage::Pixel) {
                FailText("GetPipeline: invalid pixel shader handle");
                return {};
            }
        }
        if (desc.layout.attributeCount > kMaxVertexAttributes) {
            FailText("GetPipeline: too many vertex attributes");
            return {};
        }
        auto rec = std::make_unique<PipelineRecord>();
        rec->desc = desc;
        rec->vs = vs->vs;
        rec->ps = ps != nullptr ? ps->ps : nullptr;
        rec->topology = TopologyOf(desc.topology);
        if (desc.layout.attributeCount != 0) {
            for (const LayoutEntry& e : layouts_) {
                if (e.vertexShader == desc.vertexShader.id && e.layout == desc.layout) {
                    rec->layout = e.object;
                    break;
                }
            }
            if (rec->layout == nullptr) {
                D3D11_INPUT_ELEMENT_DESC elements[kMaxVertexAttributes]{};
                for (std::uint32_t i = 0; i < desc.layout.attributeCount; ++i) {
                    const VertexAttribute& a = desc.layout.attributes[i];
                    if (a.stream >= kMaxVertexStreams) {
                        FailText("GetPipeline: vertex attribute stream out of range");
                        return {};
                    }
                    elements[i].SemanticName = SemanticNameOf(a.semantic);
                    elements[i].SemanticIndex = a.semanticIndex;
                    elements[i].Format = VertexFormatOf(a.format);
                    elements[i].InputSlot = a.stream;
                    elements[i].AlignedByteOffset = a.offset;
                    elements[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                    elements[i].InstanceDataStepRate = 0;
                }
                const HRESULT hr = device_->CreateInputLayout(elements, desc.layout.attributeCount, vs->code.data(),
                                                              vs->code.size(), &rec->layout);
                if (FAILED(hr)) {
                    Fail("CreateInputLayout", hr);
                    return {};
                }
                SetName(rec->layout.Get(), debugName);
                layouts_.push_back({desc.vertexShader.id, desc.layout, rec->layout});
            }
        }
        // D3D11 returns the same object for equal state descriptions, so per-pipeline creation is cheap.
        const D3D11_BLEND_DESC bd = BlendDescOf(desc.blend);
        const D3D11_DEPTH_STENCIL_DESC dd = DepthStencilDescOf(desc.depthStencil);
        const D3D11_RASTERIZER_DESC rd = RasterDescOf(desc.raster);
        HRESULT hr = device_->CreateBlendState(&bd, &rec->blend);
        if (FAILED(hr)) {
            Fail("CreateBlendState", hr);
            return {};
        }
        hr = device_->CreateDepthStencilState(&dd, &rec->depthStencil);
        if (FAILED(hr)) {
            Fail("CreateDepthStencilState", hr);
            return {};
        }
        hr = device_->CreateRasterizerState(&rd, &rec->raster);
        if (FAILED(hr)) {
            Fail("CreateRasterizerState", hr);
            return {};
        }
        pipelines_.push_back(std::move(rec));
        const auto id = static_cast<std::uint32_t>(pipelines_.size());
        bucket.push_back(id);
        return PipelineHandle{id};
    } catch (...) {
        FailText("GetPipeline: out of memory");
        return {};
    }
}

// ---- frame -------------------------------------------------------------------------------------------------

PresentResult D3D11Device::Present(const PresentDesc& desc) noexcept {
    ++stats_.presents;
    PresentResult result = PresentResult::Ok;
    context_.EndTransient();
    const bool toSwapChain = swapChain_ != nullptr && !desc.skipSwapChain;
    if (toSwapChain) {
        UpdateSwapChainSize();
        DrawPresentPass();
    }
    EndGpuFrame();  // M3c: GPU frame timing (DeviceStats::gpuFrame*)
    if (!toSwapChain) {
        // M3c: nothing else submits the frame when the swap chain is skipped (minimized, display off, headless):
        // without it the GPU work (and the timer queries) waited for the driver's next implicit flush.
        immediate_->Flush();
    }
    if (swapChain_ != nullptr) {
        Global().presentThread.store(GetCurrentThreadId());
        HRESULT hr = S_OK;
        if (toSwapChain) {
            const UINT sync = desc.syncInterval > 4 ? 4u : desc.syncInterval;
            // M3b: tearing only on request (PresentDesc::allowTearing); DXGI allows it with sync interval 0 only.
            const UINT flags = sync == 0 && desc.allowTearing && tearingSupported_ ? DXGI_PRESENT_ALLOW_TEARING : 0u;
            hr = swapChain_->Present(sync, flags);
            result = PresentResultFromHresult(hr);
            if (result == PresentResult::DeviceLost || result == PresentResult::Failed) {
                Fail("IDXGISwapChain::Present", hr);
            }
        }
        UpdatePresentTiming(!desc.skipSwapChain && SUCCEEDED(hr), hr);
    }
    // The next persistent back buffer: it still holds the picture of backBufferCount_ presents ago.
    currentBackBuffer_ = (currentBackBuffer_ + 1) % backBufferCount_;
    context_.OnPresent();
    CollectGpuFrames(false);  // the next frame's timer starts at its first clear or draw (NoteGpuWork)
    PumpDebugMessages();
    return result;
}

// ---- M3c: GPU frame timing --------------------------------------------------------------------------------------

void D3D11Device::BeginGpuFrame() noexcept {
    if (gpuTimerOpen_) {
        return;
    }
    GpuTimer& t = gpuTimers_[gpuTimer_];
    if (t.pending) {
        // Still not read back after kGpuTimers frames: wait for it (a query must not be restarted before its
        // results were obtained - debug-layer warning #408).
        ReadGpuTimer(t, true);
    }
    if (t.disjoint == nullptr) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(device_->CreateQuery(&qd, &t.disjoint))) {
            return;
        }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(device_->CreateQuery(&qd, &t.begin)) || FAILED(device_->CreateQuery(&qd, &t.end))) {
            t.disjoint.Reset();
            t.begin.Reset();
            return;
        }
    }
    immediate_->Begin(t.disjoint.Get());
    immediate_->End(t.begin.Get());
    gpuTimerOpen_ = true;
}

void D3D11Device::EndGpuFrame() noexcept {
    if (!gpuTimerOpen_) {
        return;
    }
    GpuTimer& t = gpuTimers_[gpuTimer_];
    immediate_->End(t.end.Get());
    immediate_->End(t.disjoint.Get());
    t.pending = true;
    gpuTimerOpen_ = false;
    gpuTimer_ = (gpuTimer_ + 1) % kGpuTimers;
}

// One frame's queries: true when they were read (or turned out unusable), false if not finished yet. `wait`
// flushes and waits (at most ~250 ms, then the frame is dropped).
bool D3D11Device::ReadGpuTimer(GpuTimer& t, bool wait) noexcept {
    try {
        if (!t.pending) {
            return true;
        }
        if (gpuHistogram_.empty()) {
            gpuHistogram_.assign(1001, 0);  // 0.1 ms bins, 1000 = 100 ms and more
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        const UINT flags = wait ? 0u : static_cast<UINT>(D3D11_ASYNC_GETDATA_DONOTFLUSH);
        HRESULT hr = immediate_->GetData(t.disjoint.Get(), &disjoint, sizeof(disjoint), flags);
        if (wait && hr == S_FALSE) {
            const ULONGLONG deadline = GetTickCount64() + 250;
            while (hr == S_FALSE && GetTickCount64() < deadline) {
                SwitchToThread();
                hr = immediate_->GetData(t.disjoint.Get(), &disjoint, sizeof(disjoint), 0);
            }
        }
        if (hr == S_FALSE && !wait) {
            return false;
        }
        UINT64 begin = 0, end = 0;
        const bool ok = hr == S_OK && immediate_->GetData(t.begin.Get(), &begin, sizeof(begin), 0) == S_OK &&
                        immediate_->GetData(t.end.Get(), &end, sizeof(end), 0) == S_OK;
        t.pending = false;
        if (!ok || disjoint.Disjoint || disjoint.Frequency == 0 || end < begin) {
            return true;
        }
        const double ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
        ++gpuFrames_;
        gpuMsTotal_ += ms;
        gpuMsMax_ = std::max(gpuMsMax_, ms);
        ++gpuHistogram_[std::min<std::size_t>(static_cast<std::size_t>(ms * 10.0), gpuHistogram_.size() - 1)];
        return true;
    } catch (...) {
        t.pending = false;
        return true;
    }
}

void D3D11Device::CollectGpuFrames(bool wait) noexcept {
    for (std::uint32_t k = 0; k < kGpuTimers; ++k) {  // oldest first (gpuTimer_ = the next slot to record)
        GpuTimer& t = gpuTimers_[(gpuTimer_ + k) % kGpuTimers];
        if (!ReadGpuTimer(t, wait)) {
            break;  // the later ones are not ready either
        }
    }
}

// M3b pacing: present id and DXGI frame statistics after Present (also after a skipped one: earlier presents
// keep becoming visible). GetFrameStatistics fails with DXGI_ERROR_FRAME_STATISTICS_DISJOINT on the first call
// and while the statistics are interrupted (mode change, occlusion); that is reported, not logged.
void D3D11Device::UpdatePresentTiming(bool presented, HRESULT hr) noexcept {
    PresentTiming t;
    t.swapChainPresented = presented;
    t.occluded = hr == DXGI_STATUS_OCCLUDED;
    UINT count = 0;
    if (SUCCEEDED(swapChain_->GetLastPresentCount(&count))) {
        t.presentId = count;
    }
    DXGI_FRAME_STATISTICS fs{};
    const HRESULT shr = swapChain_->GetFrameStatistics(&fs);
    if (SUCCEEDED(shr)) {
        t.statsValid = true;
        t.statPresentCount = fs.PresentCount;
        t.statPresentRefreshCount = fs.PresentRefreshCount;
        t.statSyncRefreshCount = fs.SyncRefreshCount;
        t.statSyncQpc = fs.SyncQPCTime.QuadPart;
    } else {
        t.statsError = static_cast<long>(shr);
    }
    lastTiming_ = t;
}

bool D3D11Device::CopyToImage(ID3D11Texture2D* source, std::uint32_t level, Format format, std::uint32_t width,
                              std::uint32_t height, ComPtr<ID3D11Texture2D>* cachedStaging, Image& out) noexcept {
    try {
        D3D11_TEXTURE2D_DESC sd{};
        source->GetDesc(&sd);
        const std::uint32_t w = MipSize(width, level);
        const std::uint32_t h = MipSize(height, level);
        // The staging texture mirrors the whole source (all levels): a standalone texture of a small BC
        // level (2x2, 1x1) would be invalid, and level-to-level copies need no box.
        ComPtr<ID3D11Texture2D> staging;
        if (cachedStaging != nullptr && *cachedStaging != nullptr) {
            D3D11_TEXTURE2D_DESC cd{};
            (*cachedStaging)->GetDesc(&cd);
            if (cd.Width == sd.Width && cd.Height == sd.Height && cd.MipLevels == sd.MipLevels && cd.Format == sd.Format) {
                staging = *cachedStaging;
            }
        }
        if (staging == nullptr) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sd.Width;
            td.Height = sd.Height;
            td.MipLevels = sd.MipLevels;
            td.ArraySize = 1;
            td.Format = sd.Format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const HRESULT hr = device_->CreateTexture2D(&td, nullptr, &staging);
            if (FAILED(hr)) {
                return Fail("CreateTexture2D (staging)", hr);
            }
            if (cachedStaging != nullptr) {
                *cachedStaging = staging;
            }
        }
        const UINT subresource = D3D11CalcSubresource(level, 0, sd.MipLevels);
        immediate_->CopySubresourceRegion(staging.Get(), subresource, 0, 0, 0, source, subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = immediate_->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) {
            return Fail("Map (staging read)", hr);
        }
        const FormatInfo& info = GetFormatInfo(format);
        const std::uint32_t rows = (h + info.blockHeight - 1) / info.blockHeight;
        const std::uint32_t rowBytes = RowPitch(format, w);
        out.format = format;
        out.width = w;
        out.height = h;
        out.rowPitch = rowBytes;
        out.pixels.resize(static_cast<std::size_t>(rowBytes) * rows);
        for (std::uint32_t y = 0; y < rows; ++y) {
            std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * rowBytes,
                        static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch, rowBytes);
        }
        immediate_->Unmap(staging.Get(), subresource);
        return true;
    } catch (...) {
        return FailText("read-back: out of memory");
    }
}

bool D3D11Device::CaptureBackBuffer(Image& out) noexcept {
    ID3D11Texture2D* current = targets_.backBuffers[currentBackBuffer_].Get();
    if (current == nullptr) {
        return FailText("CaptureBackBuffer: no back buffer");
    }
    if (targets_.samples > 1) {  // M3c: the resolved picture (a copy needs a single-sampled source)
        ResolvedPicture(targets_, currentBackBuffer_);
        current = targets_.resolve.Get();
    }
    return CopyToImage(current, 0, info_.backBufferFormat, targets_.width, targets_.height, &captureStaging_, out);
}

bool D3D11Device::CaptureOutput(Image& out) noexcept {
    if (targets_.output == nullptr) {
        return CaptureBackBuffer(out);  // output = back buffer size
    }
    ID3D11ShaderResourceView* image = PrepareOutput();
    context_.RestoreBindings();  // the downsample pass replaced targets, shaders and states mid-frame
    if (image == nullptr) {
        return FailText("CaptureOutput: no output image");
    }
    return CopyToImage(targets_.output.Get(), 0, info_.backBufferFormat, targets_.outputWidth, targets_.outputHeight,
                       &outputStaging_, out);
}

std::uint32_t D3D11Device::SupportedSampleCount(Format color, Format depth, std::uint32_t requested) const noexcept {
    if (device_ == nullptr || requested <= 1) {
        return 1;
    }
    std::uint32_t n = 1;
    while (n * 2 <= requested && n * 2 <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT) {
        n *= 2;
    }
    const DXGI_FORMAT c = TextureFormatsOf(color, kTextureRenderTarget).rtv;
    const DXGI_FORMAT d = depth != Format::Unknown ? TextureFormatsOf(depth, kTextureDepthStencil).dsv
                                                   : DXGI_FORMAT_UNKNOWN;
    for (; n > 1; n /= 2) {
        UINT levels = 0;
        if (c == DXGI_FORMAT_UNKNOWN || FAILED(device_->CheckMultisampleQualityLevels(c, n, &levels)) || levels == 0) {
            continue;
        }
        if (d != DXGI_FORMAT_UNKNOWN &&
            (FAILED(device_->CheckMultisampleQualityLevels(d, n, &levels)) || levels == 0)) {
            continue;
        }
        return n;
    }
    return 1;
}

// M3c: new back-buffer configuration (window resize with [render] resolution = auto). The pictures of the
// persistent back buffers are carried over (resolved, then stretched or downsampled into the new buffers;
// a multisampled destination gets the color in every sample), so frames without draws keep showing the
// picture of `bufferCount` presents ago; the depth buffer starts at 1.0 / stencil 0.
bool D3D11Device::ResizeBackBuffers(std::uint32_t width, std::uint32_t height, std::uint32_t sampleCount,
                                    std::uint32_t outputWidth, std::uint32_t outputHeight) noexcept {
    try {
        if (width == 0 || height == 0 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || (outputWidth == 0) != (outputHeight == 0) ||
            outputWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || outputHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            return FailText("ResizeBackBuffers: invalid size");
        }
        const std::uint32_t samples = SupportedSampleCount(info_.backBufferFormat, info_.depthFormat, sampleCount);
        const std::uint32_t outW = outputWidth != 0 ? outputWidth : width;
        const std::uint32_t outH = outputHeight != 0 ? outputHeight : height;
        if (width == targets_.width && height == targets_.height && samples == targets_.samples &&
            outW == targets_.outputWidth && outH == targets_.outputHeight) {
            return true;
        }
        context_.EndTransient();
        Targets next;
        std::string error;
        if (!CreateTargets(width, height, samples, outputWidth, outputHeight, &next, &error)) {
            return FailText(("ResizeBackBuffers: " + error).c_str());
        }
        D3D11_VIEWPORT v{};
        v.Width = static_cast<float>(width);
        v.Height = static_cast<float>(height);
        v.MaxDepth = 1.0f;
        for (std::uint32_t i = 0; i < backBufferCount_; ++i) {
            ID3D11ShaderResourceView* old = ResolvedPicture(targets_, i);
            DrawBlit(old, targets_.width, targets_.height, next.backBufferRtvs[i].Get(), v, BlitFilter::Resample);
        }
        if (next.depthDsv != nullptr) {
            immediate_->ClearDepthStencilView(next.depthDsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
        immediate_->OMSetRenderTargets(0, nullptr, nullptr);
        const std::uint32_t oldW = targets_.width, oldH = targets_.height, oldSamples = targets_.samples;
        targets_ = std::move(next);
        captureStaging_.Reset();
        outputStaging_.Reset();
        info_.backBufferWidth = width;
        info_.backBufferHeight = height;
        info_.sampleCount = targets_.samples;
        info_.outputWidth = targets_.outputWidth;
        info_.outputHeight = targets_.outputHeight;
        ++backBufferResizes_;
        context_.OnBackBuffersChanged();
        FUT_LOGI(Gfx, "d3d11: back buffers resized from %ux%u (%u samples) to %ux%u (%u samples), output %ux%u, "
                 "%llu MiB", oldW, oldH, oldSamples, width, height, targets_.samples, targets_.outputWidth,
                 targets_.outputHeight, static_cast<unsigned long long>(targets_.bytes >> 20));
        PumpDebugMessages();
        return true;
    } catch (...) {
        return FailText("ResizeBackBuffers: out of memory");
    }
}

bool D3D11Device::ReadTexture(TextureHandle texture, std::uint32_t level, Image& out) noexcept {
    TextureRecord* rec = textures_.Get(texture.id);
    if (rec == nullptr || level >= rec->mipLevels) {
        return FailText("ReadTexture: invalid texture handle or level");
    }
    return CopyToImage(rec->texture.Get(), level, rec->format, rec->width, rec->height, nullptr, out);
}

DeviceStats D3D11Device::Stats() const noexcept {
    DeviceStats s = stats_;
    // M3c: GPU frame times (CollectGpuFrames).
    s.gpuFramesTimed = gpuFrames_;
    if (gpuFrames_ != 0) {
        s.gpuFrameMsAverage = gpuMsTotal_ / static_cast<double>(gpuFrames_);
        s.gpuFrameMsMax = gpuMsMax_;
        const std::uint64_t rank = (gpuFrames_ * 99 + 99) / 100;  // nearest-rank 99th percentile
        std::uint64_t seen = 0;
        for (std::size_t bin = 0; bin < gpuHistogram_.size(); ++bin) {
            seen += gpuHistogram_[bin];
            if (seen >= rank) {
                s.gpuFrameMsP99 = std::min(static_cast<double>(bin + 1) / 10.0, gpuMsMax_);
                break;
            }
        }
    }
    s.pipelines = static_cast<std::uint32_t>(pipelines_.size());
    s.samplers = static_cast<std::uint32_t>(samplers_.size());
    s.buffers = buffers_.Live();
    s.textures = textures_.Live();
    return s;
}

// ---- factory ---------------------------------------------------------------------------------------------------

std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc, std::string* error) noexcept {
    std::string reason;
    try {
        auto device = std::make_unique<D3D11Device>();
        if (device->Init(desc, &reason)) {
            return device;
        }
    } catch (...) {
        reason = "out of memory";
    }
    if (error != nullptr) {
        try {
            *error = reason;
        } catch (...) {
        }
    }
    return nullptr;
}

}  // namespace gfx::rhi::d3d11
