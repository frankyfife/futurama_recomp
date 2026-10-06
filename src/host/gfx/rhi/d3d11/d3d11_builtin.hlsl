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
//   ResamplePS     (M3c) box / Catmull-Rom downsample with b0 ResampleConstants (render_scale, an image
//                  larger than the window, back-buffer resize)
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

// ---- M3c: downsample (supersampling resolve of [render] render_scale, present of an image larger than the
// window, carry-over of the back buffers on a resize). Drawn with PresentVS over the destination viewport.
//   mode 0 (box): the source/destination ratio is a whole number k per axis - every destination pixel is the
//          plain average of exactly its k x k source pixels (render_scale 2: 2 x 2 = an exact 4x SSAA resolve).
//   mode 1 (Catmull-Rom, B = 0, C = 1/2, with anti-ringing): fractional ratios (render_scale 1.25 / 1.5 / 1.75,
//          non-integer window fits). The cubic is stretched by the ratio (support +-2 destination pixels =
//          +-2 * ratio source pixels), so it low-passes the source before decimation; weights are normalized per
//          pixel. The result is clamped to the range of the 2 x 2 source pixels nearest to the destination center:
//          no halo from the negative lobes. Review M3c finding 1: the Mitchell-Netravali filter used before
//          (B = C = 1/3) was visibly softer. Measured offline against the render_scale 2 box reference
//          (docs/m3c_report.md): Catmull-Rom with the clamp has about 40 % less edge error at 1.5 (legal screen
//          8.8 -> 5.0, frontend 3.2 -> 2.1), nearly the reference's sharpness and no overshoot.
// Loads only (no sampler): exact texel addressing, clamped at the image border. With [render] linear_aa the
// source and destination views are sRGB, so the filter runs on linear light (the hardware decodes and encodes).
cbuffer ResampleConstants : register(b0)
{
    float4 g_resampleSize;    // x, y = source size in pixels
    float4 g_resampleRatio;   // x, y = source pixels per destination pixel; z = mode (0 box, 1 Catmull-Rom)
    float4 g_resampleOrigin;  // x, y = top-left of the destination viewport in render-target pixels
};
Texture2D g_resampleSource : register(t0);

float CatmullRom(float x)
{
    const float a = abs(x);
    const float a2 = a * a;
    const float a3 = a2 * a;
    float w = 0.0f;
    if (a < 1.0f)
    {
        w = 1.5f * a3 - 2.5f * a2 + 1.0f;
    }
    else if (a < 2.0f)
    {
        w = -0.5f * a3 + 2.5f * a2 - 4.0f * a + 2.0f;
    }
    return w;
}

float4 ResamplePS(PresentOutput input) : SV_Target
{
    const int2 size = int2(g_resampleSize.xy);
    const float2 ratio = g_resampleRatio.xy;
    // Destination pixel center (i + 0.5) relative to the viewport.
    const float2 destination = input.position.xy - g_resampleOrigin.xy;
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (g_resampleRatio.z == 0.0f)
    {
        const int2 k = int2(ratio + 0.5f);
        const int2 first = int2(floor(destination)) * k;
        [loop]
        for (int y = 0; y < k.y; ++y)
        {
            [loop]
            for (int x = 0; x < k.x; ++x)
            {
                sum += g_resampleSource.Load(int3(min(first + int2(x, y), size - 1), 0));
            }
        }
        sum /= float(k.x * k.y);
    }
    else
    {
        const float2 center = destination * ratio;  // source coordinate of the destination pixel center
        const int2 lo = max(int2(floor(center - 2.0f * ratio)), int2(0, 0));
        const int2 hi = min(int2(ceil(center + 2.0f * ratio)), size - 1);
        float weightSum = 0.0f;
        [loop]
        for (int y = lo.y; y <= hi.y; ++y)
        {
            const float wy = CatmullRom((float(y) + 0.5f - center.y) / ratio.y);
            [loop]
            for (int x = lo.x; x <= hi.x; ++x)
            {
                const float w = wy * CatmullRom((float(x) + 0.5f - center.x) / ratio.x);
                sum += w * g_resampleSource.Load(int3(x, y, 0));
                weightSum += w;
            }
        }
        sum = weightSum != 0.0f ? sum / weightSum : sum;
        // Anti-ringing: the range of the 2 x 2 source pixels around the center.
        const int2 n0 = clamp(int2(floor(center - 0.5f)), int2(0, 0), size - 1);
        const int2 n1 = min(n0 + 1, size - 1);
        const float4 a = g_resampleSource.Load(int3(n0.x, n0.y, 0));
        const float4 b = g_resampleSource.Load(int3(n1.x, n0.y, 0));
        const float4 c = g_resampleSource.Load(int3(n0.x, n1.y, 0));
        const float4 d = g_resampleSource.Load(int3(n1.x, n1.y, 0));
        sum = clamp(sum, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
    }
    return saturate(sum);
}
