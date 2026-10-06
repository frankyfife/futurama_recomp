# M3 graphics contract (phase 1 -> phase 2)

Binding for the four phase-2 agents (backend, texconv, extract, shader) and the phase-3 integration of
milestone M3 "Erstes Bild" (`docs/loader_design.md` section 6 M3, 2.2 graphics boundary). Code and
comments are English. Read first: `docs/m2_report.md` (5 measured slot usage, 9 known issues, 10 the M3
plan), `src/INTERFACES.md` (APIs, conventions, changelog), `analysis/loader/renderer.md` (vtable
semantics, data flow, formats 5.2, properties 5.4, lights 5.5, priorities 7.2).

**The original NiXBoxRenderer code in the XBE is the executable specification.** Every slot, property and
format rule below names the function it comes from; read the decompiled function
(`analysis/retail_eu_export/decomp/*.c`, `analysis/loader/scripts/renderer_dfun.py <addr>`) or the capstone
disassembly (code in Ghidra gaps) before implementing it, and mark guesses with `// ASSUMPTION:`.

## 1. Goal and exit criteria (design 6, M3)

| # | Criterion | Evidence the integration collects |
|---|---|---|
| 1 | Splash (`images\english\NI_Splash_PAL.nif`), legal screen (`images\german\legal.nif`) and the frontend menu appear completely | `[render] screenshot_frames` PNGs under `run\<name>\screenshots\`, viewed side by side with the xemu reference captures (below), `[render] trace_frames` log of every draw call of those frames with its result (`Drawn`/`Skipped` + reason), a written list of remaining visual differences |
| 2 | No D3D debug-layer error | `D3D11 debug layer: enabled - 0 errors, N warnings` exit line (or `not installed` + what was captured) |
| 3 | `--frames=3000` without crash, presented frame rate >= 50 | exit code 0, `present: 3000 frames in T s (average A Hz, slowest 1 s window W Hz ...)` with A >= 50 |

Reference captures of the original in xemu (design 6 M3, added 2026-10-05): `run\xemu_ref\` (git-ignored,
`tools\xemu_capture.py`), 1280x960 (2x internal resolution, German EEPROM language), `scenes.tsv` maps the
scenes to reference images: `ref_legal.png` (legal screen), `ref_tech_notice.png` (= our "Splash": the
NetImmerse/Gamebryo "NDL graphics technology" notice of `NI_Splash_PAL.nif`), `ref_splash.png` (the loading
screen "Ladevorgang - bitte warten"), `ref_frontend.png` (frontend main menu: a 3D scene of gears, pipes and
lightning behind the menu panel; phase-1 trace of frame 950: 346 RenderShape calls and nothing else - so
the menu text is geometry too, presumably of the game's own text system FUN_00099FE0/FUN_00098D20,
renderer.md 4; all 4838 RenderScreenTexture and 45 RenderScreenPolygon calls of a run happen during the
intro: legal/notice/loading screens), plus `t<seconds>s.png` every 3 s and `contact_sheet.png`. The top
~24 pixels of early captures show the xemu
menu bar (ignore). Our timeline is shorter because the XMV stub ends every movie at once (Intro -> Frontend
at `CApp::Frame` ~#780 = ~15.9 s in M2), so the screenshot frames are chosen from our own log, not from the
xemu times. Movie scenes (XMV) are compared from M6 on.

Plus the global rule of design 6: 0 traps, 0 `UNIMPLEMENTED`, no `error` line; `tools\m1_check.py` and
`tools\m2_check.py` keep passing (with `[render] backend = null` until the integration switches the default,
afterwards with `d3d11`).

Measured M2 slot usage per 3000 frames (m2_report 5): 16 DisplayFrame 3000, 17 ClearBuffer 3015, 21/22 3,
27 SetBackgroundColor 12, 39 PrecacheGeometry 181, 45/46/47 2977 each, **48 RenderShape ~762 000**
(346 per frontend frame), 54 RenderScreenPolygon 45, **55 RenderScreenTexture 4838** (intro screens), 56 ~762 000,
61 19, 64 5, **65 CreateSourceTextureRendererData 161** (140 new, none with a file path), 66 1, 71 1.
Not reached: 49/50/51, 15 with a texture, the file path of slot 65.

## 2. Architecture

```
guest main thread (renders; loading fibers run on the same OS thread)
  NiPCRenderer slot (gfx/nipc_renderer.cpp, integration)
    -> ni_extract   (gfx/ni_extract.*, gfx/ni_guest_layouts.h)   guest objects -> DrawItem / CameraDesc
    -> texconv      (gfx/texconv.*)                              NiPixelData -> HostImage (slot 65)
    -> ff_pipeline  (gfx/ff_pipeline.*, gfx/shaders/**)          DrawItem -> pipeline + constants + streams
    -> rhi          (gfx/rhi/rhi.h, rhi.cpp)                     backend-neutral device/context
    -> d3d11        (gfx/rhi/d3d11/**)                           D3D11 + DXGI swap chain
boot/UI thread
  platform (platform/window.*, frame_pacer.*, screenshot.*)      window, message loop, hotkeys, PNG, pacing
```

Data flow of one frame (`CApp::Frame` 0xD2630 -> `NiCamera::Click` 0x100C50, renderer.md 3.3/4):

| Guest call | Integration does (phase 3) |
|---|---|
| 17 `ClearBuffer(NiRect<float>*, mode)` | `ni::ClearRect` -> `ctx.Clear(flags, background, depth, stencil, rect)`; mode bit 1 color, 4 depth, 2 stencil; viewport = whole target first (0x146490) |
| 27/26/28 background/depth/stencil clear values | kept in `HostRenderer` (already in M2) |
| 47 `SetCameraData(loc, dir, up, right, frustum, port)` | `ni::ExtractCamera` -> `ctx.SetViewport`, `ff.SetCamera` |
| 45 / 46 `BeginScene` / `EndScene` | in-scene flag (M2), nothing else |
| 48 / 51 (49 / 50) | `ni::ExtractShape` / `ExtractStrips` (`ExtractTriangles` / `ExtractTriStrip`) with renderer +0x0C/+0x10 -> `ff.Draw` |
| 54 `RenderScreenPolygon` | `ni::ExtractScreenPolygon` -> `ff.Draw` |
| 55 `RenderScreenTexture` | `ni::ExtractScreenTexture` -> `ff.Draw` |
| 65 `CreateSourceTextureRendererData` | source order of FUN_0015E010 (M2 code) -> `tex::ConvertPixelData` -> `device.CreateTexture` -> handle in `NiPCTextureData` (+0x64, section 9) |
| 16 `DisplayFrame` (back buffer only) | screenshot (hotkey / `screenshot_frames`) -> `device.Present` -> debug messages -> `platform::NotePresentedFrame` -> `platform::PaceFrame(frame_rate)` -> frame counter / `--frames` -> `arena.Reset()` |
| 15 `SetRenderTarget(NULL)` | back buffer (M2 logic incl. the screen-offset helpers); textures are P2 |

Threading: the RHI device and everything below the slots run on the guest main thread only. The window and
its message loop belong to the boot thread (HostMain stages 12 and 14). Nothing in the render path may block
on the boot thread. Host code called from guest code follows INTERFACES 4.14 (no locks or owning objects
across guest calls; `noexcept` + `FUT_HLE_TRY/CATCH` at slot entries).

## 3. Shared types and conventions (`gfx/gfx_math.h`, `gfx/rhi/rhi.h`)

Verified against the binary (phase 1); every agent relies on them:

| Convention | Rule | Evidence |
|---|---|---|
| Matrix storage | D3D style: row vectors, `v' = v * M`, row-major `m[row][col]`, translation in row 3; HLSL `row_major`, `mul(float4(p,1), M)` | SetCameraData 0x1469E0 builds the view at this+0x610 and calls `D3DDevice_SetTransform(0 = VIEW, ...)` with that layout |
| NiTransform | 0x40 bytes: `float rotate[3][4]` (16-byte row stride, col 3 padding), `translate[3]` at +0x30, `scale` at +0x3C | NiAVObject ctor 0x102860 zeroes +0x3C/+0x4C/+0x5C (padding of the local rotation rows); FUN_001489A0 reads [0..2],[4..6],[8..10],[12..14],[15] |
| NiTransform -> world | `m[i][j] = rotate[i][j] * scale`, row 3 = translate; rows = images of the x/y/z axes; **no transpose** (`gfx::NiTransformToWorld`) | FUN_001489A0 (D3DTS_WORLD = 6); NiTriShape::vf33 0x1166D0: `out.x = t.x + s*(m[0]x + m[4]y + m[8]z)` |
| Camera / view | view rows `(right.x, up.x, dir.x, 0) (right.y, up.y, dir.y, 0) (right.z, up.z, dir.z, 0) (-loc.right, -loc.up, -loc.dir, 1)`: view x = right, y = up, z = direction (left-handed view space, the NetImmerse world is right-handed; the reflection is in the view matrix) | 0x1469E0 (param order loc, dir, up, right) |
| Projection | perspective only (no ortho in this NetImmerse version): `[0][0] = 2/(r-l)`, `[2][0] = -(r+l)/(r-l)` (both negated with left-right swap, rs+0x1FC), `[1][1] = 2/(t-b)`, `[2][1] = -(t+b)/(t-b)`, `[2][2] = f/(f-n)`, `[3][2] = -n f/(f-n)`, `[2][3] = 1`; frustum l/r/t/b are tangents at distance 1; clip z in [0, 1] | 0x1469E0 this+0x650 |
| Viewport | `X = trunc(W*port.l)`, `Y = trunc((1 - port.t) * H)`, `Width = trunc((port.r - port.l) * W)`, `Height = trunc((port.t - port.b) * H)`, MinZ 0, MaxZ 1; port/rects are normalized with **y up** | 0x146C81..0x146D1A (`_ftol` = FUN_00167080) |
| Clear rect | `D3DRECT {trunc(W*l), trunc((1-t)*H), trunc(W*r), trunc((1-b)*H)}`; NULL = whole target | 0x1464C2..0x14652B |
| Culling | stencil property draw mode + left-right swap select D3DCULL from table rs+0x1D8: default (mode 0) = **CW culled** (0x900); rhi::CullMode keeps the D3D8 meaning | FUN_001599E0, table in FUN_00158DE0 |
| Winding of normals | `normal = (v1 - v0) x (v2 - v1)` per triangle; strips alternate | NiTriShapeData::vf13 0x151AD0, NiTriStripsData::vf13 0x152460 |
| Screen polygons | vertex (x, y, z) -> `(d x, d y, d)` with `d = near + z (far - near)`, view = identity, projection +0x394 `[[2,0,0,0],[0,2,0,0],[-1,-1,P22,1],[0,0,P32,0]]`: x, y in [0, 1] with **y up**; fan (7) or strip (6, byte +0x1E) | slot 54 0x14B5F0, FUN_00149F30, FUN_001585A0, init FUN_00147CD0 |
| Screen textures | pre-transformed XYZRHW quads in render-target pixels, `x - 0.5`, `y - 0.5` (D3D8 pixel centers on integers) | FUN_0014A050, FUN_00157EA0 (FVF 4/0x44/0x144) |
| Pixel centers | the RHI uses D3D10+ rules; pre-transformed D3D8 pixels get +0.5 in the shader (`FFFrameConstants::pretransform`) so a quad at `x - 0.5` covers exactly the original pixels | GFX_CONTRACT decision |
| Colors | NiColorA floats; the original packs `ROUND(c * 255)` into ARGB D3DCOLOR (no clamp, wraps); the port passes floats and saturates in the shader | FUN_00157C00, FUN_001585A0 |
| UV sets | `uvs + set * vertexCount * 8`, at most 4 sets (DAT_002D85D0 = 4) | FUN_001437B0, FUN_00157830 |
| Textures | texture origin top-left, no swizzling, linear `NiPixelData` | renderer.md 5.2 |

## 4. ni_extract (owner: extract agent)

Files: `gfx/ni_extract.h/.cpp`, `gfx/ni_guest_layouts.h`, `tests/extract_*.cpp`.

### 4.1 Guest layouts (`ni_guest_layouts.h`)

Phase 1 verified these layouts (static_asserts + evidence in the header): NiTransform, NiBound, NiFrustum,
NiRectF, NiAVObject (+0x20 world bound, +0x30 local, +0x70 world), NiGeometryData (0x34: +0x08 u16 vertex
count, +0x0A u16 UV sets, +0x1C vertices, +0x20 normals, +0x24 colors, +0x28 UVs, +0x2C id, +0x2E revision,
+0x30 renderer data), NiTriBasedGeomData (+0x34 triangles), NiTriShapeData (+0x3C u16 triangle list),
NiTriStripsData (+0x38 strip count, +0x3C lengths, +0x40 indices), NiTrianglesData / NiTriStripData
(+0x38 active triangles), NiSkinInstance (+0x08 data, +0x0C root parent, +0x10 bones, +0x14 frame stamp),
NiPropertyState (12 property pointers +0x08..+0x34), every property used in M3, NiDynamicEffectState
(+0x0C light list, +0x10/+0x14 projected light/shadow lists, +0x18 environment map, +0x1C fog map),
NiLight (+0xC0 on, +0xC4 index, +0xCC revision, +0xE0 dimmer, +0xE4 ambient, +0xF0 diffuse, +0xFC specular)
and subclasses, NiPixelFormat/NiPixelData/NiPalette, NiTexturingProperty + Map, NiSourceTexture,
NiScreenPolygon, NiScreenTexture (+ 0x1C-byte rect), NiCamera field offsets. Extend it add-only; texconv reads
the texture part.

### 4.2 What to extract (D3D8 state as REN wrote it)

`RenderStateDesc` / `LightSetDesc` / `TextureStageDesc` are the D3D8 render states, texture stage states,
D3DMATERIAL8 and D3DLIGHT8 the original produced for the same guest data. ff_pipeline emulates D3D8 on them
and never sees NetImmerse enums. Tables (from FUN_00158DE0, the render-state cache ctor):

| Property | Rule | Function |
|---|---|---|
| NiAlphaProperty flags | bit 0 blend; bits 1-4 src, 5-8 dst AlphaFunction: 0 ONE, 1 ZERO, 2 SRCCOLOR, 3 INVSRCCOLOR, 4 DESTCOLOR, 5 INVDESTCOLOR, 6 SRCALPHA, 7 INVSRCALPHA, 8 DESTALPHA, 9 INVDESTALPHA, 10 SRCALPHASAT; bit 9 alpha test, bits 10-12 TestFunction 0 ALWAYS, 1 LESS, 2 EQUAL, 3 LESSEQUAL, 4 GREATER, 5 NOTEQUAL, 6 GREATEREQUAL, 7 NEVER; ref = byte +0x18 | FUN_00159270 |
| NiZBufferProperty flags | bit 0 test: ZFUNC LESSEQUAL, else ALWAYS (z enable stays on); bit 1 ZWRITEENABLE | FUN_00159B60 |
| NiStencilProperty | +0x18 enable, func (0 NEVER .. 7 ALWAYS - **not** the alpha order), ref, mask, fail/zfail/pass action (0 KEEP, 1 ZERO, 2 REPLACE, 3 INCRSAT, 4 DECRSAT, 5 INVERT), draw mode -> cull: 0 CCW_OR_BOTH -> CW, 1 CCW -> CW, 2 CW -> CCW, 3 BOTH -> NONE; left-right swap exchanges CW/CCW | FUN_001599E0 |
| NiWireframe / NiShade / NiDither / NiSpecular | bit 0: FILLMODE wireframe / Gouraud (0 = flat) / DITHERENABLE / SPECULARENABLE | FUN_00159CD0, FUN_00159980 |
| world scale | NORMALIZENORMALS if scale outside [0.99, 1.01] | FUN_00159200 |
| NiMaterialProperty | D3DMATERIAL8: Diffuse = (+0x28, alpha +0x50), Ambient = (+0x1C, alpha), Specular = (+0x34, alpha), Emissive = (+0x40, alpha), Power = +0x4C | FUN_00161800 |
| Lighting | REPLACE apply mode with a base texture -> LIGHTING off. Else by NiVertexColorProperty (source +0x18, lighting +0x1C): lighting E (0) + source EMISSIVE -> LIGHTING off; lighting E + IGNORE/AMB_DIFF -> LIGHTING on without lights (emissive/ambient only); E_A_D (1): lights on, color sources IGNORE -> all MATERIAL, EMISSIVE -> emissive = COLOR1, AMB_DIFF -> ambient = diffuse = COLOR1 | FUN_0015BF80 |
| Lights | effect state list +0x0C; skip `on == 0` or dimmer < 0.01; type 0 ambient: D3DRS_AMBIENT += dimmer * ambient color (clamp, the original wraps); others D3DLIGHT8: Diffuse/Specular/Ambient = dimmer * color, alpha 1; directional: Direction = +0x110; point: Position = +0xA0, Attenuation0..2 = +0x110..+0x118, Range = 0x5F7FFFFF; spot: point + Direction +0x120, Theta = Phi = angle(+0x12C) * DAT_002F91D4 / 90 (ASSUMPTION: pi -> full cone in radians), Falloff = +0x130 | FUN_0015BA40, FUN_0015B6A0 |
| NiFogProperty | flags bit 1 enable, bits 3-4 function (0 Z_LINEAR -> table LINEAR, 1 RANGE_SQ -> EXP + range fog, 2 VERTEX_ALPHA -> off); start = far - depth (far - near), end = depth (far - near) (1/maxFog - 1) + far; color +0x1C | FUN_00159700, FUN_00159900 |
| NiTexturingProperty | stage setup through the configurable texture pipeline (+0x5C8): default pipeline vtable 0x2AC104 (vf2 0x164360 is Ghidra **gap code**: read it with capstone; vf3 0x1628B0 per pass), FUN_00162020 (texture + palette), FUN_00161E40 (filter triple table DAT_0030FEFC, 3 dwords per NI filter mode), FUN_00161D60 (address U/V), FUN_00161F70 (texcoord index, texture transform). M3 needs the base map (slot 0) for all apply modes (REPLACE, DECAL, MODULATE); the other maps are captured if the pipeline puts them on stages, otherwise left for P2 | see left |

`ExtractState` fills `RenderStateDesc` for every draw; `TextureStageDesc::niTexture` is set and the
integration's `TextureResolver` fills `texture`.

**State persistence.** REN's state code is not a pure function of the current properties: it writes some
D3D states only for certain property values and otherwise leaves what the previous draw set. Example
(FUN_0015BF80): with NiVertexColorProperty lighting E and source IGNORE/AMB_DIFF it neither touches the
ambient/diffuse material sources (which a previous E_A_D + AMB_DIFF draw set to COLOR1) - so the lit alpha
(diffuse source alpha) depends on the previous draw; FUN_00159700 keeps the previous fog mode for
FOG_VERTEX_ALPHA. `ni::StateShadow` (one per renderer, passed in `ExtractContext::shadow`) carries the D3D
state the previous draw left plus REN's light-manager caches (+0x49, +0x4C); `ExtractState` starts from it and
updates it in draw order. Its `Reset()` state = D3D8 device defaults + REN's init (FUN_00147CD0, render-state
cache ctor FUN_00158DE0). Unit tests may pass `shadow = nullptr` (every draw from the defaults).

### 4.3 Geometry per slot

| Slot | Extractor | Rules |
|---|---|---|
| 48 | `ExtractShape` | vertex count = data +0x08 (0 -> Empty), index list = +0x3C with `3 * triangles` indices (zero copy), world = `NiTransformToWorld(transform)`, skin != 0 -> CPU skinning (4.4); the original first checks the in-scene flag and data vf12 (vertex count) |
| 51 | `ExtractStrips` | strips joined into one TriangleStrip index list with `rhi::kStripCut` between strips (D3D11 restarts at 0xFFFF; same winding rule as the original's per-strip DrawIndexedVertices); vertex count < 0xFFFF holds by the u16 type |
| 49 | `ExtractTriangles` | non-indexed list, `3 * activeTriangles` vertices |
| 50 | `ExtractTriStrip` | non-indexed strip, `activeTriangles + 2` vertices |
| 54 | `ExtractScreenPolygon` | property state = polygon +0x08, no effect state, positions converted into the arena (section 3), fan -> triangle list, strip if +0x1E, world identity, `useScreenCamera` |
| 55 | `ExtractScreenTexture` | quads per rect (ni_guest_layouts.h, FUN_0014A050: wrap of left/top into the target, -0.5, UVs from the base map size, corner order, indices (0,1,2) (2,1,3)), pre-transformed, fixed state: blend SRCALPHA/INVSRCALPHA, z test and write off, lighting off, vertex colors, stage 0 = the screen texture's NiTexturingProperty base map |

Zero copy where the guest array has the needed layout (positions, normals, colors, UVs, triangle lists);
everything else in the `ExtractArena` (reset by the integration at DisplayFrame). No guest writes.

### 4.4 Skinning

Call the original `FUN_0014F710` (thiscall, ecx = NiSkinInstance*, 6 stack args `(srcPositions, srcNormals
or 0, vertexCount, dstPositions, dstNormals or 0, dstStride)`, ret 0x18; `ni::kNiSkinInstanceDeform`) into
an arena buffer through `core::CallGuestThiscall`, at most once per skin instance and frame (cache keyed by
instance + `ExtractContext::frame`, like the frame stamp +0x14 the original keeps - NiPCRenderer does not
write +0x14). World matrix = the slot's NiTransform as in FUN_00149AE0. It also clears DAT_0030EB40 like the
original call. The frontend may not need it (measure: count skinned draws in the trace).

### 4.5 Camera

`ExtractCamera` reproduces 0x1469E0 including the left-right swap and the `_ftol` truncation;
`DefaultCamera` reproduces the call in FUN_0014C170 (location 0, direction (1,0,0), up (0,1,0), right
(0,0,1), frustum (-1, 1, 1, -1, 0.1, 1), port (0, 1, 1, 0)). `screenProjection` takes P22/P32 from the
projection (0x3BC/0x3CC copies in 0x1469E0).

## 5. RHI (owner: backend agent)

Files: `gfx/rhi/d3d11/**`, `tests/rhi_*.cpp`. The interface `gfx/rhi/rhi.h` and `rhi.cpp` (format table,
descriptor hashes, `CreateDevice` dispatch) are shared skeleton files (add-only).

5.1 **Interface** (rhi.h): typed handles (0 = none); `Device` creates buffers/textures/shaders, returns
hash-cached samplers and pipelines (`GetSampler`, `GetPipeline` from POD descriptors with defaulted `==`
and `HashOf`), `Present`, `CaptureBackBuffer` (before Present), `ReadTexture`, `DebugLayer`,
`DrainDebugMessages`, `Stats`, `LastError`. `CommandContext` sets targets/viewport/scissor, clears (with an
optional rect), binds pipeline/streams/index buffer (16-bit)/constant buffers/textures/samplers, draws, and
offers `UpdateBuffer` (discard) and a per-frame transient ring (`BeginTransient`/`EndTransient`).

5.2 **Formats**: RGBA8, BGRA8 (default upload), BGRX8, optional B5G6R5/B5G5R5A1/B4G4R4A4
(`IsFormatSupported`), BC1-3, A8, R8, D24S8, D32F. Vertex formats Float1-4, UNorm8x4_BGRA.

5.3 **D3D11 backend requirements**:
- Feature level >= 10_0 (shaders are SM 4.0 DXBC); `D3D11CreateDevice` with BGRA support; WARP when
  `softwareAdapter` (`[render] adapter = warp`).
- Window: DXGI flip model (`FLIP_DISCARD`, 2 buffers, BGRA8), `DXGI_SCALING_STRETCH`, back buffer =
  640x480 (Create size), `DXGI_MWA_NO_ALT_ENTER`; the HWND belongs to the boot thread (platform/window.h).
  Headless (no window): offscreen BGRA8 color + D24S8 depth of the same size; Present only counts.
- Debug layer: `DeviceDesc::debugLayer` (from `gfx::WantDebugLayer`) -> `D3D11_CREATE_DEVICE_DEBUG`; on
  `DXGI_ERROR_SDK_COMPONENT_MISSING` retry without it and set `DebugLayer().note`. Messages through
  `ID3D11InfoQueue` (no break on severity), drained every frame by the integration. The SDK layers are
  installed on the development machine (`C:\Windows\SysWOW64\d3d11_3SDKLayers.dll`, checked in phase 1).
- Caches: samplers, pipelines (blend/depth-stencil/raster objects + input layout per VS) by descriptor hash
  and equality. Transient ring: one dynamic vertex and one index buffer (e.g. 4 MiB / 1 MiB), `NO_OVERWRITE`
  appends, `DISCARD` on wrap; constant buffers: dynamic, `UpdateBuffer` = `Map(DISCARD)`.
- Strip cut: index 0xFFFF in strip topologies (D3D11 always cuts at 0xFFFF with R16 indices).
- Cull mapping: `CW` -> `FrontCounterClockwise = TRUE` + `CULL_BACK`, `CCW` -> `FALSE` + `CULL_BACK`.
- `CaptureBackBuffer`: staging copy, BGRA8 rows.
- Robustness: every API failure becomes `LastError()` text + a false/invalid result; device removal ->
  `PresentResult::DeviceLost` (the integration logs it as GFX/error and ends with `core::Fatal`).
- x87/SSE state: the guest may have changed the FPU control word; the backend must not depend on it (if a
  driver misbehaves, save/restore `_controlfp` around device calls and document it).

5.4 **Tests** (`tests/rhi_*.cpp`): headless WARP device: create/destroy resources, clear + read back, draw a
triangle with the stub shaders (`gen_shaders/ff_stub_*.h`), cull mapping (CW/CCW triangle), strip cut,
transient ring wrap, sampler/pipeline cache identity, debug-layer messages (provoke one and drain it if the
layer is available, else check the note).

## 6. ff_pipeline and shaders (owner: shader agent)

Files: `gfx/ff_pipeline.*`, `gfx/shaders/**` (`ff_shared.h` = shared layout, add-only), the `fut_add_shader`
calls in the gfx section of `src/host/CMakeLists.txt` (additive), `tests/ff_*.cpp`.

6.1 **Shared layout** (`gfx/shaders/ff_shared.h`, included by C++ and HLSL): constant buffers b0
`FFFrameConstants` (view, projection, view*projection, screenProjection, pretransform scale/offset, fog
color/params, target size), b1 `FFDrawConstants` (world, worldView, worldViewProjection, normal matrix,
material, global ambient, texture factor, feature key + counts, color sources, alpha test, fog mode), b2
`FFLightConstants` (8 view-space lights), b3 `FFStageConstants` (4 D3D8 texture stages); textures/samplers
t0-t3/s0-s3 = stages. Only float4/uint4/float4x4 members (identical packing, static_asserts). D3D8 codes:
`FF_TOP_*`, `FF_TA_*`, `FF_MCS_*`, `FF_FOG_*`, `FF_LIGHT_*`, `FF_TTFF_*`, `FF_CMP_*`.

6.2 **Vertex input**: one layout for all draws, streams 0 POSITION float3, 1 NORMAL float3, 2 COLOR0 float4,
3-6 TEXCOORD0-3 float2. Absent attributes are bound as zero-stride default buffers (normal 0, color white,
UV 0) and flagged in the feature key (`FF_FEATURE_HAS_NORMAL/HAS_COLOR`, UV count) because D3D8 behaves
differently when an attribute is absent (no diffuse -> white without lighting, material color for COLOR1).

6.3 **Feature key** (`ff::MakeFeatureKey`): `FF_FEATURE_*` bits; `FF_FEATURE_COMPILE_MASK` selects compiled
permutations (phase-1 default PRETRANSFORMED | LIGHTING; the shader agent may change the mask), the rest is
uniform branching on `features.x`.

6.4 **D3D8 semantics** to emulate (ff_pipeline.h header comment): transform paths (world/view/projection,
screen camera, pre-transformed pixels +0.5), per-vertex lighting in view space with material color sources,
point/spot/directional lights, attenuation, range, spot cone, specular with NORMALIZENORMALS/LOCALVIEWER,
texture stage combiner ops at least DISABLE, SELECTARG1/2, MODULATE/2X/4X, ADD, ADDSIGNED(2X), SUBTRACT,
BLENDDIFFUSEALPHA/TEXTUREALPHA/FACTORALPHA/CURRENTALPHA with arg modifiers COMPLEMENT/ALPHAREPLICATE,
alpha test, table fog (LINEAR/EXP/EXP2 on view depth), flat shading (`nointerpolation`).

6.5 **Build**: `fut_add_shader(<name> <file> <entry> <profile> [DEFINES...])` in `src/host/CMakeLists.txt`
runs fxc (`/WX`, Debug `/Od /Zi`, else `/O3`) into `<build>/src/host/gen_shaders/<name>.h` (`g_<name>`).
Profiles `vs_4_0`/`ps_4_0`. Phase 1 compiles `ff_stub_vs`/`ff_stub_ps` from `ff_ubershader.hlsl`
(referenced by ff_pipeline.cpp) - keep or replace them, but keep the build green.

6.6 **Tests** (`tests/ff_*.cpp`): pure translations (feature key, blend/depth/raster/sampler descriptors,
constant filling incl. light view-space transform, D3D8 lighting reference values computed on the CPU) and,
with a headless WARP device, rendered pixel checks of the combiner/lighting paths.

## 7. texconv (owner: texconv agent)

Files: `gfx/texconv.*`, `tests/texconv_*.cpp`. Format table, slot 65 source order and default texture are in
the `texconv.h` header comment (renderer.md 5.2, FUN_000EDC70, FUN_0015E010, FUN_0014AB50).
Measured in phase 1 with the validation harness (section 10, 1000 frames = legal, splash, frontend): 136 source
textures, **all 16 bit**: 105 x NiPixelFormat 0 (RGB) / 16 bpp, 31 x 1 (RGBA) / 16 bpp; no DXT, no palette, no
file path in that part of the game. The 16-bit paths are the M3 priority; the others must still be correct
(later scenes). Decisions:
- All uncompressed formats -> BGRA8 via the channel masks of NiPixelFormat (+0x04..+0x10); 16-bit packed
  channel descriptor `(channels0 & 0xFC000) >> 14` distinguishes 565/4444/1555 like FUN_000EDC70.
- DXT1/3/5 -> BC1/2/3 unchanged when every level is a multiple of 4 (else decompress to BGRA8; D3D11 needs
  BC levels in whole blocks at level 0); `ConvertOptions::keepBlockCompression = false` decompresses.
- P8 -> BGRA8 through the NiPalette entries (+0x18, 256 x 4 bytes; byte order and PAL/PALA alpha are
  ASSUMPTIONs to verify with real data). Palette revision (+0x10) changes are the integration's business.
- No quality reduction (FormatPrefs ignored: documented deviation from FUN_001572D0); mip levels as stored
  in the NiPixelData (no generation).
- BUMP/BUMPLUMA and 8-bit RGB(A): Unsupported -> default texture (counted by the integration).
Tests: synthetic NiPixelData in guest pages and host `PixelSource`s for every row of the table, mip chains,
palettes, DXT block sizes, default texture bytes, `SelectTextureSource` cases.

**Phase-2 results (texconv agent, add-only; supersedes the marked points above and in section 11):**
- The Xbox converts nothing: the image converter (vtable 0x2AAC94) accepts a target only if its packed
  descriptor (+0x18/+0x1C) equals the source's (CanConvert vf3 0x151480), Convert vf4 0x1514B0 returns the
  source itself (FUN_0015E010 passes it as destination) and ignores the mipmap argument. FUN_001572D0 only
  picks a renderer table entry (+0x528, FUN_0014A410: 555, 565, X888, 1555, 4444, 8888, P8, P8A, DXT1/3/5, 3
  bump formats) with an identical layout, else the default texture. So there is **no FormatPrefs quality
  reduction and no mipmap generation on the Xbox either** (section 7 "documented deviation" and section 11
  "textures may look better" do not apply). The only effect of FormatPrefs: layouts without a matching entry
  (or prefs whose order skips it) show the default texture - `tex::ChooseXboxFormat`, opt-in through
  `TextureSourceInput::imageFormat` -> `DefaultNoXboxFormat` (disc: 1 image, `effects\envmap\newshade.nif`
  A8B8G8R8). Recommendation for the integration: pass it (fidelity) and count it.
- FormatPrefs at NiSourceTexture +0x14 are `{pixelLayout, mipMapped, alphaFormat}` (FUN_0015E010 0x15E13B,
  FUN_001572D0, NIF stream order) - `ni::NiTextureFormatPrefs` has +0x18/+0x1C swapped (extract agent's file).
- Palette byte order r, g, b, a and the alpha rule are verified from code (slot 69 FUN_0015F1B0): alpha =
  entry alpha iff NiPalette +0x1C, independent of PAL/PALA. The disc has no palette textures.
- DXT stays BCn when **level 0** is a multiple of 4 (the D3D11 rule; smaller mips are whole blocks in both
  layouts), not "every level" - otherwise every mipmapped DXT texture would be decoded.
- Channel expansion is bit replication: the xemu captures match it texel for texel (flat areas of the tech
  notice and legal screens: 100 % exact; exact UNORM rounding differs by 1 in 376 texels).
- Disc census (all 8756 NiPixelData of the 820 NIF/UCF files): DXT1 4003, DXT3 1852, RGBA 4444 1647, RGB 565
  1253, A8B8G8R8 1; all convert (`run\m3-texconv\texconv_scan.exe`), no level dropped; the DXT decoder (used
  only for level 0 not a multiple of 4) agrees with Pillow within 1 LSB on 400 real DXT1/DXT3 textures. Live runs
  (validate): 1000 frames 136/136 ok (105 x 565, 31 x 4444), 7000 frames (Intro -> Frontend -> Attract ->
  Frontend) 232/232 ok (184 x 565, 48 x 4444).
- Additions in `texconv.h`: `ConvertOptions::keep16Bit` (565/1555/4444 stay 16 bit where the device supports
  B5G6R5/B5G5R5A1/B4G4R4A4), `HostImage::droppedMipLevels/decompressed`, `MakeTextureDesc`,
  `MakeSubresources`, `XboxFormat` + `ChooseXboxFormat` + `XboxTableFormat`, `Make*Format` (NetImmerse
  descriptor constructors), `XboxFormatPrefs` + `ReadFormatPrefs`, `IsPowerOfTwo`.

## 8. Platform, configuration and log lines (owner: backend agent)

Files: `platform/**` (window, message loop integration, hotkeys, PNG screenshots, frame pacer;
`platform.cpp` stages 12/14 move from the core agent to the backend agent for M3).
- `platform/window.h`: `MainWindow()` (HWND or nullptr), `ShowMainWindow()` (first Present, any thread),
  `MainWindowClientSize`, `ConsumeHotkeys()` (`kHotkeyScreenshot` = F12 / Print Screen). Stage 12 creates
  the window (class, title "Futurama", client area 640x480 * `window_scale`, hidden; borderless full monitor
  with `fullscreen`) only when `[render] backend = d3d11`. WM_CLOSE -> `core::ExitHostProcess(0, "window
  closed")` (**phase 5:** WM_CLOSE only sets `CloseRequested()`; see "Phase-5 results" in section 9). Stage 14
  keeps pumping messages (existing loop, watchdog unchanged).
- `platform/frame_pacer.h`: `PaceFrame(hz)` (the M2 algorithm moved from nipc_renderer.cpp),
  `NotePresentedFrame()`, `GetPresentStatistics()`, `LogPresentStatistics()` -> exit reporter line
  `present: N frames in T s (average A Hz, slowest 1 s window W Hz, longest frame L ms)`.
- `platform/screenshot.h`: `WritePngFile` (WIC, any thread), `ScreenshotPath(dir, frame)` ->
  `frame_NNNNNN.png`; log `screenshot: <path> (WxH)` on GFX/info.

Configuration (`[render]`, registered in `core/config.cpp` in phase 1, parsed into `gfx::Settings` by
`gfx/gfx_config.cpp`):

| Key | Default | Meaning |
|---|---|---|
| `backend` | `null` | `null` (M2 null renderer) or `d3d11`; the integration makes `d3d11` the default when M3 passes |
| `vsync` | 1 | M3: Present sync interval 1. **M3b:** vblank coupling on/off - `0` with `pacing = auto` = timer + Present(0, ALLOW_TEARING); ignored by `pacing = vsync` (GFX/warn); `timer`/`off` tear with `0` |
| `frame_rate` | 50 | M2/M3: limiter, 0 = off. **M3b:** default `[game] tick_rate` (60); target rate of the pacer; another value logs GFX/warn; `0` = unthrottled only with `pacing = auto` (GFX/warn with `timer`/`vsync`) |
| `pacing` | `auto` | M3b: `auto` / `vsync` / `timer` / `off` (section 8 M3b addendum, `src/INTERFACES.md` 4.20) |
| `pacing_trace` | empty | M3b: per-frame pacing CSV |
| `debug_layer` | `auto` | `auto` (Debug builds) / `on` / `off` |
| `adapter` | `hardware` | `hardware` / `warp` |
| `window_scale` | 2 | 1..8 |
| `fullscreen` | 0 | borderless full-monitor window |
| `scaling` | `linear` | phase 5: `linear` / `point` / `integer` - how Present maps the 640x480 back buffer onto the window (`rhi::PresentScaling`) |
| `screenshot_dir` | `<userdata>\screenshots` | PNG target (test runs: under `run\<name>\`) |
| `screenshot_frames` | empty | displayed-frame numbers with an automatic screenshot |
| `trace_frames` | empty | displayed-frame numbers whose draw calls are logged (kind, guest object, counts, textures, state summary, result) |
| `validate` | 0 | phase-2 harness: the null renderer runs ni_extract/texconv on the real data (section 10) |

Log lines the M3 check (phase 3, `tools\m3_check.py`) will read: `NiXBoxRenderer::Create(...) -> NiPCRenderer
... (d3d11: <adapter>, <api>, debug layer <enabled|not installed|off>)`, `D3D11 debug: <severity> #<id>: <text>`
(GFX/error for Error/Corruption, GFX/warn for Warning, GFX/debug otherwise), exit line `D3D11 debug layer:
<state> - N errors, M warnings, K corruption`, the `present:` line, `screenshot:` lines, `draw trace frame N:`
lines and the renderer statistics (draws per kind and result, textures created by source/format,
extract/ff failures).

**Phase-2 results (backend agent, add-only; supersedes the marked points of sections 5 and 8):**
- Files: `gfx/rhi/d3d11/d3d11_device.h` (public backend API), `d3d11_internal.h`, `d3d11_device.cpp` (device,
  resources, caches, Present, read-backs, debug layer), `d3d11_context.cpp` (context, rectangle clears,
  transient ring), `d3d11_convert.cpp` (descriptor translations), `d3d11_builtin.hlsl` (built-in shaders:
  `ClearVS` + `Diag*` test shaders, five `fut_add_shader` lines in the block "backend agent" of
  `src/host/CMakeLists.txt`); `platform/window.*`, `frame_pacer.*`, `screenshot.*`, `platform.cpp` (stage 12);
  `tests/rhi_d3d11_tests.cpp`, `tests/platform_tests.cpp`.
- **Debug messages: the backend logs them itself** (`D3D11 debug: <severity> #<id>: [DXGI ]<text>`, GFX/error,
  warn or debug as above, at most 10 lines per message id, all counted) after every Present, in
  `DrainDebugMessages` and after a failed API call. `DrainDebugMessages` only hands the last 256 messages to
  the caller - the integration must not log them again. Sources: `ID3D11InfoQueue` + the DXGI producer of
  `IDXGIInfoQueue` (warning and above; info/message filtered at the source). The exit line `D3D11 debug layer:
  <enabled|not installed|off|unavailable> - N errors, M warnings, K corruption (D device[s][; note])` comes from
  the exit reporter "d3d11" that the first device with a window registers (process-wide totals,
  `d3d11::GetDebugLayerTotals`). A requested but missing layer logs GFX/warn once. Device creation logs
  `d3d11: device on "<adapter>" (<api>), swap chain 640x480 BGRA8_UNorm, depth D24_UNorm_S8_UInt, debug layer
  <state>`; for its Create line the integration uses `Info().adapterName`, `Info().apiVersion` and
  `d3d11::DebugLayerStateName(device->DebugLayer())`.
- **Window**: `MainWindow()` is the 4:3 *view*, a child of the frame window that is centered with black bars
  (letterbox/pillarbox); the swap chain stretches the 640x480 back buffer onto it, so resizing and the
  borderless fullscreen toggle (Alt+Enter) need no `ResizeBuffers`. The windows are per-monitor DPI aware;
  `window_scale` means 640x480 * scale at 100 % desktop scaling (physical pixels = * DPI / 96), shrunk to the
  largest 4:3 client area that fits the work area (dev machine 3840x2160 at 225 %: scale 2 -> 2631x1973). A
  failed window creation in stage 12 is a CORE/error, `MainWindow()` stays nullptr, the stage succeeds.
- **`NotePresentedFrame`**: the first call shows the window (`ShowMainWindow`, posted) and registers the exit
  reporter "present" - the integration does not need to call either. The reporter logs two lines: the
  contract line `present: N frames in T s (average A Hz, slowest 1 s window W Hz, longest frame L ms)` and
  `present: pacing P Hz, R schedule resets (more than one frame late), S frames longer than 1.5 periods`.
  `PaceFrame` = the M2 algorithm unchanged (+ reset counter).
- **Screenshots**: `platform::SaveScreenshot(device, dir, frame)` = `CaptureBackBuffer` + `WritePngFile` +
  `screenshot: <path> (640x480)` (call before Present); 24-bit PNG (alpha dropped). Hotkeys F12 (not with a
  debugger attached: F12 is the debugger break key) and Print Screen (key-up).
- **Context semantics** (D3D8 conventions): `SetRenderTargets` resets the viewport to the whole color target;
  a depth texture must have the color target's size. `Clear` without rect = the whole target; with a rect =
  clipped to the target, color through `ClearView`, depth/stencil through the built-in `ClearVS` triangle with
  viewport `MinDepth = MaxDepth = depth` (bound pipeline, viewport and stencil ref are restored). Draws without
  a valid pipeline are skipped (no D3D call, `LastError`). Alpha blend factors `*Color` map to `*Alpha` (D3D11
  rejects color factors for alpha); `MipFilter::None` -> `MaxLOD = 0`; border color 0; zero vertex stride is
  passed through.
- **Transient ring**: per kind a list of dynamic chunks (vertex 4 MiB, index 1 MiB); within a frame the
  offset only moves forward (`NO_OVERWRITE`), a full chunk continues in the next one, the first map of a
  chunk per frame is `DISCARD` - so every span stays valid until Present (no mid-frame discard). Alignment
  >= 4, one open span at a time, a request larger than a chunk gets its own (power of two, at most 256 MiB).
- **Resources**: constant buffers alone + multiple of 16; static buffers need data; BC textures need level 0
  multiple of 4 and are sampled-only; depth textures take no initial data (typeless + SRV when also sampled);
  read-backs use a staging copy of all levels (a standalone 2x2 BC level would be invalid). Handles carry a
  12-bit generation: stale handles are rejected. Teardown releases everything; with the debug layer
  `ReportLiveDeviceObjects` must report 0 objects (tested; the negative control reports 1).
- **Measured** (dev machine): WARP + debug layer: all tests 0 debug errors. Hardware (NVIDIA RTX 5090, 11_1,
  debug layer) with the game's thread model (window owned and pumped by another thread, swap chain + Present
  on the render thread), VSync + 50 Hz pacer, 500 frames (`platform_tests` "present loop", `--no-skip`):
  average 50.08 Hz, slowest 1 s window 49.6 Hz, longest frame 27.5 ms, 0 schedule resets, 0 debug messages.
- **Config keys**: the M3 task text names `[render] limit_50hz` and `[debug] screenshot_frames`; the phase-1
  keys `[render] frame_rate` (50 = limiter, 0 = off) and `[render] screenshot_frames` are kept (no new keys).
- **For the integration**: D3D11 calls from slot 65 may run on a loading fiber's stack (`[kernel] fiber_stack`,
  design R6) - measure the stack high-water mark; no x87/`_controlfp` handling was needed.

**M3b results (pacing agent, add-only; supersede `frame_rate` default 50, `PaceFrame` and "Present(vsync ? 1 :
0)" of this section; API summary in `src/INTERFACES.md` 4.20, design `analysis/loader/pacing60.md`):**
- The game ticks at `[game] tick_rate` (60 by default, timing agent); `[render] frame_rate` defaults to it. The
  pacer couples the frames to the display of the window: Present(k) on a frame-latency waitable swap chain
  (maximum latency 1) when the refresh is k x the target (120 Hz -> k = 2), otherwise a 60 Hz high-resolution
  timer with Present(0, ALLOW_TEARING) and a VRR probe on the DXGI frame statistics (rejected -> Present(1) once
  per refresh), and a free-running timer with non-blocking presents while the window is in the background,
  minimized, the display is off or the session is locked. `[render] pacing = auto | vsync | timer | off`.
- `GpuRenderer::DisplayFrame` asks `platform::PlanPresent` for sync interval / tearing / skip and reports
  `rhi::Device::LastPresentTiming` back; `OnFrameDisplayed` calls `platform::EndFrame` (both backends).
- Measured on the dev machine (LG TV 3840x2160, 120/1 Hz from QueryDisplayConfig, RTX 5090, Debug + debug
  layer, `run\m3b-pacing\*`): auto in the foreground = `vblank k=2`: 1756 of 1757 vblank-coupled frames exactly
  2 refreshes on screen, frame starts 99.8 % within +-5 % of 16.67 ms, 60.07 Hz, 0 debug-layer messages;
  present-to-visible median 21.4 ms in the composed window (DWM latches one refresh before scan-out).
  `m3_check` (3000 frames, scenes at 180/480/816/1200, `--min-hz 59.4 --max-work-ms 16.7`): ALL PASS, 60.03 Hz
  (the window lost the foreground after ~10 s: 551 vblank + 2449 free frames). Minimized: free without presents,
  2 % of one core. `[game] tick_rate = 50` on 120 Hz: tearing-timer, VRR rejected in the composed window
  (2/3-refresh grid) -> display-rate 120 Hz. Null backend: headless timer, 900 frames in 15.0 s.
- **M3b phase 5 (review fixes, `docs/m3b_60hz_report.md`):** presenting resumes at once after a restore from
  minimized (no 30-frame hysteresis); `pacing::CadenceGuard` shifts the grid of `timer`/`free` by half a refresh
  when the on-screen durations keep missing k refreshes (alternating sign; on-time durations of a VRR panel count
  as regular; stops after 4 unsuccessful shifts); VRR probe with 2 s warm-up and one repeat after a
  rejection; the second exit line counts slow frame STARTS (Present intervals in parentheses); the on-screen line
  adds d >= 2 jumps and a least-squares refresh fit; `m3_check` M3b-7 judges foreground share and vblank
  coupling. The table above now states the M3b meaning of `vsync` and `frame_rate`.

## 9. Integration (phase 3, owns `gfx/nipc_renderer.cpp`)

- HostRenderer gains: `std::unique_ptr<rhi::Device>`, `ff::FixedFunctionPipeline`, `ni::ExtractArena`,
  `ni::StateShadow` (reset when the device is created), current `ni::CameraDesc`, frame counter, draw
  statistics. Device creation in the Create detour when
  `backend = d3d11` (window from `platform::MainWindow()`); failure -> GFX/error and fall back to the null
  renderer (the game must still run).
- Texture renderer data (`NiPCTextureData`, nipc_renderer.h): reserved host fields `+0x64` = host texture
  handle id (`rhi::TextureHandle::id`, 0 = none), `+0x68` = texconv status/source, `+0x6C` = uploaded
  NiPixelData revision. The movie player's YUY2 textures (+0x3C from the D3D shim) stay black until M6.
  `TextureResolver` looks up +0x64 along the chain at NiTexture +0x20; data destroy (slot 0 of the data
  vtables, slot 33) destroys the RHI texture.
- Slot 39 stays FALSE, NiGeometryData +0x30 stays 0 (renderer.md 5.3); a static geometry cache keyed by
  (data, id +0x2C, revision +0x2E) may follow and is invalidated by slot 58.
- Slots 69/70 keep TRUE without renderer data (P8 is expanded by texconv); 56/71 stay no-ops.
- Each slot keeps its M2 header comment, `ret N` static_assert and purge self-test.

**Phase-3 results (integration; supersedes the marked points of sections 2, 8, 9 and 11):**
- **`gfx::GpuRenderer`** (`gfx/nipc_gpu.h/.cpp`) is the host side of the d3d11 backend behind
  `HostRenderer::gpu`; `nipc_renderer.cpp` keeps the guest side. Create: `[render] backend = d3d11` (now the
  default) + `platform::MainWindow()` -> device (`WantDebugLayer`, `adapter`), `FixedFunctionPipeline::Init`,
  the 8x8 default texture (FUN_0014AB50) and a black 1x1 texture, `DefaultCamera`, back buffer + depth bound;
  any failure -> GFX/error and the null renderer. Create line: `... -> NiPCRenderer 0x.. (d3d11: <adapter>,
  <api>, debug layer <state>; vtable ...)`.
- **Slots:** 15 NULL -> `SetRenderTargets({}, {})` + whole-target viewport, a texture -> draws and clears are
  counted no-ops (`texture target`, P2); 16 -> `GpuRenderer::DisplayFrame`: frame summary (trace/screenshot
  frames), screenshot (`screenshot_frames` or F12), `Present({vsync})` (DeviceLost -> GFX/error +
  `core::Fatal`), `platform::NotePresentedFrame`, arena reset; then (both backends) `platform::PaceFrame` (the
  M2 pacer of nipc_renderer.cpp is gone), counter, `--frames`; 17 ClearBuffer (new handler, ret 8): whole-target
  viewport, mode bit 1/4/2 -> color/depth/stencil, `ni::ClearRect`, empty rect -> nothing; 19 swap flag to the
  GPU; 27 `_ftol(c * 255) & 0xFF` like 0x14B0C6 (M2 clamped); 45 `ni::BeginScene`; 47 `ni::ExtractCamera` (target
  size + swap of the GpuRenderer) -> viewport + `ff.SetCamera`; 48-51/54/55 outside a scene -> counted
  `NotInScene`, else `ni::Extract*` -> `ff.Draw` with outcome counting (`Drawn/Skipped/Failed/Empty/
  Unsupported/Error/NotInScene/TextureTarget`, first non-drawn outcome per kind logged; `Error` on GFX/warn).
- **Viewport rule:** the camera handed to the pipeline always carries the viewport set on the context
  (ClearBuffer changes only the viewport). **Half-pixel correction (supersedes section 11, first point):** the
  projection and the screen projection given to `ff.SetCamera` are shifted by +0.5 pixel (`clip.x += w / vw`,
  `clip.y -= w / vh`), so transformed geometry covers the D3D8/NV2A pixels too; measured against the xemu
  frontend captures on the static pixels: 61 % -> 93 % exactly equal.
- **Slot 65** = FUN_0015E010 complete (texconv `SelectTextureSource` with the image format and FormatPrefs ->
  FUN_001572D0 in both backends; +0x14..+0x33 = table format of the chosen entry, default texture = entry 5;
  +0x50 = uploaded mip levels; +0x54 from FormatPrefs +0x18); GPU path: `ConvertPixelData` (BGRA8 / BC1-3,
  `keep16Bit` off) -> `CreateTexture`; host fields +0x64 handle id, +0x68 `tex::TextureSource` |
  `kHostOwnsTexture` (0x100), +0x6C revision; data slot 0 destroys an owned texture. A slot-65 stack probe
  measures the loading-fiber stack use (log line `NiPCRenderer slot 65 stack: ...`).
- **TextureResolver** = FUN_0015F930: data created on demand for an NiSourceTexture, FUN_0015E260 re-upload when a
  non-static texture's (+0x38 == 0) NiPixelData revision changed, movie (+0x3C from the shim) and rendered
  textures -> black texture.
- **Log lines** (m3_check reads them): `draw trace frame N #i: <Kind> 0x.. -> <outcome>[: note]: ...` for
  `trace_frames`; `draw summary frame N: X draw calls, Y drawn, Z not drawn, C clears; <Kind> n (<outcome> n,
  ...)` for trace and screenshot frames; exit reporter "gfx": `NiPCRenderer textures: ...`, `NiPCRenderer slot 65
  stack: ...`, `d3d11 draws <Kind>: N calls - drawn, skipped, failed, empty, unsupported, error, not in scene,
  texture target`, `d3d11 frames: ...`, `ff pipeline: ...`, `ni_extract: ...` (ExtractCounters), `d3d11 device:
  ...`. A traced frontend frame (~345 lines) costs ~30 ms of logging - trace frontend frames only in diagnostic
  runs.
- **Check:** `tools\m3_check.py` (all M3 criteria + `m2_check.check_run` + general rule; screenshots compared with
  `run\xemu_ref`). Configs: `run\m3\futurama.ini` (Debug), `run\m3\rel\futurama.ini` (RelWithDebInfo,
  `debug_layer = on`), regressions `run\m3\m1`, `run\m3\m2` (d3d11), `run\m3\m2-null`.

**Phase-5 results (review fixes, `docs\m3_report.md` section 8; supersede the marked points of sections 5, 8, 9
and 11):**
- **Slot 65 step 4** (finding 1): after the upload FUN_0015E010 frees the NiPixelData pixel buffer in the game
  pool (0x15E1A6..0x15E1DD: image not the default texture, NiSourceTexture +0x3C != 0, level 0 <= 512 x 512 ->
  FUN_001511B0, `guest::kNiPixelDataFreePixels`); both backends do the same (the null renderer as if the upload
  had succeeded). `ni::kNiSourceTextureStatic` (+0x38), `ni::kNiSourceTextureDiscardPixelsHint` (+0x3C). The
  resolver skips a revision re-upload whose buffer is gone (counted). Exit lines `NiPCRenderer pixel buffers
  (...): N freed after the upload (K KiB of the game pool), ...` and (exit reporter "game") `game memory pool: U
  KiB in use, F KiB free, peak P KiB in use`. Frontend run: 115 buffers, 2558 KiB; pool at exit 5839 KiB instead
  of 8295 KiB, peak 6855 KiB instead of 8892 KiB.
- **Persistent back buffers and the present pass** (findings 2, 8, 11; supersedes the swap-chain points of the
  phase-2 results in section 8): the device renders into `DeviceDesc::bufferCount` (NiPCRenderer: 2) offscreen
  back buffers in rotation; after Present the back buffer holds the picture of two presents ago like the Xbox flip
  (Create gets D3DSWAPEFFECT_DISCARD = 1 from FUN_00146260 with BackBufferCount 1). Frames that draw nothing or clear
  only depth (legal screen / tech notice: 0, 6 or 12 draws per frame) therefore show defined pictures on every
  driver. Present draws the current back buffer into a window-size FLIP_DISCARD swap chain (ResizeBuffers when the
  view's physical client size changes; the size is read in the view's DPI context) with `[render] scaling`.
  Screenshots read the current back buffer (always 640x480). `draw summary frame N` names the frame whose picture
  a frame without draws shows (`shows the picture of frame M (K draw calls, D drawn)`); `d3d11 frames:` counts
  frames without a draw / without a color clear / with neither.
- **Vertex snapping** (finding 9; extends the half-pixel correction of the phase-3 results): the ubershader snaps
  transformed vertices to 1/16 pixel in D3D8 pixel coordinates (`SnapToSubpixelGrid`, round to nearest; NV2A
  precision) before the D3D11 rasterizer (8 fractional bits) sees them. The "Neues Spiel" button top edge lies at
  y = 189.000..189.008: without snapping row 189 stayed uncovered. Static frontend pixels exactly equal to xemu:
  93.0-93.8 % -> 98.0-98.8 % (mean difference 0.07-0.28 -> 0.011-0.015; truncation instead of rounding: 89 %).
- **Slot 60 PurgeEffect** (finding 4): IsKindOf(NiLight) through the RTTI chain (`guest::kNiLightRtti`) ->
  `ni::PurgeLight` removes the light from `StateShadow::lights` (FUN_0015B9E0; `lastEffectState` stays).
- **Close and exit** (findings 5, 10): WM_CLOSE sets `platform::CloseRequested()`; the next DisplayFrame ends the
  process on the guest main thread (`window closed - ending after frame N`), so the exit reporters run on the
  render thread; a 3 s timer of the UI thread is the fallback. The "d3d11" reporter pumps the device's info queue
  once more when it runs on the render thread; `GpuRenderer::LogStatistics` logs only a note on another thread.
- **Frame work** (finding 3): `platform::NoteFrameWorkDone()` (DisplayFrame, before screenshots and Present) ->
  third `present:` line `present: frame work N frames: average A ms, 99th percentile P ms, longest L ms (W frames
  longer than one pacing period) -> sustainable S Hz`. `m3_check` M3-3: P <= 20 ms and average presented rate >=
  49.5 Hz (a pacer schedule reset costs ~0.017 Hz forever).
- **Draw trace** adds the screen bounds of each draw in D3D8 pixels (`screen (x0, y0)-(x1, y1)`).
- **xemu references**: `run\xemu_ref` and `run\xemu_ref_steam` are 2x nearest-neighbour window grabs of a 1x
  rendering (every 2x2 block equal) - not a 2x internal resolution. `run\xemu_ref_steam` (0.62 s interval) shows
  the steam cloud at the right pipe in all phases of its 1 s cycle; the 3 s captures of `run\xemu_ref` always hit
  the invisible phase (finding 7). `m3_check` builds the frontend's static mask from both directories and masks the
  steam region explicitly.

## 10. Ownership (phase 2)

Edit only your files. Shared files: add-only changes, each logged in `src/INTERFACES.md` section 8.

| Owner | Files | Build dir | Run dir |
|---|---|---|---|
| **backend agent** | `src/host/gfx/rhi/d3d11/**`, `src/host/platform/**`, `tests/rhi_*.cpp`, `tests/platform_*.cpp` | `build\m3-backend` | `run\m3-backend\` |
| **texconv agent** | `src/host/gfx/texconv.*`, `tests/texconv_*.cpp` | `build\m3-texconv` | `run\m3-texconv\` |
| **extract agent** | `src/host/gfx/ni_extract.*`, `src/host/gfx/ni_guest_layouts.h`, `tests/extract_*.cpp` | `build\m3-extract` | `run\m3-extract\` |
| **shader agent** | `src/host/gfx/ff_pipeline.*`, `src/host/gfx/shaders/**`, the `fut_add_shader` lines of the gfx section in `src/host/CMakeLists.txt`, `tests/ff_*.cpp` | `build\m3-shader` | `run\m3-shader\` |
| **integration (phase 3)** | `src/host/gfx/nipc_renderer.*`, `gfx/nipc_validate.*`, `gfx/d3d_shim.*` (movie texture hand-over), `gfx/gfx_selftest.cpp`, `tools/m3_check.py`, `docs\*` | `build\m3-integ` | `run\m3-integ\` |
| **shared (phase 1, add-only)** | `src/host/gfx/rhi/rhi.h`, `rhi.cpp`, `src/host/gfx/gfx_math.h`, `src/host/gfx/shaders/ff_shared.h` (layout part; shader agent adds), `src/host/gfx/gfx_config.*`, `src/host/core/config.*`, `src/host/CMakeLists.txt`, `src/host/gfx/GFX_CONTRACT.md`, `src/INTERFACES.md`, `tests/gfx_contract_tests.cpp` | | |

**Real-data harness (phase 2)**: `[render] validate = 1` (with `backend = null`) makes the null renderer run
`ni::ExtractCamera` in slot 47, `ni::Extract*` in slots 48-51/54/55 (inside a scene, with renderer +0x0C/+0x10)
and `tex::ConvertPixelData` on the image slot 65 selected - on the real guest data of a game run, without D3D
(`gfx/nipc_validate.*`, phase-1 code owned by the integration later). Log: the first non-Ok/Empty result per
draw kind and status, every draw of the `[render] trace_frames` frames (`validate frame N: <kind> <object> ->
<status>: vertices, indices, primitives, stages, stage-0 texture, lighting, lights, blend, z, cull`) and at
the end `validate: extract <kind>: N calls - ok, empty, unsupported, error` plus `validate: N cameras;
texconv: ok ...; source formats ...; host formats ...; arena peak ...`. Template:
`run\m3-iface\validate\futurama.ini` (1000 frames, trace frame 950 = frontend menu: 346 Shape draws in phase
1). Phase-1 totals over 1000 frames: Shape 71 123, ScreenPolygon 45, ScreenTexture 4838 (intro), 976 cameras,
136 textures. The extract and texconv agents report these lines with 0 unsupported / error for the slots M3
needs.

Rules for every agent: build with `cmd /c "D:\FuturamaDecomp\build.cmd" build\<your dir> Debug` (also
RelWithDebInfo and `... all clang` before handing over), zero warnings with `FUT_WERROR=ON`, all unit tests
pass (`bin\fut_tests.exe`), `tools\m2_check.py` with `[render] backend = null` still passes
(`run\m3-iface\de\futurama.ini` is the template), game data and screenshots only under `run\<name>\`.
Agents that need guest code at runtime (extract: FUN_0014F710; the game run) use their own run directory.
Report deviations from this contract in your result instead of silently diverging.

## 11. Known differences and open points

- Half-pixel: pre-transformed quads are shifted +0.5 (exact pixel coverage); transformed geometry follows
  D3D11 rasterization rules (D3D8 put pixel centers on integers - sub-pixel shift, invisible at 640x480).
  **Superseded in phase 3:** the integration shifts the projection by +0.5 pixel as well (section 9,
  "Phase-3 results"); the shift was measurable against the xemu captures.
- Colors > 1 or < 0 saturate (the original's `ROUND(c * 255)` wraps); ambient sum clamps (FUN_0015BA40 wraps).
- FormatPrefs color reduction (32 -> 16 bit) is not reproduced; textures may look better than on the Xbox.
- Multi-pass texture pipelines, NiTextureEffect (projected lights/shadows, environment/fog maps), render to
  texture (slots 15/66), points/lines (52/53), batch slots and the screen-space offset option are P2.
- DAT_002F91D4 (spot angle scale) lies in BSS; ASSUMPTION pi until read at runtime.
- NiPalette entry byte order and PAL alpha: ASSUMPTION until verified with game data.
- The pipeline's base-map stage setup lives partly in Ghidra gap code (0x164360..0x1650F0).
- Window resize keeps a 640x480 back buffer (stretched); native resolution and widescreen are M10.
  **Phase 5:** the present pass scales it (`[render] scaling = linear | point | integer`); higher internal
  resolution and widescreen stay M10. **M3c:** the internal render resolution, MSAA, supersampling and
  anisotropic filtering came with M3c (section 12); widescreen stays a later point.
- **Phase 5:** transformed vertices are snapped to 1/16 pixel (NV2A precision, section 9 "Phase-5 results");
  pre-transformed quads (slot 55) are not snapped (integer coordinates in the scenes measured).

## 12. M3c: render resolution and anti-aliasing (renderer agent)

Scope: `docs/ROADMAP.md` "M3c" (anti-aliasing bullet list) and contract item 5 of the M3c workflow. User
requirements: anti-aliasing by MSAA (no FXAA, no TAA), native render resolution with supersampling on top, the
"classic" configuration bit-identical to M3 (xemu regression). Code: `gfx/gfx_config.*`, `gfx/nipc_gpu.*`,
`gfx/ff_pipeline.*`, `gfx/shaders/*`, `gfx/rhi/rhi.h`, `gfx/rhi/rhi.cpp`, `gfx/rhi/d3d11/*`,
`platform/screenshot.*`, `gfx/nipc_renderer.cpp` (one line); tests `tests/aa_tests.cpp`; check `tools/m3c_check.py`.

### 12.1 Configuration (`[render]`, schema rows in `core/config_schema.cpp`)

| Key | Values (default) | Meaning |
|---|---|---|
| `resolution` | `auto` \| `<W>x<H>` (auto) | output size of the 4:3 image: the largest 4:3 rectangle inside the window's view (auto) or inside W x H (`1920x1080` -> 1440x1080, `640x480` = Xbox); W, H 160..16384 (`core::ParseConfigResolution`) |
| `render_scale` | 1.0..2.0, UI step 0.25 (1.0) | supersampling factor on top: the back buffers are output x scale |
| `msaa` | `off` \| `2` \| `4` \| `8` (4) | samples per pixel of the back buffers and their depth buffer; clamped to the device (`rhi::Device::SupportedSampleCount`, GFX/info line) |
| `alpha_to_coverage` | bool (on) | alpha-tested materials without blending use alpha-to-coverage with alpha sharpening (only with MSAA) |
| `anisotropy` | `off` \| `2` \| `4` \| `8` \| `16` (16) | anisotropic filtering of mip-mapped 3D textures |
| `screenshot_size` | `output` \| `internal` (output) | screenshots of the presented image (output size) or of the resolved back buffer (output x render_scale) |
| `linear_aa` | bool (off) | review M3c finding 3: the MSAA resolve and the downsample average linear light (sRGB views of TYPELESS targets) instead of gamma-encoded values; rendering and blending unchanged; off = the Xbox's gamma resolve |

The existing `scaling` (linear | point | integer) applies when the output is smaller than the view. Classic =
`resolution = 640x480`, `render_scale = 1`, `msaa = off`, `anisotropy = off`. The defaults (`gfx::kDefault*`) equal
the schema defaults (unit test "aa: [render] M3c key parsers agree with the configuration schema"); the parsers
`gfx::ParseRenderResolution/ParseRenderScale/ParseMsaa/ParseAnisotropy` use the schema's value parsers.

### 12.2 Coordinate rule: the guest keeps 640x480

The guest renders a 640x480 world: slots 21/22 return the Create size, `ni_extract` computes viewports, clear
rectangles and pre-transformed quads in guest pixels, the ff camera (and so the `pretransform` constants, the +0.5
pixel shift of B19, the 1/16-pixel NV2A snap of B33 and the slot-54 screen camera) stays in guest pixels. Only the
D3D11 side grows (`gfx::RenderSizes`: output, internal = output x render_scale):
- viewport: `ScaleViewport` (guest -> internal pixels, computed in double); everything inside the viewport is
  resolution independent, so transformed geometry, slot 54 and slot 55 quads land on the same (scaled) place;
- clear rectangles: `ScaleRect` (each edge rounded to the nearest internal pixel; adjacent rectangles stay adjacent);
- the 1/16-pixel snap stays a 1/16 GUEST pixel (internal size / 640 / 16 internal pixels): vertices keep the
  position the original rasterizer gave them; the high-resolution picture is a supersampled version of that
  geometry and box-downscaled matches the xemu captures (X1 of m3c_check);
- render to texture (slot 15 with a texture, P2) keeps the texture's own size (scale 1); scissor is never enabled
  (a future one goes through `ScaleRect`); lines and points (slots 52/53, wireframe fill) are P2 and 1 internal
  pixel wide - they must be widened by the scale when they are implemented.

Point-filtered textures (all intro screens: filter 0/0/0) on a target larger than 640x480 are sampled as
"anti-aliased nearest" (`FF_STAGE_SHARP_NEAREST`, `ff::RenderOptions::sharpNearest`, `UsesSharpNearest`): bilinear
sampler, coordinates held on the texel center except within one pixel of a texel border. Plain nearest at the
non-integer scale 4.5 (2880x2160) gives alternating 4- and 5-pixel texels; anti-aliased nearest keeps every texel
4.5 pixels wide with a one-pixel seam (legal screen vs xemu after the box downscale: 1.28 -> 0.73; the rest is the
half-pixel grid of 4.5 itself). Classic is untouched (the flag is only set when internal != guest size).

### 12.3 Back buffers, MSAA, resolve, downsample, present (`rhi::DeviceDesc` / D3D11 backend)

- `DeviceDesc::width/height` = internal size, `sampleCount` (clamped), `outputWidth/outputHeight` (0 = no
  downsample); `DeviceInfo::sampleCount/outputWidth/outputHeight`.
- The persistent back buffers (Xbox flip, M3 B31) are multisampled themselves (one D24S8 depth buffer with the same
  samples): a frame without draws continues on the samples of two presents ago. Present: `ResolveSubresource` of the
  current buffer (box average of the samples) -> downsample to the output size (`ResamplePS` in
  `d3d11_builtin.hlsl`: exact box filter when the ratio is a whole number per axis - render_scale 2 = 2x2 samples
  per output pixel; Catmull-Rom (B = 0, C = 1/2) stretched by the ratio otherwise, clamped to the range of the 2x2
  source pixels nearest to the destination center - review M3c finding 1: the Mitchell-Netravali filter B = C = 1/3
  before drew text and 2D visibly softer; offline against the render_scale 2 box reference the edge error at 1.5
  drops from 8.8 to 5.0 (legal) and 3.2 to 2.05 (frontend) without overshoot, `docs/m3c_report.md`) -> present pass
  into the swap chain: 1:1 (point) when the output fills the view, `[render] scaling` when it is smaller,
  `ResamplePS` when it is larger than the view (no bilinear decimation).
- `DeviceDesc::linearResolve` / `DeviceInfo::linearResolve` (`[render] linear_aa`, review M3c finding 3): back
  buffers, resolve texture and output image are created TYPELESS; the game renders through UNORM views (blending
  exactly as before), `ResolveSubresource` uses the `_SRGB` format and `ResamplePS` reads and writes `_SRGB` views
  (the downsample in the present pass writes an `_SRGB` view of the flip-model swap chain buffer). The 1:1 copy and
  the enlarging present pass stay in gamma. Unit test: a 2:1 checkerboard gives 188 instead of 128, an MSAA edge
  pixel with coverage c gives encode(c) instead of 255c.
- Review M3c finding 2: `GpuRenderer::Create` steps down when the back buffers of a configuration cannot be created
  (`QualityFallbacks`: the request, render scale 1, MSAA halved down to off, 640x480 without MSAA; GFX/warn per step,
  the `render:` line ends with `reduced from <request>`); the view-following resize steps down the same way (scale 1,
  fewer samples) before it keeps the old buffers. When even 640x480 fails, `NiXBoxRenderer::Create` ends the game
  with `core::Fatal` (exit code 3) - the null renderer never showed the window, the game ran invisibly and a waiting
  launcher hung with it.
- `CaptureBackBuffer` = resolved internal picture, `CaptureOutput` = the downsampled image (both mid-frame safe: the
  context re-binds its state, `D3D11Context::RestoreBindings`).
- `ResizeBackBuffers(w, h, samples, outW, outH)`: new targets; the pictures of the persistent buffers are resolved
  and stretched/downsampled into the new ones (Xbox-flip contents survive a window resize), depth = 1, stencil 0.
  `GpuRenderer` calls it for `resolution = auto` when the view size stayed changed for `kResizeSettleFrames` (12)
  frames (window resize, Alt+Enter) and re-applies the guest viewport.
- Descriptors (add-only): `BlendDesc::alphaToCoverage`, `RasterDesc::multisample` (MultisampleEnable for MSAA
  targets), both hashed. Rectangle color clears use `ClearView` (works on MSAA RTVs), depth/stencil rectangles the
  ClearVS triangle (unchanged).
- GPU frame timing (`DeviceStats::gpuFrame*`): timestamp queries from the first clear/draw of a frame to the end
  of its present pass, read back without flushing (a slot still pending after 6 frames is waited for).
- Present flushes the immediate context when it skips the swap chain (minimized, display off, headless): before,
  nothing submitted those frames until the driver's next implicit flush (section 12.6).
- Memory (`render:` line): 2880x2160 with 4x MSAA 308 MiB; 5760x4320 (render_scale 2) with 8x MSAA 2396 MiB. The
  launcher's presets keep the largest buffer within the D3D11 limit per resource (max(128 MB, min(VRAM/4, 2 GB)))
  and the total within half the video memory of `--query-gpu` (review M3c finding 21).

### 12.4 Fixed-function pipeline (`ff::RenderOptions`)

- MSAA: `RasterDesc::multisample` for every draw on a multisampled target.
- Alpha-to-coverage (`UsesAlphaToCoverage`): MSAA on, `[render] alpha_to_coverage`, alpha test on, alpha blend OFF
  (blended cut-outs keep the discard: their blend softens the edge already), function GREATER / GREATEREQUAL / LESS /
  LESSEQUAL and not trivially always/never passing. The pixel shader (`FFDrawConstants::alphaTest.w = 1`) replaces
  the discard by `color.a = saturate((alpha - threshold) / fwidth(alpha) + 0.5)` (reversed for LESS*), threshold =
  where the 8-bit test switches ((ref +- 0.5) / 255): the 50 % coverage line lies exactly on the alpha test's cut,
  the transition is one pixel wide (alpha sharpening: crisp when magnified, no fading when minified).
  Known difference (review M3c finding 5): the target's alpha channel receives this coverage alpha (inside mostly
  1.0) instead of the texel alpha the D3D8 test leaves. Only a later blend with the destination alpha would show it;
  `ff::ReadsDestinationAlpha` (DESTALPHA, INVDESTALPHA, SRCALPHASAT in any factor) suspends alpha-to-coverage for the
  rest of the session at the first such draw (GFX/warn; exit line `N draws with a destination-alpha blend (alpha to
  coverage suspended)`). The frontend has none (283 draws without blending, 61 SRCALPHA/INVSRCALPHA). Writing the
  coverage through SV_Coverage would keep the texel alpha but needs shader model 4.1; flicker of alpha-to-coverage
  under camera motion is checked with a test scene in M4/M8.
- Anisotropy (`MakeSamplerDesc(stage, maxAnisotropy, screenSpace)`): only mip-mapped stages with linear
  minification AND magnification on transformed geometry; point-filtered (also MIN = LINEAR / MAG = POINT, which the
  anisotropic filter would magnify linearly - review M3c finding 4), non-mip-mapped and screen-space (slots 54/55)
  stages keep their D3D8 filter. D3D11's anisotropic filter blends mip levels (a bilinear-mip texture becomes
  trilinear). Its benefit is not measurable in the reachable scenes (no oblique surfaces in the static frontend);
  to be checked with the free camera (M4b) or a synthetic scene.
- Statistics: `PipelineStats::alphaToCoverageDraws / alphaTestDiscardDraws / anisotropicStages /
  sharpNearestStages` -> exit line `render totals: ...`.

### 12.5 Log lines (tools/m3c_check.py reads them)

- `render: output WxH (resolution auto: view WxH | WxH), internal WxH (render scale S, X x Y per 640x480 pixel),
  MSAA Nx (requested Mx; supported: 2, 4, 8), alpha to coverage on|off, anisotropy Nx, downsample
  none|box|catmull-rom, screenshots output|internal, back buffers N MiB, resolve gamma|linear light[, reduced from
  render scale S, MSAA Nx, resolution R]` (Create); a clamp adds `render: [render] msaa = M is not supported ... -
  using Nx`; a step down `render: D3D11 back buffers WxH with N samples not available (...) - trying ...` (GFX/warn).
- `render: view resized to WxH - output ... -> ..., internal ... -> ...` (resolution auto).
- `d3d11: device on ... N back buffers WxH BGRA8_UNorm (S samples), depth ..., output WxH, ...`,
  `d3d11: back buffers resized from ... to ...`.
- Exit: `render totals: output WxH, internal WxH, MSAA Nx, R back-buffer resizes (F failed)[, quality reduced]; alpha
  test: A draws with alpha to coverage, D with discard; S anisotropic texture stages, P point-filtered stages as
  anti-aliased nearest; N draws with a destination-alpha blend[ (alpha to coverage suspended)]`, `d3d11 gpu frame time: N frames timed, average A ms, 99th percentile P ms, longest L ms` (span from the first
  clear/draw of a frame to the end of its present pass: GPU work plus the gaps while the CPU still submits),
  `screenshots: N written, M failed`.
- Screenshots: captured on the render thread, the PNG is encoded on a worker thread (a 2880x2160 PNG takes
  100+ ms); the worker logs `screenshot: <path> (WxH)` when the file is complete; the exit reporter "screenshots"
  waits for pending files (`platform::FlushScreenshots`). The synchronous `SaveScreenshot(device, dir, frame)` of
  M3 is unchanged (tests).

### 12.6 Evidence (renderer agent, 2026-10-06; dev machine RTX 5090, display OFF during the runs)

- `fut_tests` (Debug, RelWithDebInfo, clang-cl, FUT_WERROR=ON): all pass; `tests/aa_tests.cpp` 11 cases on WARP +
  debug layer: MSAA resolve and partial coverage, ClearView on MSAA, Xbox flip with MSAA, box/Mitchell downsample,
  resize carry-over, GpuRenderer at 2x internal, alpha-to-coverage, anti-aliased nearest, GPU timing over 40
  frames without warnings, present pass 1:1/up/down into a window.
- Classic vs the pre-M3c build (`build\m3b`): legal / tech notice / frontend frame 1200 screenshots bit-identical
  (later runs differ only in animated content: lightning, gears, highlight pulse); xemu 0.00 / 0.00 / 0.30 / 0.02.
- `m3c_check` (Debug, debug layer on, `run\m3c-renderer\check\m3c_check\report.txt`): ALL PASS for classic, hq,
  hq_noaa, ref, ss - 0 debug-layer errors and warnings, every draw drawn, all M2 criteria, render configuration and
  screenshot sizes as expected (hq: auto in borderless fullscreen = 2880x2160 internal; ref 5760x4320 8x box; ss
  1440x1080 output / 2880x2160 internal box); presented 59.93-60.03 Hz, frame work p99 5.9-6.2 ms (average 1.4 ms
  for the AA variants, 3.55 ms classic incl. the trace frames); GPU span average 0.35 ms classic, 1.0 ms hq,
  1.57 ms ref. X1 (hq downscaled vs xemu) legal 0.73, tech notice 0.18, loading 0.61, frontend 0.98 (limits 1.0 /
  1.0 / 2.0 / 2.0). X2 edge error vs the render_scale 2 + 8x reference: no AA 3.13, MSAA 4x 1.47 (ratio 0.47),
  hard steps 1.7 % -> 0.1 %.
- `m3c_check` with the RelWithDebInfo build (debug layer on): ALL PASS, frame work p99 3.7-4.7 ms, X1 as above,
  X2 ratio 0.45 (no AA 3.13, MSAA 1.41).
- Found on the way: with the display off (or minimized) the pacer skips the swap chain, and nothing flushed the
  immediate context any more - the frame's GPU work waited for the driver's next implicit flush. Before the fix the
  frame work p99 was 15-22 ms in every variant and 55 ms with the unchanged pre-M3c build (the GPU timer queries
  even ran into debug-layer warning #408). `D3D11Device::Present` now flushes when it skips the swap chain; p99
  6 ms afterwards. With the display on the swap-chain Present flushes as before (not re-measured here: the display
  of the dev machine was off, so the present pass ran only in the windowed unit test).

### 12.7 Review fixes (M3c phase 4, 2026-10-06; display on)

- Findings 1-5 of the M3c review (`docs/loader_design.md` 8.8 R1-R5): Catmull-Rom downsample with the 2x2
  anti-ringing clamp, the quality ladder of `GpuRenderer::Create` + `core::Fatal` instead of the invisible null
  renderer, `[render] linear_aa`, anisotropy only with linear magnification, the destination-alpha rule of
  alpha-to-coverage. Finding 6 (present-to-visible latency varies by 1-2 refreshes per session) goes to M3d.
- `fut_tests` 259/259 (Debug, RelWithDebInfo, clang-cl; FUT_WERROR=ON); new aa cases: quality ladder, linear resolve
  (checkerboard 128 -> 188, MSAA edge = encode(coverage) within 1 level on WARP), fractional downsample without
  over-/undershoot, MIN = LINEAR / MAG = POINT keeps its filter, `ReadsDestinationAlpha`, present pass with an sRGB
  swap-chain view under the debug layer.
- `m3c_check` with the new variants `max` (render_scale 2 + 8x + linear_aa) and `frac` (1.5 + 4x + linear_aa):
  Debug and RelWithDebInfo ALL PASS, debug layer 0 messages; X1 0.73 / 0.18 / 0.60 / 0.98, X2 ratio 0.44-0.46.
  Classic `m3_check` (xemu 0.00 / 0.00 / 0.29-0.30 / 0.01) ALL PASS in all three builds.
- 16384x12288 with 8x: `E_OUTOFMEMORY` -> one step down to 4x, the game runs visibly (`run\m3c_final\cases`).
