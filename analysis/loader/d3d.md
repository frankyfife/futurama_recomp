# Direct3D-8-Schnittstelle (Xbox D3D8, XDK 5233) – Inventar und HLE-Grenze

Ziel: `orig\retail_eu\default.xbe` (Retail EU/PAL). Grundlage sind der Ghidra-Export (`analysis\retail_eu_export`) und eigene Capstone-Scans der rohen XBE. Alle Zahlen lassen sich mit den Skripten in `analysis\loader\scripts\d3d_*.py` reproduzieren (Abschnitt 1). Das vollständige Funktionsinventar mit Aufrufzahlen steht in `analysis\loader\d3d_functions.tsv`.

## 0. Kurzfazit

* Die D3D8-Bibliothek liegt komplett in der Section `D3D` (va 0x2342c0, raw bis 0x243890, danach BSS bis 0x246cf8). Ghidra kennt dort 266 Funktionen, 128 davon mit `D3D8::`-Namen.
* Von außerhalb der D3D-Section werden **88 D3D-Funktionen** direkt aufgerufen: 85 benannte und 3 unbenannte. Das sind **325 Call-Sites** (inkl. 5 Tail-`jmp`) in **143 Funktionen**.
* Die Aufrufer gruppieren sich so:
  * NetImmerse-Xbox-Renderer-Bibliothek („NiXBox lib“): 128 Funktionen, davon 11 `NiXBoxRenderer::vf*`
  * Spielcode: 12 Funktionen (Movie-Player und Bildschirmverschiebung)
  * XMV-Bibliothek: 3 Funktionen
  * **Kein** anderer Ni*-Code, kein XAPI/CRT/DSOUND/XPP.
* **Kein einziger Push-Buffer-Zugriff außerhalb der D3D-Section.** Es gibt null Referenzen auf `D3D__pDevice` (0x244968), `g_Device.m_pPut` (0x244970) und `m_pThreshold` (0x244974). Ebenso null Aufrufe von `D3DDevice_MakeSpace` (0x2392b0), `D3D_MakeRequestedSpace_8` (0x239130) oder `D3DDevice_BeginStateBig` (0x2392c0) und null NV2A-MMIO-Zugriffe. Alle (mindestens 69, siehe 2.3) Push-Buffer-Schreiber liegen in der D3D-Section.
* Die einzigen „Inline“-Teile außerhalb von D3D sind die geinlinten XDK-Makros `SetRenderState`, `SetTextureStageState` und `DrawIndexedPrimitive`:
  * 199 Instruktionen in 22 NiXBox-Funktionen lesen oder schreiben nur vier D3D-Globals: `D3D__RenderState`, `D3D__TextureState`, `D3D__DirtyFlags` und `D3D__IndexData` (korrigiert, vorher 198; siehe Verifikation).
  * Zusätzlich lesen 4 NiXBox-Funktionen drei **konstante** D3D-Tabellen in `.rdata`: 0x293828 und 0x293608 (Dispatcher 0x1478c0) sowie die Primitiv→Vertexzahl-Tabelle 0x293998 (10 Instruktionen in vf42, 0x149ae0, 0x14bc30; geinlintes `DrawIndexedPrimitive`/`DrawPrimitive`). Da sie read-only sind, braucht das HLE dafür nichts zu tun.
  * Dazu kommen 28 Aufrufe von `D3DDevice_SetRenderState_Simple`, die eine NV2A-Methode in `ecx` übergeben.
  * Diese Stellen muss man nicht patchen. Ein HLE liest die Globals beim Draw-Aufruf aus, so wie es die originale D3D-Lazy-State-Logik tut.
* Gerendert wird über ganze 7 Draw-Call-Sites (`DrawVertices` 2×, `DrawIndexedVertices` 5×).
* **Empfehlung (c) Hybrid, Phase 1 = D3D-API-HLE:**
  * Die 88 Einsprungpunkte werden ersetzt und die vier Lazy-State-Globals emuliert. NiXBoxRenderer, Movie-Code und XMV laufen unverändert als Originalcode.
  * NiXBox-Funktionen werden danach schrittweise durch Decompilat ersetzt, das gegen denselben Shim kompiliert.
  * Ein kompletter Ersatz auf NiRenderer-Ebene (b) bedeutet, einen ganzen NetImmerse-Renderer neu zu schreiben (rund 423 Funktionen / 82 KB NiXBox-Code funktional ersetzen). Er löst das Movie-Problem nicht, weil der Spielcode direkt in NiXBox-Interna greift.

## 1. Methodik und Skripte

| Skript (`analysis\loader\scripts\`) | Zweck | Ausgabe |
|---|---|---|
| `d3d_common.py` | Gemeinsame Helfer. `attribute(addr)` ordnet eine Instruktion ihrer Funktion zu. Fehlt die Funktion in `functions.tsv` (11 Fälle im NiXBox-Bereich), entsteht `sub_XXXXXXXX`. Der Schnitt erfolgt an MSVC-Padding nach `ret`/`jmp`. | – |
| `d3d_inventory.py` | Linearer Capstone-Scan aller x86-Sections nach `call`/`jmp` in D3D-Funktionsstarts. Dazu `ret N` je D3D-Funktion, Zeiger-Tabellen in D3D/.rdata/.data und eine Aufrufer-Klassifikation. | `analysis\loader\d3d_functions.tsv`, `scripts\out\d3d_inventory_summary.txt` |
| `d3d_scan_refs.py` | Alle Referenzen von Nicht-D3D-Code in den D3D-Adressraum: Calls, Speicheroperanden, Immediates, Datenzeiger. Die im Docstring genannte Art `rdata` (Zugriffe auf D3D-Tabellen in `.rdata`) ist **nicht implementiert**. Diese Zugriffe liefert `verify_d3d_inline_tables.py`. | `scripts\out\d3d_refs.tsv` |
| `d3d_pushbuffer_writers.py` | Welche Funktionen berühren `D3D__pDevice`, `m_pPut`/`m_pThreshold` oder rufen MakeSpace/BeginStateBig auf? | stdout: `{'D3D': 116}` referenzieren Device/PB, `{'D3D': 69}` schreiben PB, außerhalb 0 |
| `d3d_simple_sites.py` | Dekodiert die `ecx`-Werte (NV2A-Header) der 28 `SetRenderState_Simple`-Call-Sites über die D3D-Tabelle 0x293828. | stdout |
| `d3d_vsh_scan.py` | Sucht in .rdata/.data nach Xbox-Vertex-Shader-Headern (Low-Word 0x2078/0x7778/0x6878, 1..136 Instruktionen). | stdout: 2 Treffer, beide keine Shader |
| `d3d_mmio_scan.py` | Sucht NV2A-MMIO (0xFD000000–0xFDFFFFFF) außerhalb von D3D. | 2 Treffer in XPP, beide falsch-positiv: Timeout-Konstante 0xfd050f80 für `KeSetTimer` in 0x28d28c/0x28f416 |

Klassifikation der Aufrufer (in `d3d_inventory.py`):

| Gruppe | Definition | Beleg für die Grenzen |
|---|---|---|
| `NiXBoxRenderer` | benannte `NiXBoxRenderer::*` (Klassen-Recovery) | `retail_eu_ni_symbols.tsv`, vtable 0x2aa7c0 |
| `NiXBoxLib` | `FUN_`/`sub_` in 0x146020–0x14cd50 (Renderer-TU) und 0x155000–0x166871 (NiXBox-Hilfsklassen) | Strings `NiXBoxVertexBufferManager::…` (0x2ab058 ← 0x158440), `NiXBoxIndexBufferManager::…` (0x2ab7f8 ← 0x15cb42), `NiXBoxSourceTextureData::…` (0x2ab938 ← 0x15d920), `NiXBoxPaletteData::…`, `NiXBoxTextureManager::CreateTexture FAILED` (0x2abd50); die CRT beginnt bei 0x166871 (`_rand` 0x1668cc, `___CxxFrameHandler` 0x166977) |
| `NiOther` | NetImmerse-Kern 0xfa000–0x146020 und 0x14cd50–0x155000 | benannte Ni-Klassenmethoden |
| `Game` | .text 0x11000–0xf3000 (Skriptgrenze; tatsächlich reicht Spielcode bis ca. 0xf3de0, z. B. 0xf33e0 „HUD\\ErrorIcon.nif“) | XAPILIB ab 0xf3de0 (`OutputDebugStringA`). In 0xf3000–0xfa000 liegt kein D3D-Aufrufer, die ungenaue Grenze ändert keine Zahl. |
| `XMV`/`DSOUND`/`XPP` | Library-Sections | XBE-Section-Tabelle |

Hinweis: Die Bereichsgrenzen der NiXBox-Bibliothek sind aus Strings und Funktionsnamen abgeleitet (Vermutung für die exakten Kanten). Für die D3D-Aufrufer spielt das keine Rolle, weil alle in klar belegten NiXBox-Funktionen liegen.

## 2. Die D3D8-Bibliothek

### 2.1 Section, Version, Größe

* Bibliotheks-Tabelle der XBE (Header 0x160/0x164): `D3D8 1.0.5233`, `XGRAPHC 1.0.5233`, `XMV 1.0.5233`, `DSOUND`, `XAPILIB`, `LIBCMT`, `XBOXKRNL`.
* Section `D3D`: va 0x2342c0, vsz 0x12a38, raw 0xf5d0. Die BSS 0x243890–0x246cf8 enthält das Device-Objekt.
* 266 Funktionen mit 52 228 Byte Code. 128 tragen einen Namen von XbSymbolDatabase (`D3D8::…`).
* Konstante Tabellen von D3D liegen in `.rdata`: 0x293828 und 0x293608. Ghidra benennt 0x293608 fälschlich `MmFreeSystemMemory_exref`, weil es im Thunk-Bereich beginnt.

### 2.2 Zentrale Globals (aus der D3D-Disassembly abgeleitet)

| Adresse | Name (eigene Benennung) | Beleg |
|---|---|---|
| 0x244968 | `D3D__pDevice` (Zeiger auf das Device) | `D3DDevice_AddRef` 0x234440: `mov eax,[0x244968]; mov ecx,[eax+0x4fc]; inc ecx` |
| 0x244970 | `g_Device` (statisches CDevice-Objekt, 0x810 DWORDs). +0 `m_pPut`, +4 `m_pThreshold`, +8 Flags, +0x4fc RefCount | `Direct3D_CreateDevice` 0x2404a0: `mov edi,0x244970`, `D3D_g_pDevice=&DAT_00244970`, `*ppDevice=&DAT_00244970`, bei Fehler 0x810 DWORDs nullen |
| 0x246cac / 0x246ca8 | Push-Buffer-Größe / KickOff-Segmentgröße (Default 0x80000 / 0x8000) | `Direct3D_CreateDevice`, `D3D_SetPushBufferSize` 0x240480 |
| 0x2444c8 | `D3D__DirtyFlags` | `SetRenderStateNotInline` 0x235838: `or [0x2444c8],eax` |
| 0x2444c4 | `D3D__IndexData` (Data-Zeiger des aktuellen Indexbuffers) | `D3DDevice_SetIndices` 0x235190: `DAT_002444c4 = pIB[1]` bzw. 0 |
| 0x2444d0 | `D3D__TextureState[4][32]` | `SetTextureStageStateNotInline` 0x23625c: `mov [ecx*4+0x2444d0],eax` mit `ecx = stage<<5 + state` |
| 0x2446d0 | `D3D__RenderState[]` | `SetRenderStateNotInline` 0x23581d: `mov [esi*4+0x2446d0],edi` |
| 0x2469b8 | `D3D__Stream[]` (12 Byte je Stream; Anzahl nicht geprüft): +0 Stride, +8 Vertexbuffer-Zeiger | `D3DDevice_SetStreamSource` 0x237c90: `mov [esi+0x2469b8],eax` (Stride, 0x237cf2), `mov [esi+0x2469c0],edi` (VB, 0x237cf8), esi = Stream·12 |
| 0x293828 (.rdata) | NV2A-Methodenheader je „simple“ Renderstate 0..0x5b | `SetRenderStateNotInline` 0x23580a: `mov ecx,[esi*4+0x293828]; call 0x2357d0` |
| 0x293608 (.rdata) | Dirty-Flag je „deferred“ Renderstate (indiziert mit 0x5c..0x87, effektiv 0x293778..) | `SetRenderStateNotInline` 0x235831 |
| 0x2422d0 (D3D-Daten) | Funktionstabelle der „complex“ Renderstates 0x88..0xa5 (30 Einträge) | `call [esi*4+0x2420b0]` bei 0x235852 mit esi ≥ 0x88; Einträge zeigen auf `SetRenderState_PSTextureModes` (0x2422d0) … `DoNotCullUncompressed` (0x242344) |

Daraus ergibt sich die Renderstate-Aufteilung dieser D3D-Version:

* simple 0x00–0x5b: `SetRenderState_Simple` + Schattenkopie
* deferred 0x5c–0x87: nur Schattenkopie + Dirty-Flag
* complex 0x88–0xa5: eigene Funktion je State

Beleg ist der Code von `SetRenderStateNotInline` 0x235800 mit den Grenzen `cmp esi,0x5c` / `cmp esi,0x88`.

### 2.3 Push-Buffer-Mechanik

Muster (`D3DDevice_SetRenderState_Simple` 0x2357d0, `__fastcall`):

```
002357d0 a170492400     mov  eax,[0x244970]      ; m_pPut
002357d5 83c008         add  eax,8
002357d8 3b0574492400   cmp  eax,[0x244974]      ; m_pThreshold
002357de 730c           jae  0x2357ec            ; -> call D3DDevice_MakeSpace (0x2392b0), retry
002357e0 a370492400     mov  [0x244970],eax
002357e5 8948f8         mov  [eax-8],ecx         ; NV2A-Header (count<<18 | method), z.B. 0x40304
002357e8 8950fc         mov  [eax-4],edx         ; Wert
```

* `D3DDevice_MakeSpace` 0x2392b0 ruft `D3D_MakeRequestedSpace_8(kick>>1, kick)` auf, mit kick = `[0x246ca8]`. Das führt `D3D_BlockOnTime`, `D3D_SetFence` und `CDevice_KickOff` aus.
* `MakeSpace` hat 77 interne Call-Sites in **63** Funktionen (korrigiert im 2. Prüfdurchgang; der 1. Prüfdurchgang hatte 64 eingetragen). Die TSV-Spalte `int_callers` zeigt 64. Das ist ein Artefakt der Lückenaufteilung in `d3d_common.attribute()`:
  * Drei Sites (0x23b66d, 0x23b6f6, 0x2402a5) liegen hinter dem Ghidra-Ende zweier abgeschnittener Funktionen.
  * `D3DDevice_Clear`: Ghidra endet nach `call 0x23a5d0` bei 0x23b63f, der Rumpf läuft aber weiter (Sprung 0x23b460 → 0x23b70a).
  * `FUN_0023fdb0`: endet an der nicht aufgelösten Sprungtabelle `jmp [ecx*4+0x240328]` bei 0x23fe67.
  * `attribute()` zählt beide Reste als eigene Funktionen `sub_0023b644`/`sub_0023fe67`. Ordnet man jede Site der nächstvorhergehenden Ghidra-Funktion zu, ergeben sich 63 (`verify_d3d_r2_internal.py`).
* Insgesamt schreiben **mindestens** 69 D3D-Funktionen in den Push-Buffer (`d3d_pushbuffer_writers.py`), 116 berühren Device oder PB. Das Kriterium ist ein absoluter `m_pPut`-Operand oder ein Aufruf von MakeSpace/MakeRequestedSpace/BeginStateBig. Funktionen, die `m_pPut` nur über `[D3D__pDevice]` → `[reg]` schreiben und MakeSpace nie aufrufen, fallen heraus. Ein erweitertes Kriterium (Device-Zeiger geladen und NV2A-Methodenheader per `mov [reg],imm` geschrieben) findet 4 weitere: `D3D_BlockOnTime`, `FUN_0023dbc0` (z. B. `mov [eax],0xc0a10` bei 0x23dbe1), `FUN_0023dd60`, `FUN_00241c40`. Also sind es mindestens 73. Für das Ergebnis „außerhalb D3D: 0“ spielt das keine Rolle, weil auch `D3D__pDevice` außerhalb nie referenziert wird.
* **Außerhalb der D3D-Section: 0.** Auch ein Rohbyte-Scan aller Nicht-D3D-Sections nach den Immediates 0x244968/70/74/78 sowie 0x2392b0/0x239130 findet keinen Treffer.
* Zum Vergleich findet derselbe Scan für 0x2444c8 (`DirtyFlags`) 97 Stellen in .text.

### 2.4 XGRAPHC

* `XGRAPHC 1.0.5233` steht in der Bibliotheks-Tabelle, aber XbSymbolDatabase hat keine einzige `XG*`-Funktion erkannt. In `functions.tsv` gibt es nur `XAPILIB::XGet*`.
* NiXBox swizzelt Texturen selbst auf der CPU. `FUN_00155660` (thiscall) ist der Konstruktor der Header-Inline-Klasse `Swizzler` aus `xgraphics.h`: Felder w/h/d, dann Masken U/V/W durch Bit-Interleaving in einer Schleife, dann u/v/w = 0. Sie wird von den Textur-Kopierroutinen `FUN_00155d60`, `FUN_0015db00` (`NiXBoxSourceTextureData::CopyDataToSurface`) und `FUN_0015dda0` benutzt.
* `FUN_00158790` liefert Bits pro Pixel nach Xbox-`D3DFORMAT`. `FUN_00158990` ist ein `IsSwizzledFormat`-Äquivalent (Rückgabe `bool` in `al`). Beide sind cdecl (`ret` ohne N) und damit **nicht** die WINAPI-Funktionen `XGBytesPerPixelFromFormat`/`XGIsSwizzledFormat` (Vermutung: NiXBox-eigene Helfer).
* Ergebnis: Es wurde kein XGRAPHC-Funktionsaufruf gefunden. Für das HLE heißt das: Texturdaten im Gastspeicher liegen geswizzelt vor und müssen beim Upload entswizzelt werden.

## 3. Extern genutzte D3D-Funktionen (88)

Spalten: Adresse, `ret N` (Stack-Bereinigung = Argumentbytes, `0` = plain `ret`), externe Call-Sites/Aufruferfunktionen, Aufrufergruppen (R = NiXBoxRenderer-vfunc, X = NiXBox lib, G = Spiel, M = XMV). Die vollständige Liste aller 266 D3D-Funktionen inkl. interner Aufrufe steht in `d3d_functions.tsv`.

### 3.1 Device / Frame (20 Funktionen, 62 Sites)

| Adresse | Funktion | ret | Sites/Fkt | Gruppen |
|---|---|---|---|---|
| 0x2404a0 | `Direct3D_CreateDevice` | 0x18 | 1/1 | X (`FUN_0014c170`) |
| 0x240480 | `D3D_SetPushBufferSize` | 8 | 1/1 | X |
| 0x240390 | `FUN_00240390` (Vermutung `Direct3DCreate8`: `mov eax,1; ret 4`) | 4 | 1/1 | X |
| 0x2403a0 | `D3D_GetAdapterIdentifier` | 0xc | 1/1 | R vf11 |
| 0x2403d0 | `D3D_GetDeviceCaps` | 0xc | 1/1 | X |
| 0x240400 | `D3D_CheckDeviceFormat` | 0x18 | 1/1 | X |
| 0x234440 | `D3DDevice_AddRef` | 0 | 13/13 | X 12, G 1 |
| 0x234460 | `D3DDevice_Release` | 0 | 17/17 | X 16, G 1 |
| 0x240fc0 | `D3DDevice_Swap` | 4 | 1/1 | R vf16 |
| 0x23b380 | `D3DDevice_Clear` | 0x18 | 1/1 | R vf17 |
| 0x2342e0 | `D3DDevice_SetRenderTarget` | 8 | 2/2 | R vf15, X |
| 0x234ae0 | `D3DDevice_GetRenderTarget2` | 0 | 1/1 | X |
| 0x234b00 | `D3DDevice_GetDepthStencilSurface2` | 0 | 1/1 | X |
| 0x234b20 | `D3DDevice_SetViewport` | 4 | 2/2 | R vf17, vf47 |
| 0x2354f0 | `D3DDevice_SetScreenSpaceOffset` (2 floats) | 8 | 7/7 | G 7 |
| 0x234690 | `D3DDevice_GetDisplayMode` | 4 | 1/1 | M |
| 0x235250 | `D3DDevice_GetDisplayFieldStatus` | 4 | 1/1 | M |
| 0x2345a0 | `D3DDevice_InsertFence` | 0 | 3/2 | X |
| 0x2344b0 | `D3DDevice_BlockOnFence` | 4 | 3/3 | X |
| 0x238dd0 | `D3DDevice_IsFencePending` | 4 | 3/3 | X |

### 3.2 Ressourcen (20 Funktionen, 112 Sites)

| Adresse | Funktion | ret | Sites/Fkt | Gruppen |
|---|---|---|---|---|
| 0x2370d0 | `D3DDevice_CreateTexture2` (W,H,Depth,Levels,Usage,Format,Type → Objekt; Argumentnamen Vermutung, Anzahl aus `ret 0x1c`) | 0x1c | 6/6 | X 5, G 1 |
| 0x23a460 | `D3DDevice_CreateVertexBuffer2` | 4 | 4/4 | X |
| 0x23a420 | `D3DDevice_CreateIndexBuffer2` (LocalAlloc(len+0xc), Common=0x1010001, Data=hdr+0xc) | 4 | 3/3 | X |
| 0x23a540 | `D3DDevice_CreatePalette2` | 4 | 1/1 | X |
| 0x23cee0 | `D3D_CreateStandAloneSurface` | 0x10 | 3/3 | X |
| 0x2371a0 | `D3DTexture_GetSurfaceLevel2` | 8 | 3/3 | X 2, G 1 |
| 0x2371f0 | `D3DCubeTexture_GetCubeMapSurface2` | 0xc | 2/2 | X |
| 0x237190 | `Get2DSurfaceDesc` (Tail-jmp → 0x23aaa0) | – | 1/1 | X |
| 0x23cf80 | `D3DSurface_GetDesc` | 8 | 6/6 | R vf24, X 4, M 1 |
| 0x23cfa0 | `D3DSurface_LockRect` | 0x10 | 5/5 | R vf24, X 3, M 1 |
| 0x23a4b0 | `D3DVertexBuffer_Lock2` | 8 | 8/7 | R vf52/vf53, X 5 |
| 0x23a500 | `D3DVertexBuffer_GetDesc` | 8 | 4/4 | X |
| 0x23a520 | `FUN_0023a520` (Vermutung `D3DIndexBuffer_GetDesc`: schreibt Format=0x65, Type=7) | 8 | 3/3 | X |
| 0x23a5a0 | `D3DPalette_Lock2` | 8 | 1/1 | X |
| 0x2373b0 | `D3DResource_AddRef` | 4 | 6/5 | X 4, G 1 |
| 0x2373f0 | `D3DResource_Release` | 4 | 50/35 | X 32, G 3 |
| 0x2374d0 | `D3DResource_Register` (Data += Basis) | 8 | 2/2 | X (Textur-Cache) |
| 0x241790 | `D3D_AllocContiguousMemory` (→ `MmAllocateContiguousMemoryEx(size,0,0x7ffffff,align,0x404)`) | 8 | 2/2 | X (Speicherpool) |
| 0x2417b0 | `MmFreeContiguousMemory`-Thunk in der D3D-Section | – | 1/1 | X |
| 0x2347b0 | `D3DDevice_CopyRects` | 0x14 | 1/1 | R vf25 |

### 3.3 Feste Pipeline / Zustand (10 Funktionen, 55 Sites)

| Adresse | Funktion | ret | Sites/Fkt | Gruppen |
|---|---|---|---|---|
| 0x234300 | `D3DDevice_SetTransform` | 8 | 9/5 | R 2, X 3 |
| 0x234410 | `D3DDevice_SetMaterial` | 4 | 1/1 | X (Tail-jmp aus 0x161880) |
| 0x234c80 | `D3DDevice_SetLight` | 8 | 1/1 | X |
| 0x234e60 | `D3DDevice_LightEnable` | 8 | 4/3 | X |
| 0x234f50 | `D3DDevice_SetTexture` | 8 | 9/5 | X |
| 0x235100 | `D3DDevice_SetPalette` | 8 | 1/1 | X |
| 0x235190 | `D3DDevice_SetIndices` | 8 | 8/7 | R 3, X 4 |
| 0x237c90 | `D3DDevice_SetStreamSource` | 0xc | 7/6 | R 3, X 3 |
| 0x236030 | `D3DDevice_SetTextureState_TexCoordIndex` | 8 | 3/3 | X |
| 0x236140 | `D3DDevice_SetTextureState_BumpEnv` | 0xc | 12/2 | X |

### 3.4 Renderstates (31 Funktionen, 78 Sites)

* `D3DDevice_SetRenderState_Simple` 0x2357d0 (`__fastcall`): 28 Sites in 7 Funktionen.
* Die 30 „complex“ States 0x88–0xa5 liegen in der Tabelle 0x2422d0.
* Der NiXBox-Dispatcher `FUN_001478c0` ruft alle 30 complex-Setter auf. Die Xbox-spezifischen States erreicht **nur** er: `OcclusionCullEnable`, `StencilCullEnable`, `RopZCmpAlwaysRead`, `RopZRead`, `DoNotCullUncompressed`, `MultiSampleMode`, `MultiSampleRenderTargetMode`, `MultiSampleMask`, `SampleAlpha`, `Dxt1NoiseEnable`, `ShadowFunc`, `LogicOp`, `BackFillMode`, `TwoSidedLighting`, `FrontFace`, `PSTextureModes`, `LineWidth`, `EdgeAntiAlias` (dazu `ZBias`).
* Ausnahmen mit weiteren Aufrufern: `YuvEnable` ruft zusätzlich der Movie-Player 0xede90 auf (2 Sites). `MultiSampleAntiAlias` ruft zusätzlich 0x159410 auf (korrigiert, stand vorher in der „ausschließlich“-Liste).
* Weitere direkte Aufrufer: `VertexBlend` (7 Sites in 6 Funktionen: vf42, vf53, 0x1478c0, 0x147b70 (Tail-jmp), 0x149ae0, 0x14bc30), `CullMode` (3), `FillMode` (3), `ZEnable` (2), `StencilEnable`/`StencilFail` (0x1599e0), `FogColor` (0x159900), `NormalizeNormals` (0x159200), `TextureFactor` (0x162020), `MultiSampleAntiAlias` (0x159410).

### 3.5 Shader (5 Funktionen, 11 Sites) und Draw (2 Funktionen, 7 Sites)

| Adresse | Funktion | ret | Sites/Fkt | Gruppen |
|---|---|---|---|---|
| 0x237b50 | `D3DDevice_CreateVertexShader` | 0x10 | 1/1 | X `FUN_0015d4c0` |
| 0x238050 | `D3DDevice_SetVertexShader` | 4 | 4/4 | R vf52/vf53, X 2 |
| 0x241520 | `D3DDevice_CreatePixelShader` | 8 | 1/1 | X `FUN_00147740` |
| 0x241570 | `D3DDevice_DeletePixelShader` | 4 | 1/1 | X |
| 0x241590 | `D3DDevice_SetPixelShader` | 4 | 4/4 | R vf52/vf53, X 2 |
| 0x241160 | `D3DDevice_DrawVertices` | 0xc | 2/2 | R vf42, X 0x149ae0 |
| 0x241200 | `D3DDevice_DrawIndexedVertices` | 0xc | 5/5 | R vf42/vf52/vf53, X 0x149ae0/0x14bc30 |

### 3.6 Nicht extern genutzt (178 Funktionen)

* 176 D3D-Funktionen werden nur intern aufgerufen oder über Tabellen erreicht. Die übrigen 2 (0x23b790 und 0x23c1d0) haben weder Call noch Datenzeiger. Sie sind aber **nicht** unreferenziert (korrigiert): `CMiniport_InitHardware` übergibt sie als Immediate (`push 0x23c1d0` bei 0x23c898, `push 0x23b790` bei 0x23c926) an `KeInitializeDpc` bzw. `KeInitializeInterrupt`. 0x23b790 ist also die GPU-Interrupt-Service-Routine (ruft `KeInsertQueueDpc`) und 0x23c1d0 die zugehörige DPC-Routine.
* Darunter sind `D3DDevice_BlockUntilVerticalBlank` 0x2344c0 (1 interner Aufrufer 0x240690), `SetFlickerFilter`/`SetSoftDisplayFilter` (nur aus Device-Init 0x239d20 mit 5 bzw. 0), `Begin`/`End`/`SetVertexData2f` (nur aus 0x240b10), `GetBackBuffer2` (nur aus 0x241c40), `SetScissors` (nur aus `SetViewport`), `LoadVertexShader`/`SelectVertexShader`/`SetShaderConstantMode` (je 1 interner Aufrufer) sowie alle `CMiniport_*`-, Fence-, KickOff- und MakeSpace-Funktionen.
* `SetVertexShaderConstant*`, `SetVerticalBlankCallback`, `SetSwapCallback`, `BeginPush`/`EndPush` und `Present` sind nicht erkannt. Ihnen ist auch kein externer Aufruf zuzuordnen (Vermutung: nicht gelinkt).
* `FUN_00241c40` (2204 Byte) ist eine in D3D eingebaute Frame-Aufzeichnung in eine Cache-Datei. Belege: Strings „Unable to re-open movie cache file“ 0x243800, „1.00.5233.1“ 0x243834; Aufrufe von `NtCreateFile`/`NtWriteFile`. Sie läuft nur, wenn Device+0x498/0x49c gesetzt ist (`D3DDevice_Swap` 0x240fc7). Ein externer Setzer wurde nicht gefunden, die Funktion ist für das HLE irrelevant.

## 4. Aufrufer nach Art

| Gruppe | Funktionen mit D3D-Calls | zusätzlich nur Inline-State | D3D-Funktionen genutzt |
|---|---|---|---|
| `NiXBoxRenderer::vf*` | 11 | 0 | 17 |
| NiXBox lib (FUN_/sub_) | 117 | 4 (0x159700, 0x15bf80, 0x161d60, 0x161e40) | 80 |
| andere Ni*-Klassen | **0** | 0 | 0 |
| Spielcode | 12 | 0 | 8 |
| XMV | 3 | 0 | 4 |
| XAPI/CRT/DSOUND/XPP | **0** | 0 | 0 |
| **Summe** | **143** | 4 | **88** (Vereinigung) |

### 4.1 NiXBoxRenderer (vtable 0x2aa7c0, 75 Slots, Objektgröße 0x6d0)

* Slots 0–10 sind NiObject-Basis, 11–74 die Renderer-Schnittstelle (64 Slots).
* 49 verschiedene `NiXBoxRenderer::*`-Funktionen; die übrigen Slots sind geerbt oder zusammengefaltete Stubs (z. B. 0x13db0, 0x151dc0, 0xb0390).
* Die Methodennamen unten sind aus dem Verhalten abgeleitet (**Vermutung**):

| vfunc | Adresse | D3D-Aufrufe | Vermutete Rolle |
|---|---|---|---|
| vf11 | 0x146350 | `D3D_GetAdapterIdentifier` | Treiber-/Adapter-String („%s (%s-%s)“ 0x2aa790) |
| vf15 | 0x146400 | `SetRenderTarget` | Renderziel Backbuffer bzw. NiRenderedTexture. Ruft Spielfunktionen 0xa1930 (Screen-Offset setzen) bzw. 0xa1920 (Offset 0) auf, also eine Spiel-Modifikation im Renderer |
| vf16 | 0x146460 | `Swap(0)` | Frame anzeigen |
| vf17 | 0x146490 | `SetViewport`, `Clear` | Buffer löschen |
| vf24 | 0x1466b0 | `Surface_GetDesc`, `Surface_LockRect(…,0x80)` | Backbuffer → NiPixelData (Screenshot) |
| vf25 | 0x146800 | `CopyRects` | Blt (NiBltSource → Backbuffer) |
| vf42 | 0x14b700 | `SetStreamSource`, `SetIndices`, `DrawVertices`, `DrawIndexedVertices`, `RS_VertexBlend`; liest `D3D__IndexData` und die Tabelle 0x293998 | Allgemeine Geometrie: Primitivtyp aus den Geometriedaten (+0x2c), Schleife über Strip-Längen (`ushort`-Array +0x38, je Länge −2) → TriShape/TriStrips (Vermutung, korrigiert) |
| vf47 | 0x1469e0 | `SetTransform`, `SetViewport` | Kamera setzen |
| vf52 | 0x148ae0 | VB_Lock2, SetStreamSource, SetIndices, SetVertexShader, SetPixelShader(0), DrawIndexedVertices(5,…, `D3D__IndexData`) | Partikel/Billboards (Vermutung, korrigiert): Indexbuffer mit 4 Vertices je Element (`FUN_0015cbf0(n*4,…)` schreibt das Quad-Muster 0,1,2,0,2,3 direkt über `Data`). Der Vertexbuffer wird per CPU gefüllt, auf zwei Pfaden: FVF 0x142 mit Stride 0x18 (0x18 Floats = 4 Vertices je Element) und FVF 0x152 mit Stride 0x24 (0x24 Floats, zusätzlich Normale; ergänzt im 2. Prüfdurchgang). Draw mit 6 Indizes je Element (2 Dreiecke, Typ 5 = TRIANGLELIST) |
| vf53 | 0x149530 | wie vf52 + `RS_VertexBlend`; DrawIndexedVertices(2,…) | Linien (Vermutung): Typ 2 = LINELIST laut Tabelle 0x293998 (Eintrag 2 = (2,0), also 2 Vertices je Primitiv); Indexzahl `& ~1` |
| vf54 | 0x14b5f0 | `SetTransform` | Modell-Transform |

### 4.2 NiXBox-Bibliothek nach Teilsystem

Bereiche aus Strings abgeleitet; die Kanten sind Vermutung.

„Fkt.“ zählt Funktionen mit D3D-Call **oder** Inline-State (128 + 4 = 132; die erste Zeile enthält die 11 Renderer-vfuncs). Die Spalte Inline-Sites summiert sich auf 199. Die zweite Teilbibliothek beginnt praktisch erst bei 0x1552c0, denn `FUN_00153e90` (NiOther, 5134 Byte) reicht bis 0x1552bc.

| Bereich | Teilsystem (String-Beleg) | Fkt. | Calls | Inline-Sites |
|---|---|---|---|---|
| 0x146020–0x14cd50 | NiXBoxRenderer-TU (Create 0x14c170, D3D-Fehlerstrings 0x14cbb0, Renderstate-Dispatcher 0x1478c0, Pixelshader 0x147740) | 29 | 106 | 13 |
| 0x155000–0x156d00 | Textur-Cache („NiTCache.dat“ 0x2ab008 ← 0x156b40, „FAILED to find free space in TextureCache!“) – Fences, `D3DResource_Register` | 9 | 15 | 0 |
| 0x156d00–0x158700 | `NiXBoxVertexBufferManager` (0x2ab058 ← 0x158440) | 11 | 20 | 0 |
| 0x158700–0x15a100 | Caps („per-pixel z-based fog“ 0x2ab6cc ← 0x158d20), Renderstate-Helfer 0x1591c0–0x159cd0 | 13 | 41 | 58 |
| 0x15a100–0x15c400 | Texture-Pipeline („configurable texture pipeline manager“ ← 0x15b600), Lichter 0x15b990–0x15ba40 | 10 | 18 | 84 |
| 0x15c400–0x15ce00 | `NiXBoxIndexBufferManager` (0x2ab7f8 ← 0x15cb42/0x15cc06/0x15cd0a) | 6 | 15 | 0 |
| 0x15ce00–0x15d900 | Vertex-Shader-Cache (0x15d4c0, 0x15d740) | 2 | 2 | 0 |
| 0x15d900–0x160000 | `NiXBoxSourceTextureData`, `…RenderedTextureData`, `…RenderedCubeMapData`, `…SourceCubeMapData`, `NiXBoxPaletteData`, `NiXBoxBltSourceData`, `NiXBoxTextureManager` | 23 | 45 | 0 |
| 0x160000–0x161800 | Contiguous-Speicherpool („Dumping MEMORY ENTRIES“ ← 0x160d10) | 3 | 3 | 0 |
| 0x161800–0x162400 | Material / Texture-Stages (`SetMaterial`, `SetTexture`, TSS inline) | 6 | 14 | 44 |
| 0x162400–0x166871 | Shader-Set 0x163cd0/0x163d10, Geometrie-Puffer 0x165220–0x1667d0 | 20 | 25 | 0 |

### 4.3 Spielcode (12 Funktionen)

| Funktion | D3D | Zweck |
|---|---|---|
| 0xa1920, 0xa1930, 0xa1950, 0xa19d0, 0xa1a00, 0xa1a80 (+0xa1ae0) | `SetScreenSpaceOffset` | Menüoption „Bildschirm justieren“ (Strings `text_adjustscreen` 0x294710 ← 0x25970, `main_options_adjustscreen` 0x294e70 ← 0x279c0). Der Offset ist auf ±12,0 geklemmt (0xa1950/0xa1a00) und gespeichert in 0x2e7900/0x2e7914. Die Schrittfunktionen 0xa1aa0/0xa1ab0 (→ 0xa1950 mit −2,0/+2,0) und 0xa1ac0/0xa1ad0 (→ 0xa1a00) sowie 0xa19d0 werden aus Code bei 0x28c1c–0x29511 gerufen. Dieser Bereich 0x28960–0x29880 (3,8 KB) ist in Ghidra keine Funktion (Roh-Scan E8, ergänzt im 2. Prüfdurchgang). |
| 0xede40 | `D3DDevice_AddRef` | Liest den D3D-Device-Zeiger **aus dem NiXBoxRenderer-Objekt** (`*(renderer+0x60)`, Renderer = `*(DAT_002eb310+0x104)`) |
| 0xede90 | `GetSurfaceLevel2`, `Resource_Release`, `RS_YuvEnable(1/0)` | Movie-Frame holen (`FUN_00264962`, XMV) und YUV-Polygon zeichnen |
| 0xee080, 0xee190 | `Resource_Release`, `Device_Release` | Movie schließen |
| 0xee220 | `CreateTexture2(w,h,1,1,0,0x24,3)`, `Resource_AddRef/Release` | Movie öffnen („Video\\%s.xmv“ 0x2a5748). Die YUY2-Textur (Format 0x24, Vermutung `D3DFMT_YUY2`) wird **direkt in die NiXBox-Renderer-Daten** einer NiTexture geschrieben (`+0x20` → RendererData (Vermutung), dort `+0x3c` = D3DTexture*, +0x48/+0x4c W/H, +0x58 = W·H·2, passend zu 2 Byte/Pixel YUY2). Freigabe über die Renderer-vfunc `(*(renderer+0x5cc))->vf[7]`, wenn `DAT_0030edd4` gesetzt ist. |

### 4.4 XMV (3 Funktionen)

| Funktion | D3D | Vermutete XMV-API |
|---|---|---|
| 0x264360 | `GetDisplayMode` | `XMVDecoder_CreateDecoderForPackets` (Aufruf aus 0xee220 mit zwei Callbacks 0xedba0/0xede60) – Vermutung |
| 0x264962 | `GetDisplayFieldStatus` | `XMVDecoder_GetNextFrame(dec, surface, &status, 0)` – Vermutung |
| 0x265b0b | `Surface_GetDesc`, `Surface_LockRect(…,0,0x40)` | Frame-Kopie in die YUY2-Surface |

Die Disc enthält 47 `.xmv`-Dateien unter `/video/` (`orig\retail_eu\disc_files.txt`, z. B. `1-1-01-PAL.xmv` mit 55 MB). Movie-Wiedergabe ist also ein Hauptpfad und kein Randfall.

## 5. Inline-Zugriffe außerhalb der D3D-Section (der „schwierige Teil“)

### 5.1 Echte Push-Buffer-Schreiber: keine

| Prüfung | Ergebnis |
|---|---|
| Operanden/Immediates `0x244968` (`D3D__pDevice`), `0x244970` (`m_pPut`), `0x244974` (`m_pThreshold`), `0x244978` | 0 außerhalb D3D (Capstone + Rohbytes) |
| `call`/`jmp` auf `D3DDevice_MakeSpace` 0x2392b0, `D3D_MakeRequestedSpace_8` 0x239130, `D3DDevice_BeginStateBig` 0x2392c0 | 0 außerhalb D3D (intern: 63 Funktionen/77 Sites bzw. 2/2 bzw. 4/5) |
| NV2A-MMIO 0xFD000000–0xFDFFFFFF | 0 (2 falsch-positive `KeSetTimer`-Timeouts in XPP) |
| Zeiger auf D3D-Code in .rdata/.data | 1: 0x2d3e6c → 0x235770 (D3D-Static-Initializer in der C++-Init-Tabelle; er kopiert nur die Float-Konstanten ±1e8 aus 0x2421b4..0x2421c0 nach .data 0x2d75f4..0x2d7600, die sonst niemand referenziert) |
| Immediates in D3D-Code außerhalb von Calls | 1 falsch-positiv: `NiAVObject::vf03` 0x102445 `mov [eax],0x235623` (0x235623 ist kein Befehlsanfang, Konstante in einem neu allozierten 16-Byte-Objekt) |
| Zugriff auf das Device über den in `renderer+0x60` gespeicherten Zeiger | 0. Heuristik `verify_d3d_devptr.py`: `mov R,[X+0x60]` mit anschließender Dereferenzierung von R in NiXBox- und Movie-Code; einziger Treffer ist ein Stack-Parameter (0x14a3f6) |

### 5.2 Geinlinte Lazy-State-Makros (22 Funktionen, alle NiXBox lib)

| Ziel | Schreibend | Lesend |
|---|---|---|
| `D3D__DirtyFlags` 0x2444c8 | 72 | 25 |
| `D3D__RenderState` 0x2446d0 | 63 | 0 |
| `D3D__TextureState` 0x2444d0 | 33 | 1 (kein Lesezugriff, sondern Adressnahme `mov [esp+0x14],0x2444d0` bei 0x15ae30; danach greift 0x15ab40 über ein Register zu, diese Zugriffe sind hier nicht gezählt) |
| `D3D__IndexData` 0x2444c4 | 0 | 5 |
| **Summe** | | **199 Instruktionen** (72+25+63+33+1+5; korrigiert, vorher 198) |

Zusätzlich gibt es konstante D3D-Tabellen, die NiXBox inline liest (`verify_d3d_inline_tables.py`):

| Tabelle (.rdata) | Leser | Instruktionen |
|---|---|---|
| 0x293828 simple-Renderstate → NV2A-Header | 0x1478c0 (0x1478fb) | 1 |
| 0x293608 Dirty-Flag je deferred State (effektiv ab 0x293778) | 0x1478c0 (0x14791d) | 1 |
| 0x293998 Primitivtyp → (Faktor, Offset) für die Vertexzahl: Werte 0..10 = (0,0),(1,0),(2,0),(1,1),(1,1),(3,0),(1,2),(1,2),(4,0),(2,2),(0,0) | vf42, 0x149ae0, 0x14bc30 | 10 |

Geschriebene Renderstate-Indizes:

* simple: 0x39–0x42, 0x44–0x48. Sie werden zusätzlich über `SetRenderState_Simple` gesetzt.
* deferred: 0x5c–0x63, 0x66, 0x67, 0x6f, 0x70, 0x71, 0x73.
* variabel indiziert in `FUN_001478c0` (`mov [esi*4+0x2446d0],edi`).

Texture-State-Indizes: 0, 1, 3, 4, 5, 12, 14, 15, 16, 18, 19, 21 (Stage über Register).

| Funktion | Inline-Zugriffe | weitere D3D-Calls |
|---|---|---|
| 0x1478c0 (Renderstate-Dispatcher; nutzt 0x293828 bei 0x1478fb und 0x293608 bei 0x14791d, Instruktionsadressen; die alten Angaben 0x1478fe/0x147920 waren die Positionen der Displacement-Bytes) | RS w2, Dirty r1/w1 | `SetRenderState_Simple` (ecx variabel) + alle 30 complex `SetRenderState_*` |
| 0x148ae0 vf52, 0x149530 vf53, 0x14b700 vf42, 0x14bc30 | `D3D__IndexData` lesen | Draw-Pfade |
| 0x149ae0 | RS w2, Dirty w2, IndexData r1 | Draw-Pfad |
| 0x159270, 0x159410, 0x159700, 0x159980, 0x1599e0, 0x159b60, 0x159cd0 | RS w 1–13, Dirty bis w10/r3 | `SetRenderState_Simple` 27 Sites, `CullMode`, `FillMode`, `StencilEnable`, `StencilFail`, `MultiSampleAntiAlias` |
| 0x15ab40 | 52 Zugriffe (TSS w18, Dirty w26/r5, RS w2) | `SetTexture`, `TSS_BumpEnv`, `TSS_TexCoordIndex` |
| 0x15ba40, 0x15bcb0, 0x15bf80 | RS/Dirty (Licht-/Fog-States) | `SetLight`, `LightEnable`, `AddRef` |
| 0x161cd0, 0x161d60, 0x161e40, 0x161f70, 0x162020 | TSS + Dirty | `SetTexture`, `SetPalette`, `TSS_*`, `SetTransform`, `RS_TextureFactor` |

`SetRenderState_Simple`-Sites (`d3d_simple_sites.py`):

* **27** der 28 Sites übergeben eine konstante NV2A-Methode in `ecx` (korrigiert, vorher 26): 0x40300, 0x40304, 0x40310, 0x4033c, 0x40340, 0x40344, 0x40348, 0x40354, 0x4035c, 0x40364, **0x40368**, 0x4036c, 0x40374, 0x40378, 0x4037c (15 verschiedene).
* Die Rückabbildung auf den State-Index läuft über die D3D-Tabelle 0x293828. Beispiele: 0x40304 → State 0x3b, 0x40300 → 0x3c, 0x4037c → 0x42, 0x40368 → 0x47. Die konstanten Sites decken genau die States 0x39–0x42 und 0x44–0x48 ab.
* Nur **1** Site übergibt `ecx` variabel: 0x147904 (aus der Tabelle). Die Site 0x159a57 ist konstant (`mov ecx,0x40368` bei 0x159a4c, State 0x47). `d3d_simple_sites.py` dekodiert rückwärts ab 40 Byte vor dem Call und verliert dort die Synchronisation; `verify_d3d_simple.py` dekodiert vorwärts ab Funktionsanfang.

**Folge für ein D3D-HLE:**

* Diese Stellen bleiben unverändert. Die vier Globals liegen in der D3D-BSS, die der Loader ohnehin abbildet.
* Das HLE von `DrawVertices`/`DrawIndexedVertices` liest `D3D__RenderState[0..0xa5]`, `D3D__TextureState[4][32]` und `D3D__DirtyFlags` und übersetzt sie, wie es `CDevice_SetStateVB` 0x23e460 und die Lazy-Funktionen der D3D tun.
* `SetRenderState_Simple` wird über die inverse Tabelle 0x293828 dekodiert. Alternativ ignoriert man den Methodenwert und liest die Schattenkopie, die der Inline-Code direkt danach schreibt.

## 6. Device-Erzeugung, Present-Parameter, Präsentation

### 6.1 Aufrufkette

* `FUN_000d26a0` (Spiel-Init) ruft `FUN_0014c170(0x280, 0x1e0, 2, 1, 0x14000002, 0x4b, 0, 1)` auf.
* `FUN_0014c170` ist **NiXBoxRenderer::Create** (Vermutung für den Namen; alloziert 0x6d0 = NiXBoxRenderer-Größe aus `retail_eu_ni_classes.json`). Er ruft auf:
  * `FUN_00240390(0)` (Vermutung `Direct3DCreate8`)
  * `D3D_SetPushBufferSize(0x200000, 0x8000)` (Werte aus .data 0x2d7cec/0x2d7cf0)
  * `Direct3D_CreateDevice(0, 1, 0, BehaviorFlags, &pp, &renderer->m_pDevice /*+0x60*/)`
  * Gleich danach `FUN_0014aab0` (Render-Target/Depth abfragen), `FUN_001474f0`, `FUN_00147550` (Caps), `FUN_0014c0f0`, `FUN_00147740` (Pixelshader).
* `Direct3D_CreateDevice` 0x2404a0 ist stdcall mit 6 Argumenten (`ret 0x18`). Von den BehaviorFlags übernimmt es nur `& 0x10` (`and eax,0x10` bei 0x2404d7). Danach ruft es `FUN_00239d20` auf (CDevice-Init: Kontext-DMA-Objekte, `CDevice_InitializeFrameBuffers`, Video-Modus `FUN_0023b118`, `SetFlickerFilter(5)`, `SetSoftDisplayFilter(0)`).

### 6.2 D3DPRESENT_PARAMETERS

Das Layout ist doppelt belegt:

* Schreiber: `FUN_0014c170` füllt `renderer+0x27c`, 0x11 DWORDs.
* Leser: `CDevice_InitializeFrameBuffers` 0x2397c0 liest `pp[0..5,8,9,0xc,0xd..0x10]`, und `FUN_0023b118(pp[0], pp[1], pp[0xb], pp[10], pp[2], pp[0xc], …)` liest den Rest.

| Idx | Feld | Wert im Spiel | Beleg / Bedeutung |
|---|---|---|---|
| 0 | BackBufferWidth | 640 | Arg 1 |
| 1 | BackBufferHeight | 480 | Arg 2 |
| 2 | BackBufferFormat | 0x1e | Arg 3 = 2 → switch; 0x1e = Xbox `D3DFMT_LIN_X8R8G8B8` (Vermutung zum Namen; linear, 32 bpp) |
| 3 | BackBufferCount | 1 | Arg 4 |
| 4 | MultiSampleType | 0x11 | `FUN_00146180(0x14000002)`: (flags&0xf00)=0 → 0x11 (keine Kantenglättung, Vermutung `D3DMULTISAMPLE_NONE`) |
| 5 | SwapEffect | 1 | `FUN_00146260`: (flags&0xf0)=0 → 1 (Vermutung `DISCARD`) |
| 6/7 | hDeviceWindow / Windowed | 0 / 0 | |
| 8 | EnableAutoDepthStencil | 1 | (flags>>1)&1 |
| 9 | AutoDepthStencilFormat | 0x2a | Arg 6 = 0x4b ≠ 0x46/0x50 → 0x2a (Vermutung Xbox `D3DFMT_D24S8`; 0x46/0x50 würden 0x2c = D16 ergeben) |
| 10 | Flags | 1 | konstant gesetzt. Kein Bit 0x10/0x20/0x40/0x80 → kein Widescreen-, Interlace- oder Progressive-Wunsch (diese Bits prüft 0x23b118). Bit 0 ist Vermutung `LOCKABLE_BACKBUFFER`, passt zu `LockRect` auf den Backbuffer in vf24. |
| 11 | FullScreen_RefreshRateInHz | 0 | Arg 7 = 0 → 0 (Default) |
| 12 | PresentationInterval | 1 | Arg 8 = 1 → 1. Mapping 0→0x80000000, 1→1, 2→2, 3→4, 4→8 entspricht `D3DPRESENT_INTERVAL_IMMEDIATE/ONE/TWO/THREE/FOUR`, also VSync. |
| 13–16 | BufferSurfaces[3], DepthStencilSurface | 0 | genullt |

BehaviorFlags: 0x14000002 & 0x4000000 → 0x10, ohne 0x20000000/0x40000000 → |0x40, ergibt **0x50**.

50/60 Hz:

* Refresh 0 bedeutet, dass `FUN_0023b118` keinen der beiden Zweige `param_4 == 0x32` / `== 0x3c` nimmt.
* Er nimmt den ersten passenden Eintrag (640×480) der Modustabelle 0x242910 (0xb9 Einträge à 12 Byte). Erlaubt sind dabei die Bits `caps & 0xC00000`, mit `caps` aus `AvSendTVEncoderOption(0, 6, 0, &caps)` (`CMiniport_GetDisplayCapabilities` 0x23b080).
* Das Spiel erzwingt also weder 50 noch 60 Hz. Die Konsole (AV-Pack, Videonorm, PAL60-Einstellung) entscheidet (Vermutung zur genauen Bitbedeutung).
* Bitbedeutung aus dem Code von 0x23b118 (ergänzt im 2. Prüfdurchgang):
  * Refresh 0x3c (60) liefert die Klasse 0x800000 nur mit `Flags & 0x200` und `caps & 0x800000`. Dann startet ein Timer mit Periode 16 683 350 (0x23b1b6), sonst gilt die Klasse 0x400000 (0x23b229).
  * Refresh 0x32 (50) liefert die Klasse 0x400000 nur mit `Flags & 0x200` und `caps & 0x400000`. Dann startet ein Timer mit Periode 20 000 000 (0x23b248), sonst gilt die Klasse 0x800000 (0x23b1e0).
  * Das Periodenverhältnis 20 000 000 : 16 683 350 entspricht 59,94 Hz : 50 Hz. Also ist **0x400000 die 60-Hz- und 0x800000 die 50-Hz-Modusklasse**, und `Flags & 0x200` emuliert die jeweils andere Rate per Timer (Vermutung für die Interpretation, die Werte sind belegt).
  * In der Modustabelle (`verify_d3d_r2_modes.py`) gibt es Einträge mit 0x800000 nur in der Gruppe `(flags>>8)&0xff == 3` (Vermutung: PAL). Dort stehen die 640×480-Einträge der 50-Hz-Klasse (#94/#95) vor denen der 60-Hz-Klasse (#112/#113).
  * Meldet eine PAL-Konsole beide Bits, wählt Refresh 0 also vermutlich 50 Hz. 60 Hz gibt es nur, wenn `caps` 0x800000 nicht enthält (Vermutung, nicht dynamisch geprüft).
* Unabhängig davon wählt das Spiel PAL- oder NTSC-Assets über die Dateien `sysPAL.img`/`sysNTSC.img` und `FutPAL%02d.img`/`FutNTSC%02d.img` (0xd26a0, Flag `+0xe8`).
* XMV fragt `GetDisplayMode` und `GetDisplayFieldStatus` ab, vermutlich für das Video-Timing.

### 6.3 Frame-Präsentation

* `NiXBoxRenderer::vf16` → `D3DDevice_Swap(0)`. Swap ersetzt 0 durch 5 (`test ebx,ebx; jne; mov ebx,5` bei 0x240fe5–0x240fea).
* Der Movie-Player (0xee080) holt ein Objekt über die vfunc `+0x14` von `DAT_002eb310` (Vermutung: den Renderer) und ruft darauf `+0x6c` (vf27, Hintergrundfarbe (0,0,0)) und `+0x40` (vf16 = Swap) auf. Dieselbe vf27 mit (0,0,0) ruft die Spiel-Init 0xd26a0 direkt auf dem Renderer auf (Vermutung zur Bedeutung).
* Kein externer Aufruf von `BlockUntilVerticalBlank`. `SetVerticalBlankCallback`/`SetSwapCallback` hat XbSymbolDatabase nicht erkannt. Da alle 88 extern aufgerufenen D3D-Funktionen identifiziert sind (die 3 unbenannten sind etwas anderes), ruft das Spiel sie nicht auf. Synchronisiert wird nur über PresentationInterval ONE.

## 7. Ressourcen

* **Texturen:**
  * `CreateTexture2` (7 Argumente, `ret 0x1c`, liefert das Objekt): 5 NiXBox-Sites (`SourceTextureData::CreateSurf` 0x15d920, `RenderedTextureData` 0x15e370, `RenderedCubeMap` 0x15e840, `SourceCubeMap` 0x15ecf0, `TextureManager` sub_0015f8f0) plus Movie (YUY2).
  * CPU-Upload über `GetSurfaceLevel2`/`GetCubeMapSurface2` + `LockRect`, Swizzling in NiXBox (`Swizzler` 0x155660).
  * Paletten: `CreatePalette2` / `Palette_Lock2` / `SetPalette` (P8-Texturen).
  * Zusätzlich gibt es einen Textur-Cache mit `D3DResource_Register` (Data-Relokation, 0x1559aa/0x15631c) und Contiguous-Speicher aus `D3D_AllocContiguousMemory`. Für diese Texturen gibt es **keinen** `CreateTexture2`-Aufruf (Vermutung). Ein HLE muss deshalb Xbox-Resource-Header (Common/Data/Format/Size) beim `SetTexture` selbst parsen.
* **Vertexbuffer:** `CreateVertexBuffer2` (4 Sites) und `Lock2` (8). Dynamische Ringpuffer werden mit `InsertFence`/`IsFencePending`/`BlockOnFence` synchronisiert (`NiXBoxVertexBufferManager::CreateDynamicVertexBuffer` 0x158440).
* **Indexbuffer:** `CreateIndexBuffer2` legt Header und Daten mit `LocalAlloc` (XAPI 0xf63e0) im normalen Speicher an (0x23a420). NiXBox schreibt Indizes direkt über `Data`. `DrawIndexedVertices` bekommt den Indexzeiger aus `D3D__IndexData`, Indizes werden also immer aus dem Gastspeicher gelesen.
* **Adress-Semantik von `Data` (ergänzt bei der Verifikation):**
  * `D3DResource_Register` 0x2374d0 addiert die Basis und maskiert danach mit `and eax,0x0fffffff`. Ausgenommen ist nur der Typ `(Common & 0x70000) == 0x20000`.
  * `D3DVertexBuffer_Lock2` gibt `Data | 0x80000000` zurück (`or eax,0x80000000` bei 0x23a4f2).
  * Contiguous-Speicher kommt aus `MmAllocateContiguousMemoryEx(size, 0, 0x7ffffff, align, 0x404)` (0x241790, Thunk 0x293640 = Ordinal 166).
  * Für Contiguous-Ressourcen enthält `Data` also eine physische Adresse (< 128 MB), und die CPU greift über das Fenster 0x80000000+phys zu (Vermutung, gestützt auf diese drei Stellen). NiXBox schreibt Resource-Header teils selbst: Im Textur-Cache setzt sub_001558e0 `Data`/`Lock` und ruft danach `Register` auf (0x155990–0x1559aa).
  * Ein HLE braucht deshalb eine Abbildung „physische Gastadresse → Host-Speicher“ für alle Contiguous-Allokationen. Alternativ legt man den Contiguous-Pool im Host an einer festen VA ab, sodass `0x80000000|phys` gültig ist. Für einen 32-Bit-Prozess setzt das LARGEADDRESSAWARE unter WoW64 voraus (Vermutung zur Machbarkeit).
* **Render-Targets:** `SetRenderTarget`, `GetRenderTarget2`, `GetDepthStencilSurface2`, `CreateStandAloneSurface` (3 Sites), `CopyRects`.

## 8. Shader und States

* **Vertex-Shader:**
  * Genau 1 `CreateVertexShader`-Site (0x15d4c0, aus einer Tabelle Deklaration/Funktion je Index, 0x15d6a0).
  * Es gibt keinen externen Aufruf irgendeiner `SetVertexShaderConstant`-Variante; `LoadVertexShader`/`SelectVertexShader`/`SetShaderConstantMode` laufen nur intern.
  * Ein Header-Scan nach NV2A-Vertex-Programm-Tokens (`(n<<16)|0x2078/0x7778/0x6878`, `d3d_vsh_scan.py`) in .rdata/.data fand nur 2 falsche Treffer (EH-FuncInfo 0x2ae068 mit Magic 0x19930520 dahinter, 0x2b7798). Auf der Disc liegen keine Shader-Dateien (`disc_files.txt`).
  * `SetVertexShader` wird u. a. mit dem konstanten Handle/FVF 2 aufgerufen (`FUN_0014aca0`).
  * **Vermutung:** nur Fixed-Function-T&L mit Stream-Deklarationen, keine programmierbaren Vertex-Shader.
* **Pixel-Shader:**
  * 1 Register-Combiner-Programm, erzeugt bei der Device-Erzeugung in `FUN_00147740` (D3DPIXELSHADERDEF 60 DWORDs auf dem Stack).
  * Werte: Idx 53 (CombinerCount) = 0x11102, Idx 54 (PSTextureModes) = 0xe1, Idx 57/58 (C0/C1-Mapping) = 0xffffffff, Idx 59 = 0x1ff, AlphaInputs[0..1] = 0xd9301010/0xdcd81010, RGBInputs[0..1] = 0xc9200000/0xccd80000, Outputs = 0xc0. Die Feldzuordnung ist Vermutung nach XDK-Layout; sie ist konsistent mit den typischen Werten 0xffffffff/0x1ff.
  * Sonst wird `SetPixelShader(0)` benutzt (vf52/vf53): Die D3D übersetzt die Texture-Stage-States selbst in Combiner.
* **Renderstates:**
  * Der NiXBox-Dispatcher 0x1478c0 kann jeden der 166 Indizes (0..0xa5) setzen; er hält einen eigenen Cache bei `renderer+0x22c` (Wert/Vorwert je State, 8 Byte).
  * Xbox-spezifische States siehe Abschnitt 3.4.
  * `YuvEnable` nutzt das Spiel für Movies.
* **Texture-Stage-States:** `TSS_TexCoordIndex` (3 Sites), `TSS_BumpEnv` (12 Sites), Rest inline (Abschnitt 5.2).
* **Fixed-Function:** `SetTransform` (9 Sites), `SetLight`/`LightEnable` (5), `SetMaterial` (1).

## 9. Nicht-Renderer-Nutzer von D3D

| Nutzer | Direkter D3D-Zugriff? | Beleg |
|---|---|---|
| Movie-Wiedergabe (XMV, 47 Videos) | **ja**: Spielcode 0xede40–0xee220 (8 D3D-Funktionen) + XMV (4) | Abschnitt 4.3/4.4 |
| Bildschirm justieren | **ja**: 7 Spielfunktionen → `SetScreenSpaceOffset`; Aufruf auch aus `NiXBoxRenderer::vf15` | 0xa1920–0xa1ae0 |
| Fonts / Text | nein (über Ni) | `fonts/*.fnt` geladen in 0x985a0/0x99c10; keine D3D-Referenz in diesem Code |
| Ladebildschirm | nein (über Ni) | „LoadingScreen“ 0x2a57f0 ← 0xefb60, „BaseScreen“ ← 0xef680; Ladeseiten sind NIFs (`images\\%s\\Loadmenu.nif`) |
| HUD / 2D / Menüs / Splash / Legal | nein (über Ni: `NiScreenPolygon`/`NiScreenTexture`) | `menu\\frontend.nif`, `HUD\\ErrorIcon.nif`, `NI_Splash_PAL.nif`, `legal.nif` |
| Spielstand-Icons | nein (Datei-Kopie über XAPI) | „d:\\SaveGameIcons\\saveimage.xbx“ ← 0xe0420; `TitleImage.xbx`/`SaveImage.xbx` in XAPILIB 0xf8c41 |
| Screenshot | nur über `NiXBoxRenderer::vf24` (Backbuffer-`LockRect`) | Aufrufer im Spiel nicht eindeutig gefunden (indirekter vtable-Aufruf) |
| Andere Ni*-Klassen | nein | 0 Aufrufer außerhalb NiXBox |

## 10. Empfehlung zur HLE-Grenze

### 10.1 Option (a): Xbox-D3D8-API-Ebene (Cxbx-artig)

* **Umfang:** 88 Einsprungpunkte (je ein Entry-Hook; Konventionen aus `ret N` in `d3d_functions.tsv`, `SetRenderState_Simple` ist fastcall). Die Arbeit ist sehr ungleich verteilt:
  * 31 Renderstate-Setter sind trivial.
  * 20 Ressourcen-Funktionen, davon etwa 8 nicht trivial.
  * 20 Device/Frame-Funktionen, davon etwa 6 nicht trivial.
  * 10 Zustandsfunktionen, 5 Shader-, 2 Draw-Funktionen.
* **Inline:** 0 Push-Buffer-Stellen. 199 Lazy-State-Instruktionen in 22 Funktionen bleiben ungepatcht; nötig ist ein Lazy-State-Reader für 4 Globals im Draw-HLE. Die 12 Inline-Lesezugriffe auf konstante D3D-Tabellen (0x293828, 0x293608, 0x293998) brauchen nichts.
* **Vorteile:**
  * Kleine, scharf abgegrenzte Fläche.
  * NiXBox, Spiel- und XMV-Code laufen bitgenau unverändert. Das ist wichtig für frühe Spielbarkeit und als Referenz für spätere Decompilate.
  * Alle Draws laufen über 7 Sites.
  * Die Lock-Aufrufe sind zählbar: `LockRect` 5, `GetSurfaceLevel2` 3, `Palette_Lock2` 1, `VB_Lock2` 8, `Register` 2. **Ein explizites Upload-Signal gibt es aber nicht** (korrigiert im 2. Prüfdurchgang). Die Xbox-D3D kennt kein `Unlock`; `functions.tsv` enthält keine `*Unlock*`-Funktion in D3D. Die CPU schreibt nach `Lock2`/`LockRect` einfach in den Speicher, und NiXBox schreibt Indexdaten ganz ohne Lock direkt über `Data` (0x15cc96 in `FUN_0015cbf0`). Das HLE muss Daten deshalb beim Gebrauch übernehmen (`SetTexture`/`SetStreamSource`/Draw) oder Schreibzugriffe verfolgen, z. B. per Hash oder Seitenschutz.
* **Schwierigkeiten:**
  * Fixed-Function-T&L und Texture-Stages müssen nach Vulkan/D3D12 übersetzt werden (Ubershader).
  * Xbox-Formate: geswizzelt, P8, DXT, YUY2, linear; D24S8.
  * Das eine Combiner-Programm.
  * Resource-Header-Parsing (Textur-Cache) und die Übersetzung physischer `Data`-Adressen (siehe Abschnitt 7).
  * Die von `Create*2`/`Get*2` gelieferten Objekte müssen echte Xbox-Resource-Strukturen im Gastspeicher bleiben (Common/Data/Lock/Format/Size), weil Gastcode sie direkt liest und schreibt. Beispiele: `*(IB+4)` als Schreibziel für Indizes (0x15cc96); der Movie-Code legt das `D3DTexture*` in NiXBox-Daten `+0x3c` ab (0xee220); der Textur-Cache setzt `Data` selbst (sub_001558e0). Opake Host-Handles reichen also nicht (ergänzt im 2. Prüfdurchgang).
  * Das HLE von `Direct3D_CreateDevice` muss `D3D__RenderState`/`D3D__TextureState` mit den D3D-Defaults vorbelegen. NiXBox schreibt inline nur Teilmengen, und der Dispatcher 0x1478c0 überspringt Werte, die seinem eigenen Cache (`renderer+0x22c`) gleichen. Im Original erledigt das vermutlich die Device-Init 0x239d20 über `FUN_002395c0` → `SetRenderStateNotInline`; im Detail nicht geprüft (ergänzt im 2. Prüfdurchgang).
  * Fences.
  * Render-to-Texture, Cubemaps.
  * Backbuffer-Lock.

### 10.2 Option (b): NiRenderer-Ebene + kleiner D3D-Shim

* **Umfang:** Die Renderer-Schnittstelle neu implementieren: 64 vtable-Slots (11–74), davon 47 verschiedene echte Implementierungen. Funktional ersetzt wird damit die gesamte NiXBox-Bibliothek: 423 definierte Funktionen (+11 nur per vtable erreichbare, 82 190 Byte) mit TriShape/Strips/Partikel/Linien, Skinning (`VertexBlend`), Mehrfachtexturen/TSS, Licht, Fog, Stencil, Rendered Textures und Cubemaps, Paletten, Textur-Cache, VB/IB-Manager.
* **Zusätzlich:**
  * D3D-Shim für 12 Funktionen (Spiel 8 + XMV 4).
  * Die 5 Movie-Funktionen müssen neu geschrieben werden. Sie greifen auf NiXBox-Interna zu: `renderer+0x60` (Device), `renderer+0x5cc` (Texturmanager-vfunc 7), NiTexture-RendererData `+0x3c`/`+0x48`/`+0x4c`/`+0x58`. Außerdem ruft `FUN_000ee220` die NiXBox-Funktion `FUN_0014cbb0` (D3D-Fehlertext) direkt auf, und `FUN_000ede90` zeichnet über `FUN_00100c50` (Ni-Code) zwischen `YuvEnable(1)` und `YuvEnable(0)`.
  * Die Spiel-Modifikation in vf15 (Screen-Offset) muss nachgebaut werden.
* **Vorteile:** Volle Kontrolle (moderne Auflösung, Widescreen, kein Xbox-Format-Ballast). Später ist das das sauberste Ziel.
* **Nachteile:**
  * Es entsteht faktisch ein neuer NetImmerse-4.2-Renderer ohne Referenzimplementierung. Vermutung: Ein NiDX8Renderer-Quelltext ist nicht verfügbar.
  * Hohe Regressionsgefahr bei Ni-Features, die das Spiel nutzt (u. a. `NiOutline`).
  * Das Spiel ist erst nach Fertigstellung des Renderers testbar.

### 10.3 Option (c): Hybrid – **empfohlen**

1. **Phase 1:** D3D-API-HLE wie (a). Die 88 Hooks und der Lazy-State-Reader bilden einen Shim mit Xbox-D3D-Semantik über einem eigenen RHI (zuerst z. B. D3D11/D3D9-Fixed-Function für schnelles Bring-up, dann Vulkan/D3D12-Ubershader). Ziel: Spiel inkl. Movies spielbar.
2. **Phase 2:** NiXBox-Funktionen werden beim Decompilieren 1:1 ersetzt und rufen den Shim als normale C++-Funktionen mit denselben Signaturen auf. Damit verschwinden die Inline-Lazy-State-Stellen der 22 Funktionen von selbst. Die Choke-Points zuerst: Dispatcher 0x1478c0, Texture-Stage 0x15ab40, Draw-Pfade vf42/vf52/vf53/0x149ae0/0x14bc30, Texturdaten 0x15d920–0x15f8f0.
3. **Phase 3 (optional):** Hot Paths der NiXBox (Geometrie, Texturen) direkt auf das RHI heben, z. B. für native Auflösung und Widescreen. Dann gibt es Flags=1 → 16:9 und keine Swizzle-Roundtrips mehr. Der D3D-Shim bleibt nur noch für Movie/XMV und Restfälle.

Begründung in Zahlen:

* Option (c) beginnt mit 88 Funktionen und 4 Globals statt mit rund 423 NiXBox-Funktionen.
* Es gibt 0 Push-Buffer-Inline-Stellen, also entfällt der übliche Haupt-Showstopper von D3D-HLE.
* 128 der 143 D3D-Aufruferfunktionen liegen in der NiXBox-Bibliothek. Die restlichen 15 (Spiel/XMV) brauchen ohnehin D3D-Semantik (YUY2-Surface, `LockRect`, `GetDisplayFieldStatus`).
* Phase 2 und 3 lassen sich jederzeit gegen das laufende Originalverhalten vergleichen.

### 10.4 Aufwand im Vergleich

| | (a) D3D-API | (b) NiRenderer | (c) Hybrid (Phase 1 / Endzustand) |
|---|---|---|---|
| zu implementierende Einsprungpunkte | 88 D3D | 47 vfunc-Impl. (64 Slots) + 12 D3D-Shim | 88 / nach und nach weniger |
| funktional zu ersetzender Fremdcode | 0 (NiXBox bleibt) | ca. 423+11 NiXBox-Fkt. (82 KB) + 5 Movie-Fkt. | 0 / NiXBox schrittweise |
| Inline-Patches | 0 (Lazy-State-Reader für 4 Globals) | 0 | 0 |
| Push-Buffer-Sites | 0 | 0 | 0 |
| Erstes spielbares Ergebnis | früh | spät | früh |

## 11. Offene Punkte

* Namen und genaue Semantik der NiXBoxRenderer-vfuncs (Vermutungen in 4.1) gegen das NetImmerse-4.2-API (NiRenderer.h) abgleichen.
* Textur-Cache (`NiTCache.dat`, 0x155000–0x156d00): Wird er im Retail-Build tatsächlich benutzt (Z:-Laufwerk)? Wie sehen die per `D3DResource_Register` relozierten Header aus?
* Inhalt der Vertex-Shader-Tabellen in 0x15d6a0 (+0x38/+0x50) zur Laufzeit. Sind die Funktionszeiger NULL (reine Deklarationen)?
* Bedeutung der Display-Caps-Bits 0x400000/0x800000 in `FUN_0023b118`, endgültig für PAL60. Statisch spricht der Code für 0x400000 = 60 Hz und 0x800000 = 50 Hz (Abschnitt 6.2). Offen ist, welche Bits eine PAL-Konsole mit bzw. ohne PAL60 meldet.
* Vollständige Zuordnung der State-Indizes 0x39–0x48 und 0x5c–0x73 zu `D3DRS_*`-Namen. Belegt sind nur die NV2A-Methodennummern 0x300–0x37c aus Tabelle 0x293828. Vermutung (Kelvin-Methodenliste): Alpha-Test, Blend, Alpha-Ref/Func, Depth-Func/Mask, Stencil, Shade-Mode.
* Aufrufer von `NiXBoxRenderer::vf24` (Screenshot), nur indirekt erreichbar.

## Verifikation

Prüfer: unabhängiges Review dieses Berichts gegen die rohe XBE (`orig\retail_eu\default.xbe`) und den Ghidra-Export. Die Prüfskripte teilen keinen Code mit `d3d_common.py`/`d3d_inventory.py`. Sie liegen in `analysis\loader\scripts\verify_d3d_*.py`:

| Skript | Prüfung |
|---|---|
| `verify_d3d_common.py` | eigene Helfer (Sections, functions.tsv, Capstone) |
| `verify_d3d_rawscan.py` | Brute-Force-Bytescan aller Nicht-D3D-Sections ohne Funktionsgrenzen: E8/E9-rel32 auf D3D-Funktionsanfänge, Rohsuche der Device-/MakeSpace-Adressen, rohe Dwords in die D3D-BSS |
| `verify_d3d_operands.py` | eigener linearer Sweep (inkl. Lücke vor der ersten Funktion jeder Section), alle imm/disp-Operanden in den gesamten D3D-VA-Bereich, Abgleich mit den Roh-Treffern |
| `verify_d3d_callers.py` | Zuordnung der 325 Sites zu Funktionen, Gruppen, Sites hinter Ghidra-Funktionsenden |
| `verify_d3d_tables.py` | `ret N`, Sites/Funktionen und Gruppen aller 88 Funktionen neu berechnet und gegen TSV und die Tabellen in Abschnitt 3 verglichen |
| `verify_d3d_simple.py` | `ecx` aller 28 `SetRenderState_Simple`-Sites, vorwärts ab Funktionsanfang dekodiert |
| `verify_d3d_rstables.py` | complex-Tabelle 0x2420b0[0x88..0xa5] und deren externe Aufrufer |
| `verify_d3d_mmio.py` | imm/disp in 0xFD000000–0xFEFFFFFF in allen Code-Sections |
| `verify_d3d_rdata.py`, `verify_d3d_inline_tables.py` | gemeinsam genutzte `.rdata`/`.data`-Adressen; Inline-Leser der D3D-Tabellen 0x293608/0x293828/0x293998 |
| `verify_d3d_kimports.py` | Kernel-Thunks, die D3D-Code benutzt; Namen aus `kernel_common.py` |
| `verify_d3d_devptr.py` | Heuristik für Device-Zugriffe über den Zeiger in `renderer+0x60` |
| `verify_d3d_dis.py` | Disassembly einzelner Adressen (Belege unten) |

### Key Facts 1–18

| # | Ergebnis | Eigener Beleg |
|---|---|---|
| 1 | bestätigt | `D3DDevice_AddRef` 0x234440 `mov eax,[0x244968]; mov ecx,[eax+0x4fc]; inc ecx`. `Direct3D_CreateDevice` 0x2404da `mov edi,0x244970`, 0x2404e4 `mov [0x244968],edi`, Fehlerpfad `mov ecx,0x810; rep stosd` (0x24051b). `SetRenderState_Simple` 0x2357d0–0x2357eb exakt wie zitiert. |
| 2 | bestätigt | 0x2392b0 `mov eax,[0x246ca8]; push eax; shr eax,1; push eax; call 0x239130`. MakeSpace: 77 Sites in **63** Funktionen. Der 1. Prüfdurchgang hatte hier auf 64 „präzisiert“; der 2. Prüfdurchgang hat das zurückgenommen. Die 64 der TSV ist ein Artefakt (`sub_0023b644`/`sub_0023fe67` sind abgeschnittene Reste von `D3DDevice_Clear` bzw. `FUN_0023fdb0`, siehe 2.3), die ursprüngliche Angabe 63 war richtig. 69/116 per Skript reproduziert; 69 ist eine Untergrenze (mit erweitertem Kriterium ≥ 73). |
| 3 | bestätigt | Rohsuche der Dwords 0x244968/70/74/78/7c, 0x2392b0, 0x239130, 0x2392c0, 0x246ca8, 0x246cac in .text/DSOUND/XMV/XPP/.rdata/.data/DOLBY: 0 Treffer. MMIO-Operanden 0xFDxxxxxx außerhalb D3D nur `mov eax,0xfd050f80` bei 0x28d295/0x28f41f. Das ist das Low-Dword von DueTime −50 000 000 (5 s) für Thunk 0x293650 = Ordinal 149 `KeSetTimer`, 4 Pushes. |
| 4 | bestätigt | Rohscan E8/E9: genau 325 Sites (5× E9) auf genau 88 Ziele. Kategorien 20/62, 20/112, 10/55, 31/78, 5/11, 2/7 aus der TSV nachgerechnet. 0x2417b0 = `jmp [0x293638]` (Ordinal 171), 0x240390 = `mov eax,1; ret 4`, 0x23a520 schreibt 0x65/7 und endet mit `ret 8`. |
| 5 | bestätigt | 132 Aufrufer innerhalb von Ghidra-Funktionen und 15 Sites hinter Funktionsenden. Diese verteilen sich auf 11 Blöcke, die alle per vtable-Zeiger in `.rdata` referenziert werden (z. B. 0x1556f0 ← 0x2aafc8, 0x15f8f0 ← 0x2abda4). Summe 143 = 11 R + 117 X + 12 G + 3 M. Alle 321 .text-Sites liegen in 0x146d60–0x1667d0 bzw. 0xa1920–0xee220. |
| 6 | bestätigt, Summe korrigiert | Eigener Sweep: DirtyFlags 97 (w72/r25), RenderState 63 (w), TextureState 34 (33 w + 1 Adressnahme als Immediate), IndexData 5 (r), zusammen **199**, nicht 198. Die Einzelwerte im Bericht waren richtig, nur die Summe falsch. 22 Funktionen bestätigt. Andere D3D-BSS-Adressen werden außerhalb D3D nie als Operand benutzt; alle ~550 rohen Dword-Treffer in die D3D-BSS sind unaligned Bytemuster wie `d9 44 24` = `fld [esp+..]`. |
| 7 | **korrigiert** | 28 Sites in 7 Funktionen bestätigt, aber 27 konstant und nur 1 variabel (0x147904). 0x159a57 ist konstant: `mov ecx,0x40368` bei 0x159a4c, State 0x47. 0x40368 fehlte in der Konstantenliste. Tabellenleser im Dispatcher steht bei Instruktion 0x1478fb (nicht 0x1478fe). |
| 8 | bestätigt, Detail korrigiert | 0x235805 `cmp esi,0x5c`, 0x235829 `cmp esi,0x88`, 0x235852 `call [esi*4+0x2420b0]`. 30 Einträge 0x2422d0..0x242344, alle 30 von 0x1478c0 aufgerufen. `MultiSampleAntiAlias` hat aber zusätzlich 0x159410 als Aufrufer und gehörte nicht in die „ausschließlich“-Liste (3.4 korrigiert). |
| 9 | bestätigt | Decompilat 0xd26a0: `FUN_0014c170(0x280,0x1e0,2,1,0x14000002,0x4b,0,1)`. 0x14c170: pp bei `piVar3+0x9f` (= +0x27c), Format-Switch 2→0x1e, `FUN_00146180`→0x11, `FUN_00146260`→1, Tiefe 0x4b→0x2a, Flags 1, Interval 1→1, BehaviorFlags 0x10\|0x40 = 0x50. .data 0x2d7cec = 0x200000, 0x2d7cf0 = 0x8000 (nur von 0x14c374/0x14c37a gelesen). `ret 0x18` und `and eax,0x10` (0x2404d7) stimmen. |
| 10 | bestätigt | 0x23b118: Zweige nur für `param_4 == 0x32/0x3c`. Sonst `uVar6 = caps & 0xc00000`, Tabellenschleife bis 0x8ac/12 = 0xb9 Einträge. 0x23b080: `push 0x2444b8; push 0; push 6; push 0; call [0x2936d4]` (Ordinal 2, stdcall). Zusätzlich geprüft: Das Spiel fragt die Video-Flags nicht selbst ab; die einzigen `XQueryValue`-Aufrufe nutzen 0xffff (0xf6ffa) und 0x11 (0xf9b1a). |
| 11 | bestätigt | vf16 0x14646d/0x14646f `push 0; call 0x240fc0`, nur wenn `[[this+0x58]+0x10] == 0`. Swap 0x240fe5–0x240fea wie zitiert. `BlockUntilVerticalBlank` hat einen einzigen Aufrufer 0x240690. |
| 12 | bestätigt | 0xee220: `CreateTexture2(w,h,1,1,0,0x24,3)`, `+0x3c`, `+0x48`/`+0x4c`, `+0x58 = w*h*2`, `renderer+0x5cc` → vf `+0x1c`. 0xede40 liest `*(*(DAT_002eb310+0x104)+0x60)`. XMV-Aufrufkette 0xee220→0x264360, 0xede90→0x264962→0x265b0b. 47 `.xmv` in `disc_files.txt`, `1-1-01-PAL.xmv` = 55 431 168 Byte. |
| 13 | bestätigt | 0xa1950/0xa1a00 klemmen auf ±12.0. vf15 0x14641f `call 0xa1930` bzw. 0x14643b `call 0xa1920`, danach `call 0x2342e0`. Strings 0x294710/0x294e70. |
| 14 | bestätigt | Keiner der genannten Spielcode-Orte enthält eine D3D-Referenz (eigener Operanden-Sweep, Aufrufermenge Spiel = genau die 12 Funktionen). Strings `fonts/*.fnt` 0x2a1302.. ← 0x985a0, „LoadingScreen“ 0x2a57f0 ← 0xefb60, „HUD\\ErrorIcon.nif“ 0x2a5a04 ← 0xf33e0, „saveimage.xbx“ 0x2a4d60 ← 0xe0420. |
| 15 | bestätigt (Vermutung bleibt) | `CreateVertexShader`/`CreatePixelShader` je 1 Site. Pixel-Shader-Definition in 0x147740: 0x3c DWORDs genullt, Indizes 53/54/57/58/59 = 0x11102/0xe1/0xffffffff/0xffffffff/0x1ff, Alpha/RGB-Inputs wie angegeben. Einschränkung: Die Argumente von `CreateVertexShader` kommen zur Laufzeit aus Tabellen (korrigiert im 2. Prüfdurchgang, Push-Reihenfolge bei 0x15d53d–0x15d54d): `pDeclaration` = Rückgabe der vfunc `+0x2c`, `pFunction` = Argument 1 von 0x15d4c0 = Array `this+0x38` (0x15d6a0), `Usage` = Argument 2 = Array `this+0x50`; die Argumentnamen sind Vermutung nach der XDK-Signatur mit 4 Argumenten (`ret 0x10`). D3D-intern ruft `SetVertexShader` auch `LoadVertexShader`/`SelectVertexShader` auf. Programmierbare Shader sind also statisch nicht ausgeschlossen, nur nicht nachweisbar. Für Fixed-Function spricht, dass `SetVertexShader` FVF-Codes erhält: 2 (0x14aca0), 0x142/0x152 (vf52). |
| 16 | bestätigt | DrawVertices 2/2 (vf42, 0x149ae0), DrawIndexedVertices 5/5. Indexzeiger `DAT_002444c4` in allen fünf. |
| 17 | bestätigt (negativ) | Bibliotheks-Tabelle bei 0x105f0: XAPILIB, D3D8, DSOUND, XBOXKRNL, XMV, LIBCMT, XGRAPHC, alle 1.0.5233. Keine `XG*`-Namen. Die drei Aufrufer des Swizzler-Konstruktors 0x155660 enden mit `ret 8` (`FUN_00155d60`, 0x155fa1, hinter dem Ghidra-Ende), `ret 4` (`FUN_0015db00`) bzw. `ret` (`FUN_0015dda0`), nicht mit dem `ret 0x20` eines WINAPI-`XGSwizzleRect` mit 8 Argumenten (präzisiert im 2. Prüfdurchgang, vorher „`ret`/`ret 4`“). |
| 18 | bestätigt | vtable 0x2aa7c0: Slot 75 ist bereits String-Daten, also 75 Slots. Slots 11–74: 58 verschiedene Ziele, davon 47 `NiXBoxRenderer::*`; in allen 75 Slots 49. Bereiche: 99 + 324 = 423 Funktionen, 26 764 + 55 426 = 82 190 Byte. |

### Weitere geprüfte Aussagen

1. Section-Daten D3D (va/vsz/raw, BSS 0x243890–0x246cf8), 266 Funktionen, 128 benannt, 52 228 Byte: bestätigt.
2. Push-Buffer-Defaults 0x80000/0x8000 und `D3D_SetPushBufferSize` 0x240480 schreibt 0x246cac/0x246ca8: bestätigt.
3. Alle 57 Zeilen der Tabellen 3.1–3.5 (ret, Sites/Funktionen, Gruppen): stimmen mit der Neuberechnung überein. Abweichende `ret`-Mengen bei 0x2354f0/0x23a5a0 stammen aus Code in Lücken hinter dem Ghidra-Funktionsende; die TSV-Werte sind richtig.
4. Die 5 Tail-`jmp` (0x147b8e, 0x1591f1, 0x159b4e, 0x161892, 0x16678d): bestätigt.
5. Tabelle 4: 4 Funktionen nur mit Inline-State (0x159700, 0x15bf80, 0x161d60, 0x161e40): bestätigt.
6. Tabelle 4.2: Calls (Summe 304 = 325 − 17 Spiel − 4 XMV) und Inline-Sites je Bereich bestätigt; „Fkt.“ enthält die Inline-only-Funktionen (Erläuterung ergänzt).
7. Renderstate-Indizes (simple 0x39–0x42, 0x44–0x48; deferred 0x5c–0x63, 0x66, 0x67, 0x6f, 0x70, 0x71, 0x73) und TSS-Indizes 0, 1, 3, 4, 5, 12, 14, 15, 16, 18, 19, 21: bestätigt.
8. `D3DResource_Register` „Data += Basis“ (0x2374d8–0x2374f2) und `CreateIndexBuffer2` (LocalAlloc(len+0xc), Common 0x1010001, Data = hdr+0xc): bestätigt.
9. `SetIndices` schreibt `pIB->Data` nach 0x2444c4 (0x2351a9) bzw. 0 (0x2351b0): bestätigt.
10. XMV 0x264360 wird aus 0xee220 mit den Callbacks 0xedba0/0xede60 aufgerufen: bestätigt.
11. `SetVertexShader(2)` in 0x14aca0: bestätigt (dasselbe macht die D3D-Init intern).
12. Einziger Datenzeiger in D3D-Code aus .rdata/.data: 0x2d3e6c → 0x235770. Bestätigt; die übrigen Roh-Treffer liegen in XMV-/DSOUND-/DOLBY-Daten und sind keine Funktionsanfänge (Rauschen).
13. `CreateTexture2`-Aufrufer (0x15d920, 0x15e370, 0x15e840, 0x15ecf0, sub_0015f8f0, 0xee220): bestätigt.

### Korrekturen im Text

* Summe der Lazy-State-Instruktionen 198 → **199** (Kurzfazit, 5.2, 10.1).
* `SetRenderState_Simple`: 26/2 → **27 konstant / 1 variabel**; 0x40368 (State 0x47) ergänzt.
* 3.4: `MultiSampleAntiAlias` hat einen zweiten Aufrufer (0x159410).
* 3.6: 0x23b790/0x23c1d0 sind nicht unreferenziert, sondern GPU-ISR/DPC (Immediates in `CMiniport_InitHardware`). TSV-Notizen ergänzt.
* 2.3/5.1: „69 Schreiber“ als Untergrenze gekennzeichnet. (Die hier im 1. Prüfdurchgang eingetragene Änderung „MakeSpace 64 statt 63 Funktionen“ hat der 2. Prüfdurchgang zurückgenommen: richtig sind 63 Funktionen/77 Sites.)
* 4.1: Rollen-Vermutung für vf42/vf52/vf53 an die Draw-Parameter angepasst (vf52 Partikel-Quads statt TriShape, vf53 LINELIST).
* Dispatcher-Adressen 0x1478fe/0x147920 → Instruktionsadressen 0x1478fb/0x14791d.
* Grenze Spielcode/XAPILIB (0xf3000 → ca. 0xf3de0) präzisiert; Beginn der zweiten NiXBox-Teilbibliothek (0x155000 → praktisch 0x1552c0) vermerkt. Keine Zählung ändert sich dadurch.
* Methodik: `d3d_scan_refs.py` erfasst die Art `rdata` nicht (Docstring verspricht es).
* `d3d_functions.tsv`: Notizen für 0x2357d0, 0x2392b0, 0x23b790 und 0x23c1d0 aktualisiert. Damit die Datei reproduzierbar bleibt, stehen dieselben Texte jetzt im `NOTES`-Dict von `d3d_inventory.py`. Ein Neulauf erzeugt eine byte-identische TSV; die Zeile „no refs at all“ in `out/d3d_inventory_summary.txt` heißt jetzt „no call/data-pointer refs (code immediates not scanned)“. Zahlenwerte in der TSV waren alle korrekt.

### Ergänzt (fehlte im Bericht)

* **Konstante D3D-Tabellen, die NiXBox inline liest:** 0x293998 (Primitiv → Vertexzahl, 10 Instruktionen in vf42/0x149ae0/0x14bc30) sowie 0x293828/0x293608 in 0x1478c0. Unkritisch, da read-only.
* **Physische Adressen in `Data`** (Abschnitt 7): `Register` maskiert auf 0x0fffffff, `VB_Lock2` liefert `|0x80000000`, Contiguous-Speicher bis 0x7ffffff. Das HLE muss physische Gastadressen in Host-Zeiger übersetzen, weil NiXBox Header selbst schreibt (Textur-Cache).
* **Kernel-Imports nur für D3D** (`verify_d3d_kimports.py`, Namen aus `kernel_common.py`): `AvGetSavedDataAddress` (1), `AvSendTVEncoderOption` (2), `AvSetDisplayMode` (3), `AvSetSavedDataAddress` (4), `HalReadWritePCISpace` (46), `MmClaimGpuInstanceMemory` (168). Dazu die GPU-Interrupt-Kette `HalGetInterruptVector`/`KeInitializeInterrupt`/`KeConnectInterrupt`/`KeInitializeDpc`/`KeInsertQueueDpc`, die D3D sich mit DSOUND/XPP teilt. Bei vollständigem D3D-HLE reichen dafür Stubs. Ein Weiterbetrieb des Original-D3D-Codes würde dagegen NV2A-MMIO (z. B. 0xfd40014c, 0xfd001804, 0xfd600140) und einen GPU-Interrupt verlangen.
* **D3D hängt von XAPI/CRT ab:** `LocalAlloc`/`LocalFree` (je 10 D3D-Funktionen), in 0x241c40 außerdem `XPhysicalAlloc`, `Sleep`, `QueryPerformanceCounter` u. a., dazu `__allmul`/`__alldiv`. Mit dem HLE entfällt das.
* **D3D-Static-Initializer 0x235770** (läuft in `_cinit`): kopiert nur ±1e8-Floats nach .data 0x2d75f4–0x2d7600, die sonst niemand referenziert. Für den Loader harmlos.
* **Device-Init schreibt einen Debugger-Block** über `fs:[0x20]`→`+0x250` (Decompilat 0x239d20: 0x19e4, 0x2078, …, 0xabcdef00). Vermutung: Info für den Debug-Monitor. Das ist nur relevant, wenn Original-Init-Code läuft; das KPCR-Feld sollte dann 0 sein.
* **Movie-Code** ruft zusätzlich die NiXBox-Funktion `FUN_0014cbb0` auf und nutzt `XPhysicalAlloc` (Objekt +0x2c/+0x30). Das ist für Option (b) relevant (10.2 ergänzt).

### Weiterhin unsicher

* Ob zur Laufzeit programmierbare Vertex-Shader entstehen (Arrays `this+0x38` = `pFunction` und `this+0x50` = `Usage` in 0x15d6a0, korrigiert im 2. Prüfdurchgang); nur dynamisch klärbar.
* Das tatsächliche 50/60-Hz-Verhalten auf PAL-Konsolen. Statisch gilt 0x400000 = 60-Hz- und 0x800000 = 50-Hz-Klasse (6.2); welche Bits die Konsole meldet, ist offen.
* Rollen der Renderer-vfuncs (4.1) sind Verhaltens-Vermutungen. Auch die Zuordnung vf52 = Partikel und vf53 = Linien beruht nur auf den Draw-Parametern.
* Ob `SetVerticalBlankCallback`/`SetSwapCallback` überhaupt gelinkt sind. Für das HLE ist das egal, da kein externer Aufruf existiert.
* Ob 0x15ab40 über den Zeiger auf `D3D__TextureState` weitere, hier nicht gezählte Zugriffe macht. Am Ergebnis „nur 4 Globals“ ändert das nichts, denn der Zeiger zeigt auf dieselbe Global.
* Die Zahl der Push-Buffer-Schreiber in D3D ist nicht exakt bestimmt (mindestens 69, mit erweitertem Kriterium mindestens 73). Für die HLE-Grenze ist das irrelevant.

### Zweiter Prüfdurchgang

Der Review wurde wiederholt. Der zweite Prüfer hat die Key Facts in ihrer **ursprünglichen** Form und die Korrekturen des ersten Durchgangs mit neuen Skripten unabhängig nachgerechnet. Die Skripte `analysis\loader\scripts\verify_d3d_r2_*.py` teilen keinen Code mit `d3d_common.py`, `d3d_inventory.py` oder `verify_d3d_common.py`.

| Skript | Prüfung |
|---|---|
| `verify_d3d_r2_common.py` | eigene Helfer: Sections, Funktionsliste, Sweep von Funktionsstart zu Funktionsstart inkl. Lücken |
| `verify_d3d_r2_calls.py` | Sweep **und** grenzfreier Roh-Scan (E8/E9/0F8x) auf den gesamten D3D-Codebereich, nicht nur auf Funktionsanfänge; indirekte `call/jmp [abs]`; Abgleich mit TSV und den Tabellen in Abschnitt 3 |
| `verify_d3d_r2_lazy.py` | D3D-BSS-Operanden außerhalb D3D mit Capstone-Zugriffsflags (r/w/rmw/Adressnahme), geschriebene RS-/TSS-Indizes |
| `verify_d3d_r2_simple.py` | letzter `ecx`-Schreiber vor jedem `SetRenderState_Simple`-Call, Rückabbildung über 0x293828 |
| `verify_d3d_r2_internal.py` | interne Aufrufer von MakeSpace/MakeRequestedSpace/BeginStateBig/BlockUntilVerticalBlank, PB-Schreiber mit zwei Kriterien, Section-Statistik |
| `verify_d3d_r2_rets.py` | `ret N` per rekursivem Abstieg (Sprungtabellen inklusive), unabhängig von Ghidra-Größen |
| `verify_d3d_r2_vtable.py` | vtable 0x2aa7c0, vfunc-Adressen aus 4.1, NiXBox-Bereiche |
| `verify_d3d_r2_ranges.py` | Tabelle 4.2 neu berechnet |
| `verify_d3d_r2_tables.py` | Tabellen 0x293998, 0x293608[0x5c..0x87], 0x2420b0[0x88..0xa5] und deren externe Aufrufer |
| `verify_d3d_r2_modes.py` | Modustabelle 0x242910, alle 640×480-Einträge |
| `verify_d3d_r2_kimports.py` | Kernel-Thunks im D3D-Code und welche davon nur D3D nutzt |
| `verify_d3d_r2_ptrs.py` | Datenzeiger und Code-Immediates von außen in D3D-Code |
| `verify_d3d_r2_xbehdr.py` | Bibliotheks-Tabelle (Header 0x160/0x164), XG-Namen, Swizzler-Aufrufer, `.xmv`-Zahl |
| `verify_d3d_r2_threads.py` | Thread-Erzeugung (CreateThread/PsCreateSystemThreadEx) |
| `verify_d3d_r2_dis.py` | Disassembly-Belege |

**Key Facts:**

| # | Ergebnis | Beleg (2. Durchgang) |
|---|---|---|
| 1 | bestätigt | Disassembly 0x234440, 0x2404a0–0x240531 (`mov edi,0x244970`, `mov [0x244968],edi`, Fehlerpfad `mov ecx,0x810; rep stosd`, `ret 0x18`), 0x2357d0–0x2357f5 (inkl. Retry-Pfad `push edx; push ecx; call 0x2392b0; pop; pop; jmp 0x2357d0`). `D3DDevice_Release` 0x234460 nullt bei RefCount 1 ebenfalls 0x810 DWORDs. |
| 2 | bestätigt mit **63** Funktionen | 77 Sites; 3 davon liegen hinter abgeschnittenen Ghidra-Funktionen (Details in 2.3). Die Korrektur des 1. Durchgangs auf 64 ist zurückgenommen. |
| 3 | bestätigt | Außerhalb D3D referenziert kein Operand eine D3D-BSS-Adresse außer den 4 Lazy-Globals (`verify_d3d_r2_lazy.py`: „other: []“). Roh-Scan: 325 rel32-Treffer, alle auch im Sweep, keiner auf einen Nicht-Funktionsanfang in D3D, keine indirekten Calls über Speicherslots in D3D-Code. |
| 4 | bestätigt | 325 Sites (320 `call`, 5 `jmp`), 88 Ziele. Kategorien 20/62, 20/112, 10/55, 31/78, 5/11, 2/7. TSV: 266 Zeilen, 0 Abweichungen bei Sites, Aufrufern und Gruppenspalten. |
| 5 | bestätigt | 143 = 11 R + 117 X + 12 G + 3 M. Alle 11 `sub_`-Aufrufer sind echte Funktionen, weil je ein vtable-Eintrag in `.rdata` auf sie zeigt: 0x2aafc8, 0x2ab000, 0x2aaff0, 0x2aafbc, 0x2ab7c0, 0x2abda4, 0x2abdac, 0x2aaf18, 0x2aaf04, 0x2aaf08, 0x2ac188. |
| 6 | Summe **199** bestätigt (Original 198 war falsch) | DirtyFlags: 25 r, 47 rmw, 25 w (also w72/r25); RenderState 63 w; TextureState 33 w + 1 Adressnahme; IndexData 5 r. 22 Funktionen. |
| 7 | in korrigierter Form bestätigt (Original „26 konstant“ war falsch) | 28 Sites in 7 Funktionen: 27 konstant, 1 variabel (0x147904, `ecx` aus `mov ecx,[esi*4+0x293828]` bei 0x1478fb). 15 Konstanten, States 0x39–0x42 und 0x44–0x48. Jede konstante Site hat einen Inline-Schattenschreiber für denselben State, mit identischen Zahlen je State (0x3b/0x3c je 3, 0x39–0x42 sonst je 2, 0x44–0x48 je 1). Damit ist die Alternative „Schattenkopie lesen“ aus 5.2 gedeckt. |
| 8 | bestätigt | 30 verschiedene complex-Setter, alle von 0x1478c0 aufgerufen. Die Daten der deferred-Tabelle liegen effektiv bei 0x293778–0x293827. 0x293608 selbst ist Thunk-Slot 42 (Ordinal 172, `MmFreeSystemMemory` laut `kernel_common.py`), das erklärt den Ghidra-Namen. |
| 9 | bestätigt | Decompilat 0x14c170 (pp ab `piVar3+0x9f`, `Direct3D_CreateDevice(…, piVar3+0x18)` = `+0x60`). `FUN_00146180`: `flags&0xf00 == 0` → 0x11; `FUN_00146260` → 1. |
| 10 | bestätigt, ergänzt | 0x23b09f: `mov esi,0x242910`, Grenze `cmp ebp,0x8ac`, `mov eax,0xb9`. Bitbedeutung 0x400000/0x800000 jetzt in 6.2. |
| 11 | bestätigt | vf16: `Swap(0)` nur wenn `[[this+0x58]+0x10] == 0`. 0x240fe5–0x240fea. Genau ein interner Aufrufer von `BlockUntilVerticalBlank` (0x240690, selbst nur von 0x240f80 gerufen). |
| 12 | bestätigt | 0xee220 (`CreateTexture2(…,1,1,0,0x24,3)`, `+0x3c/+0x48/+0x4c/+0x58`). Die Renderer-vfunc wird über `*(RendererData+0x10)+0x5cc` erreicht, vf `+0x1c`. 47 `.xmv`. |
| 13 | bestätigt, ergänzt | Clamp ±12,0 in 0xa1950/0xa1a00; vf15 ruft 0xa1930/0xa1920. Die Erreichbarkeit aus dem Menü ist jetzt belegt: Aufrufer im undefinierten Code 0x28960–0x29880 (4.3). |
| 14 | bestätigt | Die Aufrufermenge „Spiel“ besteht genau aus den 12 Funktionen. Keine Nicht-NiXBox-Funktion greift auf D3D-BSS zu. String-Belege wie angegeben (strings.tsv). |
| 15 | bestätigt (Vermutung bleibt), Detail korrigiert | 1 `CreateVertexShader`-Site. `pFunction` kommt aus dem Array `this+0x38`, nicht aus `this+0x50` (Zeile 15 oben korrigiert). Pixel-Shader-Indizes 53/54/57/58/59 sowie 0/1, 26/27, 34/35, 45/46 aus den Stack-Offsets nachgerechnet. |
| 16 | bestätigt | Alle 5 `DrawIndexedVertices` übergeben `DAT_002444c4` (in vf42/0x149ae0/0x14bc30 + Offset·2), die Vertexzahl kommt aus 0x293998. |
| 17 | bestätigt | Bibliotheks-Tabelle 7 × 1.0.5233 bei 0x105f0. Detail zu den `ret`-Werten der Swizzler-Aufrufer korrigiert (Zeile 17 oben). |
| 18 | bestätigt | 75 Slots (Slot 75 = 0x4e4b4e55, kein Code), Slots 11–74: 58 Ziele / 47 `NiXBoxRenderer::*`, gesamt 49. 99 + 324 Funktionen, 26 764 + 55 426 Byte. Objektgröße 0x6d0 laut JSON. |

**Weitere geprüfte Aussagen (alle bestätigt, sofern nicht anders vermerkt):**

1. Alle 57 Zeilen der Tabellen 3.1–3.5: Sites/Funktionen und Gruppenspalte stimmen mit TSV und Neuberechnung überein. Auch `ret N` per rekursivem Abstieg stimmt für alle 56 Zeilen mit Rücksprung; die Thunk-Zeile ist `jmp [0x293638]`.
2. Tabelle 4.2 neu berechnet: 132 Fkt., 304 Calls, 199 Inline-Sites, alle Bereichswerte gleich. Die Site 0x160048 gehört zu `sub_0015ffe0` und wird nach Funktionsstart gezählt.
3. vfunc-Adressen in 4.1 (vf11, 15, 16, 17, 24, 25, 42, 47, 52, 53, 54) entsprechen den vtable-Slots.
4. `FUN_00153e90` reicht wirklich bis 0x1552bc (`ret 4` bei 0x1552b9; Ghidras Größe endet bei 0x15529e).
5. ISR/DPC: `push 0x23c1d0` (0x23c898) vor `KeInitializeDpc` (3 Pushes), `push 0x23b790` (0x23c926) vor `KeInitializeInterrupt` (7 Pushes). Dazu `HalGetInterruptVector(3,&irql)` und `KeConnectInterrupt` (1 Push). Die Argumentzahlen passen zu den Signaturen.
6. Nur von D3D genutzte Kernel-Imports: Ordinal 1, 2, 3, 4, 46, 168, bestätigt durch eigenen Operanden-Scan.
7. `D3D_SetPushBufferSize` schreibt 0x246cac/0x246ca8. `Direct3D_CreateDevice` setzt 0x80000/0x8000, wenn dort 0 steht.
8. `D3DResource_Register` (`and eax,0xfffffff` außer Typ 0x20000), `VB_Lock2` (`or eax,0x80000000`), `D3D_AllocContiguousMemory` (5 Pushes an Ordinal 166).
9. Tabelle 0x293998 = (0,0),(1,0),(2,0),(1,1),(1,1),(3,0),(1,2),(1,2),(4,0),(2,2),(0,0).
10. Einziger Datenzeiger in D3D-Code ist 0x2d3e6c → 0x235770 (innerhalb der C++-Init-Tabelle). Einziges Code-Immediate ist 0x102445.
11. Device-Init 0x239d20: `FUN_0023b118(pp[0],pp[1],pp[0xb],pp[10],pp[2],pp[0xc],…)`, `SetFlickerFilter(5)`, `SetSoftDisplayFilter(0)`.
12. Dispatcher-Cache `renderer+0x22c`/`+0x230` (8 Byte je State) laut Disassembly 0x1478c5/0x1478e8.
13. `1-1-01-PAL.xmv` = 55 431 168 Byte.

**Korrekturen im 2. Durchgang:**

* MakeSpace: 64 → **63** Funktionen (2.3, 5.1, Verifikationszeile 2 und Korrekturliste). TSV-Notiz für 0x2392b0 und das `NOTES`-Dict in `d3d_inventory.py` sind angepasst. Ein Neulauf ändert gegenüber vorher nur diese Notiz; `out/d3d_inventory_summary.txt` bleibt identisch.
* 10.1: „Upload-Trigger sind explizit“ war irreführend. Es gibt kein `Unlock`, und Indexdaten werden ohne Lock geschrieben.
* Verifikationszeile 15: `pFunction` = Array `this+0x38` (vorher `this+0x50`).
* Verifikationszeile 17: `FUN_00155d60` endet mit `ret 8`.
* 2.2: `D3D__Stream`-Beleg präzisiert (+0 Stride, +8 VB-Zeiger).
* 4.1: vf52 hat einen zweiten Pfad (FVF 0x152, Stride 0x24).
* 2.3: Untergrenze der PB-Schreiber mit erweitertem Kriterium mindestens 73.

**Ergänzt (fehlte im Bericht):**

* 50/60-Hz-Klassen aus dem Code (6.2).
* Resource-Objekte müssen Xbox-Layout im Gastspeicher haben; das HLE-`CreateDevice` muss die Schattenarrays vorbelegen (10.1).
* **Single-Thread:** Das Spiel erzeugt genau einen Thread (`verify_d3d_r2_threads.py`: einziger Aufrufer von `CreateThread` 0xf9a72 ist `entry` 0xf8531; `PsCreateSystemThreadEx` nur bei 0xf9aab). XMV/DSOUND/XPP erzeugen keine Threads. Alle D3D-Aufrufe laufen also auf dem Hauptthread (Fibers inklusive), und das HLE braucht keine Synchronisation gegen andere Gast-Threads.
* Zur GPU-ISR/DPC gehört eine dritte Rückruf-Routine: 0x23bc40 als `HalRegisterShutdownNotification`-Routine (`mov [eax],0x23bc40` bei 0x23c953, Aufruf 0x23c959). In Ghidra ist sie keine Funktion. Unter HLE entfällt sie wie ISR und DPC.
* D3D nutzt zusätzlich die geteilten Imports `MmPersistContiguousMemory` (178), `MmQueryAllocationSize` (180) und `XboxHardwareInfo` (322); Erstere bei 0x240764–0x240788 zusammen mit `Av{Get,Set}SavedDataAddress`. Unter vollem D3D-HLE ist das irrelevant.
* **Ghidra-Funktionsliste ist lückenhaft:** echter Code bei 0x28960–0x29880 (Menü) und abgeschnittene Funktionen in D3D (`D3DDevice_Clear`, `FUN_0023fdb0`). Die D3D-Zahlen sind davon nicht betroffen: Alle Sweeps laufen von Start zu Start einschließlich Lücken, und der grenzfreie Roh-Scan findet dieselben 325 Sites.

**Weiterhin unsicher (2. Durchgang):**

* Über welchen Pfad die Device-Init die Render-State-Defaults setzt (`FUN_002395c0` → `SetRenderStateNotInline`): nicht im Detail geprüft.
* Welche Caps-Bits PAL-Konsolen melden, und damit ob Refresh 0 auf PAL 50 oder 60 Hz ergibt.
* Programmierbare Vertex-Shader: Inhalt von `this+0x38` zur Laufzeit.
