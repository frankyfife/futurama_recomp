// Built-in shaders of the D3D11 backend (gfx/rhi/d3d11/d3d11_device.h, d3d11::BuiltinShaderCode). Compiled
// at build time by fxc (fut_add_shader in src/host/CMakeLists.txt, block "backend agent") into
// gen_shaders/d3d11_*.h; shader model 4.0 (feature level 10_0).
//
//   ClearVS        rectangle depth/stencil clear (CommandContext::Clear with a rect): a triangle that covers
//                  the whole viewport at z = 0; the backend sets the viewport to the rectangle with
//                  MinDepth = MaxDepth = the clear depth, so no constant buffer is needed (no pixel shader).
//   DiagVS         diagnostic pass-through: POSITION float3 (clip space), COLOR0 float4, TEXCOORD0 float2
//   DiagColorPS    interpolated color
//   DiagTexturePS  t0/s0 sample * color
//   DiagTintPS     color * b0.tint (constant buffer test)
//   PresentVS      present pass (review M3 findings 2/8/11): a triangle over the viewport (= destination
//                  rectangle in the swap chain) with texture coordinates 0..1 over that rectangle
//   PresentPS      t0/s0 sample of the offscreen back buffer (point or linear sampler, [render] scaling)
// The Diag* shaders are used by the headless unit tests (tests/rhi_d3d11_tests.cpp) and are available
// to host diagnostics; the game's draws use the fixed-function shaders of gfx/shaders/.

float4 ClearVS(uint id : SV_VertexID) : SV_Position
{
    // id 0 -> (-1, 1), 1 -> (3, 1), 2 -> (-1, -3): one triangle that covers the clip square.
    const float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

struct DiagVertexInput
{
    float3 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct DiagVertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

DiagVertexOutput DiagVS(DiagVertexInput input)
{
    DiagVertexOutput o;
    o.position = float4(input.position, 1.0f);
    o.color = input.color;
    o.uv = input.uv;
    return o;
}

float4 DiagColorPS(DiagVertexOutput input) : SV_Target
{
    return input.color;
}

Texture2D g_diagTexture : register(t0);
SamplerState g_diagSampler : register(s0);

float4 DiagTexturePS(DiagVertexOutput input) : SV_Target
{
    return g_diagTexture.Sample(g_diagSampler, input.uv) * input.color;
}

cbuffer DiagConstants : register(b0)
{
    float4 g_diagTint;
};

float4 DiagTintPS(DiagVertexOutput input) : SV_Target
{
    return input.color * g_diagTint;
}

struct PresentOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

PresentOutput PresentVS(uint id : SV_VertexID)
{
    // Same triangle as ClearVS; uv (0, 0) top-left .. (1, 1) bottom-right of the viewport.
    const float2 t = float2((id << 1) & 2, id & 2);
    PresentOutput o;
    o.position = float4(t * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.uv = t;
    return o;
}

Texture2D g_presentSource : register(t0);
SamplerState g_presentSampler : register(s0);

float4 PresentPS(PresentOutput input) : SV_Target
{
    return float4(g_presentSource.Sample(g_presentSampler, input.uv).rgb, 1.0f);
}
