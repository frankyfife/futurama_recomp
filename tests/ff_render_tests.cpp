// Rendered pixel checks of the fixed-function emulation (GFX_CONTRACT.md 6.6): FixedFunctionPipeline
// draws on a headless WARP device and the pixels are compared with CPU reference values of the D3D8
// fixed-function semantics (texture stage ops, lighting, fog, alpha test, pre-transformed pixels, flat
// shading, culling, strips, blending, depth).
//
// The device is a minimal test-only rhi::Device on raw D3D11 (WarpDevice below) so that these tests do not
// depend on the backend agent's gfx/rhi/d3d11 implementation (built in parallel); it follows the D3D11
// mapping rules of GFX_CONTRACT.md 5.3 (cull CW -> FrontCounterClockwise + CULL_BACK, R16 strip cuts,
// zero-stride streams, transient ring with NO_OVERWRITE/DISCARD). With the D3D11 SDK layers installed the
// device runs with the debug layer and every test checks that no warning or error was reported.
#include "doctest.h"

#include "gfx/ff_pipeline.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using namespace gfx;

// ---- minimal D3D11 WARP rhi::Device ---------------------------------------------------------------------------

class WarpDevice final : public rhi::Device, public rhi::CommandContext {
public:
    static constexpr UINT kSize = 64;

    bool Create() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                                       &device_, nullptr, &dc_);
        if (FAILED(hr)) {
            flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                                   &device_, nullptr, &dc_);
        }
        if (FAILED(hr)) {
            return false;
        }
        debugLayer_ = SUCCEEDED(device_.As(&infoQueue_));
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = kSize;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &color_)) ||
            FAILED(device_->CreateRenderTargetView(color_.Get(), nullptr, &rtv_))) {
            return false;
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) {
            return false;
        }
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        td.CPUAccessFlags = 0;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &depth_)) ||
            FAILED(device_->CreateDepthStencilView(depth_.Get(), nullptr, &dsv_))) {
            return false;
        }
        dc_->OMSetRenderTargets(1, rtv_.GetAddressOf(), dsv_.Get());
        const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
        dc_->RSSetViewports(1, &vp);
        for (int k = 0; k < 2; ++k) {
            rhi::BufferDesc ring;
            ring.size = k == 0 ? (1u << 20) : (256u << 10);
            ring.bind = k == 0 ? rhi::kBindVertex : rhi::kBindIndex;
            ring.usage = rhi::BufferUsage::Dynamic;
            ringHandle_[k] = CreateBuffer(ring);
            if (!ringHandle_[k]) {
                return false;
            }
        }
        info_.backend = rhi::Backend::D3D11;
        info_.adapterName = "WARP (test)";
        info_.softwareAdapter = info_.headless = true;
        info_.backBufferWidth = info_.backBufferHeight = kSize;
        info_.backBufferFormat = rhi::Format::BGRA8_UNorm;
        info_.depthFormat = rhi::Format::D24_UNorm_S8_UInt;
        return true;
    }

    bool HasDebugLayer() const { return debugLayer_; }

    // Warnings/errors of the debug layer since the last call (empty without the layer).
    std::vector<std::string> TakeDebugProblems() {
        std::vector<std::string> out;
        if (!infoQueue_) {
            return out;
        }
        const UINT64 n = infoQueue_->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T size = 0;
            infoQueue_->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue_->GetMessage(i, message, &size)) &&
                message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                out.emplace_back(message->pDescription, message->DescriptionByteLength);
            }
        }
        infoQueue_->ClearStoredMessages();
        return out;
    }

    struct Pixel {
        int r, g, b, a;
    };
    Pixel ReadPixel(UINT x, UINT y) {
        dc_->CopyResource(staging_.Get(), color_.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        Pixel p{-1, -1, -1, -1};
        if (SUCCEEDED(dc_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map))) {
            const auto* row = static_cast<const std::uint8_t*>(map.pData) + y * map.RowPitch + x * 4;
            p = {row[2], row[1], row[0], row[3]};
            dc_->Unmap(staging_.Get(), 0);
        }
        return p;
    }

    // ---- rhi::Device ----
    const rhi::DeviceInfo& Info() const noexcept override { return info_; }
    rhi::CommandContext& Context() noexcept override { return *this; }
    rhi::ShaderFormat NativeShaderFormat() const noexcept override { return rhi::ShaderFormat::Dxbc; }
    bool IsFormatSupported(rhi::Format format, std::uint8_t) const noexcept override {
        return format == rhi::Format::BGRA8_UNorm || format == rhi::Format::RGBA8_UNorm;
    }
    rhi::BufferHandle CreateBuffer(const rhi::BufferDesc& desc) noexcept override {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = desc.size;
        bd.BindFlags = ((desc.bind & rhi::kBindVertex) ? D3D11_BIND_VERTEX_BUFFER : 0u) |
                       ((desc.bind & rhi::kBindIndex) ? D3D11_BIND_INDEX_BUFFER : 0u) |
                       ((desc.bind & rhi::kBindConstant) ? D3D11_BIND_CONSTANT_BUFFER : 0u);
        if (desc.usage == rhi::BufferUsage::Dynamic) {
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        } else {
            bd.Usage = D3D11_USAGE_IMMUTABLE;
        }
        D3D11_SUBRESOURCE_DATA init{desc.initialData, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device_->CreateBuffer(&bd, desc.initialData != nullptr ? &init : nullptr, &buffer))) {
            return {};
        }
        buffers_.push_back(buffer);
        return {static_cast<std::uint32_t>(buffers_.size())};
    }
    rhi::TextureHandle CreateTexture(const rhi::TextureDesc& desc, const rhi::SubresourceData* levels) noexcept override {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = desc.width;
        td.Height = desc.height;
        td.MipLevels = desc.mipLevels;
        td.ArraySize = 1;
        td.Format = desc.format == rhi::Format::RGBA8_UNorm ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> init(desc.mipLevels);
        for (std::uint32_t i = 0; i < desc.mipLevels && levels != nullptr; ++i) {
            init[i] = {levels[i].data, levels[i].rowPitch, levels[i].slicePitch};
        }
        Texture t;
        if (levels == nullptr || FAILED(device_->CreateTexture2D(&td, init.data(), &t.texture)) ||
            FAILED(device_->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv))) {
            return {};
        }
        textures_.push_back(t);
        return {static_cast<std::uint32_t>(textures_.size())};
    }
    bool UpdateTexture(rhi::TextureHandle, std::uint32_t, const rhi::SubresourceData&) noexcept override { return false; }
    rhi::ShaderHandle CreateShader(rhi::ShaderStage stage, const rhi::ShaderBytecode& code, const char*) noexcept override {
        Shader s;
        s.code.assign(static_cast<const std::uint8_t*>(code.data), static_cast<const std::uint8_t*>(code.data) + code.size);
        const HRESULT hr = stage == rhi::ShaderStage::Vertex
                               ? device_->CreateVertexShader(code.data, code.size, nullptr, &s.vs)
                               : device_->CreatePixelShader(code.data, code.size, nullptr, &s.ps);
        if (FAILED(hr)) {
            return {};
        }
        shaders_.push_back(s);
        return {static_cast<std::uint32_t>(shaders_.size())};
    }
    void Destroy(rhi::BufferHandle h) noexcept override {
        if (h.id != 0 && h.id <= buffers_.size()) buffers_[h.id - 1].Reset();
    }
    void Destroy(rhi::TextureHandle h) noexcept override {
        if (h.id != 0 && h.id <= textures_.size()) textures_[h.id - 1] = {};
    }
    void Destroy(rhi::ShaderHandle h) noexcept override {
        if (h.id != 0 && h.id <= shaders_.size()) shaders_[h.id - 1] = {};
    }
    rhi::SamplerHandle GetSampler(const rhi::SamplerDesc& desc) noexcept override {
        for (std::size_t i = 0; i < samplers_.size(); ++i) {
            if (samplers_[i].desc == desc) return {static_cast<std::uint32_t>(i + 1)};
        }
        auto filterType = [](rhi::Filter f) {
            return f == rhi::Filter::Linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
        };
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = static_cast<D3D11_FILTER>(D3D11_ENCODE_BASIC_FILTER(
            filterType(desc.minFilter), filterType(desc.magFilter),
            desc.mipFilter == rhi::MipFilter::Linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
            D3D11_FILTER_REDUCTION_TYPE_STANDARD));
        sd.AddressU = Address(desc.addressU);
        sd.AddressV = Address(desc.addressV);
        sd.AddressW = Address(desc.addressW);
        sd.MipLODBias = desc.mipLodBias;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = desc.mipFilter == rhi::MipFilter::None ? 0.0f : desc.maxLod;
        Sampler s{desc, nullptr};
        if (FAILED(device_->CreateSamplerState(&sd, &s.state))) {
            return {};
        }
        samplers_.push_back(s);
        return {static_cast<std::uint32_t>(samplers_.size())};
    }
    rhi::PipelineHandle GetPipeline(const rhi::PipelineDesc& desc, const char*) noexcept override {
        for (std::size_t i = 0; i < pipelines_.size(); ++i) {
            if (pipelines_[i].desc == desc) return {static_cast<std::uint32_t>(i + 1)};
        }
        Pipeline p;
        p.desc = desc;
        const Shader* vs = desc.vertexShader.id != 0 && desc.vertexShader.id <= shaders_.size()
                               ? &shaders_[desc.vertexShader.id - 1] : nullptr;
        if (vs == nullptr || !vs->vs) {
            return {};
        }
        D3D11_INPUT_ELEMENT_DESC elements[rhi::kMaxVertexAttributes]{};
        static const char* const kSemantics[] = {"POSITION", "NORMAL", "COLOR", "TEXCOORD"};
        for (std::uint32_t i = 0; i < desc.layout.attributeCount; ++i) {
            const rhi::VertexAttribute& a = desc.layout.attributes[i];
            elements[i].SemanticName = kSemantics[static_cast<int>(a.semantic)];
            elements[i].SemanticIndex = a.semanticIndex;
            elements[i].Format = a.format == rhi::VertexFormat::Float2   ? DXGI_FORMAT_R32G32_FLOAT
                                 : a.format == rhi::VertexFormat::Float3 ? DXGI_FORMAT_R32G32B32_FLOAT
                                 : a.format == rhi::VertexFormat::Float4 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                                                         : DXGI_FORMAT_R32_FLOAT;
            elements[i].InputSlot = a.stream;
            elements[i].AlignedByteOffset = a.offset;
            elements[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        }
        if (FAILED(device_->CreateInputLayout(elements, desc.layout.attributeCount, vs->code.data(), vs->code.size(),
                                              &p.layout))) {
            return {};
        }
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = desc.blend.enable;
        bd.RenderTarget[0].SrcBlend = Blend(desc.blend.src);
        bd.RenderTarget[0].DestBlend = Blend(desc.blend.dst);
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = Blend(desc.blend.srcAlpha);
        bd.RenderTarget[0].DestBlendAlpha = Blend(desc.blend.dstAlpha);
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = desc.blend.writeMask;
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = desc.depthStencil.depthEnable;
        dd.DepthWriteMask = desc.depthStencil.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(static_cast<int>(desc.depthStencil.depthFunc) + 1);
        dd.StencilEnable = desc.depthStencil.stencilEnable;
        dd.StencilReadMask = desc.depthStencil.stencilReadMask;
        dd.StencilWriteMask = desc.depthStencil.stencilWriteMask;
        dd.FrontFace.StencilFailOp = static_cast<D3D11_STENCIL_OP>(static_cast<int>(desc.depthStencil.stencilFail) + 1);
        dd.FrontFace.StencilDepthFailOp =
            static_cast<D3D11_STENCIL_OP>(static_cast<int>(desc.depthStencil.stencilDepthFail) + 1);
        dd.FrontFace.StencilPassOp = static_cast<D3D11_STENCIL_OP>(static_cast<int>(desc.depthStencil.stencilPass) + 1);
        dd.FrontFace.StencilFunc = static_cast<D3D11_COMPARISON_FUNC>(static_cast<int>(desc.depthStencil.stencilFunc) + 1);
        dd.BackFace = dd.FrontFace;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = desc.raster.fill == rhi::FillMode::Wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        rd.CullMode = desc.raster.cull == rhi::CullMode::None ? D3D11_CULL_NONE : D3D11_CULL_BACK;
        rd.FrontCounterClockwise = desc.raster.cull == rhi::CullMode::CW;  // D3D8 CW culled = CCW is front
        rd.DepthClipEnable = desc.raster.depthClip;
        rd.ScissorEnable = desc.raster.scissorEnable;
        if (FAILED(device_->CreateBlendState(&bd, &p.blend)) || FAILED(device_->CreateDepthStencilState(&dd, &p.depth)) ||
            FAILED(device_->CreateRasterizerState(&rd, &p.raster))) {
            return {};
        }
        pipelines_.push_back(p);
        return {static_cast<std::uint32_t>(pipelines_.size())};
    }
    rhi::PresentResult Present(const rhi::PresentDesc&) noexcept override { return rhi::PresentResult::Ok; }
    bool CaptureBackBuffer(rhi::Image&) noexcept override { return false; }
    bool ReadTexture(rhi::TextureHandle, std::uint32_t, rhi::Image&) noexcept override { return false; }
    rhi::DebugLayerInfo DebugLayer() const noexcept override { return {}; }
    std::size_t DrainDebugMessages(std::vector<rhi::DebugMessage>&) noexcept override { return 0; }
    rhi::DeviceStats Stats() const noexcept override { return {}; }
    const char* LastError() const noexcept override { return "WarpDevice"; }

    // ---- rhi::CommandContext ----
    void SetRenderTargets(rhi::TextureHandle, rhi::TextureHandle) noexcept override {}
    void SetViewport(const rhi::Viewport& v) noexcept override {
        const D3D11_VIEWPORT vp{v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
        dc_->RSSetViewports(1, &vp);
    }
    void SetScissor(const rhi::Rect&) noexcept override {}
    void Clear(std::uint8_t flags, const float color[4], float depth, std::uint8_t stencil, const rhi::Rect*) noexcept override {
        if (flags & rhi::kClearColor) dc_->ClearRenderTargetView(rtv_.Get(), color);
        const UINT dsFlags = ((flags & rhi::kClearDepth) ? D3D11_CLEAR_DEPTH : 0u) |
                             ((flags & rhi::kClearStencil) ? D3D11_CLEAR_STENCIL : 0u);
        if (dsFlags != 0) dc_->ClearDepthStencilView(dsv_.Get(), dsFlags, depth, stencil);
    }
    void SetPipeline(rhi::PipelineHandle h) noexcept override {
        const Pipeline& p = pipelines_[h.id - 1];
        dc_->IASetInputLayout(p.layout.Get());
        dc_->IASetPrimitiveTopology(p.desc.topology == rhi::PrimitiveTopology::TriangleStrip
                                        ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                        : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc_->VSSetShader(shaders_[p.desc.vertexShader.id - 1].vs.Get(), nullptr, 0);
        dc_->PSSetShader(shaders_[p.desc.pixelShader.id - 1].ps.Get(), nullptr, 0);
        const float factor[4] = {0, 0, 0, 0};
        dc_->OMSetBlendState(p.blend.Get(), factor, 0xFFFFFFFFu);
        currentDepth_ = p.depth;
        dc_->OMSetDepthStencilState(p.depth.Get(), stencilRef_);
        dc_->RSSetState(p.raster.Get());
    }
    void SetStencilRef(std::uint8_t ref) noexcept override {
        stencilRef_ = ref;
        dc_->OMSetDepthStencilState(currentDepth_.Get(), ref);
    }
    void SetBlendFactor(const float[4]) noexcept override {}
    void SetVertexBuffer(std::uint32_t stream, rhi::BufferHandle buffer, std::uint32_t offset,
                         std::uint32_t stride) noexcept override {
        ID3D11Buffer* b = buffers_[buffer.id - 1].Get();
        dc_->IASetVertexBuffers(stream, 1, &b, &stride, &offset);
    }
    void SetIndexBuffer(rhi::BufferHandle buffer, std::uint32_t offset) noexcept override {
        dc_->IASetIndexBuffer(buffers_[buffer.id - 1].Get(), DXGI_FORMAT_R16_UINT, offset);
    }
    void SetConstantBuffer(std::uint8_t stages, std::uint32_t slot, rhi::BufferHandle buffer) noexcept override {
        ID3D11Buffer* b = buffers_[buffer.id - 1].Get();
        if (stages & rhi::kStageVertex) dc_->VSSetConstantBuffers(slot, 1, &b);
        if (stages & rhi::kStagePixel) dc_->PSSetConstantBuffers(slot, 1, &b);
    }
    void SetTexture(std::uint32_t slot, rhi::TextureHandle texture) noexcept override {
        ID3D11ShaderResourceView* srv = textures_[texture.id - 1].srv.Get();
        dc_->PSSetShaderResources(slot, 1, &srv);
    }
    void SetSampler(std::uint32_t slot, rhi::SamplerHandle sampler) noexcept override {
        ID3D11SamplerState* s = samplers_[sampler.id - 1].state.Get();
        dc_->PSSetSamplers(slot, 1, &s);
    }
    void Draw(std::uint32_t count, std::uint32_t first) noexcept override { dc_->Draw(count, first); }
    void DrawIndexed(std::uint32_t count, std::uint32_t first, std::int32_t base) noexcept override {
        dc_->DrawIndexed(count, first, base);
    }
    bool UpdateBuffer(rhi::BufferHandle buffer, const void* data, std::uint32_t size) noexcept override {
        D3D11_MAPPED_SUBRESOURCE map{};
        ID3D11Buffer* b = buffers_[buffer.id - 1].Get();
        if (FAILED(dc_->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
        std::memcpy(map.pData, data, size);
        dc_->Unmap(b, 0);
        return true;
    }
    rhi::TransientSpan BeginTransient(rhi::TransientKind kind, std::uint32_t size, std::uint32_t alignment) noexcept override {
        const int k = kind == rhi::TransientKind::Vertex ? 0 : 1;
        const std::uint32_t capacity = k == 0 ? (1u << 20) : (256u << 10);
        std::uint32_t offset = (ringPos_[k] + alignment - 1) / alignment * alignment;
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (offset + size > capacity || !ringMapped_[k]) {
            offset = 0;
            mode = D3D11_MAP_WRITE_DISCARD;
        }
        if (size > capacity) return {};
        D3D11_MAPPED_SUBRESOURCE map{};
        ID3D11Buffer* b = buffers_[ringHandle_[k].id - 1].Get();
        if (FAILED(dc_->Map(b, 0, mode, 0, &map))) return {};
        ringMapped_[k] = true;
        ringPos_[k] = offset + size;
        openRing_ = k;
        return {ringHandle_[k], offset, size, static_cast<std::uint8_t*>(map.pData) + offset};
    }
    void EndTransient() noexcept override {
        if (openRing_ >= 0) dc_->Unmap(buffers_[ringHandle_[openRing_].id - 1].Get(), 0);
        openRing_ = -1;
    }

private:
    static D3D11_TEXTURE_ADDRESS_MODE Address(rhi::AddressMode m) {
        switch (m) {
            case rhi::AddressMode::Clamp: return D3D11_TEXTURE_ADDRESS_CLAMP;
            case rhi::AddressMode::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
            case rhi::AddressMode::Border: return D3D11_TEXTURE_ADDRESS_BORDER;
            default: return D3D11_TEXTURE_ADDRESS_WRAP;
        }
    }
    static D3D11_BLEND Blend(rhi::BlendFactor f) {
        static const D3D11_BLEND table[] = {D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR,
                                            D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA,
                                            D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_DEST_COLOR,
                                            D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_SRC_ALPHA_SAT, D3D11_BLEND_BLEND_FACTOR,
                                            D3D11_BLEND_INV_BLEND_FACTOR};
        return table[static_cast<int>(f)];
    }
    struct Texture {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    struct Shader {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        std::vector<std::uint8_t> code;
    };
    struct Sampler {
        rhi::SamplerDesc desc;
        ComPtr<ID3D11SamplerState> state;
    };
    struct Pipeline {
        rhi::PipelineDesc desc;
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11DepthStencilState> depth;
        ComPtr<ID3D11RasterizerState> raster;
    };
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> dc_;
    ComPtr<ID3D11InfoQueue> infoQueue_;
    ComPtr<ID3D11Texture2D> color_, staging_, depth_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11DepthStencilView> dsv_;
    ComPtr<ID3D11DepthStencilState> currentDepth_;
    std::uint8_t stencilRef_ = 0;
    bool debugLayer_ = false;
    rhi::DeviceInfo info_;
    std::vector<ComPtr<ID3D11Buffer>> buffers_;
    std::vector<Texture> textures_;
    std::vector<Shader> shaders_;
    std::vector<Sampler> samplers_;
    std::vector<Pipeline> pipelines_;
    rhi::BufferHandle ringHandle_[2];
    std::uint32_t ringPos_[2] = {};
    bool ringMapped_[2] = {};
    int openRing_ = -1;
};

// ---- harness ---------------------------------------------------------------------------------------------------

struct Rgba {
    float r, g, b, a;
};

int ToByte(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<int>(std::lround(v * 255.0f));
}

class Harness {
public:
    Harness() {
        ok_ = device_.Create() && pipeline_.Init(device_, &error_);
        camera_.view = IdentityMatrix();
        camera_.projection = IdentityMatrix();
        camera_.screenProjection = IdentityMatrix();
        camera_.viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        if (ok_) {
            pipeline_.SetCamera(camera_, 64, 64);
            device_.SetViewport(camera_.viewport);
        }
    }
    ~Harness() {
        if (ok_) {
            const std::vector<std::string> problems = device_.TakeDebugProblems();
            for (const std::string& p : problems) {
                FAIL_CHECK("D3D11 debug layer: " << p);
            }
        }
    }
    bool Ok() {
        if (!ok_) {
            MESSAGE("WARP device / pipeline not available: " << error_);
        }
        return ok_;
    }
    void SetCamera(const ni::CameraDesc& camera) {
        camera_ = camera;
        pipeline_.SetCamera(camera_, 64, 64);
        device_.SetViewport(camera_.viewport);
    }
    void Clear(Rgba c = {0, 0, 0, 0}) {
        const float color[4] = {c.r, c.g, c.b, c.a};
        device_.Clear(rhi::kClearColor | rhi::kClearDepth | rhi::kClearStencil, color, 1.0f, 0, nullptr);
    }
    ff::DrawResult Draw(const ni::DrawItem& item) { return pipeline_.Draw(item); }
    WarpDevice::Pixel Read(UINT x, UINT y) { return device_.ReadPixel(x, y); }
    rhi::TextureHandle Texture(UINT w, UINT h, const std::vector<std::uint32_t>& bgra) {
        rhi::TextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = rhi::Format::BGRA8_UNorm;
        rhi::SubresourceData level{bgra.data(), w * 4, w * h * 4};
        return device_.CreateTexture(desc, &level);
    }
    WarpDevice& Device() { return device_; }

private:
    WarpDevice device_;
    ff::FixedFunctionPipeline pipeline_;
    ni::CameraDesc camera_;
    std::string error_;
    bool ok_ = false;
};

void CheckPixel(const WarpDevice::Pixel& p, Rgba expected, int tolerance = 1, bool checkAlpha = true) {
    INFO("pixel (" << p.r << ", " << p.g << ", " << p.b << ", " << p.a << ") expected (" << ToByte(expected.r) << ", "
                   << ToByte(expected.g) << ", " << ToByte(expected.b) << ", " << ToByte(expected.a) << ")");
    CHECK(std::abs(p.r - ToByte(expected.r)) <= tolerance);
    CHECK(std::abs(p.g - ToByte(expected.g)) <= tolerance);
    CHECK(std::abs(p.b - ToByte(expected.b)) <= tolerance);
    if (checkAlpha) {
        CHECK(std::abs(p.a - ToByte(expected.a)) <= tolerance);
    }
}

std::uint32_t Bgra(int r, int g, int b, int a) {
    return static_cast<std::uint32_t>(b) | (static_cast<std::uint32_t>(g) << 8) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(a) << 24);
}

// Full-screen quad (triangle strip, NDC with the identity camera) at depth z; colors/UVs optional.
struct Quad {
    float positions[12];
    float colors[16];
    float uvs[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    float normals[12];
    explicit Quad(float z = 0.5f, Rgba color = {1, 1, 1, 1}, float x0 = -1, float y0 = 1, float x1 = 1, float y1 = -1) {
        const float p[12] = {x0, y0, z, x1, y0, z, x0, y1, z, x1, y1, z};  // TL, TR, BL, BR
        std::memcpy(positions, p, sizeof(p));
        for (int i = 0; i < 4; ++i) {
            colors[i * 4 + 0] = color.r;
            colors[i * 4 + 1] = color.g;
            colors[i * 4 + 2] = color.b;
            colors[i * 4 + 3] = color.a;
            normals[i * 3 + 0] = 0.0f;
            normals[i * 3 + 1] = 0.0f;
            normals[i * 3 + 2] = -1.0f;
        }
    }
    ni::DrawItem Item(bool withColors = true, bool withUvs = false) const {
        ni::DrawItem item;
        item.geometry.vertexCount = 4;
        item.geometry.positions = {positions, 12};
        if (withColors) item.geometry.colors = {colors, 16};
        if (withUvs) {
            item.geometry.uvs[0] = {uvs, 8};
            item.geometry.uvSetCount = 1;
        }
        item.geometry.topology = ni::Topology::TriangleStrip;
        item.state.lightingEnable = false;
        item.state.cullMode = rhi::CullMode::None;
        return item;
    }
};

// ---- CPU reference of the D3D8 texture stage ops (independent of the HLSL) -----------------------------------

struct V4 {
    float v[4];
};
V4 RefArgument(std::uint32_t arg, V4 diffuse, V4 current, V4 texel, V4 factor, V4 temp) {
    V4 r{};
    switch (arg & FF_TA_SELECTMASK) {
        case FF_TA_DIFFUSE: r = diffuse; break;
        case FF_TA_CURRENT: r = current; break;
        case FF_TA_TEXTURE: r = texel; break;
        case FF_TA_TFACTOR: r = factor; break;
        case FF_TA_TEMP: r = temp; break;
        default: r = {{0, 0, 0, 0}}; break;
    }
    if (arg & FF_TA_ALPHAREPLICATE) r = {{r.v[3], r.v[3], r.v[3], r.v[3]}};
    if (arg & FF_TA_COMPLEMENT) {
        for (float& c : r.v) c = 1.0f - c;
    }
    return r;
}
float RefOp(std::uint32_t op, int c, V4 a0, V4 a1, V4 a2, V4 diffuse, V4 current, V4 texel, V4 factor) {
    const float x0 = a0.v[c], x1 = a1.v[c], x2 = a2.v[c];
    switch (op) {
        case FF_TOP_SELECTARG1: return x1;
        case FF_TOP_SELECTARG2: return x2;
        case FF_TOP_MODULATE: return x1 * x2;
        case FF_TOP_MODULATE2X: return 2 * x1 * x2;
        case FF_TOP_MODULATE4X: return 4 * x1 * x2;
        case FF_TOP_ADD: return x1 + x2;
        case FF_TOP_ADDSIGNED: return x1 + x2 - 0.5f;
        case FF_TOP_ADDSIGNED2X: return 2 * (x1 + x2 - 0.5f);
        case FF_TOP_SUBTRACT: return x1 - x2;
        case FF_TOP_ADDSMOOTH: return x1 + x2 - x1 * x2;
        case FF_TOP_BLENDDIFFUSEALPHA: return x1 * diffuse.v[3] + x2 * (1 - diffuse.v[3]);
        case FF_TOP_BLENDTEXTUREALPHA: return x1 * texel.v[3] + x2 * (1 - texel.v[3]);
        case FF_TOP_BLENDFACTORALPHA: return x1 * factor.v[3] + x2 * (1 - factor.v[3]);
        case FF_TOP_BLENDTEXTUREALPHAPM: return x1 + x2 * (1 - texel.v[3]);
        case FF_TOP_BLENDCURRENTALPHA: return x1 * current.v[3] + x2 * (1 - current.v[3]);
        case FF_TOP_MODULATEALPHA_ADDCOLOR: return c < 3 ? x1 + a1.v[3] * x2 : a1.v[3];
        case FF_TOP_MODULATECOLOR_ADDALPHA: return c < 3 ? x1 * x2 + a1.v[3] : a1.v[3];
        case FF_TOP_MODULATEINVALPHA_ADDCOLOR: return c < 3 ? (1 - a1.v[3]) * x2 + x1 : a1.v[3];
        case FF_TOP_MODULATEINVCOLOR_ADDALPHA: return c < 3 ? (1 - x1) * x2 + a1.v[3] : a1.v[3];
        case FF_TOP_DOTPRODUCT3: {
            float d = 0;
            for (int i = 0; i < 3; ++i) d += (a1.v[i] - 0.5f) * (a2.v[i] - 0.5f);
            return 4 * d;
        }
        case FF_TOP_MULTIPLYADD: return x0 + x1 * x2;
        case FF_TOP_LERP: return x0 * x1 + (1 - x0) * x2;
        default: return current.v[c];
    }
}
float Sat(float v) {
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

// One or two stages on the full-screen quad; texel = the 2x2 test texture of the sampled quadrant.
Rgba RefStages(const ni::RenderStateDesc& state, V4 diffuse, const V4 texels[4]) {
    V4 current = diffuse, temp{{0, 0, 0, 0}};
    const V4 factor{{state.textureFactor.x, state.textureFactor.y, state.textureFactor.z, state.textureFactor.w}};
    for (int s = 0; s < 4; ++s) {
        const ni::TextureStageDesc& st = state.stages[s];
        if (st.colorOp == FF_TOP_DISABLE) break;
        const V4 t = texels[s];
        V4 c0 = RefArgument(st.colorArg0, diffuse, current, t, factor, temp);
        V4 c1 = RefArgument(st.colorArg1, diffuse, current, t, factor, temp);
        V4 c2 = RefArgument(st.colorArg2, diffuse, current, t, factor, temp);
        V4 a0 = RefArgument(st.alphaArg0, diffuse, current, t, factor, temp);
        V4 a1 = RefArgument(st.alphaArg1, diffuse, current, t, factor, temp);
        V4 a2 = RefArgument(st.alphaArg2, diffuse, current, t, factor, temp);
        V4 result{};
        for (int c = 0; c < 3; ++c) result.v[c] = Sat(RefOp(st.colorOp, c, c0, c1, c2, diffuse, current, t, factor));
        if (st.colorOp == FF_TOP_DOTPRODUCT3) {
            result.v[3] = result.v[0];
        } else if (st.alphaOp == FF_TOP_DISABLE) {
            result.v[3] = current.v[3];
        } else {
            result.v[3] = Sat(RefOp(st.alphaOp, 3, a0, a1, a2, diffuse, current, t, factor));
        }
        if (st.resultArg == FF_TA_TEMP) temp = result;
        else current = result;
    }
    return {current.v[0], current.v[1], current.v[2], current.v[3]};
}

void SetStage(ni::TextureStageDesc& s, std::uint8_t colorOp, std::uint8_t c1, std::uint8_t c2, std::uint8_t alphaOp,
              std::uint8_t a1, std::uint8_t a2) {
    s.colorOp = colorOp;
    s.colorArg1 = c1;
    s.colorArg2 = c2;
    s.alphaOp = alphaOp;
    s.alphaArg1 = a1;
    s.alphaArg2 = a2;
    s.minFilter = s.magFilter = rhi::Filter::Point;
}

}  // namespace

TEST_CASE("ff render: vertex color and the white default diffuse") {
    Harness h;
    if (!h.Ok()) return;
    Quad quad(0.5f, {1.0f, 0.25f, 0.0f, 0.5f});
    h.Clear();
    REQUIRE(h.Draw(quad.Item(true)) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(10, 50), {1.0f, 0.25f, 0.0f, 0.5f});
    h.Clear();
    REQUIRE(h.Draw(quad.Item(false)) == ff::DrawResult::Drawn);  // D3D8: no diffuse in the vertex -> white
    CheckPixel(h.Read(10, 50), {1, 1, 1, 1});
    // Colors outside [0, 1] saturate (the original's ROUND(c * 255) would wrap: documented deviation).
    Quad bright(0.5f, {2.0f, -1.0f, 0.5f, 1.0f});
    h.Clear();
    REQUIRE(h.Draw(bright.Item(true)) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(32, 32), {1.0f, 0.0f, 0.5f, 1.0f});
}

TEST_CASE("ff render: texture stage ops against the CPU reference") {
    Harness h;
    if (!h.Ok()) return;
    // 2x2 texture: TL red opaque, TR green alpha 128, BL blue alpha 64, BR gray alpha 255.
    const std::vector<std::uint32_t> texels = {Bgra(255, 0, 0, 255), Bgra(0, 255, 0, 128), Bgra(0, 0, 255, 64),
                                               Bgra(128, 128, 128, 255)};
    const rhi::TextureHandle texture = h.Texture(2, 2, texels);
    REQUIRE(texture.id != 0);
    const V4 texelValues[4] = {{{1, 0, 0, 1}}, {{0, 1, 0, 128 / 255.0f}}, {{0, 0, 1, 64 / 255.0f}},
                               {{128 / 255.0f, 128 / 255.0f, 128 / 255.0f, 1}}};
    const Rgba diffuse{0.5f, 0.25f, 0.75f, 0.5f};
    Quad quad(0.5f, diffuse);
    const UINT sampleX[4] = {16, 48, 16, 48};
    const UINT sampleY[4] = {16, 16, 48, 48};

    struct Case {
        const char* name;
        std::uint8_t colorOp, c1, c2, alphaOp, a1, a2;
    };
    const Case cases[] = {
        {"SELECTARG1 texture (REPLACE)", FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"SELECTARG2 diffuse (no texture)", FF_TOP_SELECTARG2, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG2, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"MODULATE (base map)", FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"MODULATE2X", FF_TOP_MODULATE2X, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"MODULATE4X", FF_TOP_MODULATE4X, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG2, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"ADD", FF_TOP_ADD, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_ADD, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"ADDSIGNED", FF_TOP_ADDSIGNED, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_ADDSIGNED, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"ADDSIGNED2X", FF_TOP_ADDSIGNED2X, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"SUBTRACT", FF_TOP_SUBTRACT, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SUBTRACT, FF_TA_DIFFUSE, FF_TA_TEXTURE},
        {"ADDSMOOTH", FF_TOP_ADDSMOOTH, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_DIFFUSE, FF_TA_TEXTURE},
        {"BLENDTEXTUREALPHA (decal)", FF_TOP_BLENDTEXTUREALPHA, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG2, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"BLENDDIFFUSEALPHA", FF_TOP_BLENDDIFFUSEALPHA, FF_TA_TEXTURE, FF_TA_TFACTOR, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"BLENDFACTORALPHA", FF_TOP_BLENDFACTORALPHA, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_BLENDFACTORALPHA, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"BLENDCURRENTALPHA", FF_TOP_BLENDCURRENTALPHA, FF_TA_TEXTURE, FF_TA_TFACTOR, FF_TOP_SELECTARG1, FF_TA_CURRENT, FF_TA_DIFFUSE},
        {"BLENDTEXTUREALPHAPM", FF_TOP_BLENDTEXTUREALPHAPM, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"MODULATEALPHA_ADDCOLOR", FF_TOP_MODULATEALPHA_ADDCOLOR, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"MODULATECOLOR_ADDALPHA", FF_TOP_MODULATECOLOR_ADDALPHA, FF_TA_DIFFUSE, FF_TA_TEXTURE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"MODULATEINVALPHA_ADDCOLOR", FF_TOP_MODULATEINVALPHA_ADDCOLOR, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"MODULATEINVCOLOR_ADDALPHA", FF_TOP_MODULATEINVCOLOR_ADDALPHA, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"DOTPRODUCT3", FF_TOP_DOTPRODUCT3, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"TFACTOR + COMPLEMENT", FF_TOP_SELECTARG1, FF_TA_TFACTOR | FF_TA_COMPLEMENT, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TFACTOR, FF_TA_DIFFUSE},
        {"ALPHAREPLICATE texture", FF_TOP_MODULATE, FF_TA_TEXTURE | FF_TA_ALPHAREPLICATE, FF_TA_DIFFUSE, FF_TOP_SELECTARG2, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"alpha op DISABLE keeps the diffuse alpha", FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_DISABLE, FF_TA_TEXTURE, FF_TA_DIFFUSE},
        {"stage 0 DISABLE = diffuse", FF_TOP_DISABLE, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_DISABLE, FF_TA_TEXTURE, FF_TA_DIFFUSE},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        ni::DrawItem item = quad.Item(true, true);
        SetStage(item.state.stages[0], c.colorOp, c.c1, c.c2, c.alphaOp, c.a1, c.a2);
        item.state.stages[0].texture = texture;
        item.state.textureFactor = {0.2f, 0.6f, 1.0f, 0.25f};
        h.Clear();
        REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
        for (int q = 0; q < 4; ++q) {
            CAPTURE(q);
            const V4 stageTexels[4] = {texelValues[q], {{1, 1, 1, 1}}, {{1, 1, 1, 1}}, {{1, 1, 1, 1}}};
            CheckPixel(h.Read(sampleX[q], sampleY[q]),
                       RefStages(item.state, {{diffuse.r, diffuse.g, diffuse.b, diffuse.a}}, stageTexels));
        }
    }

    // Two stages: stage 0 -> TEMP (RESULTARG), stage 1 = TEMP + diffuse; then a MULTIPLYADD/LERP chain.
    {
        ni::DrawItem item = quad.Item(true, true);
        SetStage(item.state.stages[0], FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE);
        item.state.stages[0].resultArg = FF_TA_TEMP;
        item.state.stages[0].texture = texture;
        SetStage(item.state.stages[1], FF_TOP_ADD, FF_TA_TEMP, FF_TA_CURRENT, FF_TOP_SELECTARG1, FF_TA_TEMP, FF_TA_CURRENT);
        h.Clear();
        REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
        for (int q = 0; q < 4; ++q) {
            const V4 stageTexels[4] = {texelValues[q], {{1, 1, 1, 1}}, {{1, 1, 1, 1}}, {{1, 1, 1, 1}}};
            CheckPixel(h.Read(sampleX[q], sampleY[q]),
                       RefStages(item.state, {{diffuse.r, diffuse.g, diffuse.b, diffuse.a}}, stageTexels));
        }
        ni::DrawItem lerp = quad.Item(true, true);
        SetStage(lerp.state.stages[0], FF_TOP_MULTIPLYADD, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_MULTIPLYADD, FF_TA_TEXTURE, FF_TA_DIFFUSE);
        lerp.state.stages[0].colorArg0 = FF_TA_TFACTOR;
        lerp.state.stages[0].alphaArg0 = FF_TA_TFACTOR;
        lerp.state.stages[0].texture = texture;
        SetStage(lerp.state.stages[1], FF_TOP_LERP, FF_TA_DIFFUSE, FF_TA_TFACTOR, FF_TOP_LERP, FF_TA_DIFFUSE, FF_TA_TFACTOR);
        lerp.state.stages[1].colorArg0 = FF_TA_CURRENT;
        lerp.state.stages[1].alphaArg0 = FF_TA_CURRENT;
        lerp.state.textureFactor = {0.1f, 0.2f, 0.3f, 0.4f};
        h.Clear();
        REQUIRE(h.Draw(lerp) == ff::DrawResult::Drawn);
        for (int q = 0; q < 4; ++q) {
            const V4 stageTexels[4] = {texelValues[q], {{1, 1, 1, 1}}, {{1, 1, 1, 1}}, {{1, 1, 1, 1}}};
            CheckPixel(h.Read(sampleX[q], sampleY[q]),
                       RefStages(lerp.state, {{diffuse.r, diffuse.g, diffuse.b, diffuse.a}}, stageTexels));
        }
    }
}

TEST_CASE("ff render: texture coordinates (set index, texture transform, camera-space generation)") {
    Harness h;
    if (!h.Ok()) return;
    const std::vector<std::uint32_t> texels = {Bgra(255, 0, 0, 255), Bgra(0, 255, 0, 255), Bgra(0, 0, 255, 255),
                                               Bgra(255, 255, 255, 255)};
    const rhi::TextureHandle texture = h.Texture(2, 2, texels);
    Quad quad;
    const float flippedUvs[8] = {1, 0, 0, 0, 1, 1, 0, 1};  // u mirrored
    // UV set 1 (TEXCOORDINDEX 1).
    ni::DrawItem item = quad.Item(false, true);
    item.geometry.uvs[1] = {flippedUvs, 8};
    item.geometry.uvSetCount = 2;
    SetStage(item.state.stages[0], FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_SELECTARG1, FF_TA_TEXTURE, FF_TA_DIFFUSE);
    item.state.stages[0].texture = texture;
    item.state.stages[0].texCoordIndex = 1;
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(16, 16), {0, 1, 0, 1});  // left half shows the right column
    CheckPixel(h.Read(48, 48), {0, 0, 1, 1});
    // Texture transform COUNT2 on (u, v, 1, 0): u' = 0.5 u + 0.5 (translation in the third row).
    item.state.stages[0].texCoordIndex = 0;
    item.state.stages[0].transformFlags = FF_TTFF_COUNT2;
    item.state.stages[0].transform = IdentityMatrix();
    item.state.stages[0].transform.m[0][0] = 0.5f;
    item.state.stages[0].transform.m[2][0] = 0.5f;
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(4, 16), {0, 1, 0, 1});
    CheckPixel(h.Read(4, 48), {1, 1, 1, 1});
    // COUNT3 | PROJECTED: (u, v) / q with q = 2 (third column of the third row) -> the top-left texel only.
    item.state.stages[0].transformFlags = FF_TTFF_COUNT3 | FF_TTFF_PROJECTED;
    item.state.stages[0].transform = IdentityMatrix();
    item.state.stages[0].transform.m[2][2] = 2.0f;
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(60, 60), {1, 0, 0, 1});
    // CAMERASPACEPOSITION: (x, y, z, 1) * [0.5 x + 0.5, -0.5 y + 0.5] -> the same mapping as the UVs.
    item.state.stages[0].texCoordIndex = FF_TCI_CAMERASPACEPOSITION;
    item.state.stages[0].transformFlags = FF_TTFF_COUNT2;
    item.state.stages[0].transform = {};
    item.state.stages[0].transform.m[0][0] = 0.5f;
    item.state.stages[0].transform.m[1][1] = -0.5f;
    item.state.stages[0].transform.m[3][0] = 0.5f;
    item.state.stages[0].transform.m[3][1] = 0.5f;
    item.state.stages[0].transform.m[3][3] = 1.0f;
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(16, 16), {1, 0, 0, 1});
    CheckPixel(h.Read(48, 16), {0, 1, 0, 1});
    CheckPixel(h.Read(16, 48), {0, 0, 1, 1});
}

// ---- lighting --------------------------------------------------------------------------------------------------

struct RefVertexResult {
    Float4 diffuse;
    Float3 specular;
};

// D3D8 lighting equation (camera space) - the reference the shader is checked against.
RefVertexResult RefLight(const ni::DrawItem& item, const Matrix4& view, Float3 position, Float3 normal,
                         const Float4* vertexColor) {
    const ni::RenderStateDesc& s = item.state;
    const Matrix4 worldView = Multiply(item.world, view);
    const Float3 P = TransformPoint(position, worldView);
    Matrix4 inverse;
    Inverse(worldView, &inverse);
    Float3 N = TransformVector(normal, Transpose(inverse));
    if (s.normalizeNormals) N = Normalize(N);
    auto source = [&](std::uint8_t src, Float4 material) {
        return src == FF_MCS_COLOR1 && vertexColor != nullptr ? *vertexColor : material;
    };
    const Float4 md = source(s.diffuseSource, s.material.diffuse);
    const Float4 ma = source(s.ambientSource, s.material.ambient);
    const Float4 ms = source(s.specularSource, s.material.specular);
    const Float4 me = source(s.emissiveSource, s.material.emissive);
    const Float3 V = Normalize({-P.x, -P.y, -P.z});
    Float3 amb{}, dif{}, spe{};
    for (std::uint32_t i = 0; i < item.lights.count; ++i) {
        const ni::LightDesc& l = item.lights.lights[i];
        Float3 L;
        float k = 1.0f;
        const Float3 lightDir = Normalize(TransformVector(l.direction, view));
        if (l.type == FF_LIGHT_DIRECTIONAL) {
            L = {-lightDir.x, -lightDir.y, -lightDir.z};
        } else {
            const Float3 lp = TransformPoint(l.position, view);
            const Float3 d{lp.x - P.x, lp.y - P.y, lp.z - P.z};
            const float dist = std::sqrt(Dot(d, d));
            if (dist > l.range) continue;
            L = Normalize(d);
            k = 1.0f / (l.attenuation0 + l.attenuation1 * dist + l.attenuation2 * dist * dist);
            if (l.type == FF_LIGHT_SPOT) {
                const float rho = -Dot(L, lightDir);
                const float ct = std::cos(l.theta / 2), cp = std::cos(l.phi / 2);
                k *= rho > ct ? 1.0f : (rho <= cp ? 0.0f : std::pow((rho - cp) / (ct - cp), l.falloff));
            }
        }
        amb = {amb.x + k * l.ambient.x, amb.y + k * l.ambient.y, amb.z + k * l.ambient.z};
        const float ndl = Dot(N, L);
        if (ndl > 0) {
            dif = {dif.x + k * ndl * l.diffuse.x, dif.y + k * ndl * l.diffuse.y, dif.z + k * ndl * l.diffuse.z};
            if (s.specularEnable) {
                const Float3 H = Normalize({V.x + L.x, V.y + L.y, V.z + L.z});
                const float ndh = Dot(N, H);
                const float f = ndh > 0 ? std::pow(ndh, s.material.power) : 0.0f;
                spe = {spe.x + k * f * l.specular.x, spe.y + k * f * l.specular.y, spe.z + k * f * l.specular.z};
            }
        }
    }
    const Float4& g = item.lights.ambient;
    RefVertexResult r;
    r.diffuse = {Sat(me.x + ma.x * (g.x + amb.x) + md.x * dif.x), Sat(me.y + ma.y * (g.y + amb.y) + md.y * dif.y),
                 Sat(me.z + ma.z * (g.z + amb.z) + md.z * dif.z), Sat(md.w)};
    r.specular = {Sat(ms.x * spe.x), Sat(ms.y * spe.y), Sat(ms.z * spe.z)};
    return r;
}

// Flat-shaded screen-covering triangle: the pixel color is the lighting of vertex 0 (provoking vertex).
struct LitTriangle {
    float positions[9] = {-0.8f, -0.6f, 0.5f, -0.8f, 3.0f, 0.5f, 3.0f, -0.6f, 0.5f};
    float normals[9] = {0.2f, 0.1f, -1.0f, 0, 0, -1, 0, 0, -1};
    float colors[12] = {0.9f, 0.6f, 0.3f, 0.7f, 0, 0, 0, 1, 0, 0, 0, 1};
    ni::DrawItem Item(bool withColors) const {
        ni::DrawItem item;
        item.geometry.vertexCount = 3;
        item.geometry.positions = {positions, 12};
        item.geometry.normals = {normals, 12};
        if (withColors) item.geometry.colors = {colors, 16};
        item.state.cullMode = rhi::CullMode::None;
        item.state.shadeMode = ni::ShadeMode::Flat;
        item.state.lightingEnable = true;
        item.state.material.diffuse = {0.8f, 0.7f, 0.6f, 0.9f};
        item.state.material.ambient = {0.3f, 0.4f, 0.5f, 1.0f};
        item.state.material.specular = {0.5f, 0.6f, 0.7f, 1.0f};
        item.state.material.emissive = {0.05f, 0.1f, 0.0f, 1.0f};
        item.state.material.power = 6.0f;
        item.lights.ambient = {0.1f, 0.2f, 0.15f, 1.0f};
        return item;
    }
};

void CheckLit(Harness& h, const ni::DrawItem& item, const LitTriangle& tri, bool withColors) {
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    const Float4 color{tri.colors[0], tri.colors[1], tri.colors[2], tri.colors[3]};
    const RefVertexResult r = RefLight(item, IdentityMatrix(), {tri.positions[0], tri.positions[1], tri.positions[2]},
                                       {tri.normals[0], tri.normals[1], tri.normals[2]}, withColors ? &color : nullptr);
    Rgba expected{r.diffuse.x, r.diffuse.y, r.diffuse.z, r.diffuse.w};
    if (item.state.specularEnable) {
        expected = {Sat(expected.r + r.specular.x), Sat(expected.g + r.specular.y), Sat(expected.b + r.specular.z),
                    expected.a};
    }
    CheckPixel(h.Read(32, 32), expected, 2);
    CheckPixel(h.Read(40, 36), expected, 2);  // flat: the same everywhere
}

TEST_CASE("ff render: D3D8 per-vertex lighting against the CPU reference") {
    Harness h;
    if (!h.Ok()) return;
    LitTriangle tri;

    SUBCASE("no lights: emissive + material ambient * global ambient") {
        ni::DrawItem item = tri.Item(false);
        CheckLit(h, item, tri, false);
    }
    SUBCASE("directional light, material sources") {
        ni::DrawItem item = tri.Item(true);
        item.lights.count = 1;
        ni::LightDesc& l = item.lights.lights[0];
        l.type = FF_LIGHT_DIRECTIONAL;
        l.direction = {0.3f, -0.2f, 1.0f};
        l.diffuse = {0.9f, 0.8f, 0.7f, 1.0f};
        l.ambient = {0.1f, 0.05f, 0.0f, 1.0f};
        l.specular = {1, 1, 1, 1};
        CheckLit(h, item, tri, true);           // all sources MATERIAL (vertex color ignored)
        item.state.diffuseSource = FF_MCS_COLOR1;  // E_A_D + AMB_DIFF (FUN_0015BF80)
        item.state.ambientSource = FF_MCS_COLOR1;
        CheckLit(h, item, tri, true);
        item.state.emissiveSource = FF_MCS_COLOR1;  // SRC_EMISSIVE
        item.state.diffuseSource = item.state.ambientSource = FF_MCS_MATERIAL;
        CheckLit(h, item, tri, true);
        // COLOR1 without vertex colors falls back to the material.
        ni::DrawItem noColor = tri.Item(false);
        noColor.lights = item.lights;
        noColor.state.diffuseSource = FF_MCS_COLOR1;
        CheckLit(h, noColor, tri, false);
        // Specular (local viewer) and normalized normals.
        item.state.specularEnable = true;
        item.state.normalizeNormals = true;
        CheckLit(h, item, tri, true);
    }
    SUBCASE("point light: attenuation and range") {
        ni::DrawItem item = tri.Item(true);
        item.lights.count = 1;
        ni::LightDesc& l = item.lights.lights[0];
        l.type = FF_LIGHT_POINT;
        l.position = {-0.5f, -0.4f, -0.2f};
        l.diffuse = {1.0f, 0.9f, 0.8f, 1.0f};
        l.specular = {0.5f, 0.5f, 0.5f, 1.0f};
        l.ambient = {0.2f, 0.2f, 0.2f, 1.0f};
        l.attenuation0 = 0.5f;
        l.attenuation1 = 0.4f;
        l.attenuation2 = 0.3f;
        l.range = 1.0e19f;  // 0x5F7FFFFF (FUN_0015B6A0)
        item.state.specularEnable = true;
        item.state.normalizeNormals = true;
        CheckLit(h, item, tri, true);
        l.range = 0.5f;  // vertex 0 is ~0.79 away: outside the range
        CheckLit(h, item, tri, true);
    }
    SUBCASE("spot light cone and falloff") {
        ni::DrawItem item = tri.Item(false);
        item.lights.count = 2;
        ni::LightDesc& l = item.lights.lights[0];
        l.type = FF_LIGHT_SPOT;
        l.position = {-0.3f, -0.6f, 0.0f};
        l.direction = {0.0f, 0.0f, 1.0f};
        l.diffuse = {0.9f, 0.9f, 0.9f, 1.0f};
        l.attenuation0 = 1.0f;
        l.range = 1.0e19f;
        l.theta = 1.0f;   // cos(0.5) = 0.878
        l.phi = 2.4f;     // cos(1.2) = 0.362; rho at vertex 0 = 0.707 -> partial
        l.falloff = 2.0f;
        item.lights.lights[1] = l;
        item.lights.lights[1].position = {-0.8f, -0.6f, 0.0f};  // on the axis: full
        item.lights.lights[1].diffuse = {0.1f, 0.0f, 0.2f, 1.0f};
        CheckLit(h, item, tri, false);
        // theta == phi (FUN_0015B6A0 sets both): hard edge, vertex 0 outside the 1.0 rad cone.
        l.phi = l.theta;
        CheckLit(h, item, tri, false);
    }
    SUBCASE("non-uniform world scale with and without NORMALIZENORMALS") {
        ni::DrawItem item = tri.Item(false);
        item.world = IdentityMatrix();
        item.world.m[0][0] = 0.5f;
        item.world.m[2][2] = 2.0f;
        for (int i = 0; i < 3; ++i) tri.positions[i * 3 + 2] = 0.25f;  // world z 0.25 -> view z 0.5
        item.lights.count = 1;
        item.lights.lights[0].type = FF_LIGHT_DIRECTIONAL;
        item.lights.lights[0].direction = {0.5f, 0.0f, 1.0f};
        item.lights.lights[0].diffuse = {0.4f, 0.4f, 0.4f, 1.0f};
        CheckLit(h, item, tri, false);
        item.state.normalizeNormals = true;
        CheckLit(h, item, tri, false);
    }
}

TEST_CASE("ff render: smooth vs flat shading") {
    Harness h;
    if (!h.Ok()) return;
    const float positions[9] = {-1, -1, 0.5f, -1, 3, 0.5f, 3, -1, 0.5f};
    const float colors[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    ni::DrawItem item;
    item.geometry.vertexCount = 3;
    item.geometry.positions = {positions, 12};
    item.geometry.colors = {colors, 16};
    item.state.lightingEnable = false;
    item.state.cullMode = rhi::CullMode::None;
    item.state.shadeMode = ni::ShadeMode::Flat;
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(2, 61), {1, 0, 0, 1});
    CheckPixel(h.Read(60, 4), {1, 0, 0, 1});
    item.state.shadeMode = ni::ShadeMode::Gouraud;
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    const WarpDevice::Pixel p = h.Read(60, 4);
    CHECK(p.r < 128);
    CHECK(p.g > 64);
    CHECK(p.b > 64);
}

TEST_CASE("ff render: alpha test, alpha blending and specular add") {
    Harness h;
    if (!h.Ok()) return;
    Quad left(0.5f, {1, 0, 0, 0.25f}, -1, 1, 0, -1);
    Quad right(0.5f, {0, 1, 0, 0.75f}, 0, 1, 1, -1);
    h.Clear({0, 0, 1, 1});
    for (const Quad* q : {&left, &right}) {
        ni::DrawItem item = q->Item();
        item.state.alphaTestEnable = true;
        item.state.alphaFunc = rhi::CompareFunc::Greater;
        item.state.alphaRef = 0x80;
        REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    }
    CheckPixel(h.Read(16, 32), {0, 0, 1, 1});        // 64 > 128 fails: discarded
    CheckPixel(h.Read(48, 32), {0, 1, 0, 0.75f});    // 191 > 128 passes
    // Edge: alpha 128 with GREATEREQUAL 128 passes, with LESS fails.
    Quad half(0.5f, {1, 1, 0, 128 / 255.0f});
    h.Clear({0, 0, 1, 1});
    ni::DrawItem item = half.Item();
    item.state.alphaTestEnable = true;
    item.state.alphaFunc = rhi::CompareFunc::Less;
    item.state.alphaRef = 128;
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(32, 32), {0, 0, 1, 1});
    item.state.alphaFunc = rhi::CompareFunc::GreaterEqual;
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(32, 32), {1, 1, 0, 128 / 255.0f});

    // SRCALPHA / INVSRCALPHA over blue (FUN_00159270 table entries 6/7).
    Quad red(0.5f, {1, 0, 0, 0.25f});
    h.Clear({0, 0, 1, 1});
    item = red.Item();
    item.state.alphaBlendEnable = true;
    item.state.srcBlend = rhi::BlendFactor::SrcAlpha;
    item.state.dstBlend = rhi::BlendFactor::InvSrcAlpha;
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(32, 32), {0.25f, 0.0f, 0.75f, 0.25f * 0.25f + 0.75f}, 2);
    // DESTCOLOR / ZERO (multiplicative) uses the alpha equivalents on the alpha channel.
    h.Clear({0.5f, 0.5f, 0.5f, 0.5f});
    Quad mul(0.5f, {0.5f, 1.0f, 0.0f, 1.0f});
    item = mul.Item();
    item.state.alphaBlendEnable = true;
    item.state.srcBlend = rhi::BlendFactor::DestColor;
    item.state.dstBlend = rhi::BlendFactor::Zero;
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(32, 32), {0.25f, 0.5f, 0.0f, 0.5f}, 2);
}

TEST_CASE("ff render: table fog (linear, exp, exp2, range)") {
    Harness h;
    if (!h.Ok()) return;
    Quad quad(0.5f, {1, 0, 0, 1});
    struct Case {
        std::uint8_t mode;
        bool range;
        float start, end, density, expectedF;
    };
    const float rangeDepth = std::sqrt(2.0f + 0.25f);  // all four corners (+-1, +-1, 0.5)
    const Case cases[] = {
        {FF_FOG_LINEAR, false, 0.0f, 1.0f, 1.0f, 0.5f},
        {FF_FOG_LINEAR, false, 0.25f, 2.25f, 1.0f, 0.875f},
        {FF_FOG_EXP, false, 0.0f, 1.0f, 2.0f, std::exp(-1.0f)},
        {FF_FOG_EXP2, false, 0.0f, 1.0f, 1.5f, std::exp(-0.5625f)},
        {FF_FOG_LINEAR, true, 0.0f, 3.0f, 1.0f, 1.0f - rangeDepth / 3.0f},
        {FF_FOG_NONE, false, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    for (const Case& c : cases) {
        CAPTURE(static_cast<int>(c.mode));
        CAPTURE(c.range);
        ni::DrawItem item = quad.Item();
        item.state.fogEnable = true;
        item.state.fogTableMode = c.mode;
        item.state.rangeFogEnable = c.range;
        item.state.fogStart = c.start;
        item.state.fogEnd = c.end;
        item.state.fogDensity = c.density;
        item.state.fogColor = {0.0f, 0.5f, 1.0f};
        h.Clear();
        REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
        const float f = c.expectedF;
        CheckPixel(h.Read(32, 32), {f, 0.5f * (1 - f), 1 - f, 1}, 2);
    }
}

TEST_CASE("ff render: pre-transformed D3D8 pixels map exactly (slot 55 quads)") {
    Harness h;
    if (!h.Ok()) return;
    // 16x8 texture with a unique color per texel, drawn 1:1 at pixels [8, 24) x [4, 12) like FUN_0014A050
    // (x - 0.5, y - 0.5); linear filtering must still return the exact texels.
    std::vector<std::uint32_t> texels(16 * 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) texels[y * 16 + x] = Bgra(x * 16, y * 32, 255 - x * 8, 255);
    }
    const rhi::TextureHandle texture = h.Texture(16, 8, texels);
    const float positions[12] = {7.5f, 3.5f, 0.0f, 23.5f, 3.5f, 0.0f, 7.5f, 11.5f, 0.0f, 23.5f, 11.5f, 0.0f};
    const float uvs[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    const float colors[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    ni::DrawItem item;
    item.kind = ni::DrawKind::ScreenTexture;
    item.geometry.vertexCount = 4;
    item.geometry.positions = {positions, 12};
    item.geometry.colors = {colors, 16};
    item.geometry.uvs[0] = {uvs, 8};
    item.geometry.uvSetCount = 1;
    item.geometry.indices = indices;
    item.geometry.indexCount = 6;
    item.geometry.pretransformed = true;
    item.state.lightingEnable = true;  // ignored for XYZRHW
    item.state.zFunc = rhi::CompareFunc::Always;
    item.state.zWriteEnable = false;
    item.state.alphaBlendEnable = true;
    item.state.cullMode = rhi::CullMode::None;
    SetStage(item.state.stages[0], FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE);
    item.state.stages[0].texture = texture;
    item.state.stages[0].minFilter = item.state.stages[0].magFilter = rhi::Filter::Linear;
    item.state.stages[0].addressU = item.state.stages[0].addressV = rhi::AddressMode::Clamp;
    h.Clear({0, 0, 0, 0});
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            CAPTURE(x);
            CAPTURE(y);
            CheckPixel(h.Read(8 + x, 4 + y), {x * 16 / 255.0f, y * 32 / 255.0f, (255 - x * 8) / 255.0f, 1.0f});
        }
    }
    for (UINT x : {7u, 24u}) CheckPixel(h.Read(x, 6), {0, 0, 0, 0});
    for (UINT y : {3u, 12u}) CheckPixel(h.Read(10, y), {0, 0, 0, 0});

    // Sub-viewport: pixels stay render-target coordinates (the viewport only clips).
    ni::CameraDesc camera;
    camera.view = camera.projection = camera.screenProjection = IdentityMatrix();
    camera.viewport = {4.0f, 2.0f, 40.0f, 30.0f, 0.0f, 1.0f};
    h.SetCamera(camera);
    h.Clear({0, 0, 0, 0});
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(8, 4), {0, 0, 1, 1});
    CheckPixel(h.Read(23, 11), {240 / 255.0f, 224 / 255.0f, 135 / 255.0f, 1});
}

TEST_CASE("ff render: screen camera of RenderScreenPolygon (slot 54)") {
    Harness h;
    if (!h.Ok()) return;
    // Projection of SetCameraData with near 0.1 / far 1 -> this+0x394 (y up, x/y in [0, 1]).
    const float n = 0.1f, f = 1.0f;
    ni::CameraDesc camera;
    camera.view = camera.projection = IdentityMatrix();
    camera.screenProjection = {};
    camera.screenProjection.m[0][0] = camera.screenProjection.m[1][1] = 2.0f;
    camera.screenProjection.m[2][0] = camera.screenProjection.m[2][1] = -1.0f;
    camera.screenProjection.m[2][2] = f / (f - n);
    camera.screenProjection.m[2][3] = 1.0f;
    camera.screenProjection.m[3][2] = -n * f / (f - n);
    camera.viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
    h.SetCamera(camera);
    // Quad x, y in [0, 0.5] at z = 0.5 -> d = near + z (far - near) = 0.55; positions (d x, d y, d).
    const float d = n + 0.5f * (f - n);
    const float positions[12] = {0, 0, d, 0.5f * d, 0, d, 0, 0.5f * d, d, 0.5f * d, 0.5f * d, d};
    const float colors[16] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    ni::DrawItem item;
    item.kind = ni::DrawKind::ScreenPolygon;
    item.useScreenCamera = true;
    item.geometry.vertexCount = 4;
    item.geometry.positions = {positions, 12};
    item.geometry.colors = {colors, 16};
    item.geometry.topology = ni::Topology::TriangleStrip;
    item.state.lightingEnable = false;
    item.state.cullMode = rhi::CullMode::None;
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(8, 56), {0, 1, 0, 1});   // bottom-left quadrant (y up)
    CheckPixel(h.Read(8, 8), {0, 0, 0, 0});
    CheckPixel(h.Read(56, 56), {0, 0, 0, 0});
}

TEST_CASE("ff render: D3D8 cull modes, strips with cuts, depth test") {
    Harness h;
    if (!h.Ok()) return;
    // Screen-clockwise triangle (bottom-left, top-left, bottom-right).
    const float cw[9] = {-1, -1, 0.5f, -1, 1, 0.5f, 1, -1, 0.5f};
    ni::DrawItem item;
    item.geometry.vertexCount = 3;
    item.geometry.positions = {cw, 12};
    item.state.lightingEnable = false;
    struct Case {
        rhi::CullMode mode;
        bool drawn;
    };
    for (const Case c : {Case{rhi::CullMode::CW, false}, Case{rhi::CullMode::CCW, true}, Case{rhi::CullMode::None, true}}) {
        CAPTURE(static_cast<int>(c.mode));
        item.state.cullMode = c.mode;
        h.Clear();
        REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
        CheckPixel(h.Read(8, 56), c.drawn ? Rgba{1, 1, 1, 1} : Rgba{0, 0, 0, 0});
    }

    // Two strips joined by a 0xFFFF cut (ni_extract slot 51): left and right bands, gap in between.
    const float positions[24] = {-1, 1, 0.5f, -0.5f, 1, 0.5f, -1, -1, 0.5f, -0.5f, -1, 0.5f,
                                 0.5f, 1, 0.5f, 1, 1, 0.5f, 0.5f, -1, 0.5f, 1, -1, 0.5f};
    const std::uint16_t indices[9] = {0, 1, 2, 3, rhi::kStripCut, 4, 5, 6, 7};
    ni::DrawItem strips;
    strips.geometry.vertexCount = 8;
    strips.geometry.positions = {positions, 12};
    strips.geometry.indices = indices;
    strips.geometry.indexCount = 9;
    strips.geometry.topology = ni::Topology::TriangleStrip;
    strips.state.lightingEnable = false;
    strips.state.cullMode = rhi::CullMode::None;
    h.Clear();
    REQUIRE(h.Draw(strips) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(4, 32), {1, 1, 1, 1});
    CheckPixel(h.Read(60, 32), {1, 1, 1, 1});
    CheckPixel(h.Read(32, 32), {0, 0, 0, 0});

    // Depth: near red, then far green with LESSEQUAL (hidden) and with ZFUNC ALWAYS (NiZBufferProperty test off).
    Quad nearQuad(0.25f, {1, 0, 0, 1});
    Quad farQuad(0.75f, {0, 1, 0, 1});
    h.Clear();
    REQUIRE(h.Draw(nearQuad.Item()) == ff::DrawResult::Drawn);
    REQUIRE(h.Draw(farQuad.Item()) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(32, 32), {1, 0, 0, 1});
    ni::DrawItem always = farQuad.Item();
    always.state.zFunc = rhi::CompareFunc::Always;
    always.state.zWriteEnable = false;
    REQUIRE(h.Draw(always) == ff::DrawResult::Drawn);
    CheckPixel(h.Read(32, 32), {0, 1, 0, 1});
    REQUIRE(h.Draw(nearQuad.Item()) == ff::DrawResult::Drawn);  // depth 0.25 still in the buffer
    CheckPixel(h.Read(32, 32), {1, 0, 0, 1});
}

TEST_CASE("ff render: smoke test on the real D3D11 backend (headless WARP)") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::D3D11;
    desc.nativeWindow = nullptr;
    desc.width = desc.height = 64;
    desc.softwareAdapter = true;
    desc.debugLayer = true;
    std::string error;
    std::unique_ptr<rhi::Device> device = rhi::CreateDevice(desc, &error);
    if (device == nullptr) {
        MESSAGE("real D3D11 backend not available: " << error);
        return;
    }
    ff::FixedFunctionPipeline pipeline;
    REQUIRE_MESSAGE(pipeline.Init(*device, &error), error);
    ni::CameraDesc camera;
    camera.view = camera.projection = camera.screenProjection = IdentityMatrix();
    camera.viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
    rhi::CommandContext& ctx = device->Context();
    ctx.SetRenderTargets({}, {});
    ctx.SetViewport(camera.viewport);
    pipeline.SetCamera(camera, 64, 64);
    const float clear[4] = {0, 0, 0, 0};
    ctx.Clear(rhi::kClearColor | rhi::kClearDepth | rhi::kClearStencil, clear, 1.0f, 0, nullptr);

    // Left half: textured MODULATE quad; right half: lit flat triangle; top-left corner: pre-transformed quad.
    rhi::TextureDesc td;
    td.width = td.height = 1;
    td.format = rhi::Format::BGRA8_UNorm;
    const std::uint32_t texel = Bgra(255, 128, 0, 255);
    rhi::SubresourceData level{&texel, 4, 4};
    const rhi::TextureHandle texture = device->CreateTexture(td, &level);
    REQUIRE(texture.id != 0);
    Quad left(0.5f, {0.5f, 1.0f, 1.0f, 1.0f}, -1, 1, 0, -1);
    ni::DrawItem textured = left.Item(true, true);
    SetStage(textured.state.stages[0], FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE);
    textured.state.stages[0].texture = texture;
    CHECK(pipeline.Draw(textured) == ff::DrawResult::Drawn);

    const float positions[9] = {0.0f, -1.0f, 0.5f, 0.0f, 1.0f, 0.5f, 1.0f, -1.0f, 0.5f};
    const float normals[9] = {0, 0, -1, 0, 0, -1, 0, 0, -1};
    ni::DrawItem lit;
    lit.geometry.vertexCount = 3;
    lit.geometry.positions = {positions, 12};
    lit.geometry.normals = {normals, 12};
    lit.state.cullMode = rhi::CullMode::None;
    lit.state.shadeMode = ni::ShadeMode::Flat;
    lit.state.material.diffuse = {0.5f, 0.25f, 1.0f, 1.0f};
    lit.state.material.ambient = {0, 0, 0, 1};
    lit.lights.count = 1;
    lit.lights.lights[0].type = FF_LIGHT_DIRECTIONAL;
    lit.lights.lights[0].direction = {0, 0, 1};
    lit.lights.lights[0].diffuse = {1, 1, 1, 1};
    CHECK(pipeline.Draw(lit) == ff::DrawResult::Drawn);

    const float pixels[12] = {-0.5f, -0.5f, 0, 3.5f, -0.5f, 0, -0.5f, 3.5f, 0, 3.5f, 3.5f, 0};
    ni::DrawItem screen;
    screen.geometry.vertexCount = 4;
    screen.geometry.positions = {pixels, 12};
    screen.geometry.topology = ni::Topology::TriangleStrip;
    screen.geometry.pretransformed = true;
    screen.state.cullMode = rhi::CullMode::None;
    screen.state.zFunc = rhi::CompareFunc::Always;
    screen.state.zWriteEnable = false;
    CHECK(pipeline.Draw(screen) == ff::DrawResult::Drawn);

    rhi::Image image;
    REQUIRE_MESSAGE(device->CaptureBackBuffer(image), device->LastError());
    REQUIRE(image.width == 64);
    auto pixel = [&image](std::uint32_t x, std::uint32_t y) {
        const std::uint8_t* p = image.pixels.data() + y * image.rowPitch + x * 4;
        return WarpDevice::Pixel{p[2], p[1], p[0], p[3]};
    };
    CheckPixel(pixel(16, 32), {0.5f, 128 / 255.0f * 1.0f, 0.0f, 1.0f}, 2);
    CheckPixel(pixel(56, 56), {0.5f, 0.25f, 1.0f, 1.0f}, 2);
    CheckPixel(pixel(1, 1), {1, 1, 1, 1});
    CheckPixel(pixel(4, 1), {0.5f, 128 / 255.0f, 0.0f, 1.0f}, 2);  // quad ends at x = 4 (exclusive)
    std::vector<rhi::DebugMessage> messages;
    device->DrainDebugMessages(messages);
    for (const rhi::DebugMessage& m : messages) {
        if (m.severity >= rhi::DebugSeverity::Warning) {
            FAIL_CHECK("D3D11 debug layer (backend): " << m.text);
        }
    }
    pipeline.Shutdown();
    device->Destroy(texture);
}

TEST_CASE("ff render: the debug layer reports nothing for a mixed frame") {
    Harness h;
    if (!h.Ok()) return;
    if (!h.Device().HasDebugLayer()) {
        MESSAGE("D3D11 SDK layers not installed: debug-layer check skipped");
        return;
    }
    // A lit, textured, fogged, alpha-tested, stencilled draw followed by a pre-transformed one.
    const std::vector<std::uint32_t> texels = {Bgra(255, 255, 255, 255)};
    const rhi::TextureHandle texture = h.Texture(1, 1, texels);
    Quad quad;
    ni::DrawItem item = quad.Item(true, true);
    item.geometry.normals = {quad.normals, 12};
    item.state.lightingEnable = true;
    item.state.specularEnable = true;
    item.state.fogEnable = true;
    item.state.fogTableMode = FF_FOG_EXP;
    item.state.alphaTestEnable = true;
    item.state.stencilEnable = true;
    item.state.stencilPass = rhi::StencilOp::Replace;
    item.state.stencilRef = 3;
    item.lights.count = 1;
    item.lights.lights[0].type = FF_LIGHT_DIRECTIONAL;
    item.lights.lights[0].direction = {0, 0, 1};
    SetStage(item.state.stages[0], FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE, FF_TOP_MODULATE, FF_TA_TEXTURE, FF_TA_DIFFUSE);
    item.state.stages[0].texture = texture;
    h.Clear();
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    item.geometry.pretransformed = true;
    REQUIRE(h.Draw(item) == ff::DrawResult::Drawn);
    const std::vector<std::string> problems = h.Device().TakeDebugProblems();
    for (const std::string& p : problems) {
        FAIL_CHECK("D3D11 debug layer: " << p);
    }
}
