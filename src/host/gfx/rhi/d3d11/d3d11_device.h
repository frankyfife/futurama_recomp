// D3D11 backend of the RHI (gfx/rhi/rhi.h; GFX_CONTRACT.md 5). Owned by the backend agent (M3 phase 2).
//
// Device (d3d11_device.cpp): feature level 11_1 / 11_0 / 10_1 / 10_0 (shaders are SM 4.0 DXBC, compiled
// at build time by fxc), BGRA support, the D3D11.1 runtime (ID3D11DeviceContext1::ClearView for rectangle
// color clears). Adapter: DeviceDesc::softwareAdapter -> WARP (IDXGIFactory4::EnumWarpAdapter), else the
// high-performance adapter (IDXGIFactory6) or adapter 0.
//
// Window (DeviceDesc::nativeWindow, an HWND of another thread - platform/window.h): DXGI flip-model swap
// chain (FLIP_DISCARD, fallback FLIP_SEQUENTIAL), DeviceDesc::bufferCount buffers, B8G8R8A8_UNORM,
// DXGI_SCALING_STRETCH of the fixed back buffer (DeviceDesc size = NiXBoxRenderer::Create size, 640x480)
// to the window's client area, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER (the window handles
// Alt+Enter itself), DXGI_PRESENT_ALLOW_TEARING for sync interval 0 when supported. Present re-binds the
// render targets (flip model unbinds the back buffer). Headless (nativeWindow == nullptr): an offscreen
// BGRA8 color target + depth of the same size; Present counts, drains the debug messages and resets the
// transient ring.
//
// Debug layer: DeviceDesc::debugLayer -> DXGI_CREATE_FACTORY_DEBUG + D3D11_CREATE_DEVICE_DEBUG. If the SDK
// layers are not installed (DXGI_ERROR_SDK_COMPONENT_MISSING) the device is created without them and
// DebugLayer().note says so. Messages of ID3D11InfoQueue and of the DXGI producer of IDXGIInfoQueue
// (severity warning and above; info/message are filtered at the source, no break on severity) are
// pulled after every Present, by DrainDebugMessages and when an API call fails, and the backend LOGS
// THEM ITSELF on the GFX channel:
//   "D3D11 debug: <corruption|error|warning|info|message> #<id>: [DXGI ]<text>"
// (GFX/error for corruption and error, GFX/warn for warning, GFX/debug otherwise; at most 10 lines per
// message id, then one "further ... suppressed" line - every message is still counted). DrainDebugMessages
// additionally hands them to the caller (the last 256), which must not log them again. Counts go to
// DeviceStats (debugErrors = error + corruption) and to the process-wide totals below; the first device
// created for a window registers the exit reporter "d3d11" (core::AddExitReporter) that logs the M3 exit
// criterion 2 line (GFX/info):
//   "D3D11 debug layer: <enabled|not installed|off|unavailable> - N errors, M warnings, K corruption"
//
// Robustness: every failed API call sets LastError() ("<call> failed: 0x887A0005 DXGI_ERROR_DEVICE_REMOVED
// (reason ...)") and returns an invalid handle / false; Present maps DEVICE_REMOVED/RESET to
// PresentResult::DeviceLost (the caller logs GFX/error and ends with core::Fatal). Draws without a valid
// pipeline are skipped (no D3D call, LastError set) so they cannot produce debug-layer errors. Teardown
// (destructor): ClearState + Flush, releases every object, then (debug layer) ReportLiveDeviceObjects -
// LastTeardownLiveObjects() reports what was still alive.
//
// x87/SSE: the backend does not depend on the x87 control word the guest may have changed (host code is
// SSE2; D3D11/DXGI do not rely on x87 precision). No save/restore of _controlfp is done; none was needed
// in the M3 runs.
#pragma once

#include "gfx/rhi/rhi.h"

#include <cstdint>
#include <memory>
#include <string>

namespace gfx::rhi::d3d11 {

// Creates the D3D11 device, context and (with a window) the swap chain. nullptr + error on failure.
std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc, std::string* error) noexcept;

// ---- built-in shaders (gfx/rhi/d3d11/d3d11_builtin.hlsl, compiled by fxc at build time) ---------------

enum class BuiltinShader : std::uint8_t {
    ClearVS,        // rectangle depth/stencil clear (internal use of CommandContext::Clear)
    DiagVS,         // diagnostic pass-through: POSITION float3 (clip space), COLOR0 float4, TEXCOORD0 float2
    DiagColorPS,    // interpolated color
    DiagTexturePS,  // t0/s0 sample * color
    DiagTintPS,     // color * cbuffer b0 float4 tint
};
ShaderBytecode BuiltinShaderCode(BuiltinShader shader) noexcept;
// One interleaved stream (slot 0, stride kDiagVertexStride): POSITION float3 at 0, COLOR0 float4 at 12,
// TEXCOORD0 float2 at 28 - the input of DiagVS.
inline constexpr std::uint32_t kDiagVertexStride = 36;
VertexLayoutDesc DiagVertexLayout() noexcept;

// ---- process-wide debug-layer totals (M3 exit criterion 2) ----------------------------------------------

struct DebugLayerTotals {
    DebugLayerInfo layer;         // of the most recently created device that has a window (else the last)
    std::uint32_t devices = 0;    // devices created
    std::uint32_t errors = 0;     // D3D11/DXGI messages of severity error, all devices
    std::uint32_t warnings = 0;
    std::uint32_t corruption = 0;
    std::uint32_t other = 0;      // info + message (normally filtered at the source)
};
DebugLayerTotals GetDebugLayerTotals() noexcept;
// Logs "D3D11 debug layer: <state> - N errors, M warnings, K corruption" (GFX/info; GFX/error if any
// error or corruption was seen). The exit reporter "d3d11" calls it.
void LogDebugLayerSummary() noexcept;
// Text of the state for the summary/Create lines: "enabled", "not installed", "off", "unavailable".
const char* DebugLayerStateName(const DebugLayerInfo& layer) noexcept;

// Objects the debug layer still reported alive (Refcount > 0, the device itself excluded) when the most
// recent device with an enabled debug layer was destroyed; -1 = never measured. Tests check it is 0.
int LastTeardownLiveObjects() noexcept;

// ---- pure translations (tests/rhi_d3d11_tests.cpp) --------------------------------------------------------

// DXGI_FORMAT used for sampling/render-target views of `format` (DXGI_FORMAT_UNKNOWN = 0 for Unknown/Count).
std::uint32_t DxgiFormatOf(Format format) noexcept;
// IDXGISwapChain::Present HRESULT -> PresentResult (DEVICE_REMOVED/RESET/HUNG/DRIVER_INTERNAL_ERROR ->
// DeviceLost, DXGI_STATUS_OCCLUDED -> Occluded, other success codes -> Ok, other failures -> Failed).
PresentResult PresentResultFromHresult(long hr) noexcept;
// "0x887A0005 DXGI_ERROR_DEVICE_REMOVED" (name only for well-known codes).
std::string HresultText(long hr);

}  // namespace gfx::rhi::d3d11
