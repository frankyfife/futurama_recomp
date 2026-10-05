// Phase-1 stub shaders (GFX_CONTRACT.md 5.4/6.5): a pass-through vertex shader (POSITION *
// worldViewProjection, COLOR0) and a vertex-color pixel shader with an optional stage-0 texture. They are
// kept unchanged for the RHI tests of the backend agent (gen_shaders/ff_stub_vs.h / ff_stub_ps.h, also
// reachable through gfx::ff::StubShaderBytecode). The real fixed-function emulation is ff_ubershader.hlsl.
#include "ff_shared.h"

cbuffer FFFrame : register(b0)
{
    FFFrameConstants g_frame;
};
cbuffer FFDraw : register(b1)
{
    FFDrawConstants g_draw;
};
cbuffer FFLights : register(b2)
{
    FFLightConstants g_lights;
};
cbuffer FFStages : register(b3)
{
    FFStageConstants g_stages;
};

Texture2D g_texture0 : register(t0);
SamplerState g_sampler0 : register(s0);

struct FFVertexInput
{
    float3 position : POSITION;   // stream FF_STREAM_POSITION
    float3 normal : NORMAL;       // FF_STREAM_NORMAL
    float4 color : COLOR0;        // FF_STREAM_COLOR
    float2 uv0 : TEXCOORD0;       // FF_STREAM_UV0 ..
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float2 uv3 : TEXCOORD3;
};

struct FFVertexOutput
{
    float4 position : SV_Position;
    float4 diffuse : COLOR0;
    float4 specular : COLOR1;
    float2 uv0 : TEXCOORD0;
};

FFVertexOutput VSMain(FFVertexInput input)
{
    FFVertexOutput o;
    o.position = mul(float4(input.position, 1.0f), g_draw.worldViewProjection);
    o.diffuse = saturate(input.color);
    o.specular = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.uv0 = input.uv0;
    return o;
}

float4 PSMain(FFVertexOutput input) : SV_Target
{
    float4 color = input.diffuse;
    if (g_stages.stages[0].coord.w != 0)
    {
        color *= g_texture0.Sample(g_sampler0, input.uv0);
    }
    return color;
}
