// Backend-neutral parts of the RHI (rhi.h): format table, descriptor hashes, CreateDevice dispatch.
// Shared skeleton file (M3 phase 1); the backends live in gfx/rhi/<backend>/.
#include "gfx/rhi/rhi.h"

#include "gfx/rhi/d3d11/d3d11_device.h"

#include <bit>
#include <cstring>
#include <iterator>

namespace gfx::rhi {
namespace {

// name, block w/h, bytes per block (pixel), compressed, depth, alpha
constexpr FormatInfo kFormats[] = {
    {"Unknown", 1, 1, 0, false, false, false},
    {"RGBA8_UNorm", 1, 1, 4, false, false, true},
    {"BGRA8_UNorm", 1, 1, 4, false, false, true},
    {"BGRX8_UNorm", 1, 1, 4, false, false, false},
    {"B5G6R5_UNorm", 1, 1, 2, false, false, false},
    {"B5G5R5A1_UNorm", 1, 1, 2, false, false, true},
    {"B4G4R4A4_UNorm", 1, 1, 2, false, false, true},
    {"BC1_UNorm", 4, 4, 8, true, false, true},
    {"BC2_UNorm", 4, 4, 16, true, false, true},
    {"BC3_UNorm", 4, 4, 16, true, false, true},
    {"A8_UNorm", 1, 1, 1, false, false, true},
    {"R8_UNorm", 1, 1, 1, false, false, false},
    {"D24_UNorm_S8_UInt", 1, 1, 4, false, true, false},
    {"D32_Float", 1, 1, 4, false, true, false},
};
static_assert(std::size(kFormats) == static_cast<std::size_t>(Format::Count));

// FNV-1a over the bytes of individual fields (descriptors may contain padding, so never hash the raw
// struct).
struct Hasher {
    std::size_t h = 2166136261u;
    void Bytes(const void* p, std::size_t n) noexcept {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 16777619u;
        }
    }
    template <class T>
    void Add(const T& v) noexcept {
        Bytes(&v, sizeof(v));
    }
    void AddFloat(float f) noexcept {
        Add(f == 0.0f ? 0u : std::bit_cast<std::uint32_t>(f));  // +0 and -0 compare equal
    }
};

void HashBlend(Hasher& h, const BlendDesc& d) noexcept {
    h.Add(d.enable);
    h.Add(d.src);
    h.Add(d.dst);
    h.Add(d.op);
    h.Add(d.srcAlpha);
    h.Add(d.dstAlpha);
    h.Add(d.opAlpha);
    h.Add(d.writeMask);
    h.Add(d.alphaToCoverage);  // M3c
}

void HashDepthStencil(Hasher& h, const DepthStencilDesc& d) noexcept {
    h.Add(d.depthEnable);
    h.Add(d.depthWrite);
    h.Add(d.depthFunc);
    h.Add(d.stencilEnable);
    h.Add(d.stencilReadMask);
    h.Add(d.stencilWriteMask);
    h.Add(d.stencilFunc);
    h.Add(d.stencilFail);
    h.Add(d.stencilDepthFail);
    h.Add(d.stencilPass);
}

void HashRaster(Hasher& h, const RasterDesc& d) noexcept {
    h.Add(d.cull);
    h.Add(d.fill);
    h.Add(d.scissorEnable);
    h.Add(d.depthClip);
    h.Add(d.depthBias);
    h.AddFloat(d.slopeScaledDepthBias);
    h.Add(d.multisample);  // M3c
}

}  // namespace

const FormatInfo& GetFormatInfo(Format format) noexcept {
    const auto i = static_cast<std::size_t>(format);
    return i < std::size(kFormats) ? kFormats[i] : kFormats[0];
}

std::uint32_t RowPitch(Format format, std::uint32_t width) noexcept {
    const FormatInfo& f = GetFormatInfo(format);
    const std::uint32_t blocks = (width + f.blockWidth - 1) / f.blockWidth;
    return blocks * f.bytesPerBlock;
}

std::uint32_t LevelSize(Format format, std::uint32_t width, std::uint32_t height) noexcept {
    const FormatInfo& f = GetFormatInfo(format);
    const std::uint32_t rows = (height + f.blockHeight - 1) / f.blockHeight;
    return RowPitch(format, width) * rows;
}

std::size_t HashOf(const SamplerDesc& d) noexcept {
    Hasher h;
    h.Add(d.minFilter);
    h.Add(d.magFilter);
    h.Add(d.mipFilter);
    h.Add(d.addressU);
    h.Add(d.addressV);
    h.Add(d.addressW);
    h.Add(d.maxAnisotropy);
    h.AddFloat(d.mipLodBias);
    h.AddFloat(d.maxLod);
    return h.h;
}

std::size_t HashOf(const PipelineDesc& d) noexcept {
    Hasher h;
    h.Add(d.vertexShader.id);
    h.Add(d.pixelShader.id);
    h.Add(d.layout.attributeCount);
    for (std::uint32_t i = 0; i < d.layout.attributeCount && i < kMaxVertexAttributes; ++i) {
        const VertexAttribute& a = d.layout.attributes[i];
        h.Add(a.semantic);
        h.Add(a.semanticIndex);
        h.Add(a.format);
        h.Add(a.stream);
        h.Add(a.offset);
    }
    for (std::uint16_t s : d.layout.streamStrides) {
        h.Add(s);
    }
    HashBlend(h, d.blend);
    HashDepthStencil(h, d.depthStencil);
    HashRaster(h, d.raster);
    h.Add(d.topology);
    h.Add(d.colorFormat);
    h.Add(d.depthFormat);
    return h.h;
}

const char* BackendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::Null:
        return "null";
    case Backend::D3D11:
        return "d3d11";
    }
    return "unknown";
}

const char* PresentScalingName(PresentScaling scaling) noexcept {
    switch (scaling) {
    case PresentScaling::Linear:
        return "linear";
    case PresentScaling::Point:
        return "point";
    case PresentScaling::Integer:
        return "integer";
    }
    return "unknown";
}

std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc, std::string* error) noexcept {
    try {
        switch (desc.backend) {
        case Backend::D3D11:
            return d3d11::CreateDevice(desc, error);
        case Backend::Null:
            break;
        }
        if (error != nullptr) {
            *error = "null backend has no device";
        }
    } catch (...) {
        if (error != nullptr) {
            error->clear();  // allocation failure while reporting; keep it empty
        }
    }
    return nullptr;
}

}  // namespace gfx::rhi
