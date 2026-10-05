// Fixed-function ubershader: emulation of the D3D8 fixed-function pipeline as NiXBoxRenderer (REN)
// configured it (GFX_CONTRACT.md 6, ff_pipeline.h). Compiled at build time by fxc (SM 4.0) through
// fut_add_shader (src/host/CMakeLists.txt) into gen_shaders/ff_vs_*.h / ff_ps_*.h.
//
// Permutations (FF_FEATURE_VS_COMPILE_MASK / FF_FEATURE_PS_COMPILE_MASK, ff_shared.h):
//   VSMain  FF_PERM_PRETRANSFORMED = 1   D3DFVF_XYZRHW quads of slot 55 (no transform, no lighting)
//           FF_PERM_LIGHTING = 1         per-vertex D3D8 lighting (D3DRS_LIGHTING)
//   PSMain  FF_PERM_FLAT = 1             D3DSHADE_FLAT (colors of the first vertex = D3D11 provoking vertex)
// Everything else branches uniformly on g_draw.features.x (FF_FEATURE_*).
//
// Reference semantics: D3D8 fixed function (lighting equation, texture stage combiner, alpha test, table
// fog) with the Xbox/NV2A details that REN relies on:
//   - an absent vertex color makes D3DMCS_COLOR1 fall back to the material (NV2A FUN_0023CFC0 masks the
//     COLOR1/COLOR2 bits of the material-source word when the vertex lacks diffuse/specular; PC D3D8 does
//     the same); there is no specular vertex stream, so D3DMCS_COLOR2 always means the material;
//   - fog: REN programs FOGTABLEMODE (0x244844) + start/end/density (FUN_00159700), and the Xbox D3D fog
//     setup FUN_0023D520 selects the NV2A fog coordinate per vertex - planar (view z) or radial (distance)
//     with RANGEFOGENABLE (0x244854) - and a LINEAR/EXP/EXP2 function evaluated per pixel; so does this shader;
//   - colors are floats and saturate (the original packs ROUND(c * 255) into D3DCOLORs, which wraps).
#include "ff_shared.h"

#ifndef FF_PERM_PRETRANSFORMED
#define FF_PERM_PRETRANSFORMED 0
#endif
#ifndef FF_PERM_LIGHTING
#define FF_PERM_LIGHTING 0
#endif
#ifndef FF_PERM_FLAT
#define FF_PERM_FLAT 0
#endif

#if FF_PERM_FLAT
#define FF_COLOR_INTERPOLATION nointerpolation
#else
#define FF_COLOR_INTERPOLATION
#endif

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

// Texture stages 0..3 (always bound: the pipeline binds a 1x1 white texture to unused stages).
Texture2D g_texture0 : register(t0);
Texture2D g_texture1 : register(t1);
Texture2D g_texture2 : register(t2);
Texture2D g_texture3 : register(t3);
SamplerState g_sampler0 : register(s0);
SamplerState g_sampler1 : register(s1);
SamplerState g_sampler2 : register(s2);
SamplerState g_sampler3 : register(s3);

struct FFVertexInput
{
    float3 position : POSITION;   // FF_STREAM_POSITION (pretransformed: x, y in D3D8 pixels, z depth)
    float3 normal : NORMAL;       // FF_STREAM_NORMAL (zero-stride (0,0,0) when absent)
    float4 color : COLOR0;        // FF_STREAM_COLOR (zero-stride white when absent)
    float2 uv0 : TEXCOORD0;       // FF_STREAM_UV0.. (zero-stride (0,0) when absent)
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float2 uv3 : TEXCOORD3;
};

struct FFVertexOutput
{
    float4 position : SV_Position;
    FF_COLOR_INTERPOLATION float4 diffuse : COLOR0;   // D3D8 oD0 (saturated)
    FF_COLOR_INTERPOLATION float4 specular : COLOR1;  // D3D8 oD1 (saturated, alpha 0)
    float4 tex0 : TEXCOORD0;      // stage coordinates after TEXCOORDINDEX and the texture transform
    float4 tex1 : TEXCOORD1;
    float4 tex2 : TEXCOORD2;
    float4 tex3 : TEXCOORD3;
    float fogDepth : TEXCOORD4;   // planar (view z) or radial (view distance) fog coordinate
};

bool HasFeature(uint bit)
{
    return (g_draw.features.x & bit) != 0;
}

// Normalize that keeps a zero vector (absent normals are (0,0,0); D3D8 then lights with N.L = 0).
float3 SafeNormalize(float3 v)
{
    const float lengthSquared = dot(v, v);
    // max(): both sides of ?: are evaluated (and constant-folded by fxc for a constant zero vector).
    return lengthSquared > 0.0f ? v * rsqrt(max(lengthSquared, 1.0e-30f)) : v;
}

// x^e for x > 0 (0 otherwise) without pow() (fxc X3571); e <= 0 gives 1 like pow(x, 0).
float PowPositive(float x, float e)
{
    return x > 0.0f ? exp2(log2(x) * e) : 0.0f;
}

// ---- vertex shader -----------------------------------------------------------------------------------

float2 SelectUvSet(FFVertexInput input, uint set)
{
    return set == 0 ? input.uv0 : (set == 1 ? input.uv1 : (set == 2 ? input.uv2 : input.uv3));
}

// D3DTSS_TEXCOORDINDEX (set + TCI generation) and D3DTSS_TEXTURETRANSFORMFLAGS / D3DTS_TEXTUREn.
// 2D sets enter the transform as (u, v, 1, 0), generated ones as (x, y, z, 1) (D3D8 padding rule: the
// translation of a 2D texture matrix is its third row).
float4 StageCoordinates(uint stage, FFVertexInput input, float3 viewPosition, float3 viewNormal)
{
    const uint index = g_stages.stages[stage].coord.x;
    const uint generation = index & FF_TCI_MASK;
    float4 coordinates;
    if (generation == FF_TCI_CAMERASPACENORMAL)
    {
        coordinates = float4(viewNormal, 1.0f);
    }
    else if (generation == FF_TCI_CAMERASPACEPOSITION)
    {
        coordinates = float4(viewPosition, 1.0f);
    }
    else if (generation == FF_TCI_CAMERASPACEREFLECTIONVECTOR)
    {
        coordinates = float4(reflect(SafeNormalize(viewPosition), SafeNormalize(viewNormal)), 1.0f);
    }
    else
    {
        coordinates = float4(SelectUvSet(input, index & 0xFFFF), 1.0f, 0.0f);
    }
    if ((g_stages.stages[stage].coord.y & 0xFF) != FF_TTFF_DISABLE)
    {
        coordinates = mul(coordinates, g_stages.stages[stage].transform);
    }
    return coordinates;
}

// Material color source (D3DRS_*MATERIALSOURCE): COLOR1 = vertex diffuse if the vertex has one.
float4 MaterialSource(uint source, float4 material, float4 vertexColor)
{
    return (source == FF_MCS_COLOR1 && HasFeature(FF_FEATURE_HAS_COLOR)) ? vertexColor : material;
}

// D3D8 per-vertex lighting in view space. Returns diffuse (alpha = diffuse source alpha) and specular.
void LightVertex(float3 P, float3 N, float4 vertexColor, out float4 diffuseOut, out float4 specularOut)
{
    const float4 matDiffuse = MaterialSource(g_draw.colorSources.x, g_draw.materialDiffuse, vertexColor);
    const float4 matAmbient = MaterialSource(g_draw.colorSources.y, g_draw.materialAmbient, vertexColor);
    const float4 matSpecular = MaterialSource(g_draw.colorSources.z, g_draw.materialSpecular, vertexColor);
    const float4 matEmissive = MaterialSource(g_draw.colorSources.w, g_draw.materialEmissive, vertexColor);
    const bool specularEnable = HasFeature(FF_FEATURE_SPECULAR);
    // Direction to the eye: local viewer -P (camera at the view-space origin), else the view axis.
    const float3 V = HasFeature(FF_FEATURE_LOCAL_VIEWER) ? SafeNormalize(-P) : float3(0.0f, 0.0f, -1.0f);
    const float power = g_draw.materialPower.x;

    float3 ambientSum = float3(0.0f, 0.0f, 0.0f);
    float3 diffuseSum = float3(0.0f, 0.0f, 0.0f);
    float3 specularSum = float3(0.0f, 0.0f, 0.0f);
    const uint count = min(g_draw.features.y, (uint)FF_MAX_LIGHTS);
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const FFLight light = g_lights.lights[i];
        const uint type = (uint)light.attenuation.w;
        float3 L;              // vertex -> light, normalized
        float factor = 1.0f;   // attenuation * spot
        if (type == FF_LIGHT_DIRECTIONAL)
        {
            L = -light.direction.xyz;
        }
        else
        {
            const float3 toLight = light.position.xyz - P;
            const float distance = length(toLight);
            if (distance > light.position.w)
            {
                continue;  // outside D3DLIGHT8::Range
            }
            L = distance > 0.0f ? toLight / distance : float3(0.0f, 0.0f, 0.0f);
            const float denominator =
                light.attenuation.x + light.attenuation.y * distance + light.attenuation.z * distance * distance;
            // ASSUMPTION: all-zero attenuation (invalid in D3D8) counts as 1 instead of +INF.
            factor = denominator > 0.0f ? 1.0f / denominator : 1.0f;
            if (type == FF_LIGHT_SPOT)
            {
                // rho = cos(angle between the spot axis and light -> vertex); x = cos(theta/2), y = cos(phi/2).
                const float rho = dot(-L, light.direction.xyz);
                float spot;
                if (rho > light.spot.x)
                {
                    spot = 1.0f;
                }
                else if (rho <= light.spot.y)
                {
                    spot = 0.0f;
                }
                else
                {
                    spot = PowPositive((rho - light.spot.y) * light.spot.z, light.direction.w);
                }
                factor *= spot;
            }
        }
        ambientSum += factor * light.ambient.rgb;
        const float NdotL = dot(N, L);
        if (NdotL > 0.0f)
        {
            diffuseSum += factor * NdotL * light.diffuse.rgb;
            if (specularEnable)
            {
                // NV2A/OpenGL convention: no highlight on the unlit side (N.L <= 0).
                const float NdotH = dot(N, SafeNormalize(V + L));
                specularSum += factor * PowPositive(NdotH, power) * light.specular.rgb;
            }
        }
    }
    diffuseOut.rgb = saturate(matEmissive.rgb + matAmbient.rgb * (g_draw.globalAmbient.rgb + ambientSum) +
                              matDiffuse.rgb * diffuseSum);
    diffuseOut.a = saturate(matDiffuse.a);
    specularOut = specularEnable ? float4(saturate(matSpecular.rgb * specularSum), 0.0f)
                                 : float4(0.0f, 0.0f, 0.0f, 0.0f);
}

// NV2A vertex snapping (M3 review finding 9): the rasterizer of the original sees screen positions with 4
// fractional bits (1/16 pixel), D3D11 with 8. Without it an edge that the original puts exactly on a row of
// pixel centers (the "Neues Spiel" button top edge at y = 189.000 .. 189.008 in D3D8 pixels) falls just below
// the centers on the PC and the row stays uncovered. The clip position already carries the +0.5 pixel shift
// of B19 (GpuRenderer::HalfPixelShift); the snap works on D3D8 pixels of the render target (pretransform
// maps them to NDC) and keeps w (perspective-correct interpolation).
float4 SnapToSubpixelGrid(float4 clip)
{
    float4 result = clip;
    if (clip.w > 0.0f && g_frame.pretransform.x != 0.0f && g_frame.pretransform.y != 0.0f)
    {
        const float2 ndc = clip.xy / clip.w;
        const float2 pixel = (ndc - g_frame.pretransform.zw) / g_frame.pretransform.xy - 0.5f;
        const float2 snapped = round(pixel * 16.0f) / 16.0f;
        result.xy = ((snapped + 0.5f) * g_frame.pretransform.xy + g_frame.pretransform.zw) * clip.w;
    }
    return result;
}

FFVertexOutput VSMain(FFVertexInput input)
{
    FFVertexOutput o;
    const float4 vertexColor = saturate(input.color);
#if FF_PERM_PRETRANSFORMED
    // D3DFVF_XYZRHW with rhw = 1: render-target pixels with D3D8 pixel centers on integers (+0.5 for D3D11),
    // relative to the current viewport (pretransform, FillFrameConstants); z is the device depth.
    o.position = float4((input.position.xy + 0.5f) * g_frame.pretransform.xy + g_frame.pretransform.zw,
                        input.position.z, 1.0f);
    const float3 viewPosition = float3(input.position.xy, 1.0f);
    const float3 viewNormal = float3(0.0f, 0.0f, 0.0f);
    o.diffuse = HasFeature(FF_FEATURE_HAS_COLOR) ? vertexColor : float4(1.0f, 1.0f, 1.0f, 1.0f);
    o.specular = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.fogDepth = 1.0f;  // w = 1 / rhw
#else
    const float4 position = float4(input.position, 1.0f);
    o.position = SnapToSubpixelGrid(mul(position, g_draw.worldViewProjection));
    const float3 viewPosition = mul(position, g_draw.worldView).xyz;
    float3 viewNormal = mul(input.normal, (float3x3)g_draw.normalMatrix);
    if (HasFeature(FF_FEATURE_NORMALIZE))
    {
        viewNormal = SafeNormalize(viewNormal);
    }
#if FF_PERM_LIGHTING
    LightVertex(viewPosition, viewNormal, vertexColor, o.diffuse, o.specular);
#else
    // D3DRS_LIGHTING off: the vertex diffuse (white if absent) passes through; no specular stream.
    o.diffuse = HasFeature(FF_FEATURE_HAS_COLOR) ? vertexColor : float4(1.0f, 1.0f, 1.0f, 1.0f);
    o.specular = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
    o.fogDepth = g_draw.fog.y != 0 ? length(viewPosition) : viewPosition.z;
#endif
    o.tex0 = StageCoordinates(0, input, viewPosition, viewNormal);
    o.tex1 = StageCoordinates(1, input, viewPosition, viewNormal);
    o.tex2 = StageCoordinates(2, input, viewPosition, viewNormal);
    o.tex3 = StageCoordinates(3, input, viewPosition, viewNormal);
    return o;
}

// ---- pixel shader ------------------------------------------------------------------------------------

// D3DTTFF_PROJECTED: divide by the last of the COUNTn coordinates.
float2 ProjectStageCoordinates(uint stage, float4 coordinates)
{
    const uint flags = g_stages.stages[stage].coord.y;
    const uint count = flags & 0xFF;
    float q = 1.0f;
    if ((flags & FF_TTFF_PROJECTED) != 0 && count >= 2)
    {
        q = count == 2 ? coordinates.y : (count == 3 ? coordinates.z : coordinates.w);
        q = q != 0.0f ? q : 1.0f;
    }
    return coordinates.xy / q;
}

// D3DTA argument with the COMPLEMENT / ALPHAREPLICATE modifiers.
float4 Argument(uint arg, float4 diffuse, float4 current, float4 texel, float4 specular, float4 temp)
{
    const uint select = arg & FF_TA_SELECTMASK;
    float4 value;
    if (select == FF_TA_DIFFUSE)
    {
        value = diffuse;
    }
    else if (select == FF_TA_CURRENT)
    {
        value = current;
    }
    else if (select == FF_TA_TEXTURE)
    {
        value = texel;
    }
    else if (select == FF_TA_TFACTOR)
    {
        value = g_draw.textureFactor;
    }
    else if (select == FF_TA_SPECULAR)
    {
        value = specular;
    }
    else
    {
        value = temp;
    }
    if ((arg & FF_TA_ALPHAREPLICATE) != 0)
    {
        value = value.aaaa;
    }
    if ((arg & FF_TA_COMPLEMENT) != 0)
    {
        value = 1.0f - value;
    }
    return value;
}

// D3DTEXTUREOP (FF_TOP_* = PC D3D8 numbering) on four channels; the caller keeps rgb (color op) or a.
// (Single exit per function: fxc /Od reports early returns from if-chains as uninitialized results.)
float4 TextureOp(uint op, float4 a0, float4 a1, float4 a2, float4 diffuse, float4 current, float4 texel)
{
    // BUMPENVMAP / BUMPENVMAPLUMINANCE (P2: no coordinate perturbation) and unknown ops pass through.
    float4 r = current;
    if (op == FF_TOP_SELECTARG1)
        r = a1;
    else if (op == FF_TOP_SELECTARG2)
        r = a2;
    else if (op == FF_TOP_MODULATE)
        r = a1 * a2;
    else if (op == FF_TOP_MODULATE2X)
        r = 2.0f * a1 * a2;
    else if (op == FF_TOP_MODULATE4X)
        r = 4.0f * a1 * a2;
    else if (op == FF_TOP_ADD)
        r = a1 + a2;
    else if (op == FF_TOP_ADDSIGNED)
        r = a1 + a2 - 0.5f;
    else if (op == FF_TOP_ADDSIGNED2X)
        r = 2.0f * (a1 + a2 - 0.5f);
    else if (op == FF_TOP_SUBTRACT)
        r = a1 - a2;
    else if (op == FF_TOP_ADDSMOOTH)
        r = a1 + a2 - a1 * a2;
    else if (op == FF_TOP_BLENDDIFFUSEALPHA)
        r = lerp(a2, a1, diffuse.a);
    else if (op == FF_TOP_BLENDTEXTUREALPHA)
        r = lerp(a2, a1, texel.a);
    else if (op == FF_TOP_BLENDFACTORALPHA)
        r = lerp(a2, a1, g_draw.textureFactor.a);
    else if (op == FF_TOP_BLENDTEXTUREALPHAPM)
        r = a1 + a2 * (1.0f - texel.a);
    else if (op == FF_TOP_BLENDCURRENTALPHA)
        r = lerp(a2, a1, current.a);
    else if (op == FF_TOP_MODULATEALPHA_ADDCOLOR)
        r = float4(a1.rgb + a1.a * a2.rgb, a1.a);
    else if (op == FF_TOP_MODULATECOLOR_ADDALPHA)
        r = float4(a1.rgb * a2.rgb + a1.a, a1.a);
    else if (op == FF_TOP_MODULATEINVALPHA_ADDCOLOR)
        r = float4((1.0f - a1.a) * a2.rgb + a1.rgb, a1.a);
    else if (op == FF_TOP_MODULATEINVCOLOR_ADDALPHA)
        r = float4((1.0f - a1.rgb) * a2.rgb + a1.a, a1.a);
    else if (op == FF_TOP_DOTPRODUCT3)
        r = saturate(4.0f * dot(a1.rgb - 0.5f, a2.rgb - 0.5f)).xxxx;
    else if (op == FF_TOP_MULTIPLYADD)
        r = a0 + a1 * a2;
    else if (op == FF_TOP_LERP)
        r = lerp(a2, a1, a0);
    else if (op == FF_TOP_PREMODULATE)
        r = a1;  // ASSUMPTION: the premodulation of the next stage's texture is not emulated
    return r;
}

bool AlphaTestPasses(uint func, uint alpha, uint reference)
{
    bool passes = true;  // FF_CMP_ALWAYS
    if (func == FF_CMP_NEVER)
        passes = false;
    else if (func == FF_CMP_LESS)
        passes = alpha < reference;
    else if (func == FF_CMP_EQUAL)
        passes = alpha == reference;
    else if (func == FF_CMP_LESSEQUAL)
        passes = alpha <= reference;
    else if (func == FF_CMP_GREATER)
        passes = alpha > reference;
    else if (func == FF_CMP_NOTEQUAL)
        passes = alpha != reference;
    else if (func == FF_CMP_GREATEREQUAL)
        passes = alpha >= reference;
    return passes;
}

// Fog factor f (1 = no fog) of D3DRS_FOGTABLEMODE; fogParams = (start, end, density, 1 / (end - start)).
float FogFactor(float depth)
{
    const uint mode = g_draw.fog.x;
    const float4 p = g_frame.fogParams;
    // ASSUMPTION: FOGTABLEMODE NONE with fog enabled would take the vertex specular alpha (NV2A fog gen
    // SPEC_ALPHA); REN never leaves it so for drawn geometry (FUN_00159700), so it counts as no fog.
    float f = 1.0f;
    if (mode == FF_FOG_LINEAR)
    {
        // start == end (w = 0): a step at `end`.
        f = p.w == 0.0f ? (depth < p.y ? 1.0f : 0.0f) : saturate((p.y - depth) * p.w);
    }
    else if (mode == FF_FOG_EXP)
    {
        f = saturate(exp2(-depth * p.z * 1.44269504f));
    }
    else if (mode == FF_FOG_EXP2)
    {
        const float x = depth * p.z;
        f = saturate(exp2(-x * x * 1.44269504f));
    }
    return f;
}

float4 PSMain(FFVertexOutput input) : SV_Target
{
    // All stages are sampled up front (no gradients inside flow control; unused stages see white).
    float4 texels[FF_MAX_STAGES];
    texels[0] = g_texture0.Sample(g_sampler0, ProjectStageCoordinates(0, input.tex0));
    texels[1] = g_texture1.Sample(g_sampler1, ProjectStageCoordinates(1, input.tex1));
    texels[2] = g_texture2.Sample(g_sampler2, ProjectStageCoordinates(2, input.tex2));
    texels[3] = g_texture3.Sample(g_sampler3, ProjectStageCoordinates(3, input.tex3));

    const float4 diffuse = input.diffuse;
    const float4 specular = input.specular;
    float4 current = diffuse;  // D3DTA_CURRENT of stage 0 = iterated diffuse
    float4 temp = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [loop]
    for (uint i = 0; i < (uint)FF_MAX_STAGES; ++i)
    {
        const FFStage stage = g_stages.stages[i];
        if (stage.color.x == FF_TOP_DISABLE)
        {
            break;  // D3D8: a disabled color op ends the cascade
        }
        const float4 texel = texels[i];
        const float4 c1 = Argument(stage.color.y, diffuse, current, texel, specular, temp);
        const float4 c2 = Argument(stage.color.z, diffuse, current, texel, specular, temp);
        const float4 c0 = Argument(stage.color.w, diffuse, current, texel, specular, temp);
        const float4 colorResult = TextureOp(stage.color.x, c0, c1, c2, diffuse, current, texel);
        float alphaResult;
        if (stage.color.x == FF_TOP_DOTPRODUCT3)
        {
            alphaResult = colorResult.a;  // DOTPRODUCT3 replicates into alpha as well
        }
        else if (stage.alpha.x == FF_TOP_DISABLE)
        {
            alphaResult = current.a;  // invalid in D3D8 with an enabled color op: keep the current alpha
        }
        else
        {
            const float4 a1 = Argument(stage.alpha.y, diffuse, current, texel, specular, temp);
            const float4 a2 = Argument(stage.alpha.z, diffuse, current, texel, specular, temp);
            const float4 a0 = Argument(stage.alpha.w, diffuse, current, texel, specular, temp);
            alphaResult = TextureOp(stage.alpha.x, a0, a1, a2, diffuse, current, texel).a;
        }
        const float4 result = saturate(float4(colorResult.rgb, alphaResult));
        if (stage.coord.z == FF_TA_TEMP)
        {
            temp = result;
        }
        else
        {
            current = result;
        }
    }

    float4 color = current;
    if (HasFeature(FF_FEATURE_SPECULAR))
    {
        color.rgb = saturate(color.rgb + specular.rgb);  // D3DRS_SPECULARENABLE: added after the stages
    }
    if (g_draw.alphaTest.x != 0)
    {
        const uint alpha8 = (uint)(saturate(color.a) * 255.0f + 0.5f);
        if (!AlphaTestPasses(g_draw.alphaTest.y, alpha8, g_draw.alphaTest.z))
        {
            discard;
        }
    }
    if (HasFeature(FF_FEATURE_FOG))
    {
        color.rgb = lerp(g_frame.fogColor.rgb, color.rgb, FogFactor(input.fogDepth));
    }
    return color;
}
