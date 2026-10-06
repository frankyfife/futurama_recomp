// D3D11 backend: command context (d3d11_internal.h) - targets, clears (incl. rectangles), pipeline and
// resource binding, draws, constant-buffer updates and the per-frame transient ring.
#include "gfx/rhi/d3d11/d3d11_internal.h"

#include <cstring>

namespace gfx::rhi::d3d11 {
namespace {

constexpr std::uint32_t AlignUp(std::uint32_t v, std::uint32_t a) noexcept {
    return (v + a - 1) & ~(a - 1);
}

std::uint32_t RoundUpPow2(std::uint32_t v) noexcept {
    std::uint32_t p = 1;
    while (p < v && p < 0x80000000u) {
        p <<= 1;
    }
    return p;
}

}  // namespace

void D3D11Context::Init(ID3D11DeviceContext1* context) noexcept {
    ctx_ = context;
}

void D3D11Context::BindDefaultTargets() noexcept {
    colorTarget_ = 0;
    depthTarget_ = 0;
    defaultTargets_ = true;
    rtv_ = device_.BackBufferRtv();
    dsv_ = device_.BackBufferDsv();
    targetWidth_ = device_.Info().backBufferWidth;
    targetHeight_ = device_.Info().backBufferHeight;
    ApplyTargets();
    viewport_ = D3D11_VIEWPORT{0.0f, 0.0f, static_cast<float>(targetWidth_), static_cast<float>(targetHeight_), 0.0f,
                               1.0f};
    ApplyViewport();
}

void D3D11Context::ApplyTargets() noexcept {
    if (ctx_ != nullptr) {
        ID3D11RenderTargetView* rtvs[1] = {rtv_};
        ctx_->OMSetRenderTargets(rtv_ != nullptr ? 1u : 0u, rtv_ != nullptr ? rtvs : nullptr, dsv_);
    }
}

void D3D11Context::ApplyViewport() noexcept {
    if (ctx_ != nullptr) {
        ctx_->RSSetViewports(1, &viewport_);
    }
}

// Like D3D8 SetRenderTarget, binding targets resets the viewport to the whole color target.
void D3D11Context::SetRenderTargets(TextureHandle color, TextureHandle depth) noexcept {
    if (!color) {
        // The back buffer and, unless another depth target is named, its own depth buffer.
        rtv_ = device_.BackBufferRtv();
        dsv_ = device_.BackBufferDsv();
        targetWidth_ = device_.Info().backBufferWidth;
        targetHeight_ = device_.Info().backBufferHeight;
        defaultTargets_ = !depth;
    } else {
        TextureRecord* c = device_.GetTexture(color.id);
        if (c == nullptr || c->rtv == nullptr) {
            device_.FailText("SetRenderTargets: invalid color target (not a render-target texture)");
            return;
        }
        // A texture bound as a pixel shader resource would be forced to NULL by D3D11 (with a warning).
        for (std::uint32_t slot = 0; slot < kMaxTextureSlots; ++slot) {
            if (textures_[slot] == color.id) {
                SetTexture(slot, {});
            }
        }
        rtv_ = c->rtv.Get();
        dsv_ = nullptr;
        targetWidth_ = c->width;
        targetHeight_ = c->height;
        defaultTargets_ = false;
    }
    if (depth) {
        TextureRecord* d = device_.GetTexture(depth.id);
        if (d == nullptr || d->dsv == nullptr) {
            device_.FailText("SetRenderTargets: invalid depth target (not a depth-stencil texture)");
            dsv_ = nullptr;
        } else {
            dsv_ = d->dsv.Get();
        }
    }
    colorTarget_ = color.id;
    depthTarget_ = depth.id;
    ApplyTargets();
    viewport_ = D3D11_VIEWPORT{0.0f, 0.0f, static_cast<float>(targetWidth_), static_cast<float>(targetHeight_), 0.0f,
                               1.0f};
    ApplyViewport();
}

void D3D11Context::SetViewport(const Viewport& v) noexcept {
    viewport_ = D3D11_VIEWPORT{v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
    ApplyViewport();
}

void D3D11Context::SetScissor(const Rect& r) noexcept {
    const D3D11_RECT rect{r.left, r.top, r.right, r.bottom};
    ctx_->RSSetScissorRects(1, &rect);
}

void D3D11Context::Clear(std::uint8_t flags, const float color[4], float depth, std::uint8_t stencil,
                         const Rect* rect) noexcept {
    static constexpr float kBlack[4] = {0, 0, 0, 0};
    device_.NoteGpuWork();  // M3c: GPU frame timing
    const float* c = color != nullptr ? color : kBlack;
    const float d = depth < 0.0f ? 0.0f : depth > 1.0f ? 1.0f : depth;
    const UINT dsFlags = ((flags & kClearDepth) != 0 ? D3D11_CLEAR_DEPTH : 0u) |
                         ((flags & kClearStencil) != 0 ? D3D11_CLEAR_STENCIL : 0u);
    D3D11_RECT r{0, 0, static_cast<LONG>(targetWidth_), static_cast<LONG>(targetHeight_)};
    bool whole = true;
    if (rect != nullptr) {
        // D3D8 clips the clear rectangles to the target; an empty rectangle clears nothing.
        r.left = rect->left > 0 ? rect->left : 0;
        r.top = rect->top > 0 ? rect->top : 0;
        r.right = rect->right < static_cast<LONG>(targetWidth_) ? rect->right : static_cast<LONG>(targetWidth_);
        r.bottom = rect->bottom < static_cast<LONG>(targetHeight_) ? rect->bottom : static_cast<LONG>(targetHeight_);
        if (r.right <= r.left || r.bottom <= r.top) {
            return;
        }
        whole = r.left == 0 && r.top == 0 && r.right == static_cast<LONG>(targetWidth_) &&
                r.bottom == static_cast<LONG>(targetHeight_);
    }
    if (whole) {
        if ((flags & kClearColor) != 0 && rtv_ != nullptr) {
            ctx_->ClearRenderTargetView(rtv_, c);
        }
        if (dsFlags != 0 && dsv_ != nullptr) {
            ctx_->ClearDepthStencilView(dsv_, dsFlags, d, stencil);
        }
        return;
    }
    if ((flags & kClearColor) != 0 && rtv_ != nullptr) {
        ctx_->ClearView(rtv_, c, &r, 1);
    }
    if (dsFlags != 0 && dsv_ != nullptr) {
        ClearDepthStencilRect(r, flags, d, stencil);
    }
}

// D3D11 has no rectangle clear for depth-stencil views (ClearView takes RTVs only): draw a triangle over the
// viewport = rectangle at depth MinDepth = MaxDepth = `depth` with depth test ALWAYS / stencil REPLACE and
// color writes off, then restore the bound state.
bool D3D11Context::ClearDepthStencilRect(const D3D11_RECT& r, std::uint8_t flags, float depth,
                                         std::uint8_t stencil) noexcept {
    ID3D11Device1* dev = device_.D3D();
    const bool clearDepth = (flags & kClearDepth) != 0;
    const bool clearStencil = (flags & kClearStencil) != 0;
    const int variant = clearDepth && clearStencil ? 2 : clearStencil ? 1 : 0;
    if (clearVs_ == nullptr) {
        const ShaderBytecode code = BuiltinShaderCode(BuiltinShader::ClearVS);
        HRESULT hr = dev->CreateVertexShader(code.data, code.size, nullptr, &clearVs_);
        if (SUCCEEDED(hr)) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = 0;
            hr = dev->CreateBlendState(&bd, &clearBlend_);
        }
        if (SUCCEEDED(hr)) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            hr = dev->CreateRasterizerState(&rd, &clearRaster_);
        }
        for (int v = 0; v < 3 && SUCCEEDED(hr); ++v) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            const bool vd = v != 1;
            const bool vs = v != 0;
            dd.DepthEnable = vd ? TRUE : FALSE;
            dd.DepthWriteMask = vd ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            dd.StencilEnable = vs ? TRUE : FALSE;
            dd.StencilReadMask = 0xFF;
            dd.StencilWriteMask = 0xFF;
            dd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_REPLACE;
            dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
            dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            dd.BackFace = dd.FrontFace;
            hr = dev->CreateDepthStencilState(&dd, &clearDepthStencil_[v]);
        }
        if (FAILED(hr)) {
            clearVs_.Reset();
            return device_.Fail("rectangle depth/stencil clear: state creation", hr);
        }
    }
    const D3D11_VIEWPORT vp{static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                            static_cast<float>(r.bottom - r.top), depth, depth};
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(clearVs_.Get(), nullptr, 0);
    ctx_->PSSetShader(nullptr, nullptr, 0);
    ctx_->OMSetBlendState(clearBlend_.Get(), nullptr, 0xFFFFFFFFu);
    ctx_->OMSetDepthStencilState(clearDepthStencil_[variant].Get(), stencil);
    ctx_->RSSetState(clearRaster_.Get());
    ctx_->RSSetViewports(1, &vp);
    ctx_->Draw(3, 0);
    // Restore.
    ApplyViewport();
    ApplyPipeline(true);
    return true;
}

void D3D11Context::ApplyPipeline(bool force) noexcept {
    const PipelineRecord* p = device_.GetPipelineRecord(pipeline_);
    const PipelineRecord* old = force ? nullptr : applied_;
    if (p == nullptr) {
        if (force) {
            ctx_->IASetInputLayout(nullptr);
            ctx_->VSSetShader(nullptr, nullptr, 0);
            ctx_->PSSetShader(nullptr, nullptr, 0);
            ctx_->OMSetBlendState(nullptr, blendFactor_, 0xFFFFFFFFu);
            ctx_->OMSetDepthStencilState(nullptr, stencilRef_);
            ctx_->RSSetState(nullptr);
        }
        applied_ = nullptr;
        return;
    }
    if (old == nullptr || old->layout != p->layout) {
        ctx_->IASetInputLayout(p->layout.Get());
    }
    if (old == nullptr || old->topology != p->topology) {
        ctx_->IASetPrimitiveTopology(p->topology);
    }
    if (old == nullptr || old->vs != p->vs) {
        ctx_->VSSetShader(p->vs.Get(), nullptr, 0);
    }
    if (old == nullptr || old->ps != p->ps) {
        ctx_->PSSetShader(p->ps.Get(), nullptr, 0);
    }
    if (old == nullptr || old->blend != p->blend) {
        ctx_->OMSetBlendState(p->blend.Get(), blendFactor_, 0xFFFFFFFFu);
    }
    if (old == nullptr || old->depthStencil != p->depthStencil) {
        ctx_->OMSetDepthStencilState(p->depthStencil.Get(), stencilRef_);
    }
    if (old == nullptr || old->raster != p->raster) {
        ctx_->RSSetState(p->raster.Get());
    }
    applied_ = p;
}

void D3D11Context::SetPipeline(PipelineHandle pipeline) noexcept {
    if (pipeline.id == pipeline_ && applied_ != nullptr) {
        return;
    }
    if (pipeline && device_.GetPipelineRecord(pipeline.id) == nullptr) {
        device_.FailText("SetPipeline: invalid pipeline handle");
        pipeline = {};
    }
    pipeline_ = pipeline.id;
    ApplyPipeline(false);
}

void D3D11Context::SetStencilRef(std::uint8_t ref) noexcept {
    if (ref == stencilRef_) {
        return;
    }
    stencilRef_ = ref;
    if (applied_ != nullptr) {
        ctx_->OMSetDepthStencilState(applied_->depthStencil.Get(), stencilRef_);
    }
}

void D3D11Context::SetBlendFactor(const float factor[4]) noexcept {
    if (factor == nullptr || std::memcmp(factor, blendFactor_, sizeof(blendFactor_)) == 0) {
        return;
    }
    std::memcpy(blendFactor_, factor, sizeof(blendFactor_));
    if (applied_ != nullptr) {
        ctx_->OMSetBlendState(applied_->blend.Get(), blendFactor_, 0xFFFFFFFFu);
    }
}

void D3D11Context::SetVertexBuffer(std::uint32_t stream, BufferHandle buffer, std::uint32_t offset,
                                   std::uint32_t stride) noexcept {
    if (stream >= kMaxVertexStreams) {
        device_.FailText("SetVertexBuffer: stream out of range");
        return;
    }
    ID3D11Buffer* b = nullptr;
    if (buffer) {
        BufferRecord* rec = device_.GetBuffer(buffer.id);
        if (rec == nullptr || (rec->bind & kBindVertex) == 0) {
            device_.FailText("SetVertexBuffer: invalid vertex buffer handle");
        } else {
            b = rec->buffer.Get();
        }
    }
    const UINT strides[1] = {stride};
    const UINT offsets[1] = {offset};
    ctx_->IASetVertexBuffers(stream, 1, &b, strides, offsets);
}

void D3D11Context::SetIndexBuffer(BufferHandle buffer, std::uint32_t offset) noexcept {
    ID3D11Buffer* b = nullptr;
    if (buffer) {
        BufferRecord* rec = device_.GetBuffer(buffer.id);
        if (rec == nullptr || (rec->bind & kBindIndex) == 0) {
            device_.FailText("SetIndexBuffer: invalid index buffer handle");
        } else {
            b = rec->buffer.Get();
        }
    }
    ctx_->IASetIndexBuffer(b, DXGI_FORMAT_R16_UINT, offset);
}

void D3D11Context::SetConstantBuffer(std::uint8_t stages, std::uint32_t slot, BufferHandle buffer) noexcept {
    if (slot >= kMaxConstantSlots) {
        device_.FailText("SetConstantBuffer: slot out of range");
        return;
    }
    ID3D11Buffer* b = nullptr;
    if (buffer) {
        BufferRecord* rec = device_.GetBuffer(buffer.id);
        if (rec == nullptr || (rec->bind & kBindConstant) == 0) {
            device_.FailText("SetConstantBuffer: invalid constant buffer handle");
        } else {
            b = rec->buffer.Get();
        }
    }
    if ((stages & kStageVertex) != 0) {
        ctx_->VSSetConstantBuffers(slot, 1, &b);
    }
    if ((stages & kStagePixel) != 0) {
        ctx_->PSSetConstantBuffers(slot, 1, &b);
    }
}

void D3D11Context::SetTexture(std::uint32_t slot, TextureHandle texture) noexcept {
    if (slot >= kMaxTextureSlots) {
        device_.FailText("SetTexture: slot out of range");
        return;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    std::uint32_t id = 0;
    if (texture) {
        TextureRecord* rec = device_.GetTexture(texture.id);
        if (rec == nullptr || rec->srv == nullptr) {
            device_.FailText("SetTexture: invalid texture handle (or not sampled)");
        } else if (texture.id == colorTarget_ || texture.id == depthTarget_) {
            device_.FailText("SetTexture: the texture is the current render target");
        } else {
            srv = rec->srv.Get();
            id = texture.id;
        }
    }
    if (textures_[slot] == id && (id != 0 || srv == nullptr)) {
        return;
    }
    textures_[slot] = id;
    ctx_->PSSetShaderResources(slot, 1, &srv);
}

void D3D11Context::SetSampler(std::uint32_t slot, SamplerHandle sampler) noexcept {
    if (slot >= kMaxSamplerSlots) {
        device_.FailText("SetSampler: slot out of range");
        return;
    }
    ID3D11SamplerState* s = device_.GetSamplerState(sampler.id);
    if (sampler && s == nullptr) {
        device_.FailText("SetSampler: invalid sampler handle");
    }
    samplers_[slot] = s;
    ctx_->PSSetSamplers(slot, 1, &s);
}

void D3D11Context::Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) noexcept {
    if (applied_ == nullptr) {
        device_.FailText("Draw: no valid pipeline bound - skipped");
        return;
    }
    if (vertexCount == 0) {
        return;
    }
    device_.NoteGpuWork();  // M3c: GPU frame timing
    ctx_->Draw(vertexCount, firstVertex);
    DeviceStats& s = device_.MutableStats();
    ++s.draws;
    s.primitives += PrimitiveCount(applied_->desc.topology, vertexCount);
}

void D3D11Context::DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::int32_t baseVertex) noexcept {
    if (applied_ == nullptr) {
        device_.FailText("DrawIndexed: no valid pipeline bound - skipped");
        return;
    }
    if (indexCount == 0) {
        return;
    }
    device_.NoteGpuWork();  // M3c: GPU frame timing
    ctx_->DrawIndexed(indexCount, firstIndex, baseVertex);
    DeviceStats& s = device_.MutableStats();
    ++s.draws;
    s.primitives += PrimitiveCount(applied_->desc.topology, indexCount);
}

bool D3D11Context::UpdateBuffer(BufferHandle buffer, const void* data, std::uint32_t size) noexcept {
    BufferRecord* rec = device_.GetBuffer(buffer.id);
    if (rec == nullptr || rec->usage != BufferUsage::Dynamic || rec->transient) {
        return device_.FailText("UpdateBuffer: invalid handle or not a dynamic buffer");
    }
    if (data == nullptr || size == 0 || size > rec->size) {
        return device_.FailText("UpdateBuffer: no data or larger than the buffer");
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx_->Map(rec->buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr)) {
        return device_.Fail("Map (UpdateBuffer)", hr);
    }
    std::memcpy(m.pData, data, size);
    ctx_->Unmap(rec->buffer.Get(), 0);
    return true;
}

// ---- transient ring ------------------------------------------------------------------------------------------
// Per kind a list of dynamic chunks. Within a frame allocations only move forward (NO_OVERWRITE); a full
// chunk moves on to the next one (created on demand), so every span stays valid until the next Present.
// The first allocation of a frame in a chunk maps it with DISCARD (the GPU may still read the previous
// frame's data).

bool D3D11Context::CreateTransientChunk(TransientKind kind, std::uint32_t size, TransientChunk* out) noexcept {
    try {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = size;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = kind == TransientKind::Vertex ? D3D11_BIND_VERTEX_BUFFER : D3D11_BIND_INDEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer;
        const HRESULT hr = device_.D3D()->CreateBuffer(&bd, nullptr, &buffer);
        if (FAILED(hr)) {
            return device_.Fail("CreateBuffer (transient ring)", hr);
        }
        const char* name = kind == TransientKind::Vertex ? "rhi transient vertices" : "rhi transient indices";
        buffer->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
        out->buffer = buffer.Get();
        out->size = size;
        out->discardNext = true;
        out->handle = device_.AddTransientBuffer(std::move(buffer), size,
                                                 kind == TransientKind::Vertex ? kBindVertex : kBindIndex);
        if (out->handle == 0) {
            out->buffer = nullptr;
            return device_.FailText("transient ring: too many buffers");
        }
        return true;
    } catch (...) {
        return device_.FailText("transient ring: out of memory");
    }
}

TransientSpan D3D11Context::BeginTransient(TransientKind kind, std::uint32_t size, std::uint32_t alignment) noexcept {
    TransientSpan span;
    if (openRing_ >= 0) {
        device_.FailText("BeginTransient: another span is still open");
        return span;
    }
    if (size == 0 || size > kTransientMaxChunk || (alignment & (alignment - 1)) != 0) {
        device_.FailText("BeginTransient: size 0 / too large or alignment not a power of two");
        return span;
    }
    const std::uint32_t align = alignment < 4 ? 4u : alignment;
    const int k = kind == TransientKind::Vertex ? 0 : 1;
    TransientRing& ring = rings_[k];
    const std::uint32_t chunkSize = kind == TransientKind::Vertex ? kTransientVertexChunk : kTransientIndexChunk;
    std::uint32_t offset = 0;
    for (;;) {
        if (ring.current < ring.chunks.size()) {
            TransientChunk& chunk = ring.chunks[ring.current];
            offset = AlignUp(ring.offset, align);
            if (static_cast<std::uint64_t>(offset) + size <= chunk.size) {
                break;
            }
            if (ring.offset == 0) {
                // Even the empty chunk is too small: replace it (draws of earlier frames keep the old buffer
                // alive inside D3D11 until the GPU is done with it).
                device_.RemoveTransientBuffer(chunk.handle);
                TransientChunk bigger;
                if (!CreateTransientChunk(kind, RoundUpPow2(size), &bigger)) {
                    ring.chunks.erase(ring.chunks.begin() + static_cast<std::ptrdiff_t>(ring.current));
                    return span;
                }
                chunk = bigger;
                offset = 0;
                break;
            }
            ++ring.current;
            ring.offset = 0;
            continue;
        }
        TransientChunk chunk;
        if (!CreateTransientChunk(kind, size > chunkSize ? RoundUpPow2(size) : chunkSize, &chunk)) {
            return span;
        }
        try {
            ring.chunks.push_back(chunk);
        } catch (...) {
            device_.RemoveTransientBuffer(chunk.handle);
            device_.FailText("transient ring: out of memory");
            return span;
        }
        ring.offset = 0;
        offset = 0;
        break;
    }
    TransientChunk& chunk = ring.chunks[ring.current];
    D3D11_MAPPED_SUBRESOURCE m{};
    const D3D11_MAP mapType = chunk.discardNext ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
    const HRESULT hr = ctx_->Map(chunk.buffer, 0, mapType, 0, &m);
    if (FAILED(hr)) {
        device_.Fail("Map (transient ring)", hr);
        return span;
    }
    chunk.discardNext = false;
    ring.offset = offset + size;
    openRing_ = k;
    device_.MutableStats().transientBytes += size;
    span.buffer = BufferHandle{chunk.handle};
    span.offset = offset;
    span.size = size;
    span.cpu = static_cast<std::uint8_t*>(m.pData) + offset;
    return span;
}

void D3D11Context::EndTransient() noexcept {
    if (openRing_ < 0) {
        return;
    }
    TransientRing& ring = rings_[openRing_];
    if (ring.current < ring.chunks.size()) {
        ctx_->Unmap(ring.chunks[ring.current].buffer, 0);
    }
    openRing_ = -1;
}

void D3D11Context::OnPresent() noexcept {
    EndTransient();
    for (TransientRing& ring : rings_) {
        ring.current = 0;
        ring.offset = 0;
        for (TransientChunk& c : ring.chunks) {
            c.discardNext = true;
        }
    }
    // Present moved on to the next persistent back buffer and its present pass (window) replaced the
    // shaders, states, targets, viewport and PS t0/s0: bind everything of this context again.
    if (colorTarget_ == 0) {
        rtv_ = device_.BackBufferRtv();
    }
    RestoreBindings();
}

void D3D11Context::RestoreBindings() noexcept {
    if (ctx_ == nullptr) {
        return;
    }
    ApplyTargets();
    ApplyViewport();
    ApplyPipeline(true);
    ID3D11ShaderResourceView* srv = nullptr;
    if (TextureRecord* rec = device_.GetTexture(textures_[0])) {
        srv = rec->srv.Get();
    }
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->PSSetSamplers(0, 1, &samplers_[0]);
}

void D3D11Context::OnBackBuffersChanged() noexcept {
    if (colorTarget_ == 0) {
        rtv_ = device_.BackBufferRtv();
        targetWidth_ = device_.Info().backBufferWidth;
        targetHeight_ = device_.Info().backBufferHeight;
        viewport_ = D3D11_VIEWPORT{0.0f, 0.0f, static_cast<float>(targetWidth_), static_cast<float>(targetHeight_),
                                   0.0f, 1.0f};
    }
    if (defaultTargets_ || (colorTarget_ == 0 && depthTarget_ == 0)) {
        dsv_ = device_.BackBufferDsv();
    }
    RestoreBindings();
}

void D3D11Context::OnDestroyTexture(std::uint32_t id) noexcept {
    for (std::uint32_t slot = 0; slot < kMaxTextureSlots; ++slot) {
        if (textures_[slot] == id) {
            ID3D11ShaderResourceView* none = nullptr;
            ctx_->PSSetShaderResources(slot, 1, &none);
            textures_[slot] = 0;
        }
    }
    if (id == colorTarget_ || id == depthTarget_) {
        BindDefaultTargets();
    }
}

void D3D11Context::Release() noexcept {
    EndTransient();
    if (ctx_ != nullptr) {
        ctx_->ClearState();
        ctx_->Flush();
    }
    for (TransientRing& ring : rings_) {
        for (TransientChunk& c : ring.chunks) {
            device_.RemoveTransientBuffer(c.handle);
        }
        ring.chunks.clear();
        ring.current = 0;
        ring.offset = 0;
    }
    clearVs_.Reset();
    clearBlend_.Reset();
    clearRaster_.Reset();
    for (auto& s : clearDepthStencil_) {
        s.Reset();
    }
    rtv_ = nullptr;
    dsv_ = nullptr;
    samplers_.fill(nullptr);
    applied_ = nullptr;
    pipeline_ = 0;
    textures_.fill(0);
    ctx_ = nullptr;
}

}  // namespace gfx::rhi::d3d11
