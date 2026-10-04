"""Write analysis/loader/nixboxrenderer_vtable.tsv.
Addresses/override flags are read from the XBE; names, confidence and callers
are the curated results of renderer.md (evidence there; slots 16/17/31/39/54/65/69
revised by the independent review, see renderer.md "## Verifikation"). Caller lists come from
renderer_callers.py (binary scan), renderer_vcalls2.py (decomp scan) and the
broadcast functions over the renderer list DAT_002d7b1c."""
import sys, csv
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe
X = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
NIR, NIX = 0x2a9ef0, 0x2aa7c0
PURE = 0x1668b6   # _purecall -> FUN_001692b4(0x19) = _amsg_exit(R6025)
INFO = {
 0: ("~NiXBoxRenderer (scalar deleting dtor)", "high", "NiPointer-Release (generisch)"),
 1: ("GetRTTI", "high", "generisch (IsKindOf)"),
 2: ("NiObject::vf02 (geerbt)", "high", "-"), 3: ("NiObject::vf03 (geerbt)", "high", "-"),
 4: ("NiObject-Stub (geerbt, leer)", "high", "-"), 5: ("NiObject::vf05 (geerbt)", "high", "-"),
 6: ("NiObject::vf06 (geerbt)", "high", "-"), 7: ("NiObject::vf07 (geerbt)", "high", "-"),
 8: ("GetViewerStrings (NiRenderer)", "medium", "Debug-Viewer"),
 9: ("NiObject::vf09 (geerbt, leer)", "high", "-"), 10: ("NiObject::vf09 (geerbt, leer)", "high", "-"),
 11: ("GetDriverInfo", "high", "keine gefunden"),
 12: ("GetFlags (Caps, this+0x2d8)", "high", "keine gefunden"),
 13: ("FindClosestPixelFormat(const NiTexture::FormatPrefs&)", "medium", "keine virtuellen (intern direkt FUN_001572d0)"),
 14: ("unbekannt (Basis 0x800000, Xbox 0; 2 Args)", "low", "keine gefunden"),
 15: ("SetRenderTarget(NiRenderedTexture* oder NULL=Backbuffer)", "high", "GAME FUN_0001f1a0(2x), FUN_0003b960(2x)"),
 16: ("DisplayFrame (D3DDevice_Swap nur wenn Backbuffer aktiv; danach this+0x64=0 und VB-Manager-Framezaehler++ via FUN_00157970)", "high", "GAME FUN_000d2630 (Hauptschleife), FUN_0002a660(8x), FUN_000a62b0(3x), FUN_000c8e90(2x), FUN_000ee080, FUN_000a26a0(2x); NI FUN_000feec0 (NiCamera-Wrapper <- GAME FUN_0003b960)"),
 17: ("ClearBuffer(const NiRect<float>*, uiMode 1=Back 2=Stencil 4=Z; Z/Stencil nur mit Depth-Surface; setzt vorher Viewport auf volle RT-Groesse)", "high", "GAME FUN_000d2630, FUN_0001bbb0, FUN_0001f1a0(2x), FUN_00021a80, FUN_0002a660(8x), FUN_000a4bb0, FUN_000a62b0(3x), FUN_000a8290, FUN_000c8e90(5x), FUN_000d62e0, FUN_000e1970, FUN_000e3310, FUN_000a26a0(2x); NI FUN_000fee90 (NiCamera-Wrapper <- GAME FUN_0003b960, FUN_00045f10, FUN_000e8920, FUN_000ede90, FUN_000ee080)"),
 18: ("GetLeftRightSwap", "medium", "keine gefunden"),
 19: ("SetLeftRightSwap(bool)", "medium", "keine gefunden"),
 20: ("SetWBuffer/UseWBuffer(bool) -> D3DRS_ZENABLE=USEW", "medium", "keine gefunden"),
 21: ("GetBufferWidth (aktuelles Render-Target)", "high", "GAME FUN_000d5bb0, FUN_000ef370, FUN_000ef680"),
 22: ("GetBufferHeight (aktuelles Render-Target)", "high", "GAME FUN_000d5bb0, FUN_000ef370, FUN_000ef680"),
 23: ("GetBufferDepth (Bits/Pixel des Render-Targets)", "medium", "keine gefunden"),
 24: ("TakeScreenShot(const NiRect<unsigned>*) -> NiPixelData*", "high", "keine gefunden"),
 25: ("Blt/FastCopy(NiBltSource*, rect*, x, y) -> D3DDevice_CopyRects", "medium", "keine gefunden"),
 26: ("SetDepthClear(float)", "high", "keine gefunden"),
 27: ("SetBackgroundColor(const NiColor&)", "high", "GAME FUN_000d26a0, FUN_00012210, FUN_00012cc0, FUN_00012d20, FUN_00021a80, FUN_0002a660, FUN_0003b960(2x), FUN_000a62b0, FUN_000c8e90(3x), FUN_000ede90(2x), FUN_000ee080"),
 28: ("SetStencilClear(unsigned)", "high", "keine gefunden"),
 29: ("GetMaxFogValue", "medium", "keine gefunden"),
 30: ("SetMaxFogValue(float)", "medium", "keine gefunden"),
 31: ("PrecacheTexture(NiTexture*, ?, ?) -> jmp TextureManager-vf3 0x15f7e0 (ret 0xc = 3 Stack-Args; Ziel liegt ausserhalb jeder Ghidra-Funktion)", "medium", "keine gefunden"),
 32: ("PurgeAllTextures(bool)", "medium", "keine gefunden"),
 33: ("PurgeTexture(NiTexture*) (RendererData +0x20 loeschen)", "medium", "keine gefunden"),
 34: ("unbekannt (Basis-Stub return false, 2 Args)", "low", "keine gefunden"),
 35: ("unbekannt (Basis-Stub return false, 6 Args)", "low", "keine gefunden"),
 36: ("unbekannt (Basis-Stub return 0)", "low", "keine gefunden"),
 37: ("unbekannt (pure in NiRenderer, Xbox return false)", "low", "keine gefunden"),
 38: ("unbekannt (pure in NiRenderer, Xbox return 0)", "low", "keine gefunden"),
 39: ("PrecacheGeometry(NiGeometry*, uiFlags (Bit0: CPU-Arrays nach Upload in BeginScene freigeben), NiRendererSpecificProperty* (Vermutung), ?, ?); geskinnt -> FUN_00148170, sonst nur NiTriShape/NiTriStrips -> FUN_00148020", "medium", "GAME FUN_000ce930(2x), FUN_000d0dd0, FUN_000d1120"),
 40: ("unbekannt (setzt Out-Smartpointer auf NULL)", "low", "keine gefunden"),
 41: ("BeginBatch(NiPropertyState*, NiDynamicEffectState*)", "medium", "keine gefunden"),
 42: ("EndBatch (rendert Batch-Liste this+0x518)", "medium", "keine gefunden"),
 43: ("BatchRenderShape (Thunk -> 0x1499b0)", "medium", "keine gefunden"),
 44: ("BatchRenderStrips (Thunk -> 0x1499b0)", "medium", "keine gefunden"),
 45: ("BeginScene/BeginFrame (Caches reset, this+0x500=1)", "high", "NI NiCamera::Click FUN_00100c50"),
 46: ("EndScene/EndFrame (this+0x500=0)", "high", "NI NiCamera::Click FUN_00100c50"),
 47: ("SetCameraData(loc, dir, up, right, NiFrustum, NiRect<float> port); D3DDevice_SetTransform(0=VIEW / 1=PROJECTION, Xbox-Zaehlung) + SetViewport", "high", "NI NiCamera::Click FUN_00100c50; REN Create FUN_0014c170, FUN_00148870"),
 48: ("RenderShape(NiTriShapeData*, NiSkinInstance*, const NiTransform&, const NiBound&)", "high", "NI NiTriShape::vf33 0x1166d0; NiRenderer::vf43 (Basis)"),
 49: ("RenderTriangles(NiTrianglesData*, const NiTransform&, const NiBound&)", "high", "NI NiTriangles::vf33 0x11af60"),
 50: ("RenderTriStrip(NiTriStripData*, const NiTransform&, const NiBound&)", "high", "NI NiTriStrip::vf33 0x11b9a0"),
 51: ("RenderTristrips(NiTriStripsData*, NiSkinInstance*, const NiTransform&, const NiBound&)", "high", "NI NiTriStrips::vf33 0x11c460; NiRenderer::vf44 (Basis)"),
 52: ("RenderPoints(NiParticlesData*, const NiTransform&, const NiBound&)", "high", "NI NiParticles::vf33 0x10f820, NiAutoNormalParticles::vf33 0x10fc60"),
 53: ("RenderLines(NiLinesData*, const NiTransform&, const NiBound&)", "high", "NI NiLines::vf33 0x10e6f0"),
 54: ("RenderScreenPolygon(NiScreenPolygon*)", "high", "NI FUN_00106af0 (NiScreenPolygon-Draw) <- NiCamera::Click; Polygone erzeugt das Spiel selbst (ctor FUN_00106d50 aus 13 GAME-Funktionen, u.a. Movie-Player FUN_000ee220)"),
 55: ("RenderScreenTexture(NiScreenTexture*)", "high", "NI FUN_0010af70 <- NiCamera::Click"),
 56: ("unbekannt (0,0,0,Frustum-Plane-Maske; Xbox no-op)", "low", "NI FUN_000ff610 (NiCamera) <- alle Geometrie-vf33"),
 57: ("unbekannt (Basis return true)", "low", "keine gefunden"),
 58: ("PurgeGeometryData(NiGeometryData*)", "high", "NI FUN_00140190 (Broadcast) <- NiGeometryData dtor"),
 59: ("PurgeMaterial(NiMaterialProperty*)", "high", "NI FUN_001401c0 <- NiMaterialProperty dtor"),
 60: ("PurgeEffect(NiDynamicEffect*) (Xbox: nur NiLight)", "high", "NI FUN_00140160 <- NiTextureEffect/NiLight dtor"),
 61: ("PurgeScreenTextureRendererData(NiScreenTexture*)", "high", "NI FUN_001401f0 <- NiScreenTexture dtor"),
 62: ("PurgeSkinPartitionRendererData(NiSkinPartition*)", "high", "NI FUN_00140220 <- NiSkinPartition dtor"),
 63: ("unbekannt Purge* (leer; Vermutung PurgeSkinInstance)", "low", "keine gefunden"),
 64: ("PurgeScreenPolygonRendererData(NiScreenPolygon*) (leer)", "medium", "NI FUN_00140250 <- NiScreenPolygon dtor"),
 65: ("CreateSourceTextureRendererData(NiSourceTexture*) (muss true liefern, sonst liefert NiSourceTexture::Create NULL)", "high", "NI FUN_00117fc0 (Broadcast) <- NiSourceTexture::vf10, FUN_00118520, FUN_00118600 (<- GAME FUN_00099030, FUN_000ee220, FUN_000ee910; Preload-Flag DAT_002d7900=1); REN FUN_0015f8a0"),
 66: ("CreateRenderedTextureRendererData(NiRenderedTexture*)", "high", "NI NiRenderedTexture::Create FUN_0013ffe0 <- GAME FUN_0001f360, FUN_00038070, FUN_0006b670"),
 67: ("CreateRenderedCubeMapRendererData (Vermutung)", "medium", "keine gefunden"),
 68: ("CreateSourceCubeMapRendererData (Vermutung)", "medium", "keine gefunden"),
 69: ("CreatePaletteRendererData(NiPalette*)", "high", "NI FUN_00110bd0 <- NiPalette ctor FUN_00110c80/FUN_00110d40 (<- GAME FUN_00099030), vf03; REN FUN_0015df60"),
 70: ("CreateBltSourceRendererData(NiBltSource*)", "high", "NI FUN_00111d70 <- NiBltSource::vf04; REN vf25"),
 71: ("unbekannt (leer in Basis und Xbox)", "low", "GAME FUN_0002a660 (1x)"),
 72: ("unbekannt (neu in NiXBoxRenderer, return false)", "low", "keine gefunden"),
 73: ("unbekannt (neu in NiXBoxRenderer, return 0)", "low", "keine gefunden"),
 74: ("unbekannt (neu in NiXBoxRenderer, return false, 5 Args)", "low", "keine gefunden"),
}
out = r"D:\FuturamaDecomp\analysis\loader\nixboxrenderer_vtable.tsv"
with open(out, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f, delimiter="\t", lineterminator="\n")
    w.writerow(["slot", "offset", "address", "overrides_parent", "parent_impl", "guessed_name", "confidence", "callers"])
    for s in range(75):
        a = X.u32(NIX + 4 * s)
        p = X.u32(NIR + 4 * s) if s < 72 else None
        if p is None: ov = "new"
        elif p == a: ov = "no"
        elif p == PURE: ov = "yes(pure)"
        else: ov = "yes"
        n, c, cl = INFO[s]
        w.writerow([s, f"0x{4*s:03x}", f"0x{a:08x}", ov, "-" if p is None else f"0x{p:08x}", n, c, cl])
print("written", out)
