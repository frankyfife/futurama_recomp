// Internal declarations of the D3D11 backend (d3d11_device.h): handle pools, resource records, the
// device and its command context. Only the gfx/rhi/d3d11/*.cpp files include this header.
#pragma once

#include "gfx/rhi/d3d11/d3d11_device.h"

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gfx::rhi::d3d11 {

using Microsoft::WRL::ComPtr;

// ---- handle pool: id = generation << 20 | (index + 1); id 0 = none ------------------------------------------

inline constexpr std::uint32_t kHandleIndexBits = 20;
inline constexpr std::uint32_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;
inline constexpr std::uint32_t kHandleGenerationLimit = 1u << (32 - kHandleIndexBits);

template <class T>
class Pool {
public:
    // Stores `value`, returns its id (0 if the pool is full).
    std::uint32_t Add(T&& value) {
        std::uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            if (slots_.size() >= kHandleIndexMask) {
                return 0;
            }
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        Slot& s = slots_[index];
        s.value = std::move(value);
        s.used = true;
        ++live_;
        return (static_cast<std::uint32_t>(s.generation) << kHandleIndexBits) | (index + 1);
    }
    T* Get(std::uint32_t id) noexcept {
        const std::uint32_t index = (id & kHandleIndexMask) - 1;
        if (id == 0 || index >= slots_.size()) {
            return nullptr;
        }
        Slot& s = slots_[index];
        return s.used && s.generation == (id >> kHandleIndexBits) ? &s.value : nullptr;
    }
    bool Remove(std::uint32_t id) {
        if (Get(id) == nullptr) {
            return false;
        }
        const std::uint32_t index = (id & kHandleIndexMask) - 1;
        Slot& s = slots_[index];
        s.value = T{};
        s.used = false;
        s.generation = static_cast<std::uint16_t>(s.generation + 1 == kHandleGenerationLimit ? 1 : s.generation + 1);
        free_.push_back(index);
        --live_;
        return true;
    }
    std::uint32_t Live() const noexcept { return live_; }
    template <class F>
    void ForEach(F&& f) {
        for (Slot& s : slots_) {
            if (s.used) {
                f(s.value);
            }
        }
    }
    void Clear() noexcept {
        slots_.clear();
        free_.clear();
        live_ = 0;
    }

private:
    struct Slot {
        T value{};
        std::uint16_t generation = 1;
        bool used = false;
    };
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::uint32_t live_ = 0;
};

// ---- resource records ----------------------------------------------------------------------------------

struct BufferRecord {
    ComPtr<ID3D11Buffer> buffer;
    std::uint32_t size = 0;
    std::uint8_t bind = 0;
    BufferUsage usage = BufferUsage::Static;
    bool transient = false;  // a chunk of the transient ring (owned by the context)
};

struct TextureRecord {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;
    Format format = Format::Unknown;
    std::uint8_t usage = 0;
    std::uint64_t bytes = 0;
};

struct ShaderRecord {
    ShaderStage stage = ShaderStage::Vertex;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    std::vector<std::uint8_t> code;  // vertex shaders: input signature for CreateInputLayout
};

struct PipelineRecord {
    PipelineDesc desc;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depthStencil;
    ComPtr<ID3D11RasterizerState> raster;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

// ---- translations (d3d11_convert.cpp) ----------------------------------------------------------------------

struct TextureFormats {
    DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;  // typeless for sampled depth textures
    DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT rtv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
};
TextureFormats TextureFormatsOf(Format format, std::uint8_t usage) noexcept;
DXGI_FORMAT VertexFormatOf(VertexFormat format) noexcept;
const char* SemanticNameOf(VertexSemantic semantic) noexcept;
D3D11_BLEND BlendOf(BlendFactor factor, bool alphaChannel) noexcept;
D3D11_BLEND_OP BlendOpOf(BlendOp op) noexcept;
D3D11_COMPARISON_FUNC CompareOf(CompareFunc func) noexcept;
D3D11_STENCIL_OP StencilOpOf(StencilOp op) noexcept;
D3D11_TEXTURE_ADDRESS_MODE AddressOf(AddressMode mode) noexcept;
D3D11_PRIMITIVE_TOPOLOGY TopologyOf(PrimitiveTopology topology) noexcept;
D3D11_BLEND_DESC BlendDescOf(const BlendDesc& desc) noexcept;
D3D11_DEPTH_STENCIL_DESC DepthStencilDescOf(const DepthStencilDesc& desc) noexcept;
D3D11_RASTERIZER_DESC RasterDescOf(const RasterDesc& desc) noexcept;
D3D11_SAMPLER_DESC SamplerDescOf(const SamplerDesc& desc) noexcept;
std::uint64_t PrimitiveCount(PrimitiveTopology topology, std::uint32_t vertices) noexcept;

// ---- device and context ------------------------------------------------------------------------------------

class D3D11Device;

class D3D11Context final : public CommandContext {
public:
    explicit D3D11Context(D3D11Device& device) noexcept : device_(device) {}

    void SetRenderTargets(TextureHandle color, TextureHandle depth) noexcept override;
    void SetViewport(const Viewport& viewport) noexcept override;
    void SetScissor(const Rect& rect) noexcept override;
    void Clear(std::uint8_t flags, const float color[4], float depth, std::uint8_t stencil,
               const Rect* rect) noexcept override;
    void SetPipeline(PipelineHandle pipeline) noexcept override;
    void SetStencilRef(std::uint8_t ref) noexcept override;
    void SetBlendFactor(const float factor[4]) noexcept override;
    void SetVertexBuffer(std::uint32_t stream, BufferHandle buffer, std::uint32_t offset,
                         std::uint32_t stride) noexcept override;
    void SetIndexBuffer(BufferHandle buffer, std::uint32_t offset) noexcept override;
    void SetConstantBuffer(std::uint8_t stages, std::uint32_t slot, BufferHandle buffer) noexcept override;
    void SetTexture(std::uint32_t slot, TextureHandle texture) noexcept override;
    void SetSampler(std::uint32_t slot, SamplerHandle sampler) noexcept override;
    void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) noexcept override;
    void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::int32_t baseVertex) noexcept override;
    bool UpdateBuffer(BufferHandle buffer, const void* data, std::uint32_t size) noexcept override;
    TransientSpan BeginTransient(TransientKind kind, std::uint32_t size, std::uint32_t alignment) noexcept override;
    void EndTransient() noexcept override;

    // Device-internal.
    void Init(ID3D11DeviceContext1* context) noexcept;
    void BindDefaultTargets() noexcept;  // back buffer + depth, full viewport
    // After Present (the device's present pass changed shaders, states, targets, viewport, PS t0/s0 and
    // moved to the next back buffer): re-bind the current back buffer (if it is the color target) and
    // everything the context had bound, start a new transient frame.
    void OnPresent() noexcept;
    void OnDestroyTexture(std::uint32_t id) noexcept;
    void Release() noexcept;             // ClearState, drop references and the transient chunks

    static constexpr std::uint32_t kMaxTextureSlots = 16;
    static constexpr std::uint32_t kMaxSamplerSlots = 16;
    static constexpr std::uint32_t kMaxConstantSlots = 14;
    static constexpr std::uint32_t kTransientVertexChunk = 4u << 20;
    static constexpr std::uint32_t kTransientIndexChunk = 1u << 20;
    static constexpr std::uint32_t kTransientMaxChunk = 256u << 20;

private:
    struct TransientChunk {
        std::uint32_t handle = 0;  // BufferHandle id
        ID3D11Buffer* buffer = nullptr;  // owned by the pool record
        std::uint32_t size = 0;
        bool discardNext = true;
    };
    struct TransientRing {
        std::vector<TransientChunk> chunks;
        std::size_t current = 0;
        std::uint32_t offset = 0;
    };

    void ApplyTargets() noexcept;
    void ApplyViewport() noexcept;
    void ApplyPipeline(bool force) noexcept;
    bool ClearDepthStencilRect(const D3D11_RECT& rect, std::uint8_t flags, float depth, std::uint8_t stencil) noexcept;
    bool CreateTransientChunk(TransientKind kind, std::uint32_t size, TransientChunk* out) noexcept;

    D3D11Device& device_;
    ID3D11DeviceContext1* ctx_ = nullptr;  // owned by the device

    // Bound state.
    std::uint32_t colorTarget_ = 0;  // TextureHandle id, 0 = back buffer
    std::uint32_t depthTarget_ = 0;
    bool defaultTargets_ = true;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11DepthStencilView* dsv_ = nullptr;
    std::uint32_t targetWidth_ = 0;
    std::uint32_t targetHeight_ = 0;
    D3D11_VIEWPORT viewport_{};
    std::uint32_t pipeline_ = 0;  // PipelineHandle id
    const PipelineRecord* applied_ = nullptr;
    std::uint8_t stencilRef_ = 0;
    float blendFactor_[4] = {0, 0, 0, 0};
    std::array<std::uint32_t, kMaxTextureSlots> textures_{};  // TextureHandle ids bound to PS t#
    std::array<ID3D11SamplerState*, kMaxSamplerSlots> samplers_{};  // bound PS s# (owned by the device)

    // Rectangle depth/stencil clear (lazily created).
    ComPtr<ID3D11VertexShader> clearVs_;
    ComPtr<ID3D11BlendState> clearBlend_;
    ComPtr<ID3D11RasterizerState> clearRaster_;
    ComPtr<ID3D11DepthStencilState> clearDepthStencil_[3];  // depth, stencil, both

    TransientRing rings_[2];
    int openRing_ = -1;
};

class D3D11Device final : public Device {
public:
    D3D11Device() noexcept;
    ~D3D11Device() override;
    D3D11Device(const D3D11Device&) = delete;
    D3D11Device& operator=(const D3D11Device&) = delete;

    bool Init(const DeviceDesc& desc, std::string* error) noexcept;

    const DeviceInfo& Info() const noexcept override { return info_; }
    CommandContext& Context() noexcept override { return context_; }
    ShaderFormat NativeShaderFormat() const noexcept override { return ShaderFormat::Dxbc; }
    bool IsFormatSupported(Format format, std::uint8_t usage) const noexcept override;

    BufferHandle CreateBuffer(const BufferDesc& desc) noexcept override;
    TextureHandle CreateTexture(const TextureDesc& desc, const SubresourceData* levels) noexcept override;
    bool UpdateTexture(TextureHandle texture, std::uint32_t level, const SubresourceData& data) noexcept override;
    ShaderHandle CreateShader(ShaderStage stage, const ShaderBytecode& code, const char* debugName) noexcept override;
    void Destroy(BufferHandle buffer) noexcept override;
    void Destroy(TextureHandle texture) noexcept override;
    void Destroy(ShaderHandle shader) noexcept override;

    SamplerHandle GetSampler(const SamplerDesc& desc) noexcept override;
    PipelineHandle GetPipeline(const PipelineDesc& desc, const char* debugName) noexcept override;

    PresentResult Present(const PresentDesc& desc) noexcept override;
    bool CaptureBackBuffer(Image& out) noexcept override;
    bool ReadTexture(TextureHandle texture, std::uint32_t level, Image& out) noexcept override;

    DebugLayerInfo DebugLayer() const noexcept override { return debugLayer_; }
    std::size_t DrainDebugMessages(std::vector<DebugMessage>& out) noexcept override;
    DeviceStats Stats() const noexcept override;
    const char* LastError() const noexcept override { return lastError_.c_str(); }

    // Backend-internal (context, tests).
    ID3D11Device1* D3D() const noexcept { return device_.Get(); }
    BufferRecord* GetBuffer(std::uint32_t id) noexcept { return buffers_.Get(id); }
    TextureRecord* GetTexture(std::uint32_t id) noexcept { return textures_.Get(id); }
    const PipelineRecord* GetPipelineRecord(std::uint32_t id) const noexcept;
    ID3D11SamplerState* GetSamplerState(std::uint32_t id) const noexcept;
    // The current one of the persistent back buffers (rotated by Present).
    ID3D11RenderTargetView* BackBufferRtv() const noexcept { return backBufferRtvs_[currentBackBuffer_].Get(); }
    ID3D11DepthStencilView* BackBufferDsv() const noexcept { return depthDsv_.Get(); }
    std::uint32_t AddTransientBuffer(ComPtr<ID3D11Buffer> buffer, std::uint32_t size, std::uint8_t bind) noexcept;
    void RemoveTransientBuffer(std::uint32_t id) noexcept;
    DeviceStats& MutableStats() noexcept { return stats_; }
    // Records `what` + HRESULT text (+ device-removed reason) as LastError, logs it at debug level and
    // pulls the debug-layer messages (they usually explain the failure). Returns false.
    bool Fail(const char* what, HRESULT hr) noexcept;
    bool FailText(const char* text) noexcept;  // LastError without an HRESULT
    void PumpDebugMessages() noexcept;

private:
    bool CreateFactoryAndDevice(const DeviceDesc& desc, std::string* error) noexcept;
    bool SetUpDebugLayer() noexcept;
    bool CreateSwapChain(const DeviceDesc& desc, std::string* error) noexcept;
    bool CreateBackBuffers(const DeviceDesc& desc, std::string* error) noexcept;
    bool CreatePresentPass(std::string* error) noexcept;
    bool UpdateSwapChainSize() noexcept;  // ResizeBuffers to the window's client size (render thread)
    void DrawPresentPass() noexcept;      // current back buffer -> swap chain buffer (scaling_)
    bool CreateDepthBuffer(const DeviceDesc& desc, std::string* error) noexcept;
    void SetName(ID3D11DeviceChild* object, const char* name) noexcept;
    void HandleDebugMessage(DebugSeverity severity, std::int32_t id, bool dxgi, const char* text) noexcept;
    bool CopyToImage(ID3D11Texture2D* source, std::uint32_t level, Format format, std::uint32_t width,
                     std::uint32_t height, ComPtr<ID3D11Texture2D>* cachedStaging, Image& out) noexcept;
    void ReportLiveObjects() noexcept;

    DeviceInfo info_;
    DebugLayerInfo debugLayer_;
    DeviceStats stats_;
    std::string lastError_;

    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D11Device1> device_;
    ComPtr<ID3D11DeviceContext1> immediate_;
    ComPtr<ID3D11InfoQueue> infoQueue_;
    ComPtr<IDXGIInfoQueue> dxgiInfoQueue_;
    ComPtr<IDXGISwapChain1> swapChain_;
    bool tearingSupported_ = false;
    void* window_ = nullptr;  // HWND of the swap chain
    std::uint32_t swapWidth_ = 0, swapHeight_ = 0;
    UINT swapFlags_ = 0;
    ComPtr<ID3D11RenderTargetView> swapRtv_;  // buffer 0 of the swap chain (released for ResizeBuffers)
    // Persistent back buffers (DeviceDesc::bufferCount, rotated by Present; review M3 findings 2/8).
    static constexpr std::uint32_t kMaxBackBuffers = 3;
    ComPtr<ID3D11Texture2D> backBuffers_[kMaxBackBuffers];
    ComPtr<ID3D11RenderTargetView> backBufferRtvs_[kMaxBackBuffers];
    ComPtr<ID3D11ShaderResourceView> backBufferSrvs_[kMaxBackBuffers];
    std::uint32_t backBufferCount_ = 1;
    std::uint32_t currentBackBuffer_ = 0;
    // Present pass (window only).
    PresentScaling scaling_ = PresentScaling::Linear;
    ComPtr<ID3D11VertexShader> presentVs_;
    ComPtr<ID3D11PixelShader> presentPs_;
    ComPtr<ID3D11SamplerState> presentPoint_, presentLinear_;
    ComPtr<ID3D11RasterizerState> presentRaster_;
    ComPtr<ID3D11DepthStencilState> presentDepth_;
    ComPtr<ID3D11BlendState> presentBlend_;
    std::uint64_t swapResizes_ = 0;
    ComPtr<ID3D11Texture2D> depthBuffer_;
    ComPtr<ID3D11DepthStencilView> depthDsv_;
    ComPtr<ID3D11Texture2D> captureStaging_;

    Pool<BufferRecord> buffers_;
    Pool<TextureRecord> textures_;
    Pool<ShaderRecord> shaders_;
    std::vector<std::unique_ptr<PipelineRecord>> pipelines_;  // id = index + 1, never destroyed
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> pipelineCache_;
    std::vector<std::pair<SamplerDesc, ComPtr<ID3D11SamplerState>>> samplers_;  // id = index + 1
    std::unordered_map<std::size_t, std::vector<std::uint32_t>> samplerCache_;
    struct LayoutEntry {
        std::uint32_t vertexShader;
        VertexLayoutDesc layout;
        ComPtr<ID3D11InputLayout> object;
    };
    std::vector<LayoutEntry> layouts_;

    std::vector<DebugMessage> pendingMessages_;  // for DrainDebugMessages (bounded)
    std::unordered_map<std::int64_t, std::uint32_t> messageCounts_;  // per (producer, id): lines logged

    D3D11Context context_;
};

}  // namespace gfx::rhi::d3d11
