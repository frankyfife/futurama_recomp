# NetImmerse-Renderer-Schnittstelle (Futurama, retail_eu)

Thema: `NiRenderer` / `NiXBoxRenderer` als Andockpunkt für einen eigenen Renderer (Vulkan/D3D12) und als mögliche Grafik-HLE-Grenze für den Loader.
Basis: `orig\retail_eu\default.xbe`, Ghidra-Export `analysis\retail_eu_export`, `analysis\retail_eu_ni_classes.json`.
Alle Skripte liegen unter `analysis\loader\scripts\renderer_*.py` (Liste in Abschnitt 10). Die vollständige VTable steht in `analysis\loader\nixboxrenderer_vtable.tsv`.

Konventionen: „REN“ = NiXBoxRenderer-Modul (Adressbereiche siehe 5.1), „NI“ = übriger NetImmerse-Kern (0xfd000–0x166871), „GAME“ = Spielcode (< 0xfd000). „Vermutung“ markiert Namen oder Deutungen, die nicht direkt aus dem Binary belegt sind.

---

## 0. Kurzfassung

* `NiXBoxRenderer` (VTable `0x2aa7c0`, 75 Slots, Objektgröße `0x6d0` = 1744) wird **genau einmal** erzeugt: `FUN_0014c170` (= `NiXBoxRenderer::Create`, cdecl, 8 Args, `add esp,0x20` beim Aufrufer) wird von `FUN_000d26a0` (App-Init, App-VTable `0x2a3ef8` Slot 2) mit `(640, 480, 2, 1, 0x14000002, 0x4b, 0, 1)` aufgerufen. Das Ergebnis liegt in `app+0x104`; App-Slot 5 (`0xd2690`: `mov eax,[ecx+0x104]; ret`) ist der Getter, über den der Spielcode an den Renderer kommt (`DAT_002eb310` bzw. `DAT_002e7b84` = App-Instanz).
* Von den 75 Slots werden im ganzen Binary nur etwa **30** wirklich angesprochen. Spielcode ruft **direkt** nur **8 Slots** auf: 15, 16, 17, 21, 22, 27, 39 und 71. Indirekt löst er weitere aus: 66 über `NiRenderedTexture::Create`, 65/69 über `NiSourceTexture::Create(NiPixelData*)` `FUN_00118600` bzw. den `NiPalette`-ctor (Preload-Flag `DAT_002d7900` = 1) und 54 über eigene `NiScreenPolygon`s (siehe 4). Der NetImmerse-Kern nutzt die Szenen-Slots 45–56 (über `NiCamera::Click` `0x100c50` und die `vf33`-Display-Funktionen der Geometrieklassen) sowie die Purge-/Create-Slots 58–62, 64–66, 69 und 70 (63, 67 und 68 haben keine Aufrufer), und zwar über Broadcast-Funktionen auf der globalen Rendererliste `DAT_002d7b1c`.
* Die Render-API dieser NetImmerse-Version ist **datenorientiert**: `RenderShape(NiTriShapeData*, NiSkinInstance*, const NiTransform&, const NiBound&)`. Den aktuellen `NiPropertyState*` und `NiDynamicEffectState*` schreibt der Aufrufer **direkt** in `renderer+0xc` / `renderer+0x10` (`NiTriShape::vf33` `0x1166d0`). Ein eigener Renderer muss deshalb das Layout der `NiRenderer`-Basis exakt einhalten.
* Spielcode ruft D3D8 direkt nur in zwei Bereichen auf: (a) im **Movie-Player** (`FUN_000ede40/ede90/ee080/ee190/ee220`; er holt `IDirect3DDevice8*` aus `renderer+0x60`) und (b) in der **Bildschirmzentrierung** (`FUN_000a1920…a1ae0` → `D3DDevice_SetScreenSpaceOffset`). Zusammen sind das 8 benannte D3D8-Funktionen. Zwei dieser Helfer (`FUN_000a1920`/`FUN_000a1930`) ruft ausschließlich `NiXBoxRenderer::vf15` (SetRenderTarget) auf, sie gehören also funktional zum Renderer-Pfad. REN ruft 80 benannte D3D8-Funktionen direkt auf; 71 davon nutzt nur REN. Zählt man den REN-Code mit, den Ghidra keiner Funktion zugeordnet hat (rund 14 KB, meist VTable-Methoden der Hilfsklassen, siehe Verifikation), sind es 82 benannte, davon 73 nur von REN.
* REN schreibt außerdem **direkt in D3D-interne Zustandsglobals** (Deferred Render-/Texture-State und Dirty-Flags im BSS der D3D-Sektion, `0x2444c4–0x24489c`). Das sind 22 Funktionen mit 175 Zugriffen; GAME und NI tun das nie. Eine HLE auf D3D-Ebene müsste diese Interna emulieren. Eine HLE auf **NiRenderer-Ebene** (Ersatz von `Create` und der VTable) umgeht das vollständig. Das ist die empfohlene Grafikgrenze.
* Texturen werden auf der CPU **geswizzelt** hochgeladen (`FUN_0015db00`), auch Palettentexturen (P8); DXT-Formate bleiben unverändert. Skinning läuft praktisch nur auf der **CPU**: `renderer+0x504` (Bone-Matrix-Anzahl für den Partition-Pfad) wird nur mit 0 beschrieben, der Hardware-Partition-Pfad `FUN_0014bc30` ist damit tot. Die Pipeline ist Fixed-Function (D3D-Texture-Stage-States, `D3DDevice_SetLight`). `FUN_00147740` erzeugt zwar einen festen Pixel-Shader (`this+0x5c`), dieses Handle wird aber nirgends mit `SetPixelShader` gebunden (siehe 5.8).
* Das Spiel greift an zwei Stellen am Renderer-Interface vorbei, was ein Ersatz-Renderer beachten muss: Der **Movie-Player** `FUN_000ee220` schreibt direkt in `NiXBoxSourceTextureData` (+0x3c D3D-Textur, +0x48/+0x4c Größe, +0x58, +0x60) und liest `renderer+0x5cc`. Außerdem gibt `PrecacheGeometry` (Slot 39) mit Flag-Bit 0 die CPU-Vertex-/Indexdaten frei, nachdem BeginScene die statischen Buffer gebaut hat (siehe 5.3).

---

## 1. Klassen und Layout

### 1.1 Metadaten (aus `retail_eu_ni_classes.json` und Binary)

| Klasse | RTTI | VTable | Slots | Größe | ctor / dtor |
|---|---|---|---|---|---|
| `NiRenderer` | `0x30e890` | `0x2a9ef0` | 72 (nach Slot 71 folgt der String `0x2aa010`) | 20 (`0x14`) | ctor `FUN_00140550`, dtor `FUN_001406e0`, vf00 `0x140750` |
| `NiXBoxRenderer` | `0x30ee00` | `0x2aa7c0` | 75 (nach Slot 74 folgt der String „UNKNOWN!“ `0x2aa8ec`) | 1744 (`0x6d0`) | ctor `FUN_0014af00`, dtor `FUN_0014c860`, vf00 `0x14cb90`, Create `FUN_0014c170` |

Belege: Der RTTI-Initializer `FUN_00141e60("NiXBoxRenderer",0x6d0,&NiRenderer::ms_RTTI)` steht in `initstubs_00230000.c`. Die VTable-Stores `mov dword ptr [esi],0x2aa7c0` liegen bei `0x14af26` (ctor) und `0x14c881` (dtor), ermittelt mit `renderer_xref.py`. `FUN_000a4900(0x6d0)` steht in `FUN_0014c170`.

`0x1668b6` (`FUN_001692b4(0x19)` = `_amsg_exit(R6025)`) ist `_purecall`. 30 Slots von `NiRenderer` sind pure virtual (siehe TSV, `overrides_parent = yes(pure)`).

### 1.2 Felder der `NiRenderer`-Basis (von außen benutzt, daher für einen eigenen Renderer verbindlich)

| Offset | Bedeutung | Beleg |
|---|---|---|
| +0x00 | vtable | ctor `FUN_00140550` |
| +0x04 | Refcount (NiRefObject) | `piVar[1]++` in `FUN_00013d40`, `FUN_000d26a0` |
| +0x08 | `NiAccumulator*` (Sorter, Smartpointer) | `FUN_00153400` (SetAccumulator, Vermutung Name). `NiCamera::Click` ruft `acc->vf11/vf12` (0x2c/0x30). Spielcode schreibt direkt: `FUN_000a2c20` setzt `NiAlphaAccumulator`, `FUN_0002a660` setzt `renderer[2]=0` |
| +0x0c | aktueller `NiPropertyState*` | direkt gesetzt in `NiTriShape::vf33` (`piVar10[3] = this+0xc0`), `NiTriStrip::vf33`, `FUN_00106af0`. Basis `NiRenderer::vf41` schreibt ebenfalls +0xc |
| +0x10 | aktueller `NiDynamicEffectState*` | wie oben (`piVar10[4] = this+0xc4`) |

Der Basis-ctor `FUN_00140550` hängt jeden Renderer an eine globale Liste an: Kopf `DAT_002d7b1c`, Ende `DAT_002d7b20`, Zähler `DAT_002d7b18`. Über diese Liste rufen die statischen Broadcast-Funktionen (Abschnitt 3.2) Purge-/Create-Slots auf. **Ein eigener Renderer muss sich ebenfalls eintragen**, entweder über `FUN_00140550` oder durch Nachbildung.

### 1.3 Wichtige Felder von `NiXBoxRenderer`

| Offset | Inhalt | Beleg |
|---|---|---|
| +0x14…+0x38 | Default-Render-Target-Deskriptor: Breite +0x14, Höhe +0x18, `NiXBoxPixelFormat*` +0x1c, Color-Surface +0x28, Depth-Surface +0x34 | `FUN_0014aab0` (`D3DDevice_GetRenderTarget2`, `GetDepthStencilSurface2`) |
| +0x58 | aktuelles Render-Target (→ +0x14 oder RendererData einer `NiRenderedTexture`) | vf15, vf21–23 |
| +0x5c | Pixel-Shader-Handle (fester PSH) | `FUN_00147740` → `D3DDevice_CreatePixelShader`; nur noch in `FUN_00147cd0` (=0) und `FUN_0014aca0` (`DeletePixelShader`) angefasst, nie an `SetPixelShader` übergeben |
| +0x60 | `IDirect3DDevice8*` | `Direct3D_CreateDevice(..., piVar3+0x18)` in `FUN_0014c170`; **Spielcode liest es** in `FUN_000ede40` |
| +0x64 | unbekannt, wird von vf16 und vf17 auf 0 gesetzt | `0x14647a`, `0x1465e6` |
| +0x26c…+0x2c0 | Adapter, DeviceType, hWnd, BehaviorFlags, `D3DPRESENT_PARAMETERS` (17 Dwords ab +0x27c) | `FUN_0014c170` |
| +0x2c4/+0x2c8 | Breite/Höhe | `FUN_0014c170` |
| +0x2cc/+0x2d0/+0x2d4 | Clear-Farbe (Default `0xff808080`), Depth-Clear (1.0), Stencil-Clear | `FUN_00147cd0`, vf26–28 |
| +0x2d8 | Caps-Flags (`0x6d`, mit Stencil `0xed`) | `FUN_001474f0` |
| +0x414 | `D3DVIEWPORT8` | vf17, vf47 |
| +0x42c (0xd4 Byte) | `D3DCAPS8` | `D3D_GetDeviceCaps` in `FUN_00147550` |
| +0x500 | „in Scene“-Flag | vf45 setzt 1, vf46 setzt 0. Draw-Slots prüfen das Flag |
| +0x504 | Bone-Matrix-Anzahl für Hardware-Skinning (wird nur mit 0 beschrieben) | Schreibzugriffe nur in `FUN_00147550`/`FUN_00147cd0` |
| +0x510/+0x514 | Precache-Queue | vf39 → `FUN_00148020`, verarbeitet in vf45 → `FUN_00148300` |
| +0x518/+0x51c, +0x520/+0x524 | Batch-Liste und Batch-Property-/Effect-State | vf41–44 |
| +0x528, +0x560 | zwei Tabellen mit je 14 `NiPixelFormat*` (Texturen / Render-Targets) | `FUN_0014a410(this+0x528,3)`, `FUN_00157160` |
| +0x5ac | Geometrie-Manager (vtable, `FUN_0015c2e0`) | — |
| +0x5b0 | Handle aus `GeomMgr->vf1(1,0)` (Vermutung: Default-Geometriegruppe); wird an GeomMgr-vf6 in vf48 (`0x14c526`) und an `FUN_0015c5b0` in vf39 übergeben | `FUN_00147550` `0x147724`, `FUN_00147cd0` (=0) |
| +0x5b4 | `NiXBoxVertexBufferManager` (`FUN_00157ae0`) | String `0x2ab058` |
| +0x5b8 | `NiXBoxIndexBufferManager` (`FUN_0015c7b0`) | Strings `0x2ab7f8…` |
| +0x5bc | `NiXBoxTextureManager` (`FUN_0015fa90`, vtable `0x2abd90`) | String `0x2abd50` |
| +0x5c0 | Render-State-Cache (`FUN_00159c20`) | vf18/19/29/30 |
| +0x5c4 | Licht-Manager (Vermutung, `FUN_0015bcb0`) | `D3DDevice_LightEnable` in `FUN_0015b990`/`FUN_0015b9e0` |
| +0x5c8 | „Configurable texture pipeline manager“ (`FUN_0015b600`) | Strings `0x2ab710`/`0x2ab758` |
| +0x5cc | Texture-Cache (`FUN_00156fd0`, „NiTCache.dat“), nur bei `DAT_0030edd4` ∈ {1,2} | `FUN_00147550`; auch der Movie-Player `FUN_000ee220` liest es (über `NiXBoxSourceTextureData+0x10` → Renderer) |

`DAT_0030edd4` liegt im BSS (`.data` Raw endet bei `0x2d9438`). Es wurden nur Lesezugriffe gefunden (`renderer_xref.py 30edd4`). Der Texture-Cache-Pfad ist daher vermutlich inaktiv, und Texturen gehen direkt über `D3DDevice_CreateTexture2` (Vermutung: keine indirekten Schreiber).

### 1.4 `NiXBoxRenderer::Create` (`FUN_0014c170`)

Signatur nach Aufrufstelle `0xd26be–0xd26e7` und Code: `NiXBoxRenderer* Create(uint width, uint height, int bbFmt, uint bbCount, uint flags, int depthFmt, uint refresh, int interval)` (cdecl).

* `bbFmt`: 0→-1, 2→`0x1e`, 3→`0x11`, 4→`0x1c`, sonst `0x12`. Das entspricht den Xbox-XDK-Codes `LIN_X8R8G8B8`, `LIN_R5G6B5`, `LIN_X1R5G5B5`, `LIN_A8R8G8B8` (Werte aus dem XDK `d3d8types.h`, Zuordnung der Namen Vermutung aus dem Gedächtnis).
* `depthFmt` wird in PC-`D3DFORMAT`-Codes übergeben: 0 → -1 (kein Z), `0x46`(D16_LOCKABLE) oder `0x50`(D16) → Xbox `0x2c`, sonst → `0x2a` (Xbox D24S8). Das Spiel übergibt `0x4b` (= PC `D3DFMT_D24S8`).
* `flags`: Bit1 → `EnableAutoDepthStencil`, `0x4000000` → BehaviorFlags `0x10`. `0x20000000` → `|0x20`, `0x40000000` → `|0x80`, ohne beide → `|0x40` (Hardware-VP). `&0xf00` → MultiSample (`FUN_00146180`).
* Bei Fehlschlag von `Direct3D_CreateDevice` liefert `Create` NULL und legt den Fehlertext (`FUN_0014cbb0`) in `DAT_0030e790` ab; `FUN_000d26a0` gibt dann false zurück.
* `interval`: 0→`0x80000000`, 1→1, 2→2, 3→4, 4→8 (PresentationInterval).
* Ablauf: `FUN_00240390(0)` (einmalig, Ergebnis in `DAT_0030edd0`; der Code ist nur `mov eax,1; ret 4`, liefert also immer die Konstante 1; Vermutung: `Direct3DCreate8`-Äquivalent der D3D-Lib mit Dummy-Objekt) → `D3D_SetPushBufferSize(0x200000, 0x8000)` → `Direct3D_CreateDevice` → `FUN_0014aab0` (Back-Buffer-Deskriptor) → `FUN_001474f0` (Caps-Flags) → `FUN_00147550` (Manager, Caps, Render-State) → `FUN_0014c0f0` (Pixelformate, Texture-Pipeline) → `FUN_00147740` (PSH) → `vf47 SetCameraData` mit Default-Frustum (-1,1,1,-1,0.1,1) und Port (0,1,1,0).

---

## 2. VTable `NiXBoxRenderer` – Slot für Slot

Spalten: Ovr = überschreibt `NiRenderer` (`pure` = Basis ist `_purecall`, `new` = neu ab Slot 72), Args = Stack-Args nach `ret N` (`renderer_retn.py`, thiscall). Aufrufer siehe Abschnitt 3; die vollständigen Listen stehen in der TSV.

| Slot | Off | Adresse | Ovr | Args | Name (Begründung) | Konf. |
|---|---|---|---|---|---|---|
| 0 | 0x00 | 0x14cb90 | ja | 1 | scalar deleting dtor (`FUN_0014c860` + `FUN_000a3880`) | hoch |
| 1 | 0x04 | 0x14b090 | ja | 0 | GetRTTI | hoch |
| 2–7, 9, 10 | | NiObject | nein | | geerbte NiObject-Slots | hoch |
| 8 | 0x20 | 0x140640 | nein | 1 | GetViewerStrings (NiRenderer, schreibt RTTI-Namen) | mittel |
| 11 | 0x2c | 0x146350 | pure | 0 | **GetDriverInfo**: `D3D_GetAdapterIdentifier`, `"%s (%s-%s)"` | hoch |
| 12 | 0x30 | 0x14b130 | pure | 0 | **GetFlags**: liefert `this+0x2d8` | hoch |
| 13 | 0x34 | 0x1463e0 | pure | 1 | **FindClosestPixelFormat(FormatPrefs&)** → `FUN_00157160`: PIX_DEFAULT(5)→TRUE_COLOR, ALPHA_DEFAULT/NONE → Tabelle +0x540/+0x544 | mittel |
| 14 | 0x38 | 0x14b0a0 | ja | 2 | unbekannt (Basis `0x800000`, Xbox 0) | niedrig |
| 15 | 0x3c | 0x146400 | pure | 1 | **SetRenderTarget(NiRenderedTexture*)**: NULL→Backbuffer, `D3DDevice_SetRenderTarget`, nicht während Scene. Ruft Spielcode `FUN_000a1930`/`FUN_000a1920` (ScreenSpaceOffset) | hoch |
| 16 | 0x40 | 0x146460 | pure | 0 | **DisplayFrame**: `D3DDevice_Swap(0)`, wenn das aktuelle Target der Backbuffer ist; danach `this+0x64=0` und Tail-Call `FUN_00157970` (`inc [VB-Manager+0x20]`, Frame-Zähler der dynamischen VBs) | hoch |
| 17 | 0x44 | 0x146490 | pure | 2 | **ClearBuffer(const NiRect<float>*, mode)**: Bit 1→TARGET(`0xf0`), 4→ZBUFFER(1, nur wenn das Target eine Depth-Surface hat), 2→STENCIL(2, nur mit Caps-Bit `0x80` und Depth-Surface). Farbe/Z/Stencil aus +0x2cc..+0x2d4. Setzt vorher `D3DDevice_SetViewport(this+0x414)` mit voller Target-Breite/-Höhe; das Rechteck wird nur bei nicht-geswizzeltem Target umgerechnet | hoch |
| 18 | 0x48 | 0x146600 | ja | 0 | **GetLeftRightSwap**: `rs+0x1fc` (negiert x der Projektion in vf47) | mittel |
| 19 | 0x4c | 0x146620 | ja | 1 | **SetLeftRightSwap(bool)** (Basis: return false) | mittel |
| 20 | 0x50 | 0x146640 | ja | 1 | **W-Buffer an/aus**: `FUN_001591c0` → `D3DRS_ZENABLE` = 2 (USEW) oder 1. false, wenn Caps-Bit `0x100` fehlt (nie gesetzt) | mittel |
| 21 | 0x54 | 0x146680 | pure | 0 | **GetBufferWidth** (aktuelles Target) | hoch |
| 22 | 0x58 | 0x146690 | pure | 0 | **GetBufferHeight** | hoch |
| 23 | 0x5c | 0x1466a0 | pure | 0 | GetBufferDepth (Bits/Pixel aus `NiPixelFormat+0x14`) | mittel |
| 24 | 0x60 | 0x1466b0 | pure | 1 | **TakeScreenShot(rect*)** → neues `NiPixelData` (0x4c, `FUN_00111260`), `LockRect` + Kopie | hoch |
| 25 | 0x64 | 0x146800 | pure | 4 | Blt/FastCopy(`NiBltSource*`, rect, x, y) → `D3DDevice_CopyRects`, ruft bei Bedarf Slot 70 | mittel |
| 26 | 0x68 | 0x14b0b0 | ja | 1 | **SetDepthClear(float)** (+0x2d0) | hoch |
| 27 | 0x6c | 0x14b0c0 | pure | 1 | **SetBackgroundColor(const NiColor&)** → ARGB mit A=0xff (+0x2cc) | hoch |
| 28 | 0x70 | 0x14b120 | ja | 1 | **SetStencilClear** (+0x2d4) | hoch |
| 29 | 0x74 | 0x146930 | ja | 0 | **GetMaxFogValue** (Basis 1.0, `rs+0x188`) | mittel |
| 30 | 0x78 | 0x146940 | ja | 1 | **SetMaxFogValue(float)**: x ≤ 0 → 1e-5, `rs+0x188 = x`, `rs+0x184 = 1/x-1` (verwendet im Fog-Code `FUN_00159700`) | mittel |
| 31 | 0x7c | 0x146990 | pure | 3 (jmp) | **PrecacheTexture(NiTexture*, ?, ?)** → `jmp [vt+0xc]` auf TextureManager-Slot 3 (`0x15f7e0`: legt `NiXBoxSourceTextureData` an, endet mit `ret 0xc`; der Code liegt außerhalb jeder Ghidra-Funktion, er fehlt also im Dekompilat) | mittel |
| 32 | 0x80 | 0x1469a0 | pure | 1 | **PurgeAllTextures(bool)**: `FUN_00157720` läuft über die NiTexture-Liste `DAT_0030ef7c` und löscht +0x20 | mittel |
| 33 | 0x84 | 0x148000 | pure | 1 | **PurgeTexture(NiTexture*)** (RendererData +0x20) | mittel |
| 34–36 | | Basis-Stubs | nein | 2/6/0 | unbekannt, return false/0 | niedrig |
| 37, 38 | 0x94/0x98 | 0x1605f0 / 0x13db0 | pure | 1/0 | unbekannt, Xbox return 0 | niedrig |
| 39 | 0x9c | 0x14b140 | ja | 5 | **PrecacheGeometry(NiGeometry*, uiFlags, NiRendererSpecificProperty* (Vermutung), ?, ?)**: Geometrie mit Skin-Instanz (`geom+0xcc` ≠ 0) wird unabhängig von der Klasse über `FUN_00148170` gequeued, ungeskinnte nur bei RTTI `NiTriShape`/`NiTriStrips` über `FUN_00148020`; gebaut wird in BeginScene. Flag-Bit 0 = CPU-Daten danach freigeben (5.3); das Spiel übergibt `(geom, 1 oder 0, 0, 0, 0)` | mittel |
| 40 | 0xa0 | 0x140130 | nein | 2 | unbekannt, `*out = NULL` | niedrig |
| 41 | 0xa4 | 0x146e20 | ja | 2 | **BeginBatch(NiPropertyState*, NiDynamicEffectState*)** (+0x520/+0x524; Basis: +0xc/+0x10) | mittel |
| 42 | 0xa8 | 0x14b700 | ja | 0 | **EndBatch**: rendert die Liste +0x518 mit gemeinsamem State (`ret` bei `0x14bc18`) | mittel |
| 43, 44 | 0xac/0xb0 | 0x14bc20 (jmp 0x1499b0) | ja | 4 | **BatchRenderShape / BatchRenderStrips** (Basis leitet an 48/51 weiter) | mittel |
| 45 | 0xb4 | 0x148950 | ja | 0 | **BeginScene/BeginFrame**: Precache-Queue (`FUN_00148300`), Caches/Lichter reset, `this[0x500]=1` | hoch |
| 46 | 0xb8 | 0x1469c0 | ja | 0 | **EndScene/EndFrame**: `this[0x500]=0` | hoch |
| 47 | 0xbc | 0x1469e0 | pure | 6 | **SetCameraData(loc, dir, up, right, NiFrustum, NiRect port)**: View-Matrix (`this+0x610`) → `D3DDevice_SetTransform(0, …)`, Projektion aus dem Frustum → `SetTransform(1, …)` (Xbox-Zählung: 0 = VIEW, 1 = PROJECTION, nicht PC 2/3), Viewport | hoch |
| 48 | 0xc0 | 0x14c4f0 | pure | 4 | **RenderShape(NiTriShapeData*, NiSkinInstance*, NiTransform*, NiBound*)** → `FUN_00149ae0` (bzw. totes `FUN_0014bc30`) | hoch |
| 49 | 0xc4 | 0x14b590 | pure | 3 | **RenderTriangles** (`NiTriangles`) → `FUN_00149ae0` | hoch |
| 50 | 0xc8 | 0x14b590 | pure | 3 | **RenderTriStrip** (`NiTriStrip`), gleiche Implementierung wie 49 | hoch |
| 51 | 0xcc | 0x14c4f0 | pure | 4 | **RenderTristrips** (`NiTriStrips`), gleiche Implementierung wie 48 | hoch |
| 52 | 0xd0 | 0x148ae0 | pure | 3 | **RenderPoints** (`NiParticles`, CPU-Quads in ein dynamisches VB) | hoch |
| 53 | 0xd4 | 0x149530 | pure | 3 | **RenderLines** (`NiLines`) | hoch |
| 54 | 0xd8 | 0x14b5f0 | pure | 1 | **RenderScreenPolygon(NiScreenPolygon*)** (Ortho-Matrizen +0x354/+0x394). Weg für HUD/Menü/Movie-Quads des Spiels (siehe 4) | hoch |
| 55 | 0xdc | 0x14c570 | pure | 1 | **RenderScreenTexture(NiScreenTexture*)** (baut einen temporären PropertyState) | hoch |
| 56 | 0xe0 | 0x151dc0 | pure | 4 | unbekannt: `(0,0,0, ~cam+0x228 & 0x3f)` (Frustum-Plane-Maske), Xbox leer | niedrig |
| 57 | 0xe4 | 0x256c0 | nein | 1 | unbekannt, return true | niedrig |
| 58 | 0xe8 | 0x146ea0 | ja | 1 | **PurgeGeometryData(NiGeometryData*)** → GeomMgr vf9 | hoch |
| 59 | 0xec | 0x146e90 | ja | 1 | **PurgeMaterial(NiMaterialProperty*)** → `FUN_00158ba0` → `FUN_001618a0` (Tail-Calls, `ret 4`) | hoch |
| 60 | 0xf0 | 0x146e40 | ja | 1 | **PurgeEffect(NiDynamicEffect*)** (nur `NiLight`) | hoch |
| 61 | 0xf4 | 0x146ef0 | ja | 1 | **PurgeScreenTextureRendererData** | hoch |
| 62 | 0xf8 | 0x146ec0 | ja | 1 | **PurgeSkinPartitionRendererData** | hoch |
| 63 | 0xfc | Stub | nein | 1 | unbekannt, leer (Vermutung PurgeSkinInstance) | niedrig |
| 64 | 0x100 | Stub | nein | 1 | PurgeScreenPolygonRendererData (Xbox leer) | mittel |
| 65 | 0x104 | 0x147350 | pure | 1 | **CreateSourceTextureRendererData(NiSourceTexture*)** → `FUN_0015e010`. Rückgabe false → `NiSourceTexture::Create` liefert NULL (`FUN_00118600`/`FUN_00118520`), ein Stub muss also true liefern | hoch |
| 66 | 0x108 | 0x147380 | pure | 1 | **CreateRenderedTextureRendererData(NiRenderedTexture*)** → `FUN_0015e430` (String `0x2aba78` nennt den Namen). Rückgabe false → `NiRenderedTexture::Create` löscht das Objekt und liefert NULL | hoch |
| 67 | 0x10c | 0x1473b0 | ja | 1 | CreateRenderedCubeMapRendererData (Vermutung, `NiXBoxRenderedCubeMapData`) | mittel |
| 68 | 0x110 | 0x1473e0 | ja | 1 | CreateSourceCubeMapRendererData (Vermutung, `NiXBoxSourceCubeMapData`) | mittel |
| 69 | 0x114 | 0x147410 | pure | 1 | **CreatePaletteRendererData(NiPalette*)** → `NiXBoxPaletteData` (`D3DDevice_CreatePalette2`) | hoch |
| 70 | 0x118 | 0x147470 | ja | 1 | **CreateBltSourceRendererData(NiBltSource*)** → `NiXBoxBltSourceData` | hoch |
| 71 | 0x11c | 0xb0390 | nein | 0 | unbekannt, leer (Spielcode ruft es trotzdem, `FUN_0002a660`) | niedrig |
| 72–74 | | Stubs | new | 1/1/5 | unbekannt, return 0 | niedrig |

Bilanz (`renderer_vtable_tsv.py`): 25 Slots überschrieben, 30 Implementierungen pure virtueller Basisslots, 17 geerbt, 3 neu.

---

## 3. Wer ruft welchen Slot?

Methoden: `renderer_callers.py` ist eine Binär-Analyse mit Registerverfolgung von Renderer-Quellen (`[x+0x1a8]` = NiCamera-Renderer, App+0x104, Rückgabe von App-Slot 5). `renderer_vcalls2.py` durchsucht die Ghidra-Dekompilate nach virtuellen Aufrufen und löst die Zuweisung des Empfängers auf. Dazu kommen die Callgraph-Aufrufer der Broadcast-Funktionen. Direkte (nicht-virtuelle) Aufrufe der Slot-Funktionen gibt es laut `callgraph.tsv` nicht.

### 3.1 Spielcode (GAME)

| Slot | Aufrufer (Anzahl Aufrufstellen) |
|---|---|
| 15 SetRenderTarget | `FUN_0001f1a0` (2), `FUN_0003b960` (2) – Render-to-Texture (Schatten/Projektion) |
| 16 DisplayFrame | `FUN_000d2630` (Hauptschleife), `FUN_0002a660` (8), `FUN_000a62b0` (3), `FUN_000c8e90` (2), `FUN_000ee080`, `FUN_000a26a0` (2); über den NiCamera-Wrapper `FUN_000feec0` aus `FUN_0003b960` |
| 17 ClearBuffer | `FUN_000d2630`, `FUN_0002a660` (8), `FUN_000c8e90` (5), `FUN_000a62b0` (3), `FUN_0001f1a0` (2), `FUN_000a26a0` (2), `FUN_0001bbb0`, `FUN_00021a80`, `FUN_000a4bb0`, `FUN_000a8290`, `FUN_000d62e0`, `FUN_000e1970`, `FUN_000e3310`; über den Wrapper `FUN_000fee90` aus `FUN_0003b960`, `FUN_00045f10`, `FUN_000e8920`, `FUN_000ede90`, `FUN_000ee080` |
| 21/22 Width/Height | `FUN_000d5bb0`, `FUN_000ef370`, `FUN_000ef680` |
| 27 SetBackgroundColor | `FUN_000d26a0`, `FUN_00012210`, `FUN_00012cc0`, `FUN_00012d20`, `FUN_00021a80`, `FUN_0002a660`, `FUN_0003b960` (2), `FUN_000a62b0`, `FUN_000c8e90` (3), `FUN_000ede90` (2), `FUN_000ee080` |
| 39 PrecacheGeometry | `FUN_000ce930` (2), `FUN_000d0dd0`, `FUN_000d1120` |
| 66 (indirekt) | `NiRenderedTexture::Create` `FUN_0013ffe0` ← `FUN_0001f360`, `FUN_00038070`, `FUN_0006b670` |
| 65 (indirekt) | `NiSourceTexture::Create(NiPixelData*, FormatPrefs*)` `FUN_00118600` ← `FUN_00099030`, `FUN_000ee220` (Movie), `FUN_000ee910`; ruft wegen `DAT_002d7900` = 1 (nie beschrieben) sofort den Broadcast `FUN_00117fc0` auf |
| 69 (indirekt) | `NiPalette`-ctor `FUN_00110d40` ← `FUN_00099030` |
| 54 (indirekt) | `NiScreenPolygon`-ctor `FUN_00106d50` ← 13 Spielfunktionen (`0x194d0`, `0x24aa0`, `0x279c0`, `0x662f0`, `0x6b670`, `0xd5e00`, `0xe5b70`, `0xe62a0`, `0xe7340`, `0xe75f0`, `0xee220`, `0xf30b0`, `0xf33e0`); Einhängen in die Kameraliste über `FUN_00019460` (`add ecx,0x160` → NiTArray `cam+0x160`, Name Vermutung `NiCamera::AddScreenPolygon`, 15 Spielfunktionen mit 21 Aufrufstellen, dazu die NI-Aufrufer `0x13d3b0`/`0x13e360`), Entfernen über `FUN_00100f30` (9 Spielaufrufer); gezeichnet in `NiCamera::Click` |
| 71 (no-op) | `FUN_0002a660` (`call [eax+0x11c]` bei `0x2a6e2`) |

`FUN_0002a660` und `FUN_000a62b0` leeren alle Backbuffer: `SetBackgroundColor`, dann 3–8× `ClearBuffer(NULL,1)` + `DisplayFrame`.

### 3.2 NetImmerse-Kern (NI)

| Slot(s) | Aufrufer |
|---|---|
| 45, 46, 47 | `NiCamera::Click` `FUN_00100c50` (Name Vermutung), 17 Aufrufer im Spiel |
| 48–53 | `vf33` (= Display/RenderImmediate(NiCamera*)) von `NiTriShape` 0x1166d0, `NiTriangles` 0x11af60, `NiTriStrip` 0x11b9a0, `NiTriStrips` 0x11c460, `NiParticles` 0x10f820, `NiAutoNormalParticles` 0x10fc60, `NiLines` 0x10e6f0. Dazu die Basis-`NiRenderer::vf43/44` → 48/51 |
| 54, 55 | `FUN_00106af0` / `FUN_0010af70` aus `NiCamera::Click` (Listen `cam+0x164` / `cam+0x194`) |
| 56 | `FUN_000ff610` (aus allen Geometrie-vf33 vor dem Render-Call) |
| 58, 59, 60, 61, 62, 64 | Broadcasts über `DAT_002d7b1c`: `FUN_00140190`←`NiGeometryData` dtor; `FUN_001401c0`←`NiMaterialProperty` dtor; `FUN_00140160`←`NiTextureEffect`/`NiLight` dtor; `FUN_001401f0`←`NiScreenTexture` dtor; `FUN_00140220`←`NiSkinPartition` dtor; `FUN_00140250`←`NiScreenPolygon` dtor |
| 65 | `FUN_00117fc0` (Broadcast) ← `NiSourceTexture::vf10`, `FUN_00118520`, `FUN_00118600`; REN-intern `FUN_0015f8a0` |
| 66 | `FUN_0013ffe0` (`NiRenderedTexture::Create(w,h,NiRenderer*,FormatPrefs*)`, verlangt Zweierpotenzen) |
| 69 | `FUN_00110bd0` ← `NiPalette`-ctor/vf03; REN `FUN_0015df60` |
| 70 | `FUN_00111d70` ← `NiBltSource::vf04`; REN vf25 |

**Keine Aufrufer gefunden** für die Slots 11–14, 18–20, 23–26, 28–38, 40–44, 57, 63, 67, 68, 72–74. Das heißt nicht zwingend, dass sie tot sind (Aufrufe über unverfolgte Zeiger sind möglich), aber kein Muster im Dekompilat oder Binary trifft sie. Für die Slots ab 40 hat die Review das zusätzlich typunabhängig geprüft: Rohscan aller `call/jmp [reg+off]` im ganzen Binary, auch im Code außerhalb von Ghidra-Funktionen (`verify_renderer_highoff.py`, `verify_renderer_gaps.py`). Alle Treffer für 40–44, 57, 63, 67, 68 und 72–74 haben Spielobjekte als Empfänger: `this` von Spielmethoden mit Feldern weit hinter `0x6d0` (z. B. `+0x7be`, `+0xb8e`) oder Felder von Spielobjekten; das Spiel hat eigene VTables mit 81 bzw. 78 Slots (`0x2a2e58`, `0x2a2fa0`). Ghidras Dekompilat von `FUN_0003b960` zeigt einen Slot-40-Aufruf auf dem Renderer (`piVar12 = piVar4`). Das ist ein Dekompilerfehler: Laut Binary ist der Empfänger bei `0x3c041` `edi = [[ebx+0x2c]+0x14]`, derselbe wie beim `+0xa4`-Aufruf bei `0x3bebf`.

### 3.3 Darstellungsablauf `NiCamera::Click` (`FUN_00100c50`)

1. `renderer->SetCameraData(cam+0xa0 loc, +0x110 dir, +0x11c up, +0x128 right, +0x134 frustum, +0x14c port)`
2. `renderer->BeginScene()`
3. Nur wenn `scene = cam+0x15c` existiert und `scene->vf28()` true liefert: `acc = renderer+8` (AddRef); `acc->vf11(cam)` (StartAccumulating, Vermutung). Liefert das false, wird `renderer+8` für diesen Frame auf NULL gesetzt.
4. `FUN_00101a30(scene, cam)`: Culling (`cam+0x240`, `FUN_001019b0`), dann `obj->vf33(cam)` (Display) bzw. `vf20`; danach `FUN_0013e360`
5. `acc->vf12()` (FinishAccumulating; der Alpha-Sorter rendert die verzögerten Objekte), danach `renderer->SetAccumulator(acc)` (`FUN_00153400`) zum Wiederherstellen
6. Erster Durchlauf der Screen-Polygone `cam+0x164` (nur solche, für die `FUN_00106a10` und `FUN_00106a30` true liefern; sie werden danach aus der Liste genommen)
7. Liste `cam+0x17c`: jedes Objekt direkt per `obj->vf33(cam)` zeichnen, danach aus der Liste entfernen
8. (ab hier unabhängig von der Szene) Screen-Texturen `cam+0x194` → Slot 55, dann alle übrigen Screen-Polygone `cam+0x164` → Slot 54, zuletzt das Einzelpolygon `cam+0xc0` → Slot 54
9. `renderer->EndScene()`

Die Display-Funktion der Geometrie (z. B. `NiTriShape::vf33`) setzt `renderer+0xc/+0x10` und ruft `FUN_00140140` auf. Diese Funktion reicht das Objekt an `renderer+8` (Accumulator, `acc->vf13`) weiter; nimmt der Accumulator es an (der `NiAlphaAccumulator::vf13` `0x2ade0` liest dafür `renderer+0xc` → Alpha-Property), endet die Display-Funktion ohne Render-Call. Sonst folgen `FUN_000ff610` (Slot 56) und der Render-Slot. Danach zählt sie Statistiken hoch (`DAT_0030eb7c/80/84`). Das Alpha-Sortieren liegt damit vollständig außerhalb des Renderers.

---

## 4. Erzeugung und Renderloop auf Spielseite

```
FUN_000d29b0 (Programm-Main nach Entry)
  app = new App (0x108 Bytes, vtable 0x2a3ef8; Basis-ctor FUN_000a2240 setzt DAT_002e7b84)
  DAT_002eb310 = app
  app->vf2  = FUN_000d26a0  Init:  NiXBoxRenderer::Create(640,480,2,1,0x14000002,0x4b,0,1) -> app+0x104
                                   renderer->SetBackgroundColor(0,0,0); Sprachen/PAL-NTSC; FUN_000a26a0 (Clear+Swap)
  while (app->vf3 = FUN_000d2630):           // ein Frame
        renderer->ClearBuffer(NULL, DAT_002d6740)   // Initialwert 4 = nur Z, zeitweise 5
        app->vf8 = FUN_000a24a0 -> Movie-Frame (FUN_000ede90) falls Movie aktiv
        FUN_000a2c20: Fixed-Timestep-Loop, Context->Update (vf8), Context->Render (vf0xc, Profiler "Context::Render")
                      beim Contextwechsel: FUN_0002a660 (Backbuffer leeren) + neuer NiAlphaAccumulator -> renderer+8
        FUN_000f2cf0: Debug-Text-Overlay (fps, x/y/z) über eigenes Textsystem + NiCamera::Click (FUN_00098d20)
        renderer->DisplayFrame()            // D3DDevice_Swap
  app->vf4 Shutdown, delete app
```

Die Kamera bekommt den Renderer über `FUN_00013d40` (Inline-Setter `cam+0x1a8`, Name Vermutung `NiCamera::SetRenderer`). Ein Beispiel ist `FUN_000ee220`: `FUN_00013d40(*(DAT_002eb310+0x104))`. `FUN_00013d40` hat aber nur 2 Aufrufer (`0x3b960`, `0xee220`). Der häufigste Weg ist `FUN_00098ef0` (thiscall auf einem Kamera-Wrapper, `*this` = Kamera; setzt `cam+0x1a8` mit Refcount und ruft `FUN_00101840(0,1)`), 21 Spielfunktionen. Weitere 13 Spielfunktionen schreiben `cam+0x1a8` inline (z. B. `FUN_0001d020`, `FUN_000a4de0`, `FUN_000e9450`, `FUN_000ef680`; `verify_renderer_slotscan.py stores`). Für den Ersatz-Renderer ist das egal, weil überall nur der Zeiger gespeichert wird.

Weitere Sonderwege:

* **Render-to-Texture** `FUN_0003b960`: `SetRenderTarget(tex)` → `SetBackgroundColor(1,1,1)` → `cam->SetRenderer` → `ClearBuffer(cam port, 5)` → `Click` → `DisplayFrame` (no-op bei Textur-Target) → `SetRenderTarget(NULL)` → alte Farbe wiederherstellen.
* **Movie-Player** `0xed000–0xee300`: XMV dekodiert in eine D3D-Textur (`D3DDevice_CreateTexture2` in `FUN_000ee220`, Device aus `renderer+0x60` über `FUN_000ede40`). Gezeichnet wird mit `D3DDevice_SetRenderState_YuvEnable(1)`, `NiCamera::Click`, `YuvEnable(0)` (`FUN_000ede90`).
* **2D/HUD**: Das Spiel nutzt kein eigenes Overlay-API des Renderers, sondern Geometrie in eigenen Kameras (Textsystem `FUN_00099fe0`/`FUN_00098d20`) **und `NiScreenPolygon`s**. Der `NiScreenPolygon`-ctor `FUN_00106d50` (VTable-Store `mov [ebp],0x2a6884` bei `0x106d77`, von `renderer_xref.py` falsch dekodiert) wird aus 13 Spielfunktionen aufgerufen, u. a. `FUN_000f33e0` (aus der App-Init), `FUN_000d5e00` und dem Movie-Player `FUN_000ee220` (Movie-Quad). Die Polygone werden über `FUN_00019460` in die Kameraliste `cam+0x164` gehängt und gehen über `NiCamera::Click` → `FUN_00106af0` an Slot 54. Nur `NiScreenTexture` entsteht ausschließlich über Streaming/Klonen (ctor `0x10b340` nur aus `CreateObject`/`vf02`).

---

## 5. Hilfsklassen und Datenfluss

### 5.1 Modulgrenzen

REN = `[0x146020, 0x14cd50)` ∪ `[0x1552c0, 0x166871)`. Belege: Die Slot-Implementierungen liegen in 0x146350–0x14cb90. Die Debug-Strings der Hilfsklassen (`NiXBoxVertexBufferManager` 0x158440, `NiXBoxIndexBufferManager` 0x15cb42, `NiXBoxSourceTextureData` 0x15d920/0x15db00, `NiXBoxRenderedTextureData` 0x15e370, `…CubeMapData` 0x15e840/0x15ecf0, `NiXBoxPaletteData` 0x15f390, `NiXBoxBltSourceData` 0x15f580, `NiXBoxTextureManager` 0x15f91d, Memory-Pool-Dump 0x160d10) liegen in diesem Bereich. Die Funktionen 0x161800–0x1667d0 werden aus REN aufgerufen (z. B. `FUN_00163cd0`←`FUN_00159f10`). Ab `0x166871` beginnt die CRT (`_purecall` 0x1668b6). `0x14cd50–0x1552c0` ist generischer NI-Code (Viewer-Strings, `NiTriBasedGeomData`, `NiMorpherController`, `NiOutline`-Hilfe 0x153e90). Kleine Unschärfe an den Rändern: Vermutung.

### 5.2 Texturen: `NiSourceTexture` / `NiPixelData` → GPU

* Einstieg: Slot 65 → `FUN_0015e010` (`NiXBoxSourceTextureData`, 0x70 Bytes, ctor `FUN_0015d9e0`).
  1. `NiPixelData` holen: `tex+0x34` oder über den Image-Converter-Singleton `FUN_001511d0()->vf2(filename)` (Datei laden).
  2. Zielformat: `FUN_001572d0(pixelFormat, formatPrefs(tex+0x14), renderer+0x528)`, Fallback +0x5a0/+0x598. Nicht-Zweierpotenzen fallen auf den Default zurück (Bitschleifen auf `width/height`).
  3. Konvertieren: `converter->vf4(src, dstFormat, …)`.
  4. `FUN_0015d920` CreateSurf: `D3DDevice_CreateTexture2(w, h, 1, mipLevels, 0, d3dfmt, 3)`; d3dfmt aus `FUN_000edc70`.
  5. `FUN_0015db00` CopyDataToSurface: pro Mip `D3DTexture_GetSurfaceLevel2` → `LockRect`. Linear kopiert wird, wenn das Zielformat nicht geswizzelt ist (`FUN_00158990`) **und** `NiPixelData+0x2c` (Palette) NULL ist; das trifft auf DXT1/3/5 zu (`FUN_00158790` liefert für 0xc/0xe/0xf -1, nicht <8). Sonst läuft ein **Software-Swizzle** (`FUN_00155660` liefert die Bitmasken; Schleife `u = mask & (u - mask)` = Morton-Interleave) für 8/16/32 bpp, also auch für **Palettentexturen** (P8, 8 bpp).
  6. Palette: `FUN_0015df60` → Slot 69 (`NiXBoxPaletteData`, `D3DDevice_CreatePalette2`, `SetPalette` in `FUN_00162020`).
* `NiPixelFormat`-Format → Xbox-`D3DFORMAT` (`FUN_000edc70`, Werte im Binary, Namen nach XDK `d3d8types.h` aus dem Gedächtnis):

| NiPixelFormat (+0) | bpp (+0x14) | Ergebnis | XDK-Name (Vermutung) |
|---|---|---|---|
| 0 RGB | 16 | 5 bzw. 3 (G=6 Bit → 5) | R5G6B5 / X1R5G5B5 |
| 0 RGB | 24/32 | 7 | X8R8G8B8 |
| 0/1 | 8 | -1 | – |
| 1 RGBA | 16 | 5 / 4 / 2: dasselbe Kanalfeld wie bei RGB (Maske `0xfc000`, Shift 14 aus `0x2d7b6c`/`0x2d7b7c`) = 6 → 5, = 4 → 4, sonst 2 | R5G6B5 / A4R4G4B4 / A1R5G5B5 |
| 1 RGBA | 32 | 6 | A8R8G8B8 |
| 1 RGBA | 24 | 7 + `"NiXBoxPixelFormat> 24Bit ILLEGAL!"` | X8R8G8B8 |
| 2/3 PAL/PALA | – | 0xb | P8 |
| 4/5/6 | – | 0xc / 0xe / 0xf | DXT1 / DXT3 / DXT5 |
| 8 BUMP | 16 | 0x28 | V8U8 |
| 9 BUMPLUMA | 16/32 | 0x27 / 7 | L6V5U5 / X8L8V8U8 |

* Render-Targets: Slot 66 → `FUN_0015e430` (`NiXBoxRenderedTextureData`, `CreateTexture2` + `GetSurfaceLevel2`, `D3D_CreateStandAloneSurface` für Z). Slot 15 nimmt die RendererData `rt+0x20` (NULL → return false) und nutzt deren `vf12()` (0x30) als Target-Deskriptor (+0x14 Color, +0x20 Depth), Aufruf `D3DDevice_SetRenderTarget(desc+0x14, desc+0x20)` (`0x146440–0x146448`).
* Freigabe: `NiTexture`-RendererData liegt bei `+0x20` (Slots 15, 33, 65–68). `NiPalette`/`NiBltSource` haben sie bei `+0x14` (Slots 69/70).
* Für den PC-Port: `NiPixelData` liegt linear vor und kann direkt hochgeladen werden (BCn für DXT, P8 über die Palette expandieren). Das Swizzling entfällt.
* Nicht-Zweierpotenz-Texturen ersetzt `FUN_0015e010` komplett durch die Default-`NiPixelData` `renderer+0x5a0` (Format `+0x598`), nicht nur das Format.
* Der Movie-Player umgeht diesen Weg: `FUN_000ee220` legt mit `NiSourceTexture::Create(NULL, &DAT_002d7904)` eine Textur ohne Pixeldaten an (Slot 65 gibt ihr zunächst die Default-Textur), gibt deren D3D-Textur frei (`D3DResource_Release` bzw. Texture-Cache-Slot 7 über `*(data+0x10)+0x5cc`) und schreibt seine eigene Textur (`D3DDevice_CreateTexture2(w,h,1,1,0,0x24,3)`, Format 0x24 = YUY2, Name Vermutung) samt Größe/Revision direkt in `NiXBoxSourceTextureData` (+0x3c, +0x48, +0x4c, +0x54, +0x58, +0x60).

### 5.3 Geometrie: `NiGeometryData` → Vertex-/Index-Buffer

* RendererData liegt bei `NiGeometryData+0x30` (Größe 0x34). Felder laut `FUN_00146f20`/`FUN_00149ae0`: +4 Flags (Bit0 = precached/statisch, Bit23 = hat Normalen, Bits24+ = Anzahl UV-Sets), +8 Manager, +0xc Vertex-Shader/FVF, +0x10 Stride, +0x14 Vertexanzahl, +0x18 VB-Wrapper (+8 = `IDirect3DVertexBuffer8`), +0x1c Indexanzahl, +0x20/+0x28 Basis/Start, +0x24 `IDirect3DIndexBuffer8`, +0x2c Primitivtyp, +0x30 Primitivanzahl, +0x34/+0x38 Strip-Anzahl/-Längen.
* Primitivtypen (Tabelle `0x293998`, `count = a*prim + b`): 1 PointList (1,0), 2 LineList (2,0), 3/4 LineLoop/Strip (1,1), 5 TriList (3,0), 6/7 TriStrip/Fan (1,2), 8 QuadList (4,0), 9 QuadStrip (2,2). Das bestätigt die Xbox-`D3DPRIMITIVETYPE`-Nummerierung.
* **Statisch** (precached) und **dynamisch**: `PrecacheGeometry` (Slot 39) queued; BeginScene (`FUN_00148300`) erzeugt die Buffer. Ist im Queue-Eintrag Flag-Bit 0 gesetzt (das Spiel übergibt 1 in `FUN_000ce930`, `FUN_000d1120`), gibt `FUN_00148300` danach die **CPU-Kopien frei**: `NiGeometryData::Replace` `FUN_001437e0(n,0,0,0,0)` löscht Positionen/Normalen/Farben/UVs (Bits 0x10/0x20/0x40/0x80 behalten einzelne Arrays), und ohne Bit 0x100 löscht `FUN_00115db0(n,0)` (`NiTriShapeData`) bzw. `FUN_0011bc00` (`NiTriStripsData`) die Indexliste. Ein Ersatz-Renderer darf Slot 39 daher nur als echtes Upload-und-Freigeben oder als No-op (`return false`, CPU-Daten bleiben) umsetzen, nicht als „merken und später hochladen“. Beim Zeichnen gilt: ist der VB vorhanden und Bit0 gesetzt, wird er sofort wiederverwendet. Andernfalls wird neu befüllt, wenn `NiGeometryData+0x2e` (Revision/Dirty) ≠ 0, eine Skin-Instanz übergeben ist oder die Größe nicht passt. `+0x2e` wird am Ende von `FUN_00146f20` nur dann auf 0 gesetzt, wenn der Manager-Slot `vf10` (`+0x28`) false liefert (`FUN_00157c00`: `D3DVertexBuffer_Lock2`, Positionen/Normalen per `FUN_00157920`, Farben float→ARGB, UV-Sets).
* Indizes: `FUN_0015cb30` (IB-Manager `PackBuffer`). `DrawIndexedVertices` bekommt einen CPU-Zeiger `DAT_002444c4 + offset*2`. Das ist eine D3D-interne Index-Basis aus `SetIndices`; Xbox kopiert Indizes in den Push-Buffer.
* Draw-Kern `FUN_00149ae0` (RenderShape/Triangles/Strip/Strips, Screen-Polygon/-Texture):
  1. `FUN_0015aa60` (Pipeline-Objekt wählen, auch über `NiRendererSpecificProperty`)
  2. `FUN_00159cd0` (Properties → States)
  3. `FUN_00159e50` (Passanzahl/Flags)
  4. `VertexBlend` zurücksetzen
  5. `FUN_001489a0` (World-Transform → `SetTransform`)
  6. `FUN_0015bf80` (Lichter)
  7. Passschleife: `FUN_00159e60` (Pass-Setup), `FUN_00146f20` (VB/IB sicherstellen), `FUN_00159f10/20` (PS/VS), `SetStreamSource`/`SetIndices`, `FUN_00159f30` (Pass-Konstanten), `DrawVertices` bzw. `DrawIndexedVertices` je Strip
  8. Danach Fog restaurieren und `FUN_0015ab00`.

### 5.4 Properties → Render-States

`NiPropertyState` (an `renderer+0xc`) ist ein Array von Property-Zeigern ab +8. Die Zuordnung folgt aus den Zugriffen in `FUN_00159cd0`. Die Typreihenfolge entspricht der bekannten NetImmerse-Aufzählung (Vermutung):

| Offset | Property | Behandlung (Beleg) |
|---|---|---|
| +0x08 | NiAlphaProperty | `FUN_00159270`: Blend/AlphaTest über `D3DDevice_SetRenderState_Simple` + Shadow `0x2447b8…cc` |
| +0x0c | NiDitherProperty | Bit0 → Render-State (Shadow `0x2447d4`) |
| +0x10 | NiFogProperty | `FUN_00159700`: `D3DRS_FogEnable` (`0x244840`), Start/End/Density aus `rs+0x17c/0x180/0x184`, Fog-Tiefe `fog+0x18`; Farbe `FUN_00159900` (`SetRenderState_FogColor`) |
| +0x14 | NiMaterialProperty | `FUN_00161a80` (legt bei Bedarf das 0x48-Byte-Material-RendererData an `NiMaterialProperty+0x58` an, Revision `+0x54`, und setzt ein Dirty-Flag), danach ruft `FUN_00159cd0` selbst `FUN_00161880(rs+8)` → `D3DDevice_SetMaterial`, nur wenn dirty. Das ist keine Aufrufkette. Purge über Slot 59 |
| +0x18 | NiRendererSpecificProperty | `FUN_0015aa60`: Daten mit Typ 6 → eigenes Pipeline-Objekt |
| +0x1c | NiShadeProperty | `FUN_00159980` (Shade-Mode, Shadow `0x2447d8`) |
| +0x20 | NiSpecularProperty | Bit0 → `0x24486c` (Dirty `0x3000`) |
| +0x24 | NiStencilProperty | `FUN_001599e0`: `StencilEnable/StencilFail/CullMode` + Shadows `0x2447e0…f0` |
| +0x28 | NiTexturingProperty | Texture-Pipeline `+0x5c8` (`FUN_0015ab40`, `FUN_00162020`: `SetTexture`, `SetPalette`, `TextureFactor`, `BumpEnv`, `TexCoordIndex`, Texture-Transform) |
| +0x2c | NiVertexColorProperty | an Licht-Setup `FUN_0015bf80` übergeben |
| +0x30 | NiWireframeProperty | Bit0 → `SetRenderState_FillMode(0x1b01/0x1b02)` |
| +0x34 | NiZBufferProperty | `FUN_00159b60` (Shadows `0x2447b4/0x2447d0`) |

Ein Render-State-Cache (+0x5c0) speichert die letzten Werte (z. B. `rs+0x684` FillMode, `rs+0x674` VertexBlend, `rs+0x50c` FogEnable) und vermeidet Doppel-Sets.

`FUN_00159cd0` dereferenziert die Einträge +0x0c, +0x10, +0x20 und +0x30 ohne NULL-Prüfung (z. B. `*(*(ps+0x30)+0x14) & 1`). Ein `NiPropertyState` ist also immer voll besetzt (Default-Properties). Ein Ersatz-Renderer darf sich darauf verlassen.

### 5.5 Lichter

`FUN_0015bf80(effectState, texturingProp, vertexColorProp)` → `FUN_0015ba40` (Fixed Function):

* Zuerst werden die Lichter des letzten Draws deaktiviert (`D3DDevice_LightEnable(light+0xc4, 0)`).
* Dann wird die Lichtliste des EffectState (`effectState+0xc`) durchlaufen. Berücksichtigt werden nur Lichter mit On-Flag (`+0xc0`) und Dimmer `+0xe0` ≥ 0.01.
* Die Lichtschleife läuft nur, wenn das Flag `lightMgr+0x49` gesetzt ist und ein EffectState übergeben wurde.
* Liefert `light->vf40()` 0 (Vermutung: `GetEffectType()==AMBIENT`, also `NiAmbientLight`), wird `dimmer*color(+0xe4..+0xec)` zu einer Ambient-Summe addiert. Die Summe geht als ARGB (A=0xff) **direkt** in den D3D-Render-State-Shadow `0x24489c` mit Dirty-Bit `0x1000` in `0x2444c8`, nicht über ein D3D-API (Vermutung: `D3DRS_AMBIENT`).
* Alle anderen Lichter (`NiPointLight`, `NiDirectionalLight`, `NiSpotLight`) bekommen ein 0x70-Byte-RendererData-Objekt an `NiLight+0x108`: D3DLIGHT8 plus Index bei +0x6c = `NiLight+0xc4`. `FUN_0015b6a0` füllt es typabhängig, danach folgen `D3DDevice_SetLight(index, data)` und `LightEnable(index, 1)`.

BeginScene deaktiviert alle Lichter (`FUN_0015b990`). Slot 60 (PurgeEffect) behandelt nur `NiLight` (`IsKindOf(NiLight::ms_RTTI)` in vf60) und gibt dieses RendererData frei (`FUN_0015b9e0`). Wie `FUN_0015b6a0` die einzelnen Lichttypen auf D3DLIGHT8 abbildet, ist nicht im Detail gelesen (offen).

### 5.6 NiTextureEffect / Mehrpass-Pipeline

Die Texture-Pipeline (`FUN_0015b600` → `FUN_0015b4e0` → Init `FUN_0015a120`) legt `NiAlphaProperty`-Vorlagen für Zusatzpässe an. Sie verwendet `NiTextureEffect`-Felder (`+0x160` Filter, `+0x164` Clamp, `+0x168` TextureMode, `+0x16c` CoordMode, `+0x170` PlaneEnable; Namen aus den Viewer-Strings in `0x11a7c0`), belegt durch Zugriffe in `FUN_0015a120`, `FUN_00163f10`, `FUN_00164200`. In REN gibt es keine RTTI-Prüfung auf `NiTextureEffect`. Wie Projected Light/Shadow und Environment Map auf Pässe/Stages verteilt werden, ist **offen**. Vermutung: Mehrpass über `GetPassCount`/`SetupPass` des Pipeline-Objekts (`FUN_00159e50`/`FUN_00159e60`).

### 5.7 Skinning

`RenderShape` nimmt den Partition-Pfad `FUN_0014bc30` (Hardware-VertexBlend pro `NiSkinPartition`) nur, wenn `this+0x504 > 3`. `+0x504` wird nur mit 0 beschrieben (`renderer_xref`/grep: `FUN_00147550`, `FUN_00147cd0`), also ist dieser Pfad im Retail-Build tot. Tatsächlich läuft der Weg über `FUN_00146f20` → `FUN_00157c00`: Mit Skin-Instanz und neuem Frame-Zähler deformiert `FUN_0014f710` (5015 Bytes) Positionen und Normalen auf der CPU direkt in den gelockten VB (Vermutung aus der Struktur: Software-Skinning).

### 5.8 Shader

* Fester Pixel-Shader: `FUN_00147740` baut eine `D3DPIXELSHADERDEF` auf dem Stack → `D3DDevice_CreatePixelShader` → `this+0x5c`. Gebunden wird dieses Handle nirgends: `this+0x5c` wird sonst nur in `FUN_00147cd0` (=0) und `FUN_0014aca0` (`SetPixelShader(0)`, `DeletePixelShader`) angefasst. RenderPoints/RenderLines rufen `D3DDevice_SetPixelShader(0)` (Fixed-Function-Combiner). Pass-spezifische Pixel-Shader setzt nur `FUN_00163cd0` (← `FUN_00159f10`) aus dem Pipeline-Pass-Objekt (`pass+0x3c` → `+0x20`).
* Vertex-Shader-Verwaltung `FUN_0015d4c0` (`D3DDevice_CreateVertexShader`, Cache-Arrays). Sonst werden FVF-Handles über `SetVertexShader` gesetzt. Ob eigene VS-Programme benutzt werden, ist offen.

---

## 6. D3D8-Nutzung: NiXBoxRenderer und der Rest

Skript `renderer_d3dusage.py` (`detail`/`direct`). Basis: `callgraph.tsv`. „Transitiv“ heißt: Ein D3D-Sektionsaufruf zählt für einen Aufrufer außerhalb der Sektion auch dann, wenn er nur über D3D-interne Hilfsfunktionen erreicht wird.

| Kennzahl | Wert |
|---|---|
| Funktionen in Sektion D3D | 266 (128 als `D3D8::*` benannt) |
| benannte D3D8-Funktionen, transitiv **nur** aus REN erreichbar | **97** (korrigiert, vorher 93); mit REN-Code in Ghidra-Lücken **99** |
| … auch/nur von außerhalb erreichbar | 29 (vorher 26) |
| … ohne externen Aufrufer | nur Ghidra-Funktionen: 2 (`D3DResource_Register`, `D3D_AllocContiguousMemory`); mit Lücken-Code **0**, denn beide ruft REN-Code außerhalb der Ghidra-Funktionen auf (`0x1559aa`/`0x15631c` bzw. `0x160f85`/`0x161058`). (Ursprünglich 9; `D3DDevice_Begin`/`SetVertexData2f` erreicht REN laut Callgraph über `D3DDevice_Swap` → `FUN_00241c40` → `FUN_002410a0` → `FUN_00240b10`, `LoadVertexShader` über `SetVertexShader`) |
| alle D3D-Sektionsfunktionen nur aus REN / auch extern / ohne | 189 / 67 / 10 (vorher 177 / 53 / 36); mit Lücken-Code 192 / 67 / 7 |
| direkt aufgerufen von REN | 82 (80 benannt), davon **71 benannte nur von REN**; mit Lücken-Code 85 Ziele (82 benannt, dazu der Kernel-Thunk `MmFreeContiguousMemory` `0x2417b0`), **73 benannte nur von REN** |
| direkt aufgerufen von GAME | **8**: `D3DDevice_AddRef`, `D3DDevice_Release`, `D3DDevice_CreateTexture2`, `D3DTexture_GetSurfaceLevel2`, `D3DResource_AddRef`, `D3DResource_Release`, `D3DDevice_SetRenderState_YuvEnable`, `D3DDevice_SetScreenSpaceOffset` |
| direkt aufgerufen von XMV-Lib | 4: `GetDisplayMode`, `GetDisplayFieldStatus`, `D3DSurface_GetDesc`, `D3DSurface_LockRect` |

Die 29 „auch extern“ erreichbaren Funktionen sind fast alle interne Abhängigkeiten dieser 8+4 Einstiege (`CDevice_KickOff`, `D3D_SetFence`, `D3D_BlockOnResource`, `MakeSpace`, `CDevice_FreeFrameBuffers` usw.). Die ursprünglichen Transitiv-Zahlen stammten aus `renderer_d3dusage.py`, das Teilergebnisse innerhalb von Aufrufzyklen memoisiert und dadurch Erreichbarkeit verliert. Die korrigierten Werte liefern `verify_renderer_d3dtrans.py` (aufwärts ohne Memo) und `verify_renderer_d3d.py` (abwärts per BFS) übereinstimmend. Die Direktwerte (82/80/71, GAME 8, XLIB 4) bestätigt zusätzlich ein Rohscan aller `call/jmp rel32`, solange man nur Code innerhalb der Ghidra-Funktionen zählt. Der Byte-Scan `verify_renderer_bytescan.py` findet 15 weitere E8/E9-Aufrufstellen, alle in REN-Code außerhalb von Ghidra-Funktionen (`InsertFence` 2×, `D3DResource_Register` 2×, `SetTexture` 4×, `AddRef`, `CreateTexture2`, `SetTextureState_TexCoordIndex`, `D3D_AllocContiguousMemory` 2×, `CreateVertexBuffer2`, Kernel `MmFreeContiguousMemory`). Daher die Werte „mit Lücken-Code“. GAME bleibt bei 8 Funktionen.

**Zugriffe auf D3D-Interna** (`renderer_d3dglobals.py`; Befehle mit Speicheroperand im D3D-BSS `0x243890–0x246cf8`):

* REN: 22 Funktionen, 175 Befehle auf 43 Globals: 145 schreibende (davon 47 Read-Modify-Write wie `or [0x2444c8],edi`) und 30 rein lesende. Das sind Dirty-Flags `0x2444c8` (97×), Index-Basis `0x2444c4`, Deferred Texture-State `0x244500…0x24459c` (`D3D_g_DeferredTextureState`) und Render-State-Shadows `0x2447b4…0x24489c` (z. B. `D3DRS_FogEnable` = `0x244840`). Zusätzlich legt `FUN_0015ab40` bei `0x15ae30` die Adresse `0x2444d0` (D3D-BSS) als Immediate in eine lokale Variable (`mov [esp+0x14],0x2444d0`). Die weitere Verwendung ist nicht verfolgt; Vermutung: Zeiger in ein D3D-internes Array. Solche indirekten Zugriffe sind in den Zahlen nicht enthalten.
* GAME, NI, CRT: **0** Zugriffe, auch im Code außerhalb von Ghidra-Funktionen (`verify_renderer_gaps.py`).
* Inline-Push-Buffer-Schreibstellen außerhalb der D3D-Sektion wurden **nicht** gefunden. REN nutzt nur die Deferred-State-Arrays; Kommandos laufen über D3D-Funktionen.

Folgerung: Ersetzt man den `NiXBoxRenderer` komplett, bleiben vom D3D8-API nur die 8 Spielfunktionen (Movie-Player, Screen-Offset) und die XMV-Lib übrig. Beide werden ohnehin ersetzt bzw. gepatcht.

---

## 7. Empfehlung: eigener Renderer (`NiVulkanRenderer` / `NiD3D12Renderer`)

### 7.1 Grenze und Einbau

1. **Grenze = NiRenderer-VTable.** `FUN_0014c170` (Create) im Loader so patchen (Detour), dass ein eigenes Objekt zurückkommt. Dieses Objekt braucht:
   * die `NiRenderer`-Basis exakt: vtable +0, refcount +4, accumulator +8, propState +0xc, effectState +0x10. Den Basis-ctor `FUN_00140550` mitbenutzen, damit der Renderer in `DAT_002d7b1c` eingetragen wird. Beim Zerstören `FUN_001406e0` aufrufen.
   * eine eigene 75-Slot-VTable mit derselben Slotbelegung (C++-Klasse mit `__thiscall`-Thunks oder eine handgebaute Tabelle). Unbenutzte Slots bekommen sichere Stubs (`return 0`, `ret N` passend zu Abschnitt 2).
   * ein RTTI-Objekt, das `IsKindOf(NiRenderer)` erfüllt (Slot 1). Das vorhandene `NiXBoxRenderer::ms_RTTI` (`0x30ee00`) kann man weiterverwenden.
2. **Felder, die Fremdcode direkt liest:** `+0x60` (D3D-Device) im Movie-Player `FUN_000ede40`. Entweder den Movie-Player ersetzen (XMV-HLE) oder einen Dummy-Device-Zeiger bereitstellen. Der Movie-Player `FUN_000ee220` schreibt außerdem direkt in `NiXBoxSourceTextureData` (+0x3c/+0x48/+0x4c/+0x54/+0x58/+0x60) und liest `renderer+0x5cc` (5.2). Mit einer eigenen Texture-RendererData-Struktur muss der Movie-Pfad also zwingend mit ersetzt werden. Ein Dekompilat-Scan (`verify_renderer_fieldreads.py`) fand sonst nur Zugriffe auf +4, +8 (u. a. `FUN_0003b960` sichert den Accumulator), +0xc (`NiAlphaAccumulator::vf13` `0x2ade0`) und +0x60.
   * **Rückgabewerte:** Slot 65 und 66 müssen bei Erfolg true liefern, sonst liefern `NiSourceTexture::Create` bzw. `NiRenderedTexture::Create` NULL. Slot 39 darf false liefern (die Aufrufer werten das Ergebnis nicht aus), muss dann aber die CPU-Daten unangetastet lassen (5.3).
3. **Spielcode mit direktem D3D**: `FUN_000a1920…a1ae0` (`SetScreenSpaceOffset`, Bildlage ±12 px) auf no-op oder eine Viewport-Verschiebung legen. `NiXBoxRenderer::vf15` setzt über `FUN_000a1930`/`FUN_000a1920` beim Wechsel auf den Backbuffer den Nutzer-Offset (`DAT_002e7928/2c`) und bei Textur-Targets 0. Ein eigener `SetRenderTarget` muss das nachbilden, falls die Bildlage-Option erhalten bleiben soll. Den Movie-Pfad `FUN_000ed000–0xee300` zusammen mit der XMV-Ersetzung neu schreiben.

### 7.2 Prioritäten bis zu den ersten Pixeln

| Prio | Slots / Aufgaben | Begründung |
|---|---|---|
| **P0 – Frame-Gerüst** | Create-Ersatz; 0 (dtor), 1 (RTTI); **17 ClearBuffer**, **16 DisplayFrame**, **27 SetBackgroundColor**, **21/22 Width/Height**, **15 SetRenderTarget(NULL)**; **45/46 BeginScene/EndScene**, **47 SetCameraData** (View/Proj aus Frustum wie `0x1469e0`, inkl. Viewport aus Port); 56 und 71 als no-op | wird jeden Frame von `FUN_000d2630`, `NiCamera::Click` und den Lade-/Flush-Routinen aufgerufen |
| **P1 – erste Geometrie** | **54 RenderScreenPolygon** (Frontend/HUD/Movie-Quads des Spiels, siehe 4); **48 RenderShape**, **51 RenderTristrips**, 49/50; RendererData-Cache an `NiGeometryData+0x30` (statisch über Slot 39 vorbereitet, dynamisch über Revision `+0x2e`); **65 CreateSourceTextureRendererData** (`NiPixelData` linear → RGBA8/RGB565/BCn, Palette → expandieren oder Slot 69); PropertyState → Pipeline-State in der Reihenfolge Alpha, ZBuffer, Texturing (Base-Map), VertexColor/Material, dann Specular/Fog/Stencil/Wireframe/Shade/Dither; Lichter aus dem EffectState (Fixed-Function-Lighting im Shader nachbilden) | damit erscheinen Level-Geometrie und Figuren |
| **P2 – vollständige Szene** | 52 RenderPoints (Partikel), 53 RenderLines; Purge-Slots 58–62, 64 (Lebensdauer der Daten); 69 Palette, 70 BltSource; **66 + 15 Render-to-Texture** (Schatten-/Effektkameras `FUN_0003b960`, `FUN_0001f1a0`); Skinning (CPU-Deform `FUN_0014f710` weiterverwenden oder GPU-Skinning über `NiSkinInstance`); 29/30 Fog-Max; Mehrpass-Texture-Pipeline / NiTextureEffect; 41–44 Batch (nur falls doch aufgerufen); 55 Screen-Texture | vollständige Optik |
| **P3 – Stubs/optional** | 11–14, 18–20, 23–26, 28, 31–38, 40, 57, 63, 67, 68, 72–74 | keine Aufrufer gefunden; Stubs mit korrektem `ret N` genügen (Achtung: Slot 31 hat 3 Stack-Args, `ret 0xc`, weil er per `jmp` in TextureManager-vf3 `0x15f7e0` springt). TakeScreenShot (24) später für Savegame-Bilder prüfen |

### 7.3 Alternative: HLE auf D3D-Ebene

Machbar, aber teurer. Nötig wären 80+ D3D8-Funktionen (mit Lücken-Code 82) plus die Emulation der Deferred-State-Globals und Dirty-Flags (`0x2444c8`, `0x244500…`, `0x2447b4…`), die REN direkt beschreibt. Außerdem kommen Xbox-Spezifika hinzu: Index-Pointer statt Index-Buffer, geswizzelte Texturen, Push-Buffer-Fences (`BlockOnFence`/`IsFencePending` in `FUN_00155440`). Als Zwischenschritt zum Booten ist ein minimales D3D-Stub-Set (Create/Clear/Swap) denkbar. Als Ziel wird die NiRenderer-Ebene empfohlen.

---

## 8. Offene Punkte

* Typspezifische D3DLIGHT8-Befüllung (`FUN_0015b6a0`) und vollständige NiTextureEffect-/Mehrpass-Logik (`FUN_0015ab40`, `FUN_00162020`, Pipeline-Vtables um `0x2ac0f8`).
* Ob `D3DDevice_CreateVertexShader` (`FUN_0015d4c0`) zur Laufzeit echte VS-Programme lädt; Aufrufer `FUN_0015d6a0` ist nicht verfolgt.
* Exakte Semantik der Slots 14, 34–38, 40, 56, 57, 63, 71–74 (keine Aufrufer bzw. no-op).
* Direkte Feldzugriffe des Spielcodes auf `NiXBoxRenderer`: Der Dekompilat-Scan der Review (`verify_renderer_fieldreads.py`) fand nur +4/+8/+0xc/+0x60 und indirekt +0x5cc (Movie-Player). Zugriffe über Zeiger, die nicht aus `app+0x104`, App-Slot 5 oder `cam+0x1a8` stammen, erfasst er nicht.
* **Lücken im Ghidra-Export:** Rund 14 KB im REN-Bereich gehören zu keiner Ghidra-Funktion. Das ist überwiegend Code: viele dieser Lücken beginnen genau an VTable-Einträgen, es sind also Methoden der Hilfsklassen, z. B. TextureManager-vf1/vf3/vf5 `0x15f7d0–0x15f930`, `0x160ff0–0x1612e0` mit `D3D_AllocContiguousMemory`/`MmFreeContiguousMemory` (Vermutung: Speicherpool-Klasse, VTable-Eintrag `0x2aaf04`), `0x164360–0x1650f0` (3,5 KB). Im NI-Bereich sind es rund 23 KB, im Spielcode rund 120 KB; dort mischen sich Code und eingebettete Tabellen, das wurde nicht einzeln geprüft (`verify_renderer_gaps.py`). Diese Stellen fehlen in `callgraph.tsv` und im Dekompilat. Die Slot- und D3D-Zahlen dieses Berichts wurden dafür per Byte-/Lückenscan nachgeprüft (siehe Verifikation). Für die Dekompilierung von REN müssen diese Funktionen aber erst angelegt werden.
* Mögliche indirekte Schreiber von `DAT_0030edd4` (Texture-Cache-Modus) und `+0x504` (Hardware-Skinning) sind nicht ausgeschlossen. Gefunden wurden nur direkte Adressierungen.
* Namen `NiCamera::Click` (`0x100c50`), `NiCamera::SetRenderer` (`0x13d40`) und `SetAccumulator` (`0x153400`) sind aus dem Verhalten abgeleitet.

---

## 9. Belegstellen (Auswahl, schnell prüfbar)

* VTable-Dump / Override: `python scripts\renderer_vtables.py`
* Stack-Args je Slot: `python scripts\renderer_retn.py 2aa7c0 75`
* Renderer-Aufrufer (Binär-Scan): `python scripts\renderer_callers.py`
* Virtuelle Aufrufe im Dekompilat mit aufgelösten Empfängern: `python scripts\renderer_vcalls2.py b0 128`
* D3D8-Nutzung: `python scripts\renderer_d3dusage.py detail` bzw. `direct`
* D3D-Global-Zugriffe: `python scripts\renderer_d3dglobals.py`
* Raw-Xrefs (VTable-Stores, Globals): `python scripts\renderer_xref.py 2aa7c0 30edd4`
* Hilfs-VTables im Modul: `python scripts\renderer_modvtables.py`
* TSV-Erzeugung: `python scripts\renderer_vtable_tsv.py`

## 10. Skripte

| Datei | Zweck |
|---|---|
| `renderer_vtables.py` | NiRenderer- und NiXBoxRenderer-VTable vergleichen |
| `renderer_dfun.py` | Dekompilat-Index (Cache in `%TEMP%`), Funktionen nach Adresse ausgeben |
| `renderer_vt.py` | VTable einer NI-Klasse mit Override-Markierung |
| `renderer_xref.py` | Raw-Referenzen (imm/disp32) auf eine VA mit dekodiertem Befehl (rät den Befehlsanfang rückwärts und dekodiert manche Stellen falsch, z. B. `0x106d77`; für Vollständigkeit `verify_renderer_globalaccess.py` nutzen) |
| `renderer_vcalls.py`, `renderer_vcalls2.py` | Virtuelle Aufrufe im Dekompilat nach Offset, mit Auflösung der Empfänger-Zuweisung |
| `renderer_appscan.py`, `renderer_camscan.py`, `renderer_callers.py` | Binär-Scans für Renderer-vcalls (App+0x104, Kamera+0x1a8, App-Slot 5) |
| `renderer_retn.py` | `ret N` je Slot (folgt nur direkten `jmp`, nicht `jmp [reg+N]` wie in Slot 31) |
| `renderer_modvtables.py` | Hilfsklassen-VTables im Renderer-Modul |
| `renderer_d3dusage.py` | D3D8-Erreichbarkeit REN/NI/GAME/XLIB (Achtung: die Transitiv-Werte sind wegen Memoisierung in Zyklen zu niedrig, siehe Verifikation; Direktwerte stimmen) |
| `renderer_d3dglobals.py` | Zugriffe auf D3D-BSS-Globals |
| `renderer_vtable_tsv.py` | erzeugt `nixboxrenderer_vtable.tsv` |

---

## Verifikation

Prüfer: unabhängiges Review dieses Berichts gegen die rohe XBE (`orig\retail_eu\default.xbe`, Capstone) und den Ghidra-Export. Ghidra selbst wurde nicht geöffnet. Neue Prüfskripte liegen unter `analysis\loader\scripts\verify_renderer_*.py`. Aus einem früheren, abgebrochenen Review-Lauf stammen `verify_renderer_common/vtable/vf33/broadcast/vcalls/d3d/d3dtrans/d3dglobals/fieldreads/...`. Die Ausgaben von `verify_renderer_vtable.py`, `verify_renderer_vf33.py` und `verify_renderer_broadcast.py` wurden in diesem Lauf neu erzeugt und gegen Disassembly geprüft.

| Skript (neu in diesem Lauf) | Prüfung |
|---|---|
| `verify_renderer_slotscan.py` | Datenfluss-Scan des Dekompilats pro Anweisung: Renderer-Quellen sind App-Slot 5, `app+0x104`, `cam+0x1a8`, `this` in Renderer-Methoden und Listenläufer. Zuweisungen löschen den Status. Modi `slots`, `stores`, `passes` und `fields` |
| `verify_renderer_highoff.py`, `verify_renderer_highoff_recv.py` | typunabhängiger Rohscan aller `call/jmp [reg+off]` für die Slots 40–74 und Empfängerausdruck im Dekompilat; Spiel-VTables mit mehr als 40 Slots |
| `verify_renderer_d3ddirect.py [gaps]` | direkte D3D-Aufrufe je Region (REN/NI/GAME/XLIB), transitive BFS; mit `gaps` auch Code außerhalb von Ghidra-Funktionen |
| `verify_renderer_bytescan.py` | Brute-Force-E8/E9-Scan über `.text` auf D3D-Funktionsanfänge, Vergleich mit dem linearen Sweep |
| `verify_renderer_gaps.py` | Code außerhalb von Ghidra-Funktionen: Größe je Region, VTable-Referenzen, D3D-Aufrufe, D3D-BSS-Zugriffe, Aufrufe mit Renderer-Offsets |
| `verify_renderer_d3dbss.py` | D3D-BSS-Zugriffe mit Capstone-Zugriffsflags (Lesen/Schreiben/Read-Modify-Write), Immediates in die D3D-BSS |
| `verify_renderer_field504.py` | alle Lese- und Schreibzugriffe auf `[reg+0x504]` (ganzes Binary) sowie `+0x5c`/`+0x500` in REN |
| `verify_renderer_globals2.py` | alle Zugriffe auf `DAT_0030edd4`, `DAT_002d7900`, `DAT_002d6740`, `DAT_002e7928/2c` samt Initialwert |
| `verify_renderer_rawcallers.py` | Rohscan `call/jmp rel32` auf beliebige Ziele (unabhängig von `callgraph.tsv`) |

### Kernaussagen (Key Facts 1–17)

| # | Aussage | Ergebnis | Beleg der Review |
|---|---|---|---|
| 1 | `Create` = `FUN_0014c170`, cdecl, 8 Args, nur aus `FUN_000d26a0`, Ergebnis in `app+0x104` | bestätigt | `0xd26be–0xd26da` pusht `1,0,0x4b,0x14000002,1,2,0x1e0,0x280`, `add esp,0x20` bei `0xd26e7`, `mov [esi+0x104],edi` bei `0xd26ff`; Rohscan: einziger Aufrufer `0xd26a0`; ctor `0x14af00` nur aus `0x14c170` |
| 2 | App-Slot 5 `0xd2690` = `mov eax,[ecx+0x104]; ret`; App in `DAT_002eb310`/`DAT_002e7b84` | bestätigt | App-VTable `0x2a3ef8` Slot 2/3/4/5 = `0xd26a0/0xd2630/0xd2920/0xd2690`; App-Größe `0x108` (`FUN_000d29b0`). Die Zahl „63 Aufrufe“ aus der Kurzfassung des Auftrags steht nicht im Bericht; nachgezählt sind es **82** Aufrufstellen `(*DAT_002eb310/2e7b84 + 0x14)()` in 50 Funktionen, dazu Aufrufe über `this` in App-Methoden |
| 3 | 25 überschrieben / 30 pure / 17 geerbt / 3 neu | bestätigt | `verify_renderer_vtable.py`, TSV ohne Abweichung |
| 4 | Szenen-Slots 45/46/47, Reihenfolge in `NiCamera::Click` | bestätigt | `0x100c8b–0x100cc7` (`lea` +0x14c/+0x134/+0x128/+0x11c/+0x110/+0xa0, `call [edx+0xbc]`), `0x100cd5` `+0xb4`, Ende `+0xb8`; `0x148997` `mov byte [esi+0x500],1`, `0x1469ca` =0; vf47 `ret 0x18` |
| 5 | Render-Slots 48–55 aus den `vf33`-Funktionen bzw. `FUN_00106af0`/`FUN_0010af70` | bestätigt | Aufrufstellen wie angegeben; `NiTriShape::vf33` pusht `this+0x20`, `this+0x70`, `[this+0xcc]`, `[this+0xc8]`; `FUN_00106af0` setzt `r+0xc = poly+8`, `r+0x10 = 0` und springt `jmp [vt+0xd8]` |
| 6 | Aufrufer schreiben `+0xc/+0x10`; das Spiel setzt den Accumulator `+8` | bestätigt | Stores `0x1166f0/0x1166f9` (edi = `cam+0x1a8`); `FUN_000a2c20` setzt den neuen `NiAlphaAccumulator` nach `GetRenderer()+8`; `FUN_0002a660` `0x2a730` `mov [esi+8],0` |
| 7 | Broadcasts über die Liste `DAT_002d7b1c`, Eintrag im ctor `FUN_00140550` | bestätigt | Alle 10 Referenzen auf `0x2d7b1c` geprüft; Aufrufer-dtors über ihren VTable-Store identifiziert (`0x14374d` NiGeometryData, `0x114b3d` NiMaterialProperty, `0x119f8d` NiTextureEffect, `0x14555d` NiLight, `0x10afbd` NiScreenTexture, `0x10bb4d` NiSkinPartition, `0x106a7d` NiScreenPolygon). Listenobjekt `0x2d7b18` (+0 Zähler, +4 Kopf, +8 Ende, Knoten {Renderer, next, prev}); Austragen im dtor über `FUN_00140690` mit `ecx=0x2d7b18` |
| 8 | Spiel nutzt direkt nur die Slots 15/16/17/21/22/27/39/71 | bestätigt | `verify_renderer_slotscan.py` liefert genau diese Slots mit denselben Aufruferlisten wie Abschnitt 3.1. Die Treffer des früheren Scans `verify_renderer_vcalls.py` auf den Slots 3/6/11/12/33/38/40/41 sind Fehlalarme durch Variablen-Wiederverwendung bzw. Accumulator-Aufrufe; zum Slot-40-Artefakt in `FUN_0003b960` siehe 3.2. Keine direkten (nicht-virtuellen) Aufrufe der Slot-Funktionen, auch nicht im Rohscan |
| 9 | DisplayFrame/ClearBuffer | bestätigt | `0x146463–0x14646f`: `[[this+0x58]+0x10]==0` → `D3DDevice_Swap(0)` (`0x240fc0`); ClearBuffer-Flags `bVar6(2) \| (mode&1?0xf0:0) \| bVar5(1)`, Stencil nur mit `(char)this[0x2d8] < 0` |
| 10 | Spiel ruft 8 D3D-Funktionen direkt, Device aus `renderer+0x60` | bestätigt | Rohscan: genau 8 Ziele (`0xede40`, `0xede90`, `0xee080`, `0xee190`, `0xee220`, `0xa1920–0xa1ae0`); `FUN_000ede40`: `*(*(DAT_002eb310+0x104)+0x60)`; auch im Code der Ghidra-Lücken keine weiteren Spiel-Aufrufe |
| 11 | REN: 80 benannte direkt, davon 71 exklusiv; transitiv 93 | **teilweise korrigiert** | Innerhalb der Ghidra-Funktionen gilt 82/80/71, transitiv **97** (war im Bericht schon korrigiert). Neu: 15 weitere Aufrufstellen in REN-Code ohne Ghidra-Funktion. Mit ihnen sind es 82 benannte direkt, davon 73 exklusiv, transitiv 99, und es gibt **keine** unerreichte benannte D3D8-Funktion mehr (bisher galten `D3DResource_Register` und `D3D_AllocContiguousMemory` als unerreicht). Abschnitt 6 und die Kurzfassung sind angepasst |
| 12 | 22 Funktionen, 145 Schreib- und 30 Lesezugriffe, GAME/NI = 0 | bestätigt, präzisiert | 175 Befehle auf 43 Globals: 98 reine Schreib-, 47 Read-Modify-Write- und 30 reine Lesebefehle. Capstone zählt 77 lesende Befehle, wenn man RMW mitzählt. Neu gefunden: Immediate `0x2444d0` in `FUN_0015ab40` (`0x15ae30`). GAME/NI/CRT/XLIB 0, auch in den Lücken. Beispiel `0x15974b`/`0x159751` bestätigt |
| 13 | CPU-Swizzle, DXT linear, Formatabbildung | bestätigt | `FUN_0015db00`: linear, wenn `(!IsSwizzled && pal==NULL) \|\| bpp<8`; `FUN_00158990` meldet 0xb (P8) und 0xc/0xe/0xf **nicht** als geswizzelt, P8 geht über die Palettenprüfung in den Swizzle-Pfad. Alle Zeilen der Tabelle in 5.2 mit `FUN_000edc70` abgeglichen (Maske `0xfc000`, Shift `0xe` aus `0x2d7b6c`/`0x2d7b7c`) |
| 14 | `+0x504` nur 0 → Hardware-Skinning tot; CPU-Skinning über `FUN_0014f710` | bestätigt (Software-Skinning bleibt Vermutung) | Schreibzugriffe auf `[reg+0x504]` in REN nur `0x14768d` (=0) und `0x147ee7` (`ebx`=0 ab `0x147cd1`); die übrigen Treffer im ganzen Binary sind Byte-Felder von Spielobjekten. vf48 `0x14c52d` `cmp [esi+0x504],4; jb` (nur mit Skin-Instanz); `FUN_0014f710` hat einen einzigen Aufrufer `0x157c23` |
| 15 | RendererData `+0x30`, Precache, Revision `+0x2e`, Primitivtabelle | bestätigt, präzisiert | Tabelle `0x293998` = (0,0),(1,0),(2,0),(1,1),(1,1),(3,0),(1,2),(1,2),(4,0),(2,2),(0,0). `+0x2e` wird nur zurückgesetzt, wenn Manager-vf10 false liefert (5.3 ergänzt) |
| 16 | PropertyState → States in `FUN_00159cd0` | bestätigt | alle sechs Zuordnungen im Dekompilat; Material-Kette korrigiert (5.4) |
| 17 | Fixed-Function-Lichter | bestätigt, ergänzt | `FUN_0015ba40`: `vf40` (`+0xa0`), `+0xc0`, `+0xe0 ≥ 0.01`, `+0xe4..+0xec`, RendererData `+0x108`, Index `+0xc4` → `obj+0x6c`; neu: Ambient direkt nach `0x24489c` (5.5) |

### Weitere geprüfte Einzelaussagen

* Klassenmetadaten: Die Strings hinter den VTables (`0x2aa010` „%s = (%5.3f,…)“, `0x2aa8ec` „UNKNOWN!“), die VTable-Stores `0x14af26`/`0x14c881` und der RTTI-Stub in `initstubs_00230000.c:833` sind bestätigt.
* `Create`: Push-Buffer-Größen `0x200000`/`0x8000` (aus `0x2d7cec`/`0x2d7cf0`), PRESENT_PARAMETERS ab `+0x27c` (17 Dwords), BehaviorFlags-Logik, Interval-Tabelle, Default-Frustum (-1,1,1,-1,0.1,1) und Port (0,1,1,0) sind bestätigt. `FUN_00240390` ist `mov eax,1; ret 4` (präzisiert).
* Hauptschleife: `FUN_000d29b0` ruft vf2, vf3 in einer Schleife und vf4 auf. `FUN_000d2630` ruft `ClearBuffer(0, DAT_002d6740)` und `DisplayFrame` über `param_1[0x41]` auf. `DAT_002d6740` ist mit 4 initialisiert und wird nur von `FUN_00012210`/`00012db0`/`000c3820` auf 5 bzw. 4 gesetzt. App-vf8 `FUN_000a24a0` → `FUN_000ede90`.
* `ret N`: Slot 31 springt nach `0x15f7e0`, das mit `ret 0xc` endet (`0x15f871`); Slot 42 hat `ret` bei `0x14bc18`, Slot 52 `ret 0xc` bei `0x149526`. Slot 59 läuft über Tail-Calls bis `ret 4` bei `0x1618d5`. Slot 14: Basis `mov eax,0x800000; ret 8`, Xbox `xor eax,eax; ret 8`.
* SetRenderTarget `0x146400`: false während einer Szene; bei NULL Backbuffer `this+0x14` plus `FUN_000a1930`, sonst RendererData `rt+0x20` → `vf12` plus `FUN_000a1920`. Die Bildlage-Globals `0x2e7928/2c` schreiben nur `FUN_000a1950/19d0/1a00/1a80/1ae0`.
* `DAT_0030edd4` (BSS) wird an 8 Stellen nur gelesen. `DAT_002d7900` hat das Byte 1 und wird nie geschrieben. Caps `+0x2d8` setzt REN nur auf `0x6d`/`0xed` (`FUN_001474f0`).
* Pixel-Shader: `FUN_00147740` übergibt `this+0x5c` als Out-Parameter an `D3DDevice_CreatePixelShader` (`0x14778b`/`0x14780e`). `SetPixelShader` rufen in REN nur vf52, vf53 und `FUN_0014aca0` mit 0 auf, dazu `FUN_00163cd0` mit dem Pass-Shader.
* Aufruferzahlen per Rohscan: `NiScreenPolygon`-ctor 13 GAME, `FUN_00100f30` 9 GAME, `NiCamera::Click` 17 GAME, `NiRenderedTexture::Create` 3 GAME, `FUN_00118600` 3 GAME plus REN `0x163a40`. **Korrigiert:** `FUN_00019460` hat 15 statt 16 GAME-Funktionen (21 Aufrufstellen), dazu 2 NI-Aufrufer.
* Movie-Player `FUN_000ee220`: `CreateTexture2(w,h,1,1,0,0x24,3)` und die Schreibzugriffe auf `NiXBoxSourceTextureData` +0x3c/+0x48/+0x4c/+0x54/+0x58/+0x60 sind bestätigt; der Texture-Cache-Aufruf läuft über `+0x1c` (Slot 7) auf `*(data+0x10)+0x5cc`.
* Feldzugriffe von GAME/NI auf den Renderer (`verify_renderer_slotscan.py fields`): nur +4, +8, +0xc und +0x60. Zusätzlich liest NI `FUN_00117b30` +8. Das deckt sich mit 7.1.
* `NiRenderedTexture::Create` `FUN_0013ffe0` verlangt Zweierpotenzen und ruft bei Fehlschlag von Slot 66 den dtor auf. `FUN_00118600` liefert bei false aus Slot 65 NULL (`DAT_002d7900` ≠ 0).

### Korrekturen in diesem Lauf

1. 3.1: `FUN_00019460` hat 15 statt 16 GAME-Funktionen (21 Aufrufstellen), dazu 2 NI-Aufrufer.
2. 6 und Kurzfassung: Die D3D-Zahlen sind um den REN-Code außerhalb von Ghidra-Funktionen ergänzt (82/73 direkt, 99 transitiv, 0 unerreicht, 192/67/7); die Aussage „ohne externen Aufrufer: `D3DResource_Register`, `D3D_AllocContiguousMemory`“ gilt nur für Ghidra-Funktionen.
3. 5.4: Die Material-„Kette“ `FUN_00161a80 → FUN_00161880` gibt es nicht; beide werden nacheinander aus `FUN_00159cd0` aufgerufen.
4. 5.2: Slot 15 ruft `vf12` der RendererData `rt+0x20` auf, nicht `rt->vf12()`.
5. 6: „145 Schreib-/30 Lesezugriffe“ ist erläutert (47 davon RMW).
6. Präzisierungen: `FUN_00240390` (Konstante 1), vf39 (geskinnter Pfad), `+0x2e`-Reset nur bedingt, vf47 mit Xbox-Transform-Zählung 0/1, Feld `+0x5b0` ergänzt (1.3). TSV-Zeilen 31/39/47 und `renderer_vtable_tsv.py` sind entsprechend angepasst; eine Neuerzeugung liefert inhaltlich dieselbe TSV.

### Ergänzt (fehlte im Bericht)

* **Ghidra-Lücken:** Rund 14 KB REN-Code gehören zu keiner Ghidra-Funktion, darunter VTable-Methoden der Texture-Manager- und Pool-Klassen, D3D-Aufrufe sowie `D3D_AllocContiguousMemory`/`MmFreeContiguousMemory` (siehe 8). Wichtig für die Dekompilierung und für jede `callgraph.tsv`-basierte Zählung.
* Ghidra-Artefakt in `FUN_0003b960`: ein scheinbarer Slot-40-Aufruf auf dem Renderer (3.2).
* `FUN_00098ef0` als häufigster Kamera-Renderer-Setter (21 GAME-Funktionen) und Inline-Stores nach `cam+0x1a8` (4).
* Ambient-Licht wird direkt in den D3D-Shadow `0x24489c` geschrieben (5.5); die Lichtschleife hängt am Flag `lightMgr+0x49`.
* `NiPropertyState` ist immer voll besetzt (5.4).
* Immediate-Adresse `0x2444d0` in `FUN_0015ab40` (6).

### Offen / unsicher

* Spielcode außerhalb von Ghidra-Funktionen (rund 120 KB, Code und Tabellen gemischt) wurde nur per Rohscan auf D3D-Aufrufe, D3D-BSS-Zugriffe und Aufrufe mit Offsets `0xa0–0x128` geprüft; deren Empfänger wurden nur stichprobenartig angesehen (`0x6cb24`, `0x66fe6`, `0x83c6d`, `0x50eac`, `0x86776`, `0x83493`, `0x6017f`, `0x85cf4`, `0x352f0`, alle Spielobjekte). Für die niedrigen Slots 11–38 stützt sich „keine Aufrufer“ weiter nur auf die Quellverfolgung (Dekompilat und Binär-Registerverfolgung), nicht auf einen typunabhängigen Beweis.
* Dass `FUN_0014f710` Software-Skinning ist, die Typreihenfolge im `NiPropertyState`, `D3DRS_AMBIENT` für `0x24489c` und die Namen der Slots 14, 34–38, 40, 56, 57, 63 und 72–74 bleiben Vermutungen.
