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
    // M3c (add-only): alpha-to-coverage - the pixel shader's output alpha selects the covered samples of a
    // multisampled target (D3D11_BLEND_DESC::AlphaToCoverageEnable). Only meaningful with MSAA.
    bool alphaToCoverage = false;
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
    // M3c (add-only): the target is multisampled (D3D11 MultisampleEnable: MSAA rasterization of lines and on
    // feature level 10_0; triangles on 10_1+ are multisampled on an MSAA target anyway).
    bool multisample = false;
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
// M3c: the source is the presented image (DeviceDesc::outputWidth x outputHeight, else the back buffer size).
// When it has exactly the size of the destination rectangle it is copied 1:1 (point); when it is LARGER than
// the destination (the image does not fit the window) it is downsampled with the same filter as the
// supersampling downsample (box for an integer ratio, Catmull-Rom with anti-ringing otherwise) instead of
// bilinear taps.
enum class PresentScaling : std::uint8_t { Linear, Point, Integer };
const char* PresentScalingName(PresentScaling scaling) noexcept;

struct DeviceDesc {
    Backend backend = Backend::D3D11;
    void* nativeWindow = nullptr;     // HWND; nullptr = headless device (no swap chain)
    std::uint32_t width = 640;        // back buffer size (M3: = NiXBoxRenderer::Create size; M3c: the internal
    std::uint32_t height = 480;       // render resolution); Present scales it to the window's client area (`scaling`)
    // Persistent back buffers in rotation (1..3; the Xbox flip with BackBufferCount 1 = 2 buffers): after
    // Present the back buffer holds what was rendered `bufferCount` presents ago (Device::Present).
    std::uint32_t bufferCount = 2;
    PresentScaling scaling = PresentScaling::Linear;
    Format backBufferFormat = Format::BGRA8_UNorm;
    Format depthFormat = Format::D24_UNorm_S8_UInt;
    bool debugLayer = false;          // request the API validation layer (D3D11: D3D11_CREATE_DEVICE_DEBUG)
    bool softwareAdapter = false;     // D3D11: WARP
    // M3b (display-coupled pacing, platform/frame_pacer.h): 1..3 = the swap chain gets a frame-latency
    // waitable object (Device::FrameLatencyWaitable) with this maximum frame latency; 0 = none (the device-wide
    // frame latency 2 of M3; Present itself blocks when the queue is full). Ignored without a window.
    std::uint32_t frameLatencyWaitable = 0;
    // M3c (render resolution and anti-aliasing, add-only; defaults = the M3 behaviour):
    // Samples per pixel of the persistent back buffers and their depth buffer (1 = no MSAA; 2/4/8 are clamped
    // to what the device supports for backBufferFormat + depthFormat, DeviceInfo::sampleCount). Every
    // persistent back buffer is multisampled, so a frame that draws nothing still continues on the samples of
    // `bufferCount` presents ago (Xbox flip); Present resolves the current one.
    std::uint32_t sampleCount = 1;
    // Size of the image Present shows (and CaptureOutput returns): the resolved back buffer (width x height) is
    // downsampled to it - exact box filter for an integer ratio, Catmull-Rom with anti-ringing otherwise
    // (supersampling, [render] render_scale). 0 x 0 = the back buffer size (no downsample).
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    // Review M3c finding 3 ([render] linear_aa): the MSAA resolve and the downsample average linear light
    // (sRGB decode -> filter -> sRGB encode) instead of the gamma-encoded values (what the Xbox and the M3c
    // default do). Rendering and blending into the back buffers stay unchanged (UNORM views), and so does the
    // present pass when it copies 1:1 or enlarges. Needs a back buffer format with an sRGB variant.
    bool linearResolve = false;
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
    // M3b (pacing): Present may use PresentDesc::allowTearing (D3D11: DXGI_FEATURE_PRESENT_ALLOW_TEARING and a
    // swap chain created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING); maximum frame latency of the waitable
    // object (0 = no waitable object).
    bool tearingSupported = false;
    std::uint32_t frameLatency = 0;
    // M3c: samples per pixel of the back buffers (after clamping DeviceDesc::sampleCount) and the size of the
    // presented image (= the back buffer size without a downsample).
    std::uint32_t sampleCount = 1;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    bool linearResolve = false;  // DeviceDesc::linearResolve in effect
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
    // M3c (add-only): GPU time per frame from timestamp queries (start of the frame after the previous Present
    // to the end of the present pass), read back a few frames later; 0 frames = not measured (no support or
    // disjoint timestamps).
    std::uint64_t gpuFramesTimed = 0;
    double gpuFrameMsAverage = 0.0;
    double gpuFrameMsP99 = 0.0;
    double gpuFrameMsMax = 0.0;
};

enum class PresentResult : std::uint8_t { Ok, Occluded, DeviceLost, Failed };
struct PresentDesc {
    std::uint32_t syncInterval = 1;   // 0 = no vsync, 1..4 = show for (at least) N vblanks (DXGI semantics)
    // M3b (display-coupled pacing, platform/frame_pacer.h; add-only):
    bool allowTearing = false;        // DXGI_PRESENT_ALLOW_TEARING; only with syncInterval 0 and
                                      // DeviceInfo::tearingSupported (ignored otherwise). Before M3b the backend
                                      // set it implicitly for syncInterval 0.
    bool skipSwapChain = false;       // do not present into the window (minimized, display off, no free slot):
                                      // only the persistent back buffers rotate (Xbox flip semantics kept)
};

// M3b: what the swap chain reported after Present (Device::LastPresentTiming). The frame statistics
// (DXGI_FRAME_STATISTICS) describe the newest present that became visible, which is usually one or two
// presents older than this one.
struct PresentTiming {
    bool swapChainPresented = false;  // this Present reached the swap chain (not skipped, no failure)
    bool occluded = false;            // DXGI_STATUS_OCCLUDED
    std::uint32_t presentId = 0;      // IDXGISwapChain::GetLastPresentCount after this Present
    bool statsValid = false;          // IDXGISwapChain::GetFrameStatistics succeeded
    long statsError = 0;              // its HRESULT otherwise (e.g. 0x887A000B DXGI_ERROR_FRAME_STATISTICS_DISJOINT)
    std::uint32_t statPresentCount = 0;         // present id of the newest visible present
    std::uint32_t statPresentRefreshCount = 0;
    std::uint32_t statSyncRefreshCount = 0;     // vblank count at which it became visible
    std::int64_t statSyncQpc = 0;               // QPC time of that vblank
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
    // The caller paces separately (platform/frame_pacer.h: PlanPresent chooses desc, EndFrame waits).
    virtual PresentResult Present(const PresentDesc& desc) noexcept = 0;
    // M3b (display-coupled pacing; add-only, defaults for backends without a swap chain):
    // The swap chain's frame-latency waitable object (a Win32 HANDLE owned by the device; DeviceDesc::
    // frameLatencyWaitable), nullptr if there is none. It is signaled when the swap chain can take the next
    // frame; whoever presents must wait on it (consume one signal) once per swap-chain Present.
    virtual void* FrameLatencyWaitable() const noexcept { return nullptr; }
    // Result and frame statistics of the most recent Present.
    virtual PresentTiming LastPresentTiming() const noexcept { return {}; }
    // Copy of the current back buffer (BGRA8_UNorm, DeviceDesc size, independent of the window size). Used
    // by the screenshot key and [render] screenshot_frames before Present. M3c: a multisampled back buffer is
    // resolved first (the picture at the internal render resolution).
    virtual bool CaptureBackBuffer(Image& out) noexcept = 0;
    // M3c (add-only, defaults for backends without a downsample): the image Present would show for the current
    // back buffer - resolved and downsampled to DeviceInfo::outputWidth x outputHeight ([render] screenshot_size
    // = output). Call before Present.
    virtual bool CaptureOutput(Image& out) noexcept { return CaptureBackBuffer(out); }
    // M3d F1 (add-only; default false = not supported, Present behaves as usual): until the next Present the
    // persistent back buffer shown by the PREVIOUS Present stands for the current one - the present pass,
    // CaptureBackBuffer and CaptureOutput read it - and that Present keeps the rotation (the current back buffer
    // stays current for the next frame). For a frame between two logic ticks that drew nothing (the pacer's
    // PresentRequest::repeatPrevious): the swap chain still gets one Present per frame, so the frame-latency
    // waitable keeps its rhythm of one signal per Present on the vblank plans, and the screen shows the previous
    // picture for one more frame. True if the device will do so.
    virtual bool RepeatPreviousPicture() noexcept { return false; }
    // Highest sample count <= `requested` (1, 2, 4, 8) that the device supports for a render target of `color`
    // together with a depth buffer of `depth` (Format::Unknown = no depth); 1 = no multisampling.
    virtual std::uint32_t SupportedSampleCount(Format color, Format depth, std::uint32_t requested) const noexcept {
        (void)color;
        (void)depth;
        (void)requested;
        return 1;
    }
    // Recreates the persistent back buffers, their depth buffer and the output image with a new size / sample
    // count / output size (DeviceDesc meaning; the sample count is clamped like at creation). The pictures the
    // back buffers hold are resampled into the new ones (the Xbox-flip contents survive a window resize); the
    // depth buffer is cleared to 1 / stencil 0. Binds the back buffer with a whole-target viewport. False (old
    // buffers kept) on failure.
    virtual bool ResizeBackBuffers(std::uint32_t width, std::uint32_t height, std::uint32_t sampleCount,
                                   std::uint32_t outputWidth, std::uint32_t outputHeight) noexcept {
        (void)width;
        (void)height;
        (void)sampleCount;
        (void)outputWidth;
        (void)outputHeight;
        return false;
    }
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
