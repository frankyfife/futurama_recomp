# G1: Konturlinien („Ink Lines“) der Figuren – Technik, Stand im PC-Port, Umsetzungsplan

Stand: 2026-10-12. Basis: Retail-EU-XBE (`orig\retail_eu\default.xbe`, Capstone), Ghidra-Export `analysis\retail_eu_export`, Klassen `analysis\retail_eu_ni_classes.json`, Spieldaten auf dem ISO (nur gelesen, nichts kopiert) und drei kurze Läufe des Ports (`build\freecam\bin\futurama.exe`, Arbeitsbaum vom 2026-10-12, Level 1-1, je etwa 30 s, `run\g1_outlines`, git-ignoriert).

Adressen sind VAs der Retail-EU-XBE. `FUN_xxx` sind Ghidra-Namen. „Vermutung“ kennzeichnet Deutungen, die nicht direkt aus Binary, Daten oder Lauf belegt sind.

## Inhalt

1. Kurzfassung
2. Technik A: vorgefertigte Inverted-Hull-Schalen „QDOutline“ (alle Figuren im Level)
3. Technik B: `NiOutline`, Silhouettenkanten als Bildschirm-Bänder (HUD-Porträt, einige Menü-/Cockpit-NIFs)
4. Was der Port heute mit diesen Aufrufen macht (Lauf-Belege)
5. Herkunft von Farbe und Dicke
6. Abhängigkeiten und Nebenbefunde (HUD-Porträt, Render-to-Texture, Partikel, Slot 56, Schiff)
7. Umsetzungsplan für den D3D11-Renderer (Farbe/Dicke als `[render]`-Option, Standard bitgleich)
8. Prüfung mit kurzen Läufen
9. Offen / Vermutungen
10. Skripte

---

## 1. Kurzfassung

* Das Spiel hat **zwei** unabhängige Konturverfahren. **Keines** nutzt Render-to-Texture, Linienprimitive, Wireframe/Line-Width, Z-Bias, Stencil oder Vertex-Shader-Programme. Die Vermutung aus `docs\m4_report.md` 6.1 („NiOutline + Render-to-Texture“) ist **widerlegt**.
* **A – Figuren im Level (Fry, Bender, Leela, Gegner):** klassische **Inverted Hull**, offline vom Exporter erzeugt. Jede Figur (65 von 67 `chars\*.ucf`, dazu 11 Level-`defs.nif`/`stage*.nif`) enthält einen `NiLODNode` namens **„QDOutline“** (LOD 0–45 Einheiten, darüber leer). Darin liegt pro Körper-Mesh eine **Schale**: dieselben Dreiecke in derselben Reihenfolge, Vertices verschweißt und **um genau 0,05 Modelleinheiten entlang der Normalen** nach außen verschoben (oder 0 bei Meshes ohne Linien), geskinnt mit eigener `NiSkinInstance`. Die Schale hat `NiStencilProperty` mit Draw-Mode **DRAW_CW** (Stencil aus) → der Renderer setzt `D3DCULL_CCW`, also werden die Vorderseiten verworfen und nur die Rückseiten gezeichnet. Dazu `NiMaterialProperty` mit Emissive 0 und **Alpha 0,75**, `NiAlphaProperty` 0x20ED (SRCALPHA/INVSRCALPHA, ohne Sortierung) und keine Lichter → Ergebnis **schwarz, 75 % deckend**. Gezeichnet wird über den normalen Pfad Slot 48 (RenderShape, CPU-Skinning `FUN_0014F710`).
* **B – HUD-Porträt (Kopf oben links) und einige Menü-/Cockpit-NIFs:** NetImmerse-Klasse **`NiOutline`** (NiNode-Ableitung, VTable 0x2A9CC0). Pro Frame bestimmt `NiOutline::vf33` (0x13E840) mit `FUN_00153E90` auf der CPU die **Silhouettenkanten** (Vorder-/Rückseite je Dreieck gegen die Kameraposition), verkettet sie und baut daraus **Bänder in Bildschirmkoordinaten** (halbe Breite 0,003 der Bildhöhe, in x × 0,66). Die Bänder werden als `NiScreenPolygon`-Dreiecksstreifen zu je 404 Vertices (schwarz, Textur `outline.tga`) in die Kameraliste gehängt und in `NiCamera::Click` über **Slot 54** gezeichnet.
* **Der Port zeichnet beide Verfahren bereits.** Der Trace zeigt 13 Schalen von Fry mit `cull 2` (CCW), Material-Alpha 0,75 und Ambient 0 sowie 2 Band-Polygone (404 Vertices) pro Frame für den HUD-Kopf. Ein Speicherexperiment (Schalenmaterial zur Laufzeit magenta gefärbt) zeigt einen geschlossenen Rand um Fry. Die Linie ist aber **dünn**: etwa 0,9 px bei 640×480 bzw. 1,8 px bei 1280×960, 75 % schwarz. Die Aussage „keine Konturen“ stimmt also nicht; die Linien sind bei 1280×960 nur schwer zu sehen. Relativ zur Bildgröße ist die Dicke dieselbe wie im Original, weil Daten und States gleich sind (Vermutung, kein scharfes Referenzbild, siehe 9).
* **Farbe/Dicke:** Bei A kommt die Farbe aus der Beleuchtung (Emissive 0, Ambient-Summe 0) und das Alpha aus dem Material (0,75). Die Dicke steckt in den Modelldaten (0,05 Einheiten, Weltmaß, wird mit der Entfernung dünner). Bei B ist die Farbe ein Immediate 0,0,0 an der Aufrufstelle 0x1551B3–0x1551C2. Die Dicke ist das `.data`-Float `DAT_002D7AB0` = 0,003, x-Faktor 0,66 (`.rdata` 0x2AAE38).
* **Plan (Abschnitt 7):** Optionen `[render] outline_color`, `outline_opacity`, `outline_thickness`. Die Standardwerte lassen den heutigen Pfad unangetastet und sind dadurch bitgleich zum jetzigen (= originalen) Bild. Schalen werden über die Elternkette (`transform − 0x70` → Eltern → `NiLODNode` „QDOutline“) erkannt. Die Dicke wird über eine Bind-Pose-Positionskopie skaliert, die vor `FUN_0014F710` eingesetzt wird; der Verschiebungsvektor pro Vertex kommt aus der Paarung mit dem Körper-Mesh in Dreiecksreihenfolge. Für B werden `DAT_002D7AB0` gepatcht und die Vertexfarben in `ExtractScreenPolygon` ersetzt.

---

## 2. Technik A: vorgefertigte Inverted-Hull-Schalen „QDOutline“

### 2.1 Daten (Fry, `FutPAL00.img` `chars\fry-1-1.ucf`, NIF 4.2.1, `scripts\g1_shells.py`)

```
#3656 NiLODNode 'QDOutline': LOD ranges [(0.0, 45.0), (45.0, FLT_MAX)], children [NiNode, NiNode]
  #3657 NiNode → 13 × NiTriShape (geskinnt, keine Normalen, keine UVs, keine Vertexfarben)
  #3716 NiNode (leer = LOD 1)
  Properties aller Schalen (gemeinsam):
    NiStencilProperty  enable 0, func 4, ref 0, mask 0xFFFFFFFF, fail 0, zfail 0, pass 3, drawMode 2 (DRAW_CW)
    NiVertexColorProperty flags 0
    NiAlphaProperty    flags 0x20ED  (Bit0 Blend, src 6 SRCALPHA, dst 7 INVSRCALPHA, Bit 13 „no sorter“)
    NiMaterialProperty ambient 0.5, diffuse 0.5, specular 0, emissive 0, gloss 4, alpha 0.75
    NiShadeProperty    flags 0
```

* Jede Schale passt zu genau einem Körper-Mesh `Fry_MESHShape:0…12`: **gleiche Dreiecksanzahl, Dreieck k der Schale = Dreieck k des Körpers** (alle 13 gepaart). Abstand je Dreiecksecke |Schale − Körper| ist nur **0,000 (1256 Ecken) oder 0,050 (4273 Ecken)**, Richtung = Körpernormale (cos ≈ 1,0; 0,5–0,97 nur an Nähten mit gesplitteten Normalen). Fry ist etwa 10 Einheiten hoch, die Verschiebung beträgt also 0,5 % der Figurgröße.
* `Fry_MESHShape:0` trägt die Extra-Data `NiData_NoLines=` und hat eine Schale mit Verschiebung 0. `NiData_NoLines` (2 200 Vorkommen auf dem Disc) liest der Spielcode **nicht** (kein String in `strings.tsv`). Vermutung: Der Exporter („QD“-Werkzeug) wertet es aus und setzt dort die Verschiebung 0.
* Verbreitung (`scripts\g1_census.py`): „QDOutline“ steht in 65 von 67 Figurendateien (alle außer `giantbird_distance/perched.ucf`) und in 11 Leveldateien (`defs.nif`, `stage1/2.nif`). `level1-1.nif` enthält keine.

### 2.2 Spielcode

* String „QDOutline“ liegt bei 0x296C18. Drei Spielfunktionen laufen rekursiv über Figurenbäume, ändern Material- und Vertexfarb-Properties und **überspringen** den Teilbaum „QDOutline“ (`repe cmpsb`, Länge 10):
  * `FUN_0003F8D0` (0x3FAD4–0x3FAE4; Aufrufer `FUN_00041E30`, `FUN_0004F430`) legt eine `NiVertexColorProperty` (+0x18 = 1, +0x1C = 0) an bzw. setzt sie.
  * `FUN_00096390` (Aufrufer u. a. `FUN_00041E30`, `FUN_00065630`, `FUN_00095DC0`, `FUN_00097410`): Material Ambient ← Diffuse-Kopie, Emissive ← `DAT_0030E8A0..A8`, Vertexfarben × 0,2.
  * `FUN_00096610` (Aufrufer u. a. `FUN_0004F430`, `FUN_000E8ED0`): dasselbe ohne Vertexfarben.
  Ohne diese Ausnahme würden die Schalen aufgehellt. Deshalb bleibt ihr Material schwarz/0,75.
* Eine weitere Laufzeitbehandlung der Schalen gibt es nicht. Sie sind gewöhnliche geskinnte `NiTriShape`s: `NiTriShape::vf33` 0x1166D0 → Slot 48 RenderShape. Die LOD-Auswahl macht `NiLODNode` (Gastcode).

### 2.3 Renderer-States (NiXBoxRenderer, vom Port nachgebildet)

| Eigenschaft | Original | Beleg |
|---|---|---|
| Culling | `FUN_001599E0`: Tabelle Draw-Mode·2 + Links-rechts-Tausch → `D3DRS_CULLMODE`; Mode 0 (Normalfall) → `D3DCULL_CW` 0x900, **Mode 2 → `D3DCULL_CCW` 0x901** = Vorderseiten weg | `ni_extract.cpp` 167 `kCullTable`, 873ff. |
| Farbe | Beleuchtung an. Der EffectState der Schalen enthält kein Licht, deshalb schreibt `FUN_0015BA40` die Ambient-Summe 0 (`0x24489C`). Ergebnis Emissive 0 + Ambient 0,5·0 = **schwarz** | `ni_extract.cpp` 1227 `EvaluateLights` |
| Alpha | `D3DRS_DIFFUSEMATERIALSOURCE = COLOR1` ohne Vertexfarbe → NV2A nimmt das Material (`FUN_0023CFC0` maskiert die COLOR1-Bits) → **Alpha 0,75** | `ff_ubershader.hlsl` 13, 146 |
| Blend | SRCALPHA/INVSRCALPHA, Alpha-Test aus | Trace „blend 1 (4/5)“ |
| Z | Default-`NiZBufferProperty`: Test LESSEQUAL, Schreiben an | Trace „z func 3 write 1“ |
| Wireframe/Line-Width/BackFillMode/ZBias | **nicht benutzt**. REN setzt über den State-Cache `FUN_001478C0` nur die Indizes 0x61, 0x8A, 0x8E, 0x8F, 0x69 (alle Aufrufstellen in `FUN_00159410`, Rohscan); BackFillMode 0x8C, LineWidth 0x9D und ZBias 0x95 bleiben auf den XDK-Defaults (Vermutung: SOLID / 1.0 / 0) | Rohscan aller `call 0x1478C0` (E8-Ziele in `.text`, capstone): 5 Stellen 0x159692–0x1596E8 |

Wirkung: Wo die Schale über die Körpersilhouette hinausragt, sind ihre Rückseiten sichtbar. Im Inneren verdeckt der Z-Test sie (der Körper liegt davor). Das ergibt einen Rand von 0,05 Einheiten Weltbreite.

---

## 3. Technik B: `NiOutline` (Silhouettenkanten → Bildschirm-Bänder)

### 3.1 Klassen

| Klasse | RTTI | VTable | Größe | ctor / Create | Felder (Beleg) |
|---|---|---|---|---|---|
| `NiOutline` : `NiNode` | 0x3044CC | 0x2A9CC0 (45 Slots) | 0x110 | 0x13D070 / 0x13D230 | +0xE0 Kamera (in vf33 gesetzt), +0xE4 Intensität (≥ 0,0001, sonst normaler `NiNode::vf33`; Default 1,0; gesetzt über `FUN_0013C910` aus `FUN_00039890`), +0xE8 `m_spOLData`, +0xF0/+0xF8 Liste/Anzahl der Konturgeometrien, +0x104 Z-Prioritäten |
| `NiOutlineData` : `NiObject` | 0x2FA410 | 0x2A9C00 | 0x2C | 0x13C0B0 | +8 Kanten (16 B: v0, v1, Dreieck0, Dreieck1 (−1 = Rand), Flags), +0xC Dreiecke (16 B: 3 Vertices, 3 Kanten, Submesh), +0x10 Vertex→Kanten, +0x18 Farben je Submesh, +0x1C `m_nEdges`, +0x20 `m_nTris`, +0x24 `m_nVertices`, +0x28 „Farben neu lesen“ |

Ablauf pro Frame:

* **Erzeugung:** `FUN_0013E550` hängt rekursiv einen `NiOutline` über die `NiTriBasedGeom`-Kinder eines `NiNode`, und `FUN_0013DA10` baut die Kantendaten (`" edges: %d\n"`, Warnung „Missing lipsynchmesh! Outlining could be wrong.“). Einziger Aufrufer ist `FUN_000E8ED0`, der Lader des HUD-Kopfes. Dazu kommen gestreamte `NiOutline`-Objekte in `cockpit\chickencockpit.nif` und `pause\level_breakdown_menu.nif` (`g1_census.py`).
* **`NiOutline::vf33` 0x13E840** (Display):
  * Bedingungen: `m_nEdges ≠ 0`, `DAT_002D7AB8 ≠ 0` (0x13E86A) und Intensität ≥ 0,0001.
  * `FUN_0013D840` sammelt die Kinder. Geometrie mit Alpha-Blending fällt heraus, außer `NiData_OutlineNoSort`.
  * `FUN_0013C9B0` liest einmalig die Extra-Data `NiData_OutlineInterior`/`NoInterior`/`None`/`Zprio`/`OutlineColor=`.
  * Je Kind: `DAT_002FA460` = Geometrie, `DAT_0030EB40` = Zielpuffer für Weltpositionen. Bei Skin wird `skin+0x14 = 1` gesetzt (erzwingt neues Deformieren). Dann `FUN_00101A30` (Culling + Display). „Sichtbar“ gilt, wenn `DAT_0030EB40` danach 0 ist (`DAT_002FA45C[i]`).
  * Zum Schluss `FUN_00153E90(cam)`.
* **Weltpositionen:** Für starre Geometrie schreibt `NiTriShape::vf33` (0x1166D0, nach dem Slot-48-Aufruf) bzw. `NiTriStrips::vf33` (0x11C460) sie, für geskinnte Geometrie der **Renderer** am Ende von `FUN_0014F710` (Rotation +0x70 · Skalierung +0xAC + Translation +0xA0 von `DAT_002FA460`). Danach wird `DAT_0030EB40 = 0` gesetzt.
* **`FUN_00153E90` (Silhouettenbau):**
  * Orientierung jedes Dreiecks gegen die Kameraposition `cam+0xA0..A8`.
  * Kantenklasse über die 256er-Tabelle `DAT_002FA3DC`; Bit 0x80 = Silhouette.
  * Projektion `FUN_00153D30` mit `cam+0xC4` (4×4 World→Clip): x, y → [0,1], z = Sichttiefe/(far − near). `near·0.0` bei 0x153DEC, die Tiefe ist also linear und nicht um near verschoben.
  * Kanten mit Vertices außerhalb [−0,1; 1,1] entfallen.
  * Kanten werden zu Ketten verbunden (≤ 99 Segmente vor und zurück) und zu einem Band mit Gehrungsfugen extrudiert: halbe Breite x = 0,003/(port.r − port.l)·0,66, y = 0,003/(port.t − port.b) (0x15475B, 0x154767, 0x15477D). Tiefe z − 1e-5 (0x15523E/0x155253). UV (0,5; 0)/(0,5; 1) quer zum Band.
* **`FUN_0013D3B0` (Ausgabe):**
  * Pool `DAT_00304568[0x2800]` von `NiScreenPolygon`s mit 0x194 = 404 Vertices (ctor 0x106D50 bei 0x13D653). `FUN_00106A20(1)` setzt Strip (+0x1E), `FUN_00106A40(1)` das Flag +0x1F für den ersten Durchlauf in `NiCamera::Click`.
  * Properties sind die fünf globalen Objekte aus `FUN_0013CE60`: Texturing 0x3044BC (Textur „outline.tga“, Apply-Mode 2), Alpha 0x3044E8 (Blend SRCALPHA/INVSRCALPHA), Z-Buffer 0x30E568 (Test an, Schreiben aus), VertexColor 0x3044C8 (SRC_EMISSIVE, LIGHTING_E) und 0x3044A0 (Bit 0 gelöscht, Vermutung Specular/Dither aus).
  * Farbe: Der Aufruf bei 0x1551DB übergibt **r, g, b = 0, 0, 0** (Immediates `push eax` mit eax = 0) und als Farbausgabe NULL. Alpha ist 1, nur das letzte Vertexpaar jedes Bandes hat Alpha 0.
  * Ein volles Polygon bekommt `+0x1C |= 7` und kommt über `FUN_00019460` in die Liste `cam+0x160`, sofern `DAT_002D7AB8 == 2` (0x13D4BD).
  * `NiCamera::Click` setzt den Pool mit `FUN_0013C750` zurück und hängt mit `FUN_0013E360` das letzte, mit Nullen aufgefüllte Polygon an. Gezeichnet wird über Slot 54.
  * Ergänzung 2026-10-13 (Abschnitt 12): Ein Polygon enthält viele Ketten hintereinander. Die Nullfüllung beim Abschließen (`FUN_0013D3B0` bei vollem Polygon, `FUN_0013E360(Kamera)`, cdecl mit einem Argument) beginnt bei Anzahl − 2 und überschreibt damit auch das letzte Vertexpaar der letzten Kette.
* **Globale Schalter:** `DAT_002D7AB8` = 2 (Modus, `.data`). Nur das Render-to-Texture `FUN_0003B960` setzt ihn um seinen `NiCamera::Click` herum (0x3BBD4–0x3BBEE) auf 0 und stellt ihn danach wieder her. `DAT_002D7AB4` = 0,7 hat keinen Leser.
* „outline.tga“ (0x2A37B4) holt der Level-Lader `FUN_000C5B40` (0xC5CD4–0xC5CEE) über `FUN_000C29F0`; die Textur steckt in `effects\level*.nif` (19 Dateien). Danach folgt `FUN_0013CE60(tex)`.
* `NiData_OutlineColor=RRRGGGBBB` wird zwar in `NiOutlineData+0x18` geparst (Dezimal-Tripel / 255), von `FUN_00153E90` aber nicht verwendet. Vermutung: Rest einer früheren Version.

---

## 4. Was der Port heute mit diesen Aufrufen macht

### 4.1 Lauf-Belege (Level 1-1, `[render] trace_frames`, `scripts\g1_probe.py`, `scripts\g1_magenta.py`)

* **Schalen (A) werden gezeichnet.** Trace Frame 1620 (Auszug):
  * Körper #14–#26: `Shape … skinned … cull 1 … blend 0 … material diffuse (0.80 …) … global ambient (0.75 …)`.
  * Schalen #354–#366: `Shape … Drawn … skinned … stage 0 op 3/3, NiTexture 0 … lighting 1 (0 lights) … blend 1 (4/5) … z func 3 write 1, cull 2 … material diffuse (0.50 0.50 0.50 0.75) … global ambient (0.00 0.00 0.00)`.
  * Bildschirmgrenzen einer Schale gegen ihren Körper (640×480-Koordinaten): #355 (272,66; 149,65)–(309,74; 187,84) gegen #15 (273,57; 150,38)–(308,82; 187,84). Das sind etwa **0,9 px je Seite**.
* **Experiment:** Bei t = 29 s wurden die sechs Schalenmaterialien im laufenden Gast (`NiMaterialProperty`-VTable 0x2A7A60, Werte 0,5/0,5/0/4/0,75) auf Emissive (1,0,1) und Alpha 1 gesetzt. Der Screenshot `run\g1_outlines\shots\frame_001960.png` zeigt einen geschlossenen magenta Rand um Fry, etwa 2 px bei 1280×960. Ohne Eingriff ist derselbe Rand dunkel (Zoom auf Ärmel/Boden in `frame_001661.png`).
* **Bänder (B) werden gezeichnet:** pro Frame 2 × `ScreenPolygon … 404 vertices … (strip, screen camera) … NiTexture 0x033BB040 … colors 1, blend 1 (4/5) … z func 3 write 0`, im Bereich des HUD-Porträts. Die Probe zeigt im Level genau **ein** `NiOutline`, das des HUD-Kopfes: 8 starre `NiTriShape`s, 1675 Kanten, 1079 Dreiecke, 715 Vertices, Sichtbarkeitsflags alle 1, `DAT_002D7AB8` = 2, `DAT_002D7AB0` = 0,003. Die Figuren im Level haben **kein** `NiOutline`.

### 4.2 Port-Pfad je Aufruf

| Aufruf | Port | Bewertung |
|---|---|---|
| Slot 48 Schale (A) | `nipc_renderer.cpp` `RenderGeometry` → `GpuRenderer::DrawGeometry` → `ni::ExtractShape` → `FinishGeometry` → `Skin` (ruft den Gast `FUN_0014F710` synchron) → `ExtractState` | korrekt: Cull aus `NiStencilProperty` (`kCullTable`), Blend, Material-Alpha mit COLOR1-Fallback, Ambient 0 |
| Slot 54 Band (B) | `ni::ExtractScreenPolygon` (`ni_extract.cpp` 1929): Strip, Tiefe `d = near + z·(far − near)`, Vertexfarben/UV aus dem Gast | korrekt |
| `NiWireframeProperty`, FillMode | übersetzt (`ni_extract.cpp` 914, 1444) | von den Konturen nicht benutzt |
| Line-Width, BackFillMode, ZBias, Stencil-Ops | nicht übersetzt | im Original nicht gesetzt (2.3) und von den Konturen nicht benutzt |
| Slot 15/66 Render-to-Texture | verworfen (`TextureTarget`) | betrifft keine Kontur (6) |
| Slot 56 (`FUN_000FF610`, vor jedem Render-Aufruf), Slot 52 Partikel | No-op | betrifft keine Kontur |

**Ein echtes Restrisiko für B:** `ni_extract.cpp` `Skin()` speichert die Deformation pro (Skin, Data, Frame). Ein zweiter Draw derselben Skin-Instanz im selben Frame ruft `FUN_0014F710` nicht mehr auf und füllt `DAT_0030EB40` nicht. Das Original erzwingt das Deformieren mit `skin+0x14 = 1` (`NiOutline::vf33`). Heute ohne Folgen: 0 Cache-Treffer im Lauf, und die `NiOutline`-Kinder des HUD-Kopfes sind starr. Ein `NiOutline` mit geskinnten Kindern verlöre aber seine Kontur. Behebung siehe 7.4.

---

## 5. Herkunft von Farbe und Dicke

| | Farbe | Deckkraft | Dicke |
|---|---|---|---|
| A Schalen | Beleuchtung: Emissive 0 + Ambient(0,5) · Ambient-Summe 0 (keine Lichter im EffectState) = schwarz | Material-Alpha 0,75 (`NiMaterialProperty`+0x50, je Figur in den Daten) | Modelldaten: Verschiebung 0,05 Modelleinheiten entlang der Normalen (0 bei `NoLines`). Weltmaß, wird mit der Entfernung dünner; über 45 Einheiten Abstand schaltet der LOD die Schale ab |
| B Bänder | Immediates 0,0,0 bei 0x1551B3–0x1551C2 → Vertexfarben; moduliert mit `outline.tga` | Vertex-Alpha 1 (Bandende 0) × Textur-Alpha | Bildschirmmaß: ±`DAT_002D7AB0` (0,003) · Bildhöhe (bezogen auf den Kamera-Port), in x zusätzlich × 0,66 (`0x2AAE38`). Bei 480 Zeilen ≈ 2,9 px Gesamtbreite. Z-Versatz −`DAT_002D7AAC` (1e-5) |

Bildschirmbreite von A: Fry ist etwa 10 Einheiten hoch und im Screenshot etwa 330 von 960 Zeilen. 0,05 Einheiten entsprechen dann etwa 1,65 px bei 960 Zeilen, also etwa 0,8 px bei 480 Zeilen. Das passt zu den Trace-Grenzen (0,9 px). Die Breite folgt der Kamera und ist **relativ zur Bildgröße unabhängig von der Auflösung**.

---

## 6. Abhängigkeiten und Nebenbefunde

* **HUD-Porträt:** Technik B, direkt in den Backbuffer mit eigener Kamera/Viewport, **kein** Render-to-Texture. Im Port bereits sichtbar (dunkle Haar-/Kinnkontur im Porträt).
* **Render-to-Texture (Slot 15/66, im Port verworfen):** Pro Frame landen die **13 Körper-Meshes von Fry** in einer Textur (Trace #1–#13 `TextureTarget`, dieselben Objekte wie #14–#26). Ohne Schalen, und `NiOutline` ist dabei abgeschaltet (`FUN_0003B960`, 0x3BBD4–0x3BBEE). Der Hintergrund wird weiß gelöscht (`SetBackgroundColor(1,1,1)`, renderer.md 4). Vermutung: projizierter Figurenschatten. Unabhängig von den Konturen, gehört aber zur offenen P2-Lücke Render-to-Texture.
* **Partikel (Slot 52), Slot 56:** keine Verbindung zu den Konturen.
* **Schwarze Unterseite des Planet-Express-Schiffs:** `level1-1.nif` enthält weder „QDOutline“ noch `NiOutline`. Eine Verbindung zu den Konturverfahren wurde nicht gefunden; nicht weiter untersucht (offen).
* **Freecam (M4b):** Der LOD der Schalen hängt an der Kamera. Fährt die Freecam weiter als 45 Einheiten weg, verschwinden die Schalen, genau wie im Original. Ein Screenshot aus großer Entfernung zeigt daher keine Kontur.

---

## 7. Umsetzungsplan für den D3D11-Renderer

Ziel: Farbe (Pflicht) und Dicke (Wunsch) einstellbar. Der Standard ist **exakt der heutige Pfad**: Bei Standardwerten läuft kein neuer Code, der ein `DrawItem` verändert.

### 7.1 Konfiguration

`[render]`:

| Schlüssel | Werte | Standard | Wirkung |
|---|---|---|---|
| `outline_color` | `original` \| `RRGGBB` | `original` | Farbe von A und B |
| `outline_opacity` | `original` \| 0.0–1.0 | `original` | A: ersetzt Material-Alpha 0,75; B: multipliziert das Vertex-Alpha (Bandenden bleiben 0) |
| `outline_thickness` | 0.0–4.0 | 1.0 | Faktor auf die Originaldicke von A und B; 0 = Konturen aus |

Dateien:

* `src\host\core\config_schema.cpp`: drei Einträge in der `render`-Sektion mit deutschem/englischem Text wie die vorhandenen Schlüssel.
* `src\host\core\config.h`: Kommentar ergänzen.
* `src\host\gfx\gfx_config.h/.cpp`: `Settings::outlineColor` (`std::optional<std::uint32_t>`), `outlineOpacity` (`std::optional<float>`), `outlineThickness` (float). Parsen mit `ParseOrWarn`; bei Abweichung vom Standard eine `GFX/info`-Zeile.
* `src\host\gfx\GFX_CONTRACT.md`: Konfigurationstabelle und neuer Abschnitt „Konturen“ (Technik A/B, Belege aus diesem Bericht).

### 7.2 Konstanten (`src\host\gfx\ni_guest_layouts.h`, nur ergänzen)

* `kNiLODNodeVtable 0x2A7980`, `kNiTriShapeVtable` (aus der Klassenliste)
* `kAvParent 0x14` (heute nur in `interp_history.cpp`), `kObjectName 0x08` (`FUN_000E2EE0`)
* `kOutlineNodeName "QDOutline"` (Vergleich über 10 Bytes wie im Spiel)
* `kOutlineWidth 0x2D7AB0`, `kOutlineMode 0x2D7AB8`, `kOutlinePositions 0x30EB40`
* `kOutlineTexturingProperty 0x3044BC`, `kOutlineAlphaProperty 0x3044E8`

### 7.3 Technik A (Schalen) – `ni_extract.cpp/.h`, `nipc_gpu.cpp`

1. **Erkennen** (nur wenn eine Option vom Standard abweicht): In `GpuRenderer::DrawGeometry` bzw. `FinishGeometry` gilt `object = input.transform − 0x70` (wie `interp_history.cpp` 1499). Bis zu 3 Elternstufen über `+0x14` prüfen: VTable `NiLODNode` und Name „QDOutline“. Ergebnis pro `object` in einer kleinen Tabelle zwischenspeichern (Schlüssel `object` + `NiGeometryData*`). Bei Slot 58 (PurgeGeometryData) bzw. `OnObjectDestroyed` invalidieren.
2. **Farbe/Deckkraft:** Nach `ExtractState` für Schalen setzen:
   * `state.material.emissive.rgb = Farbe`
   * `ambient.rgb = diffuse.rgb = 0`, damit eventuelle Lichter nicht tönen
   * `diffuse.a = opacity`, sonst unverändert (0,75)
   Weil Ambient und Lichter 0 sind, ist das Ausgabebild genau die Wunschfarbe. Kein Shaderwechsel nötig.
3. **Dicke (k ≠ 1):** Die Bind-Pose-Positionen werden ersetzt, **bevor** `FUN_0014F710` skinnt. Damit stimmen Skinning, Interpolation (M3d-Ersatz-Skins) und Bounds automatisch.
   * Pro Schalen-`NiGeometryData` einmalig den Verschiebungsvektor `o(v) = p_Schale(v) − p_Körper(v)` bestimmen. Dazu den Körper-Mesh suchen: die Nachfahren des QDOutline-Elternknotens (`NiNode` +0xC4/+0xCC), `NiTriShape` außerhalb von QDOutline, gleiche Dreiecksanzahl, alle Ecken in Dreiecksreihenfolge ≤ 0,06 entfernt (2.1).
   * Zuordnung von o(v) über die erste Ecke, die den Vertex v benutzt. Ohne Partner: Schale unverändert lassen und zählen.
   * Pro Draw ein Arena-Array `p'(v) = p(v) + (k − 1)·o(v)` anlegen und `Skin()` als `srcPositions` übergeben. Neuer Parameter, heute `gd->vertices`. Starre Schalen bekommen dasselbe Array in `FillStreams`.
   * `k = 0`: Schalen-Draw überspringen (`DrawOutcome::Skipped`, Hinweis „outline off“).
   * Die Paarung über o(v) bewahrt die künstlerische Entscheidung (Verschiebung 0 bleibt 0) und das Weltmaß des Originals. Eine Extrusion entlang selbst berechneter Normalen würde auch die `NoLines`-Bereiche (32 % der Ecken bei Fry) aufziehen und ist deshalb nur als Notlösung gedacht.
4. **Zähler und Log** am Ende (`GFX/info`): „outlines: N Schalen-Draws (P gepaart, U ohne Partner, R umgefärbt), M Bänder (umgefärbt)“.

### 7.4 Technik B (Bänder)

* **Dicke:** Nach dem Einhängen des Renderers (Create-Ersatz in `nipc_renderer.cpp`) einmal `DAT_002D7AB0 = 0,003·k` in den Gastspeicher schreiben, nur bei k ≠ 1. Bei k = 0 `DAT_002D7AB8 = 0`. `FUN_0003B960` sichert und restauriert den Wert, 0 bleibt also 0.
* **Farbe/Deckkraft:** In `ExtractScreenPolygon` erkennen über `ps->texturing == Read(0x3044BC) && ps->alpha == Read(0x3044E8)` (Property-Offsets aus renderer.md 5.4). Dann die Farben in die Arena kopieren und rgb ersetzen bzw. das Alpha multiplizieren. Den Gastspeicher nicht beschreiben.
* **Skin-Cache (4.2):** In `Skin()` den Cache umgehen, solange `Read<u32>(0x30EB40) ≠ 0`. Das entspricht `skin+0x14 = 1` im Original und ändert für heutige Läufe nichts (0 Treffer).

### 7.5 Bitgleichheit des Standards

* Alle neuen Zweige hängen an `outlineColor || outlineOpacity || outlineThickness != 1.0f`. Bei Standardwerten werden weder Gastspeicher noch `DrawItem` noch Pipeline-Schlüssel verändert, und die Erkennung läuft gar nicht.
* Die Skin-Cache-Änderung (7.4) wirkt nur, wenn `DAT_0030EB40 ≠ 0`. Das passiert nur in `NiOutline::vf33` und heute nie bei geskinnten Kindern; die Draw-Liste bleibt also gleich.
* Tests (`tests\`, neue Datei `ff_outline_tests.cpp` oder Erweiterung der ni_extract-Tests):
  * Paarung an einem synthetischen Würfel (Körper + Schale mit 0/0,05-Verschiebung, verschweißte Vertices)
  * Farbersetzung im `RenderStateDesc`
  * Ersetzung der Band-Farben
  * Konfigurationsparser (`original`, Hex, Bereichsgrenzen)
  * Standardwerte → `DrawItem` byteweise gleich dem ohne Erkennung

---

## 8. Prüfung mit kurzen Läufen (je ≤ 1 min)

Build: `cmd /c "D:\FuturamaDecomp\build.cmd" build\g1-outlines RelWithDebInfo` (PowerShell).

Skript: wie in dieser Analyse eine gekürzte Fassung von `tools\input_scripts\m4_newgame.txt` bis `screenshot level1-1_start`, dann `hold RX=1.0 40t`, `screenshot`, `quit 0`. Level 1-1 ist bei etwa 27 s spielbar, Ende nach etwa 29 s. Konfiguration nach dem Muster von `run\g1_outlines\futurama.ini`: `trace_frames = 1560,1620`, `[debug] timeout = 90`, eigenes `userdata` je Lauf (sonst überschreibt ein Lauf das Log des vorigen).

1. **Basislinie** mit dem alten Build, dann **Standard** mit dem neuen Build: Die Trace-Zeilen der Frames 1560/1620 (ohne Zeitstempel) müssen gleich sein, vor allem Schalen #354–#366 (`cull 2`, Material 0,75, Bounds) und die 404-Vertex-Bänder; dazu die Exit-Zähler `d3d11 draws Shape/ScreenPolygon`. Pixelvergleich nur ergänzend, weil absolute Ticks zwischen Läufen um einige Ticks schwanken (`tools\input_scripts\README.md`).
2. `outline_color = FF00FF`: magenta Rand um Fry wie `run\g1_outlines\shots\frame_001960.png` (aber 75 % deckend), magenta Konturen im HUD-Porträt. Log „R umgefärbt“ > 0.
3. `outline_thickness = 3`: Schalen-Bounds im Trace je Seite um etwa 1,8 px (640×480) breiter; HUD-Bänder breiter; Log „13 gepaart, 0 ohne Partner“ für Fry.
4. `outline_thickness = 0`: keine Schalen-Draws (`Skipped`) und keine 404-Vertex-Polygone.
5. `fut_tests.exe` (neue Tests).

---

## 9. Offen / Vermutungen

* **Gleichheit mit dem Original:** Daten, Gastcode und States sind gleich; die Dicke relativ zum Bild ist es damit auch. Ein scharfes Referenzbild fehlt: Die xemu-Aufnahmen `run\xemu_ref\t0153.0s.png`, `t0171.0s.png` (Attract-Demo mit Fry) sind zu unscharf, um 1 px zu messen. Dass die Konturen auf einem Röhren-/PAL-Fernseher kräftiger wirkten, ist eine Vermutung. Ein A/B-Vergleich mit einer neuen 2×-xemu-Aufnahme derselben Szene wäre der nächste Schritt.
* Bedeutung der Bits in 0x3044A0 (Specular/Dither) und des Werts `DAT_002D7AB4` = 0,7 (ungelesen).
* Bedeutung von „QD“ und `NiData_NoLines`: Vermutung Exporter.
* XDK-Defaults für BackFillMode/LineWidth/ZBias nicht nachgelesen. Dass sie nicht gesetzt werden, ist belegt (Rohscan der Aufrufstellen von `FUN_001478C0`); ob `D3D_CreateDevice` sie setzt, nicht.
* Schwarze Schiffsunterseite in Level 1-1: nicht untersucht.
* Ob alle 65 Figuren Material-Alpha 0,75 und Verschiebung 0,05 haben, ist nur an Fry geprüft (`g1_shells.py` läuft für jede Datei).

---

## 10. Skripte (`analysis\loader\scripts`)

| Datei | Zweck |
|---|---|
| `g1_shells.py` | QDOutline einer Figurendatei: LOD, Schalen-Properties, Paarung Schale↔Körper, Verschiebungshistogramm (liest nur vom ISO) |
| `g1_census.py` | Zählung QDOutline / NiOutline / NiData_Outline* / NiData_NoLines / outline.tga / NiStencilProperty je Archiv |
| `g1_probe.py` | startet den Port und liest im laufenden Gast die NiOutline-Globals und alle `NiOutline`-Objekte |
| `g1_magenta.py` | Experiment: Schalenmaterial im laufenden Gast magenta/deckend färben (Nachweis 4.1) |

---

## 11. Umsetzung (2026-10-12)

Umgesetzt nach Abschnitt 7; Doku in `src\host\gfx\GFX_CONTRACT.md` Abschnitt 14 und `src\INTERFACES.md`.

**Dateien**

* Neu: `src\host\gfx\outline.h/.cpp`. Enthält:
  * Erkennung über die Elternkette (`FindOutlineNode`)
  * Paarung Schale↔Körper und Skalierung (`PairShellToBody`, `ScaleShell`, Cache in `Outlines`)
  * Farbe der Schalen (`ApplyShellColor`)
  * Arena-Kopie der Bandfarben (`RecolorRibbon`, `IsRibbonPolygon`)
  * Gast-Globals (`ApplyGuestSettings`)
  * Zähler und Exit-Zeile `outlines: …`
* Neu: `tests\outline_tests.cpp` (5 Fälle); `tests\extract_tests.cpp` hat einen Fall mehr („positionsOverride“).
* `core\config_schema.cpp`: Grafik-Reiter nach „Helligkeitsrichtige Kantenglättung“:
  * `outline_color` (Text, `original` \| RRGGBB, Validator)
  * `outline_opacity` (Text, `original` \| 0..1, Validator)
  * `outline_thickness` (DSpin 0–4, Schritt 0,25, Standard 1,0)
  Einen Farbwähler hat der Launcher nicht; das Textfeld zeigt „original“ bzw. den Hexwert.
* `gfx\gfx_config.*`: `OutlineSettings` und die Parser.
* `gfx\ni_extract.*`: `GeometryInput::positionsOverride` (additiv); `Skin()` mit Quellpositionen, Cache-Umgehung bei Override oder `DAT_0030EB40 ≠ 0`.
* `gfx\ni_guest_layouts.h`: `kNiOutlinePositionBuffer`.
* `gfx\nipc_gpu.cpp`: `DrawGeometry`, `DrawScreenPolygon`, Statistik.
* `gfx\nipc_renderer.cpp`: ein Aufruf in `CreateRenderer`; die M4b-Freecam-Haken bleiben unverändert.

**Builds/Tests**

* RelWithDebInfo (`build\g1-outlines`), Debug mit `FUT_WERROR=ON` (`build\g1-outlines-werr`) und clang-cl Debug mit `FUT_WERROR=ON` (`build\g1-outlines-clang`): alle ohne Warnung.
* `fut_tests` in allen drei Builds: 447 Fälle bestanden, 1 übersprungen.

**Läufe** (je etwa 30 s, Skript `run\g1_outlines\g1_level.txt`, `run\g1_impl\<variante>\`, `trace_frames = 1560,1620`)

| Variante | Ergebnis |
|---|---|
| `base` (alter Build `build\freecam`) vs. `default` (neuer Build, keine Option) | Normalisierte Trace-Zeilen (Gastadressen und Bildschirmgrenzen entfernt, sortiert): Frame 1560 **0 Unterschiede** bei 440 Draws. Frame 1620: 3 Level-Objekte unterschiedlich, weil der Lauf um 2 Ticks verschoben ist; die 13 Schalen sind gleich. Keine `outlines:`-Zeile, also kein Outline-Code aktiv. |
| `outline_color = FF00FF` | 3328 Schalen- und 514 Band-Draws umgefärbt; Schalen `em(1,0,1)`, Alpha 0,75. Magenta Rand um Fry, magenta Gesichtslinien (Mund/Nase sind ebenfalls Schalenkanten) und magenta HUD-Porträt. |
| `outline_thickness = 3` | 13 Paarungen, 0 ohne Partner; 3328 Schalen-Draws skaliert. Schale minus Körper je Seite: 0,92 → 2,72/2,79 px (640×480, Frame 1620). HUD-Bänder `DAT_002D7AB0 = 0,009`. Bei Faktor 3 schimmert die Schale an Nase und Augenhöhlen durch; das ist eine Grenze der Inverted Hull. |
| `outline_thickness = 0` | 3341 Schalen-Draws als `Empty` übersprungen; keine 404-Vertex-Bänder (ScreenPolygon 12 statt 14 pro Frame). Fry ohne Rand und ohne Mund-/Nasenlinien, Porträt ohne Konturen. |

Bildausschnitte (Fry 2× vergrößert, rechts das HUD-Porträt):

* `run\g1_impl\base\crop_fry.png`
* `run\g1_impl\default\crop_fry.png`
* `run\g1_impl\magenta\crop_fry.png`
* `run\g1_impl\thick3\crop_fry.png`
* `run\g1_impl\off\crop_fry.png`
* Übersicht: `run\g1_impl\compare_sheet.png`


---

## 12. Avatar-Fix: schwarze Ausreißer im HUD-Porträt (2026-10-13)

Nutzermeldung (12. und 13.10.): „kleinere Grafikfehler im Avatarbild oben links“, „schwarze Polygonausreißer“. Im eigenen Screenshot des Nutzers (`%LOCALAPPDATA%\FuturamaPC\screenshots\frame_022096.png`, nur gelesen) läuft ein dunkler, auslaufender Keil vom Ohr nach links unten über den Porträt-Hintergrund.

### 12.1 Nachstellung

Läufe in Level 1-1 mit 2560×1920, ohne MSAA, neun Screenshots über etwa 4 s (`run\avatar\portrait.txt`). Der Keil ist in **jedem** Bild da (`run\avatar\base\shots\`, Ausschnitt `run\avatar\base_p01_zoom.png`). Für die Ursache wurde vorübergehend (danach wieder entfernt) in den Trace-Frames jedes 404-Vertex-Band mit Gastpositionen, Farben und UVs ins Log geschrieben; `run\avatar\ribana.py` und `ribdraw.py` werten das aus. `run\avatar\rib1481.png` zeichnet die Dreiecke des Frames 1481 nach: schwarz/blau = Dreiecke eines Bandes (blau = weggeculled), rot/rosa = Dreiecke zwischen zwei Ketten.

### 12.2 Ursache: zwei Eigenheiten des Originalcodes

Die Bänder baut der Gastcode selbst (`FUN_00153E90`), der Port zeichnet sie nur. Beide Fehler stecken also im Original:

1. **Brücken zwischen Ketten.** `FUN_0013D3B0` hängt jede Kette an den laufenden Strip an: Farbe (0, 0, 0, 1), das letzte Vertexpaar Alpha 0 (0x13D43F–0x13D463). Im Strip entstehen so zwischen dem Ende einer Kette und dem Anfang der nächsten zwei Dreiecke mit Alpha (0, 0, 1) und (0, 1, 1). Sie sind bis zur halben Porträtbreite lang. Frame 1481: 54 + 19 Ketten in 2 Polygonen, etwa die Hälfte der Brücken zeigt zur Kamera (Cull CW wie jede Geometrie ohne `NiStencilProperty`). Die meisten liegen hinter dem Kopf und scheitern am Tiefentest. Wo sie über den Hintergrund laufen, bleiben dünne dunkle Splitter (Haar, Ohr).
2. **Nullfüllung ab Anzahl − 2.** Beim Abschließen eines Polygons füllen `FUN_0013D3B0` (0x13D4CF `add ecx,-2`, wenn die nächste Kette nicht mehr passt) und `FUN_0013E360` (0x13E390 `add edx,-2`, aus `NiCamera::Click` 0x100D48) Positionen und Farben ab **Anzahl − 2** mit Nullen. Das letzte Paar der letzten Kette landet damit auf (0, 0, 0). Für Slot 54 ist das die linke untere Ecke des Ports, Tiefe = near, vor allem anderen. Das vorletzte Paar hat noch Alpha 1. So entsteht pro Polygon ein Keil, der bis zur Port-Ecke reicht und nach links unten ausläuft. Bei 1920 Zeilen sind davon etwa 40 px deutlich sichtbar. **Das ist der gemeldete Ausreißer** (`rib1481.png`: die zwei langen Dreiecke zur Ecke).

Gehrungsspitzen (`FUN_00153E90`, Gehrung ohne Begrenzung) kommen in den gemessenen Frames nicht vor: Paarbreite höchstens das 1,4-Fache des Medians (`run\avatar\bandstat.py`). Interpolation (M3d), Breitbild (G3), MSAA und die Konturoptionen sind nicht beteiligt. Die Fehler entstehen im Gastcode vor dem Zeichnen, und dieselben Daten zeigen sie auch bei 4:3 ohne Optionen.

Auf der Xbox entstehen dieselben Dreiecke (gleicher Code, gleiche Daten). Bei 640×480 auf dem Fernseher sind Keil und Splitter dort 1–2 px breit. Ein scharfes xemu-Bild des Porträts gibt es nicht (`run\xemu_ref` zeigt nur die unscharfe Attract-Aufnahme).

### 12.3 Behebung (immer an, keine Option)

* **Zeichnen als Kettenliste:** `GpuRenderer::DrawScreenPolygon` gibt jedes Band-Polygon (`IsRibbonPolygon`) an `outline::SplitRibbonStrip`. Daraus wird eine Dreiecksliste in der Arena:
  * Strip-Dreieck k bleibt, wenn Vertex k Alpha ≠ 0 hat. Dann liegt k vor dem Endpaar seiner Kette, und alle drei Vertices gehören zu ihr.
  * Kein Vertex darf Füllung sein (Position (0, 0, 0) mit Alpha 0).
  * Bei ungeradem k gilt die vertauschte Reihenfolge von D3D, damit die Ausrichtung gleich bleibt.

  Die Bänder selbst bleiben unverändert: gleiche Vertices, gleiche Farben, gleiches Culling, auslaufendes Kettenende.
* **Kettenende zurückholen:** Zwei Probes sichern vor einem Abschluss mit Nullfüllung die 6 Floats des Endpaars und schreiben sie danach zurück (`PoolCloseZeroesTail`: Polygon vorhanden, Modus 2, Anzahl ≥ 2, bei `FUN_0013D3B0` Anzahl + n > 404):
  * `probe:NiOutline::AppendRibbon` (0x13D3B0, thiscall mit 7 Stack-Argumenten, `ret 0x1C`, gestohlen `6A FF 68 1E CD 17 00`)
  * `probe:NiOutline::FlushRibbons` (0x13E360, cdecl mit der Kamera, gestohlen `A1 B8 7A 2D 00`)

  Die Farben bleiben (0, 0, 0, 0), wie das Original sie setzt. Capstone-Durchlauf über .text 0x11000–0x1A0000: kein Sprung und kein Rohzeiger in die gestohlenen Bytes. Aufrufer: 0x1551DB, 0x13D81E (Rekursion nach vollem Polygon) und 0x100D48.
  * Ohne die Probes (etwa über `[hooks] disable`) fängt die Füllungsregel den Keil trotzdem ab. Dann fehlt aber das letzte, auslaufende Segment einer Kette pro Polygon.
* Die Option `outline_color`/`outline_opacity` kopiert die Farben wie bisher (nach dem Aufteilen). `outline_thickness = 0` schaltet die Bänder im Gast ab, die Probes tun dann nichts (Modus 0).
* Exit-Zeile: `outline ribbons: N strips drawn as chain lists (T triangles, B visible bridge/corner triangles left out), R chain ends restored after the pool zero fill`.

### 12.4 Prüfung

* `fut_tests`: 526 Fälle (4 neu in `tests\outline_tests.cpp`):
  * Kettenliste mit Brücken, Füllung, Windungsreihenfolge und Nullfüllung
  * `PoolCloseZeroesTail`
  * `SplitRibbonStrip` an einem Gast-Polygon
  * Probe-Ziele sind Funktionsanfänge
* Läufe (je ≤ 35 s, Ton 0, Vibration 0, `movies = skip`):

| Lauf | Einstellungen | Ergebnis |
|---|---|---|
| `base` (vorher) | 1-1, 2560×1920, 4:3, ohne MSAA, Standardkonturen | Keil vom Ohr nach links unten in allen 9 Bildern |
| `fix` | wie `base`, mit Trace-Frames | Keil weg; Ohrkontur jetzt geschlossen (wiederhergestelltes Kettenende). Bilddifferenz zu `base` nur im Keil und dem Ohrsegment (`run\avatar\diff_p01.png`). 986 Polygone, 986 Kettenenden zurückgeholt, 70 246 sichtbare Brücken-/Eckdreiecke ausgelassen. Strip-Länge 402 statt 400 (Endpaar nicht mehr genullt). |
| `user21` | 2-1 Bender, Einstellungen des Nutzers (16:9, MSAA 8, `30260E`/0,6/0,75), 2560×1440 | keine Ausreißer (`run\avatar\user21_zoom.png`) |
| `leela31` | 3-1 Leela, MSAA 8 | keine Ausreißer (`run\avatar\leela31_zoom.png`) |
| `breakdown` | Statistikbildschirm (`level_breakdown_menu.nif` hat ebenfalls `NiOutline`) über G7 | unauffällig |

Vorher/nachher-Übersicht: `run\avatar\cmp_sheet.png`. Oben 4 Bilder vorher, unten nachher, jeweils 300×320-Ausschnitt bei 2560×1920.

**Offen:** Nicht geprüft sind sprechende Köpfe (Lippensynchron-Mesh, „Missing lipsynchmesh!“) und das Cockpit von Level 1-6. Die Gehrung bleibt unbegrenzt wie im Original; eine Spitze bei fast umkehrenden Silhouettenkanten wurde nicht beobachtet.
