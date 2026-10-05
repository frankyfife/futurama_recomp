// Thin rendering hardware interface (RHI) of the PC renderer (design 2.2 "RHI und Backends", M3;
// GFX_CONTRACT.md 5). The M3 backend is D3D11 (gfx/rhi/d3d11/**); M10 adds Vulkan or D3D12 behind the
// same interface, so nothing here names a D3D/DXGI/Win32 type (the window is a `void*` native handle).
//
// Layering: NiPCRenderer slots (gfx/nipc_renderer.cpp) -> ni_extract (guest objects -> DrawItem) ->
// ff_pipeline (D3D8 fixed function on top of the RHI) -> rhi::Device / rhi::CommandContext -> backend.
//
// Rules (binding for every backend):
//   - Objects are referred to by typed 32-bit handles; id 0 = no object. Handles are only valid on the
//     device that created them; a backend may encode index + generation in the id.
//   - State objects come from small POD descriptors and are hash-cached by the backend: an equal
//     descriptor returns the same handle (GetSampler, GetPipeline); callers never destroy them.
//   - Buffers, textures and shaders are created/destroyed explicitly (Create* / Destroy).
//   - Coordinates follow D3D: clip space x, y in [-1, 1] with y up, z in [0, 1]; viewport and scissor
//     origin top-left in pixels; texture origin top-left; matrices are the caller's business (row
//     vectors, gfx/gfx_math.h). Pixel centers are at +0.5 (D3D10+ rules).
//   - CullMode keeps the D3D8 meaning (the screen-space winding that is culled), see RasterDesc.
//   - Threading: a device and its context are used by one thread at a time - the guest main thread that
//     runs the NiPCRenderer slots (loading fibers run on that same OS thread). Tests may use a device on
//     their own thread. The window passed in DeviceDesc may belong to another thread (the boot/UI thread
//     runs its message loop, platform/window.h).
//   - Nothing throws across this interface (noexcept); failures return an invalid handle / false and
//     the reason stays readable through Device::LastError().
// Shared skeleton header (phase 1 of M3): the backend agent implements it; changes are add-only and
// logged in INTERFACES.md section 8.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gfx::rhi {

// ---- handles -------------------------------------------------------------------------------------

template <class Tag>
struct Handle {
    std::uint32_t id = 0;
    constexpr explicit operator bool() const noexcept { return id != 0; }
    friend constexpr bool operator==(Handle, Handle) noexcept = default;
};
using BufferHandle = Handle<struct BufferTag>;
using TextureHandle = Handle<struct TextureTag>;
using SamplerHandle = Handle<struct SamplerTag>;
using ShaderHandle = Handle<struct ShaderTag>;
using PipelineHandle = Handle<struct PipelineTag>;

// ---- formats ---------------------------------------------------------------------------------------

// Texture / render target formats (renderer.md 5.2 source formats -> GFX_CONTRACT.md 7 table). Channel
// order is the memory byte order (BGRA8_UNorm = bytes B, G, R, A = D3D8 A8R8G8B8 / DXGI B8G8R8A8).
enum class Format : std::uint8_t {
    Unknown = 0,
    RGBA8_UNorm,       // bytes R, G, B, A
    BGRA8_UNorm,       // bytes B, G, R, A (D3D8 A8R8G8B8) - the default upload format of texconv
    BGRX8_UNorm,       // bytes B, G, R, x; sampled alpha = 1 (D3D8 X8R8G8B8)
    B5G6R5_UNorm,      // optional (IsFormatSupported), D3D8 R5G6B5
    B5G5R5A1_UNorm,    // optional, D3D8 A1R5G5B5
    B4G4R4A4_UNorm,    // optional, D3D8 A4R4G4B4
    BC1_UNorm,         // DXT1 (NiPixelFormat 4)
    BC2_UNorm,         // DXT3 (NiPixelFormat 5)
    BC3_UNorm,         // DXT5 (NiPixelFormat 6)
    A8_UNorm,          // alpha only
    R8_UNorm,          // single channel (luminance via shader swizzle)
    D24_UNorm_S8_UInt, // depth/stencil of the back buffer (Create gets PC D3DFMT_D24S8 = 0x4B)
    D32_Float,
    Count
};

struct FormatInfo {
    const char* name;
    std::uint8_t blockWidth;    // 1 for uncompressed, 4 for BCn
    std::uint8_t blockHeight;
    std::uint8_t bytesPerBlock; // bytes per pixel for uncompressed formats
    bool compressed;
    bool depth;
    bool hasAlpha;
};
// Static description of a format (Unknown/Count -> a zero entry named "Unknown"). Implemented in rhi.cpp.
const FormatInfo& GetFormatInfo(Format format) noexcept;
// Bytes of one row of blocks / of a whole 2D level (blocks rounded up).
std::uint32_t RowPitch(Format format, std::uint32_t width) noexcept;
std::uint32_t LevelSize(Format format, std::uint32_t width, std::uint32_t height) noexcept;

// Vertex attribute formats (ff_pipeline uses Float2/3/4; UNorm8x4_BGRA is the packed D3DCOLOR order).
enum class VertexFormat : std::uint8_t { Float1, Float2, Float3, Float4, UNorm8x4_BGRA };

// ---- resources -------------------------------------------------------------------------------------

enum BindFlags : std::uint8_t { kBindVertex = 1, kBindIndex = 2, kBindConstant = 4 };
enum class BufferUsage : std::uint8_t {
    Static,   // initialData required, never written again
    Dynamic,  // CPU-written (UpdateBuffer = whole-buffer discard); constant buffers are always Dynamic
};
struct BufferDesc {
    std::uint32_t size = 0;           // bytes; constant buffers: multiple of 16
    std::uint8_t bind = 0;            // BindFlags
    BufferUsage usage = BufferUsage::Static;
    const void* initialData = nullptr;
    const char* debugName = nullptr;  // optional (debug layer / graphics debuggers)
};

enum TextureUsage : std::uint8_t { kTextureSampled = 1, kTextureRenderTarget = 2, kTextureDepthStencil = 4 };
struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;      // >= 1; all levels must be provided at creation if data is given
    Format format = Format::Unknown;
    std::uint8_t usage = kTextureSampled;  // TextureUsage bits
    const char* debugName = nullptr;
};
// One mip level of initial/updated texture data (tightly packed rows allowed: rowPitch = RowPitch()).
struct SubresourceData {
    const void* data = nullptr;
    std::uint32_t rowPitch = 0;
    std::uint32_t slicePitch = 0;
};

// CPU image (screenshots, read-backs, tests): one level, rows of rowPitch bytes.
struct Image {
    Format format = Format::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowPitch = 0;
    std::vector<std::uint8_t> pixels;
};

// ---- samplers --------------------------------------------------------------------------------------

enum class Filter : std::uint8_t { Point, Linear };
enum class MipFilter : std::uint8_t { None, Point, Linear };  // None = sample level 0 only
enum class AddressMode : std::uint8_t { Wrap, Clamp, Mirror, Border };
struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    MipFilter mipFilter = MipFilter::None;
    AddressMode addressU = AddressMode::Wrap;
    AddressMode addressV = AddressMode::Wrap;
    AddressMode addressW = AddressMode::Wrap;
    std::uint8_t maxAnisotropy = 1;   // 1 = off
    float mipLodBias = 0.0f;
    float maxLod = 1000.0f;           // highest mip level that may be sampled (0 = level 0 only)
    friend bool operator==(const SamplerDesc&, const SamplerDesc&) = default;
};

// ---- fixed-function pipeline state -------------------------------------------------------------------

// Constant/InvConstant = the dynamic blend factor (CommandContext::SetBlendFactor).
enum class BlendFactor : std::uint8_t {
    Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DestAlpha, InvDestAlpha, DestColor,
    InvDestColor, SrcAlphaSat, Constant, InvConstant
};
enum class BlendOp : std::uint8_t { Add, Subtract, RevSubtract, Min, Max };
struct BlendDesc {
    bool enable = false;
    BlendFactor src = BlendFactor::One;
    BlendFactor dst = BlendFactor::Zero;
    BlendOp op = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::Zero;
    BlendOp opAlpha = BlendOp::Add;
    std::uint8_t writeMask = 0xF;     // bit 0 R, 1 G, 2 B, 3 A
    friend bool operator==(const BlendDesc&, const BlendDesc&) = default;
};

enum class CompareFunc : std::uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class StencilOp : std::uint8_t { Keep, Zero, Replace, IncrSat, DecrSat, Invert, Incr, Decr };
// One stencil face description for both faces (NiStencilProperty has no two-sided stencil).
struct DepthStencilDesc {
    bool depthEnable = true;          // false: no depth test and no depth write
    bool depthWrite = true;
    CompareFunc depthFunc = CompareFunc::LessEqual;
    bool stencilEnable = false;
    std::uint8_t stencilReadMask = 0xFF;
    std::uint8_t stencilWriteMask = 0xFF;
    CompareFunc stencilFunc = CompareFunc::Always;
    StencilOp stencilFail = StencilOp::Keep;
    StencilOp stencilDepthFail = StencilOp::Keep;
    StencilOp stencilPass = StencilOp::Keep;
    friend bool operator==(const DepthStencilDesc&, const DepthStencilDesc&) = default;
};

// D3D8 cull semantics: CW culls triangles whose screen-space vertex order is clockwise (D3DCULL_CW,
// Xbox value 0x900), CCW culls counter-clockwise ones (0x901). With the D3D-style matrices of
// gfx_math.h the screen winding is the same in D3D8 and D3D11 (D3D11 backend: CW -> FrontCounterClockwise
// = TRUE + CULL_BACK; CCW -> FrontCounterClockwise = FALSE + CULL_BACK).
enum class CullMode : std::uint8_t { None, CW, CCW };
enum class FillMode : std::uint8_t { Solid, Wireframe };
struct RasterDesc {
    CullMode cull = CullMode::None;
    FillMode fill = FillMode::Solid;
    bool scissorEnable = false;
    bool depthClip = true;
    std::int32_t depthBias = 0;
    float slopeScaledDepthBias = 0.0f;
    friend bool operator==(const RasterDesc&, const RasterDesc&) = default;
};

enum class PrimitiveTopology : std::uint8_t {
    TriangleList,
    TriangleStrip,  // 16-bit index 0xFFFF restarts the strip (kStripCut)
    LineList,
    LineStrip,
    PointList
};
inline constexpr std::uint16_t kStripCut = 0xFFFF;

enum class VertexSemantic : std::uint8_t { Position, Normal, Color, TexCoord };
inline constexpr std::uint32_t kMaxVertexStreams = 8;
inline constexpr std::uint32_t kMaxVertexAttributes = 8;
struct VertexAttribute {
    VertexSemantic semantic = VertexSemantic::Position;
    std::uint8_t semanticIndex = 0;   // TEXCOORD0..3, COLOR0..1
    VertexFormat format = VertexFormat::Float3;
    std::uint8_t stream = 0;          // vertex buffer slot
    std::uint16_t offset = 0;         // bytes inside the stream's vertex
    friend bool operator==(const VertexAttribute&, const VertexAttribute&) = default;
};
struct VertexLayoutDesc {
    VertexAttribute attributes[kMaxVertexAttributes] = {};
    std::uint8_t attributeCount = 0;
    // Default stride per stream; SetVertexBuffer passes the actual stride (0 is allowed: every vertex
    // reads the same element, used for absent attributes).
    std::uint16_t streamStrides[kMaxVertexStreams] = {};
    friend bool operator==(const VertexLayoutDesc&, const VertexLayoutDesc&) = default;
};

enum class ShaderStage : std::uint8_t { Vertex, Pixel };
enum ShaderStageBits : std::uint8_t { kStageVertex = 1, kStagePixel = 2 };
enum class ShaderFormat : std::uint8_t { Dxbc, Dxil, SpirV };
struct ShaderBytecode {
    ShaderFormat format = ShaderFormat::Dxbc;
    const void* data = nullptr;
    std::size_t size = 0;
};

// Everything a D3D12/Vulkan pipeline object needs. The D3D11 backend maps it to {VS, PS, input layout,
// blend, depth-stencil, rasterizer state, topology}; GetPipeline hash-caches it.
struct PipelineDesc {
    ShaderHandle vertexShader;
    ShaderHandle pixelShader;
    VertexLayoutDesc layout;
    BlendDesc blend;
    DepthStencilDesc depthStencil;
    RasterDesc raster;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    Format colorFormat = Format::BGRA8_UNorm;
    Format depthFormat = Format::D24_UNorm_S8_UInt;
    friend bool operator==(const PipelineDesc&, const PipelineDesc&) = default;
};

// Hashes of the descriptors (rhi.cpp; equal descriptors -> equal hashes) for the backend caches.
std::size_t HashOf(const SamplerDesc& desc) noexcept;
std::size_t HashOf(const PipelineDesc& desc) noexcept;

// ---- commands ----------------------------------------------------------------------------------------

struct Viewport {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    float minDepth = 0.0f, maxDepth = 1.0f;
};
struct Rect {  // pixels, right/bottom exclusive
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};

enum ClearFlags : std::uint8_t { kClearColor = 1, kClearDepth = 2, kClearStencil = 4 };

enum class TransientKind : std::uint8_t { Vertex, Index };
// CPU-writable range of a per-frame ring buffer (BeginTransient .. EndTransient). Valid until the next
// Present; the data must be written before EndTransient and is then bound with SetVertexBuffer /
// SetIndexBuffer(buffer, offset).
struct TransientSpan {
    BufferHandle buffer;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    void* cpu = nullptr;  // nullptr = allocation failed
};

class CommandContext {
public:
    virtual ~CommandContext() = default;

    // color = {} -> the current back buffer (DeviceDesc: one of `bufferCount` persistent offscreen
    // buffers, see Device::Present) with its depth buffer when depth = {}; a texture target uses `depth`
    // only if given.
    virtual void SetRenderTargets(TextureHandle color, TextureHandle depth) noexcept = 0;
    virtual void SetViewport(const Viewport& viewport) noexcept = 0;
    virtual void SetScissor(const Rect& rect) noexcept = 0;
    // ClearFlags; rect == nullptr clears the whole target (D3DDevice_Clear with 0 rects), else only the
    // rectangle (ClearBuffer with an NiRect, renderer.md 2 slot 17).
    virtual void Clear(std::uint8_t flags, const float color[4], float depth, std::uint8_t stencil,
                       const Rect* rect) noexcept = 0;

    virtual void SetPipeline(PipelineHandle pipeline) noexcept = 0;
    virtual void SetStencilRef(std::uint8_t ref) noexcept = 0;
    virtual void SetBlendFactor(const float factor[4]) noexcept = 0;
    virtual void SetVertexBuffer(std::uint32_t stream, BufferHandle buffer, std::uint32_t offset,
                                 std::uint32_t stride) noexcept = 0;
    // 16-bit indices only (NetImmerse uses unsigned short everywhere).
    virtual void SetIndexBuffer(BufferHandle buffer, std::uint32_t offset) noexcept = 0;
    virtual void SetConstantBuffer(std::uint8_t stages /*ShaderStageBits*/, std::uint32_t slot,
                                   BufferHandle buffer) noexcept = 0;
    virtual void SetTexture(std::uint32_t slot, TextureHandle texture) noexcept = 0;  // pixel shader t#
    virtual void SetSampler(std::uint32_t slot, SamplerHandle sampler) noexcept = 0;  // pixel shader s#

    virtual void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) noexcept = 0;
    virtual void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::int32_t baseVertex) noexcept = 0;

    // Whole-buffer update of a Dynamic buffer (discard semantics), e.g. constant buffers per draw.
    virtual bool UpdateBuffer(BufferHandle buffer, const void* data, std::uint32_t size) noexcept = 0;
    // Per-frame ring allocations for vertex/index data that changes every draw (guest arrays, skinned or
    // converted vertices). One span may be open at a time.
    virtual TransientSpan BeginTransient(TransientKind kind, std::uint32_t size, std::uint32_t alignment) noexcept = 0;
    virtual void EndTransient() noexcept = 0;
};

// ---- device ------------------------------------------------------------------------------------------

enum class Backend : std::uint8_t { Null, D3D11 };
const char* BackendName(Backend backend) noexcept;

// How Present maps the back buffer (DeviceDesc size, 640x480) onto the window ([render] scaling, review M3
// finding 11): Linear = bilinear to the largest 4:3 rectangle of the client area, Point = nearest texel
// to the same rectangle, Integer = nearest texel with the largest whole factor that fits, centered with
// black borders (falls back to Point when even factor 1 does not fit).
enum class PresentScaling : std::uint8_t { Linear, Point, Integer };
const char* PresentScalingName(PresentScaling scaling) noexcept;

struct DeviceDesc {
    Backend backend = Backend::D3D11;
    void* nativeWindow = nullptr;     // HWND; nullptr = headless device (no swap chain)
    std::uint32_t width = 640;        // back buffer size (= NiXBoxRenderer::Create size); Present scales it
    std::uint32_t height = 480;       // to the window's client area (`scaling`)
    // Persistent back buffers in rotation (1..3; the Xbox flip with BackBufferCount 1 = 2 buffers): after
    // Present the back buffer holds what was rendered `bufferCount` presents ago (Device::Present).
    std::uint32_t bufferCount = 2;
    PresentScaling scaling = PresentScaling::Linear;
    Format backBufferFormat = Format::BGRA8_UNorm;
    Format depthFormat = Format::D24_UNorm_S8_UInt;
    bool debugLayer = false;          // request the API validation layer (D3D11: D3D11_CREATE_DEVICE_DEBUG)
    bool softwareAdapter = false;     // D3D11: WARP
};

struct DeviceInfo {
    Backend backend = Backend::Null;
    std::string adapterName;
    std::string apiVersion;           // e.g. "D3D11 feature level 11_1"
    bool softwareAdapter = false;
    bool headless = false;
    std::uint32_t backBufferWidth = 0;
    std::uint32_t backBufferHeight = 0;
    Format backBufferFormat = Format::Unknown;
    Format depthFormat = Format::Unknown;
};

enum class DebugSeverity : std::uint8_t { Info, Message, Warning, Error, Corruption };
struct DebugMessage {
    DebugSeverity severity = DebugSeverity::Info;
    std::int32_t id = 0;
    std::string text;
};
struct DebugLayerInfo {
    bool requested = false;   // DeviceDesc::debugLayer
    bool available = false;   // the layer is installed (D3D11: d3d11_3SDKLayers.dll / Graphics Tools)
    bool enabled = false;     // the device runs with it
    std::string note;         // e.g. "D3D11 SDK layers not installed (DXGI_ERROR_SDK_COMPONENT_MISSING)"
};

struct DeviceStats {
    std::uint64_t draws = 0;
    std::uint64_t primitives = 0;
    std::uint64_t presents = 0;
    std::uint64_t transientBytes = 0;
    std::uint32_t pipelines = 0;      // distinct cached pipeline states
    std::uint32_t samplers = 0;
    std::uint32_t buffers = 0;        // live
    std::uint32_t textures = 0;       // live
    std::uint64_t textureBytes = 0;   // live
    std::uint32_t debugErrors = 0;    // Error + Corruption messages so far
    std::uint32_t debugWarnings = 0;
};

enum class PresentResult : std::uint8_t { Ok, Occluded, DeviceLost, Failed };
struct PresentDesc {
    std::uint32_t syncInterval = 1;   // 0 = no vsync, 1 = every vblank ([render] vsync)
};

class Device {
public:
    virtual ~Device() = default;

    virtual const DeviceInfo& Info() const noexcept = 0;
    virtual CommandContext& Context() noexcept = 0;
    virtual ShaderFormat NativeShaderFormat() const noexcept = 0;
    // usage = TextureUsage bits.
    virtual bool IsFormatSupported(Format format, std::uint8_t usage) const noexcept = 0;

    virtual BufferHandle CreateBuffer(const BufferDesc& desc) noexcept = 0;
    // `levels` = desc.mipLevels entries (level 0 first) or nullptr for an uninitialized texture.
    virtual TextureHandle CreateTexture(const TextureDesc& desc, const SubresourceData* levels) noexcept = 0;
    virtual bool UpdateTexture(TextureHandle texture, std::uint32_t level, const SubresourceData& data) noexcept = 0;
    virtual ShaderHandle CreateShader(ShaderStage stage, const ShaderBytecode& code, const char* debugName) noexcept = 0;
    virtual void Destroy(BufferHandle buffer) noexcept = 0;
    virtual void Destroy(TextureHandle texture) noexcept = 0;
    virtual void Destroy(ShaderHandle shader) noexcept = 0;

    virtual SamplerHandle GetSampler(const SamplerDesc& desc) noexcept = 0;
    virtual PipelineHandle GetPipeline(const PipelineDesc& desc, const char* debugName) noexcept = 0;

    // Shows the current back buffer (vsync per desc) and moves on to the next of the `bufferCount`
    // persistent back buffers, whose contents stay defined: like the Xbox flip (D3DSWAPEFFECT_DISCARD with
    // BackBufferCount 1 flips two buffers, renderer.md 3.1), a frame that draws nothing shows the picture of
    // `bufferCount` presents ago (review M3 findings 2/8). With a window the picture is copied into the swap
    // chain (window size, DeviceDesc::scaling); the swap chain itself is redrawn completely every Present.
    // The caller paces to 50 Hz separately (platform/frame_pacer.h).
    virtual PresentResult Present(const PresentDesc& desc) noexcept = 0;
    // Copy of the current back buffer (BGRA8_UNorm, DeviceDesc size, independent of the window size). Used
    // by the screenshot key and [render] screenshot_frames before Present.
    virtual bool CaptureBackBuffer(Image& out) noexcept = 0;
    virtual bool ReadTexture(TextureHandle texture, std::uint32_t level, Image& out) noexcept = 0;

    virtual DebugLayerInfo DebugLayer() const noexcept = 0;
    // Moves the queued validation messages to `out` (appends), returns their number.
    virtual std::size_t DrainDebugMessages(std::vector<DebugMessage>& out) noexcept = 0;
    virtual DeviceStats Stats() const noexcept = 0;
    virtual const char* LastError() const noexcept = 0;
};

// Creates a device for desc.backend (Null -> nullptr with error "null backend"). On failure returns
// nullptr and, if `error` is given, the reason. Implemented in rhi.cpp (dispatch) + the backend.
std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc, std::string* error) noexcept;

}  // namespace gfx::rhi
