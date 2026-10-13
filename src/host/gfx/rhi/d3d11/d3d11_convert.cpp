// RHI descriptor -> D3D11 translations of the D3D11 backend (d3d11_internal.h, d3d11_device.h).
#include "gfx/rhi/d3d11/d3d11_internal.h"

#include <cstdio>

namespace gfx::rhi::d3d11 {

TextureFormats TextureFormatsOf(Format format, std::uint8_t usage) noexcept {
    TextureFormats f;
    const bool sampled = (usage & kTextureSampled) != 0;
    switch (format) {
    case Format::RGBA8_UNorm:
        f.resource = f.srv = f.rtv = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case Format::BGRA8_UNorm:
        f.resource = f.srv = f.rtv = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case Format::BGRX8_UNorm:
        f.resource = f.srv = f.rtv = DXGI_FORMAT_B8G8R8X8_UNORM;
        break;
    // DXGI lists the channels from the least significant bit: B5G6R5 = blue in bits 0-4, red in 11-15,
    // i.e. the bit layout of D3D8 R5G6B5 / A1R5G5B5 / A4R4G4B4 (renderer.md 5.2).
    case Format::B5G6R5_UNorm:
        f.resource = f.srv = f.rtv = DXGI_FORMAT_B5G6R5_UNORM;
        break;
    case Format::B5G5R5A1_UNorm:
        f.resource = f.srv = f.rtv = DXGI_FORMAT_B5G5R5A1_UNORM;
        break;
    case Format::B4G4R4A4_UNorm:
        f.resource = f.srv = f.rtv = DXGI_FORMAT_B4G4R4A4_UNORM;
        break;
    case Format::BC1_UNorm:
        f.resource = f.srv = DXGI_FORMAT_BC1_UNORM;
        break;
    case Format::BC2_UNorm:
        f.resource = f.srv = DXGI_FORMAT_BC2_UNORM;
        break;
    case Format::BC3_UNorm:
        f.resource = f.srv = DXGI_FORMAT_BC3_UNORM;
        break;
    case Format::A8_UNorm:
        f.resource = f.srv = f.rtv = DXGI_FORMAT_A8_UNORM;
        break;
    case Format::R8_UNorm:
        f.resource = f.srv = f.rtv = DXGI_FORMAT_R8_UNORM;
        break;
    case Format::D24_UNorm_S8_UInt:
        f.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT;
        f.resource = sampled ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
        f.srv = sampled ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_UNKNOWN;
        break;
    case Format::D32_Float:
        f.dsv = DXGI_FORMAT_D32_FLOAT;
        f.resource = sampled ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
        f.srv = sampled ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN;
        break;
    case Format::BC7_UNorm:
        f.resource = f.srv = DXGI_FORMAT_BC7_UNORM;
        break;
    case Format::Unknown:
    case Format::Count:
        break;
    }
    return f;
}

std::uint32_t DxgiFormatOf(Format format) noexcept {
    const TextureFormats f = TextureFormatsOf(format, kTextureSampled);
    return static_cast<std::uint32_t>(GetFormatInfo(format).depth ? f.dsv : f.srv);
}

DXGI_FORMAT VertexFormatOf(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float1:
        return DXGI_FORMAT_R32_FLOAT;
    case VertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VertexFormat::UNorm8x4_BGRA:
        return DXGI_FORMAT_B8G8R8A8_UNORM;  // D3DCOLOR byte order B, G, R, A
    }
    return DXGI_FORMAT_UNKNOWN;
}

const char* SemanticNameOf(VertexSemantic semantic) noexcept {
    switch (semantic) {
    case VertexSemantic::Position:
        return "POSITION";
    case VertexSemantic::Normal:
        return "NORMAL";
    case VertexSemantic::Color:
        return "COLOR";
    case VertexSemantic::TexCoord:
        return "TEXCOORD";
    }
    return "UNKNOWN";
}

// alphaChannel: D3D11 rejects *_COLOR factors for the alpha blend; D3D8 used the alpha part of the same
// factor for the alpha channel, which is what the *_ALPHA factor gives.
D3D11_BLEND BlendOf(BlendFactor factor, bool alphaChannel) noexcept {
    switch (factor) {
    case BlendFactor::Zero:
        return D3D11_BLEND_ZERO;
    case BlendFactor::One:
        return D3D11_BLEND_ONE;
    case BlendFactor::SrcColor:
        return alphaChannel ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case BlendFactor::InvSrcColor:
        return alphaChannel ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case BlendFactor::SrcAlpha:
        return D3D11_BLEND_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestAlpha:
        return D3D11_BLEND_DEST_ALPHA;
    case BlendFactor::InvDestAlpha:
        return D3D11_BLEND_INV_DEST_ALPHA;
    case BlendFactor::DestColor:
        return alphaChannel ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case BlendFactor::InvDestColor:
        return alphaChannel ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case BlendFactor::SrcAlphaSat:
        return D3D11_BLEND_SRC_ALPHA_SAT;
    case BlendFactor::Constant:
        return D3D11_BLEND_BLEND_FACTOR;
    case BlendFactor::InvConstant:
        return D3D11_BLEND_INV_BLEND_FACTOR;
    }
    return D3D11_BLEND_ONE;
}

D3D11_BLEND_OP BlendOpOf(BlendOp op) noexcept {
    switch (op) {
    case BlendOp::Add:
        return D3D11_BLEND_OP_ADD;
    case BlendOp::Subtract:
        return D3D11_BLEND_OP_SUBTRACT;
    case BlendOp::RevSubtract:
        return D3D11_BLEND_OP_REV_SUBTRACT;
    case BlendOp::Min:
        return D3D11_BLEND_OP_MIN;
    case BlendOp::Max:
        return D3D11_BLEND_OP_MAX;
    }
    return D3D11_BLEND_OP_ADD;
}

D3D11_COMPARISON_FUNC CompareOf(CompareFunc func) noexcept {
    switch (func) {
    case CompareFunc::Never:
        return D3D11_COMPARISON_NEVER;
    case CompareFunc::Less:
        return D3D11_COMPARISON_LESS;
    case CompareFunc::Equal:
        return D3D11_COMPARISON_EQUAL;
    case CompareFunc::LessEqual:
        return D3D11_COMPARISON_LESS_EQUAL;
    case CompareFunc::Greater:
        return D3D11_COMPARISON_GREATER;
    case CompareFunc::NotEqual:
        return D3D11_COMPARISON_NOT_EQUAL;
    case CompareFunc::GreaterEqual:
        return D3D11_COMPARISON_GREATER_EQUAL;
    case CompareFunc::Always:
        return D3D11_COMPARISON_ALWAYS;
    }
    return D3D11_COMPARISON_ALWAYS;
}

D3D11_STENCIL_OP StencilOpOf(StencilOp op) noexcept {
    switch (op) {
    case StencilOp::Keep:
        return D3D11_STENCIL_OP_KEEP;
    case StencilOp::Zero:
        return D3D11_STENCIL_OP_ZERO;
    case StencilOp::Replace:
        return D3D11_STENCIL_OP_REPLACE;
    case StencilOp::IncrSat:
        return D3D11_STENCIL_OP_INCR_SAT;
    case StencilOp::DecrSat:
        return D3D11_STENCIL_OP_DECR_SAT;
    case StencilOp::Invert:
        return D3D11_STENCIL_OP_INVERT;
    case StencilOp::Incr:
        return D3D11_STENCIL_OP_INCR;
    case StencilOp::Decr:
        return D3D11_STENCIL_OP_DECR;
    }
    return D3D11_STENCIL_OP_KEEP;
}

D3D11_TEXTURE_ADDRESS_MODE AddressOf(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Wrap:
        return D3D11_TEXTURE_ADDRESS_WRAP;
    case AddressMode::Clamp:
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    case AddressMode::Mirror:
        return D3D11_TEXTURE_ADDRESS_MIRROR;
    case AddressMode::Border:
        return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

D3D11_PRIMITIVE_TOPOLOGY TopologyOf(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::TriangleList:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;  // 16-bit index 0xFFFF cuts the strip
    case PrimitiveTopology::LineList:
        return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::LineStrip:
        return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case PrimitiveTopology::PointList:
        return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

D3D11_BLEND_DESC BlendDescOf(const BlendDesc& d) noexcept {
    D3D11_BLEND_DESC b{};
    b.AlphaToCoverageEnable = d.alphaToCoverage ? TRUE : FALSE;  // M3c (MSAA + alpha-tested materials)
    b.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC& t = b.RenderTarget[0];
    t.BlendEnable = d.enable ? TRUE : FALSE;
    t.SrcBlend = BlendOf(d.src, false);
    t.DestBlend = BlendOf(d.dst, false);
    t.BlendOp = BlendOpOf(d.op);
    t.SrcBlendAlpha = BlendOf(d.srcAlpha, true);
    t.DestBlendAlpha = BlendOf(d.dstAlpha, true);
    t.BlendOpAlpha = BlendOpOf(d.opAlpha);
    // RHI write mask bits 0..3 = R, G, B, A = D3D11_COLOR_WRITE_ENABLE_RED .. _ALPHA.
    t.RenderTargetWriteMask = static_cast<UINT8>(d.writeMask & D3D11_COLOR_WRITE_ENABLE_ALL);
    return b;
}

D3D11_DEPTH_STENCIL_DESC DepthStencilDescOf(const DepthStencilDesc& d) noexcept {
    D3D11_DEPTH_STENCIL_DESC s{};
    s.DepthEnable = d.depthEnable ? TRUE : FALSE;
    s.DepthWriteMask = d.depthEnable && d.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    s.DepthFunc = CompareOf(d.depthFunc);
    s.StencilEnable = d.stencilEnable ? TRUE : FALSE;
    s.StencilReadMask = d.stencilReadMask;
    s.StencilWriteMask = d.stencilWriteMask;
    D3D11_DEPTH_STENCILOP_DESC face{};
    face.StencilFailOp = StencilOpOf(d.stencilFail);
    face.StencilDepthFailOp = StencilOpOf(d.stencilDepthFail);
    face.StencilPassOp = StencilOpOf(d.stencilPass);
    face.StencilFunc = CompareOf(d.stencilFunc);
    s.FrontFace = face;  // NiStencilProperty has no two-sided stencil (rhi.h)
    s.BackFace = face;
    return s;
}

D3D11_RASTERIZER_DESC RasterDescOf(const RasterDesc& d) noexcept {
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = d.fill == FillMode::Wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    // CullMode keeps the D3D8 meaning (the screen winding that is culled, rhi.h): CW -> clockwise triangles
    // are back faces (FrontCounterClockwise = TRUE) and culled; CCW -> counter-clockwise ones.
    switch (d.cull) {
    case CullMode::None:
        r.CullMode = D3D11_CULL_NONE;
        r.FrontCounterClockwise = FALSE;
        break;
    case CullMode::CW:
        r.CullMode = D3D11_CULL_BACK;
        r.FrontCounterClockwise = TRUE;
        break;
    case CullMode::CCW:
        r.CullMode = D3D11_CULL_BACK;
        r.FrontCounterClockwise = FALSE;
        break;
    }
    r.DepthBias = d.depthBias;
    r.DepthBiasClamp = 0.0f;
    r.SlopeScaledDepthBias = d.slopeScaledDepthBias;
    r.DepthClipEnable = d.depthClip ? TRUE : FALSE;
    r.ScissorEnable = d.scissorEnable ? TRUE : FALSE;
    r.MultisampleEnable = d.multisample ? TRUE : FALSE;  // M3c: MSAA target
    r.AntialiasedLineEnable = FALSE;
    return r;
}

D3D11_SAMPLER_DESC SamplerDescOf(const SamplerDesc& d) noexcept {
    D3D11_SAMPLER_DESC s{};
    if (d.maxAnisotropy > 1) {
        s.Filter = D3D11_FILTER_ANISOTROPIC;
        s.MaxAnisotropy = d.maxAnisotropy > 16 ? 16u : d.maxAnisotropy;
    } else {
        // D3D11_FILTER encoding: bit 0 mip linear, bit 2 mag linear, bit 4 min linear.
        unsigned f = 0;
        if (d.mipFilter == MipFilter::Linear) {
            f |= 0x01;
        }
        if (d.magFilter == Filter::Linear) {
            f |= 0x04;
        }
        if (d.minFilter == Filter::Linear) {
            f |= 0x10;
        }
        s.Filter = static_cast<D3D11_FILTER>(f);
        s.MaxAnisotropy = 1;
    }
    s.AddressU = AddressOf(d.addressU);
    s.AddressV = AddressOf(d.addressV);
    s.AddressW = AddressOf(d.addressW);
    s.MipLODBias = d.mipLodBias;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    // D3D8 default border color: 0x00000000 (transparent black).
    s.BorderColor[0] = s.BorderColor[1] = s.BorderColor[2] = s.BorderColor[3] = 0.0f;
    s.MinLOD = 0.0f;
    // MipFilter::None = D3DTEXF_NONE: only level 0 is sampled.
    s.MaxLOD = d.mipFilter == MipFilter::None ? 0.0f : (d.maxLod < 0.0f ? 0.0f : d.maxLod);
    return s;
}

std::uint64_t PrimitiveCount(PrimitiveTopology topology, std::uint32_t vertices) noexcept {
    switch (topology) {
    case PrimitiveTopology::TriangleList:
        return vertices / 3;
    case PrimitiveTopology::TriangleStrip:
        return vertices >= 3 ? vertices - 2 : 0;  // strip cuts make it an upper bound
    case PrimitiveTopology::LineList:
        return vertices / 2;
    case PrimitiveTopology::LineStrip:
        return vertices >= 2 ? vertices - 1 : 0;
    case PrimitiveTopology::PointList:
        return vertices;
    }
    return 0;
}

PresentResult PresentResultFromHresult(long hr) noexcept {
    const auto h = static_cast<HRESULT>(hr);
    if (h == DXGI_STATUS_OCCLUDED) {
        return PresentResult::Occluded;
    }
    if (SUCCEEDED(h)) {
        return PresentResult::Ok;
    }
    if (h == DXGI_ERROR_DEVICE_REMOVED || h == DXGI_ERROR_DEVICE_RESET || h == DXGI_ERROR_DEVICE_HUNG ||
        h == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        return PresentResult::DeviceLost;
    }
    return PresentResult::Failed;
}

std::string HresultText(long hr) {
    struct Name {
        HRESULT hr;
        const char* name;
    };
    static constexpr Name kNames[] = {
        {S_OK, "S_OK"},
        {S_FALSE, "S_FALSE"},
        {E_FAIL, "E_FAIL"},
        {E_INVALIDARG, "E_INVALIDARG"},
        {E_OUTOFMEMORY, "E_OUTOFMEMORY"},
        {E_NOTIMPL, "E_NOTIMPL"},
        {E_NOINTERFACE, "E_NOINTERFACE"},
        {E_ACCESSDENIED, "E_ACCESSDENIED"},
        {DXGI_STATUS_OCCLUDED, "DXGI_STATUS_OCCLUDED"},
        {DXGI_ERROR_INVALID_CALL, "DXGI_ERROR_INVALID_CALL"},
        {DXGI_ERROR_NOT_FOUND, "DXGI_ERROR_NOT_FOUND"},
        {DXGI_ERROR_UNSUPPORTED, "DXGI_ERROR_UNSUPPORTED"},
        {DXGI_ERROR_DEVICE_REMOVED, "DXGI_ERROR_DEVICE_REMOVED"},
        {DXGI_ERROR_DEVICE_HUNG, "DXGI_ERROR_DEVICE_HUNG"},
        {DXGI_ERROR_DEVICE_RESET, "DXGI_ERROR_DEVICE_RESET"},
        {DXGI_ERROR_DRIVER_INTERNAL_ERROR, "DXGI_ERROR_DRIVER_INTERNAL_ERROR"},
        {DXGI_ERROR_WAS_STILL_DRAWING, "DXGI_ERROR_WAS_STILL_DRAWING"},
        {DXGI_ERROR_SDK_COMPONENT_MISSING, "DXGI_ERROR_SDK_COMPONENT_MISSING"},
        {DXGI_ERROR_ACCESS_LOST, "DXGI_ERROR_ACCESS_LOST"},
        {D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS, "D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS"},
        {D3D11_ERROR_FILE_NOT_FOUND, "D3D11_ERROR_FILE_NOT_FOUND"},
        {D3D11_ERROR_TOO_MANY_UNIQUE_VIEW_OBJECTS, "D3D11_ERROR_TOO_MANY_UNIQUE_VIEW_OBJECTS"},
    };
    const auto h = static_cast<HRESULT>(hr);
    char text[96];
    const char* name = nullptr;
    for (const Name& n : kNames) {
        if (n.hr == h) {
            name = n.name;
            break;
        }
    }
    if (name != nullptr) {
        std::snprintf(text, sizeof(text), "0x%08lX %s", static_cast<unsigned long>(h), name);
    } else {
        std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(h));
    }
    return text;
}

}  // namespace gfx::rhi::d3d11
