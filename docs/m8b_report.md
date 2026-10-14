# M3e-Rest: Mehrpass-Pipeline (Maps und Textureffekte) – Bericht

Stand: 2026-10-14. Vorgänger: [`m8_report.md`](m8_report.md) (Render to Texture, projizierte Schatten, Partikel). Abweichungen: [`loader_design.md`](loader_design.md) 8.23 P2-10 bis P2-12. Schnittstellen: `src\INTERFACES.md` 4.42. Vertrag: `src\host\gfx\GFX_CONTRACT.md` 16.

## Kurzfassung

* **Die Standard-Texture-Pipeline des Originals wird jetzt ganz nachgebildet.** vf2 0x164360 (Ghidra-Lücke, mit capstone gelesen) wurde Zweig für Zweig portiert: Dark-, Detail-, Glow-, Gloss-, Bump- und Decal-Maps, projizierte Lichter (mit und ohne Clip-Ebene), projizierte Schatten, Umgebungs- und Nebel-Maps landen in denselben Pässen und Stufen wie bei REN, mit dessen Op-Tabellen, Pass-Alpha-Properties, Platzregeln und dem Effektstufen-Cache.
* **Was das Spiel davon benutzt** (statischer Scan aller 753 NIFs der Disc und Zensus aller 22 Level):
  * **Umgebungs-Map** in **jedem Level** auf Figuren, Gegnern, Requisiten, Cockpit und HUD-Porträt: eine Sphere-Map (Koordinatenmodus 2) mit einer 16×1-**Toon-Rampe** (links Weiß, eine Spalte 0xEE, dann 0xDD), NEAREST, WRAP. Die Seite der Figuren, deren Normale in Welt-+z zeigt, wird um 13 % dunkler, mit harter Kante: die Cartoon-Schattierung des Originals. Sie fehlte im Port bisher ganz (das war der Zähler „texture effects not reproduced“).
  * **Projizierte Schatten** (geclippt, Frys/Benders/Leelas Schattenbild): schon seit M8.
  * **Glow-Maps**: 4 Properties, die Lavalampen in Level 3-2 (`slwa/slwb/slwg.nif`) und ein Testlevel.
  * **Dark-Maps**: 2 Properties (1-6 Brunnennebel `stage4.nif`, 4-1 Sumpf `swamp1e.nif`); **Detail-Maps**: 7 Properties in 4 Sumpfteilen von 4-1 (`swamp2d…g.nif`).
  * **Nicht benutzt**: Gloss-, Bump- und Decal-Maps, projizierte Lichter, Nebel-Maps. Alle 7 498 `NiTexturingProperty` haben Apply-Mode MODULATE. Die NIFs enthalten keinen `NiTextureEffect`; Umgebungs-Map und Schatten legt der Spielcode an.
* **Der alte Zähler „multi-map approximated“ war irreführend:** Er zählte jeden Draw mit geclipptem Schatten (seit M8 nachgebildet). Die Zahlen des Nutzers (z. B. 4-1: 161 595 / 119 583) bestehen aus Schatten-Draws bzw. Umgebungs-Map-Draws.
* **Nach der Änderung:** in allen Läufen 0 nicht nachgebildete Effekte, 0 angenäherte Mehrpass-Draws, 0 Passüberläufe. GPU-Zeit unverändert (Mittel über 22 Level vorher 0,97 ms, nachher 0,95 ms je Bild). Das 3D-Hauptmenü (xemu-Referenz) hat keine Effekte und bleibt bitgleich.

## Was REN tut (Adressen)

| Teil | Adresse | Inhalt |
|---|---|---|
| Pipeline-vf2 | 0x164360 (Lücke) | Einstufiger Pfad nur, wenn +0x770 (Effektstufen) = 0 und Byte +0x1C (weitere Maps) = 0. Sonst: Basis + Detail (`FUN_001600E0`), Dark (`FUN_00163E20`); mit projizierten Lichtern zuerst die Lichtpässe ({geclipptes Licht, Clip-Stufe} + Dark + ungeclippte Lichter, je Pass max(Stufen, Texturen) − 2), danach die Basis in eigenem Pass (Typ 2: ZERO/SRCCOLOR, das Licht mal die Basis); Decals je in neuem Pass (`FUN_00162740`); Schatten (nicht bei Blending/Alpha-Test, 0x1649A0); Umgebungs-Map (mit Bump `FUN_00162910` und Gloss; `FUN_00162670(0, n, n)`); Glow (`FUN_00162670(0, 1, 1)`: in Pass 0 oder einen Pass mit Typ-0-Anfang, sonst eigener Pass ONE/ONE); Nebel-Map (neuer Pass, wenn Pipeline +8 ≠ 0 – das ist der Passzähler von vf3 aus dem vorigen Draw) |
| Stufenobjekte | Ctor 0x161B20, Pool +0x40 (8 × 0xCC), Standardstufe +0x6A4 | `FUN_00163D60` Map-Stufe (Pool wird auch ohne UV-Satz verbraucht), `FUN_001629C0` Ops nach Position und Typ |
| Pässe | Ctor 0x15FE20, `FUN_00162520`, `FUN_00162740`, `FUN_00162670`, `FUN_0015FDD0` | +0x08 Zahl, +0x0C benutzt/−1, +0x10 Texturstufen, +0x38 Alpha-Property; je 4 Stufen und 4 Texturen (DAT_002D85D0/D4) |
| Tabellen | `FUN_0015A120` | Erste Stufe +0x108, spätere +0x1F8 (Typ 0 ADD, 1 BLENDTEXTUREALPHA, 2 MODULATE, 3 MODULATE2X, 8 BUMPENVMAPLUMINANCE); Pass-Alpha +0x2E8 + 4 · Typ: 0/8 ONE+ONE, 1/6 SRCALPHA+INVSRCALPHA, 2 ZERO+SRCCOLOR, 4 dasselbe mit Alpha-Test GREATER 0x80, Rest deckend |
| Effektstufen | `FUN_00164200`, `FUN_00163F10`, Handler +0x774: 0x162C90, 0x162E60, 0x163040, 0x163160, 0x163280; Clip `FUN_00162AD0` (Licht), `FUN_00162BB0` (Schatten); Init `FUN_001633F0`/`FUN_00163A40`; vf6 0x162420 | Neuaufbau nur bei anderem Effect State (Manager +0xB08), BeginScene leert (`FUN_00159EF0`). Sphere-Map: CAMERASPACENORMAL, COUNT2, M = inverse Ansicht · P, Zeile 3 = T. WORLD_PERSPECTIVE: COUNT3 \| PROJECTED (Manager +0x310 = 0x103). Typen: Lichter 0, Schatten 2, Umgebung 2, Nebel 1, Licht-Clip 2, Schatten-Clip 0 |
| Pass-Schleife | vf3 0x1628B0, `FUN_00149AE0` | Ab Pass 1 `FUN_00159270(Pass-Alpha)`, Nebel aus; Pass apply 0x15FFE0 (leerer Pass: kein Draw) |

## Umsetzung

| Teil | Dateien | Inhalt |
|---|---|---|
| Pipeline | `gfx\ni_extract.h/.cpp` | `BuildPasses` (vf2), `CaptureEffectStages` (`FUN_00164200`), `MakeEffectStage` mit allen fünf Koordinatenmodi, `MakeClipStage` für Lichter und Schatten, `ApplyPassAlpha` mit der Typtabelle, `EmitExtraPasses` (vf3). Neue Typen `PipelineStage`, `EffectStageCache` (in `StateShadow`), `pipelinePassesDrawn`; `SphereMapTransform`, `LightClipTransform`. Ersetzt die M8-Sonderfunktion für Schatten (die ungeclippten Schatten liegen jetzt wie in REN im ersten Pass, wenn Platz ist) |
| Shader | – | Keine Änderung nötig: Der Ubershader konnte schon alle Ops, TCI-Modi (Normale, Position, Reflexion) und TTFF (auch PROJECTED). Die Zusatzpässe zeichnet `GpuRenderer` wie seit M8 |
| Zensus | `gfx\multimap_census.h/.cpp` (neu), `gfx\nipc_gpu.cpp`, `gfx\texture_pack.h/.cpp` (`UploadedHash`) | Exit-Zeilen `multi-map census:` je Rolle, Kombination und Textur (G2-Hash, Größe, Modi) |
| Scan | `analysis\loader\scripts\m8b_multimap_scan.py` (neu) | Statischer Scan der NIFs (`NiTexturingProperty::vf03` 0x10A4B0, Map-Laden `FUN_0010A3B0`/`FUN_0010A440`) |
| Tests | `tests\extract_multimap_tests.cpp` (neu, 9 Fälle), `tests\ff_render_tests.cpp` (+1 WARP-Fall), `tests\extract_tests.cpp` | Stufen und Pässe für Dark/Detail/Glow, Decal, Umgebungs-Map (als Stufe und als eigener Pass hinter dem Schatten), projizierte Lichter, Nebel-Map, Bump, Effektstufen-Cache, Matrizen; WARP: die Toon-Rampe als Stufe und als ZERO/SRCCOLOR-Pass ergibt dasselbe Bild |

**Zusammenspiel:** Texturpakete (G2) ersetzen auch die Zusatz-Maps, weil alle Stufen über denselben Resolver gehen; die Rampe `75c2f2cb9907d279` liegt im waifu2x-Paket als 32×32 (Stufe bei 0,5 erhalten, die 0xEE-Spalte verwischt). MSAA, Breitbild, Render to Texture und Interpolation (die Matrizen entstehen mit der Kamera des jeweiligen Bildes, wie im Original über SetCameraData) bleiben unverändert. Der klassische Modus (640×480, ohne MSAA) zeigt dieselben Unterschiede wie 1280×960.

## Messung je Level

Läufe: Startlevel über `[game] start_level`, Route `run\mm\level.txt` (etwa 25 s Spiel, Screenshots s1–s6), 1280×960, MSAA 4×, Ton aus, Rumble aus, Filme übersprungen, Unverwundbar. „Vorher“ = Stand M8 mit Zensus (`build\census`), „nachher“ = M3e-Rest (`build\mm`). Zensus-Zahlen = Draws mit dem Effekt (auch ausgeschaltete Umgebungs-Maps, z. B. 1-1: 37 303 an, 12 363 aus).

| Level | Schatten-Draws | Umgebungs-Map-Draws | vorher „nicht nachgebildet“ | nachher Mehrpass-Draws | nachher Zusatzpässe | nachher nicht nachgebildet | GPU ms Ø/99 % vorher / nachher |
|---|---|---|---|---|---|---|---|
| 1-1 | 218292 | 49666 | 49612 | 252719 | 178021 | 0 | 1.29/3.3 / 1.48/3.3 |
| 1-2 | 15017 | 57464 | 57397 | 54357 | 10082 | 0 | 0.81/2.4 / 1.01/2.6 |
| 1-3 | 10623 | 57408 | 57408 | 49943 | 7753 | 0 | 0.93/2.5 / 0.61/2.7 |
| 1-4 | 136914 | 81306 | 81355 | 178419 | 120788 | 0 | 1.14/3.0 / 0.85/2.8 |
| 1-5 | 46074 | 73478 | 73547 | 96671 | 25316 | 0 | 1.1/2.8 / 0.8/2.3 |
| 1-7 | 19798 | 52360 | 52415 | 55860 | 15084 | 0 | 0.93/2.4 / 0.54/2.3 |
| 1-6 | 49384 | 58464 | 58464 | 71787 | 47528 | 0 | 1.03/2.6 / 0.67/2.5 |
| 2-1 | 41519 | 23893 | 23893 | 55784 | 37827 | 0 | 0.96/2.7 / 0.95/2.6 |
| 2-6 | 66248 | 11446 | 11446 | 73944 | 64560 | 0 | 0.98/2.5 / 1.0/2.6 |
| 2-2 | 21825 | 17420 | 17430 | 34490 | 18292 | 0 | 0.94/2.5 / 0.92/2.4 |
| 2-3 | 30178 | 20992 | 20992 | 43778 | 25722 | 0 | 0.95/2.4 / 0.94/2.4 |
| 2-4 | 64553 | 46072 | 46072 | 85897 | 66358 | 0 | 1.04/3.0 / 1.02/2.8 |
| 2-5 | 13417 | 15725 | 15745 | 25212 | 9444 | 0 | 0.9/2.4 / 0.91/2.5 |
| 3-1 | 58802 | 73172 | 72864 | 112465 | 51399 | 0 | 0.72/2.3 / 1.08/2.8 |
| 3-2 | 5355 | 37776 | 37824 | 32900 | 4899 | 0 | 0.54/1.9 / 0.94/2.5 |
| 3-3 | 25062 | 34011 | 33963 | 48829 | 17268 | 0 | 0.95/2.5 / 0.95/2.5 |
| 3-6 | 30747 | 53134 | 53063 | 58359 | 36903 | 0 | 0.98/2.7 / 0.99/2.6 |
| 3-4 | 16865 | 42131 | 42157 | 46633 | 13061 | 0 | 1.01/2.6 / 0.97/2.4 |
| 4-1 | 52249 | 38471 | 38471 | 80502 | 24771 | 0 | 1.09/3.1 / 1.09/3.2 |
| 5-1 | 58019 | 17841 | 17841 | 71685 | 49871 | 0 | 1.09/3.2 / 1.09/3.2 |
| 5-2 | 45409 | 48290 | 48333 | 81336 | 40506 | 0 | 1.05/2.9 / 1.04/2.9 |
| 5-3 | 21270 | 37461 | 37431 | 45551 | 20330 | 0 | 1.01/2.7 / 1.01/2.7 |

Die Zusatzpässe sind fast nur Schattenpässe (seit M8); die Umgebungs-Map ist meist eine zweite Stufe im Basispass und bekommt nur hinter einem Schattenpass einen eigenen Pass (z. B. 1-1: 178 021 Zusatzpässe, davon 175 977 Schatten). Die GPU-Zeit schwankt zwischen den Läufen in beide Richtungen (Taktstufen der Grafikkarte), im Mittel ohne Unterschied.

Glow im Spiel: Lauf `run\mm\snap\L3-2` (Level 3-2, `game snap_to_node NibblerA1`): 164 Glow-Draws (Textur 16×32, UV-Satz 1, BILERP), Kombination „Glow + geclippter Schatten, geblendet“ – auf der Lavalampe entfällt der Schatten (Blending), die Glow-Map wird als ADD-Stufe in den Basispass gelegt.

## Prüfung

* **Bauten:** MSVC RelWithDebInfo `build\mm` und clang-cl Debug `build\mm-clang` mit `FUT_WERROR=ON` ohne Warnungen. `fut_tests` **559/559** in beiden Bauten (vorher 549; neu 9 Fälle in `extract_multimap_tests.cpp` und 1 WARP-Fall), 1 übersprungen (Handtest).
* **Vorher/nachher** (alle unter `run\` des Arbeitsbaums; angesehen):
  * Vergleichsbögen je Level: `run\multimap\sheets\L*.png` (je Zeile vorher | nachher | |Differenz| × 8). Sichtbar ist überall dasselbe: Figuren, Gegner und Requisiten bekommen das dunklere Toon-Band auf der einen Seite (0,87 der Helligkeit), z. B. Fry in 1-1 (`run\multimap\zoom\L1-1_fry_zoom.png`: auf dem Rücken überwiegend 0,87, an der Kante 0,93), Bender und der Roboter in 5-1, Leela und die Skelette in 3-1, das Cockpit und Frys Arm im Chicken Walker (1-6), das HUD-Porträt in allen Leveln. Böden und Wände ändern sich nicht. Einzelne Bilder mit 20–99 % geänderten Pixeln (4-1, 5-x, 2-6) zeigen nur, dass die Läufe auseinanderlaufen (Kamera leicht versetzt, Ratte ertrunken) – die Differenz besteht dort aus Kanten der ganzen Szene.
  * Glow: `run\multimap\zoom\L3-2_lavalamp_zoom.png` (vorher | nachher): das helle Glühen im Glas der Lavalampe, `run\multimap\zoom\L3-2_frame_001579.png`.
  * Klassisch 640×480 ohne MSAA, 1-1: `run\mm\classic\` – dieselben Änderungen (0,5–3,1 % der Pixel), `run\multimap\zoom\L1-1_classic640_frame_001864.png`.
  * 3D-Hauptmenü (die xemu-Referenz `run\xemu_ref` t0078 ff.): `run\mm\menu\` – keine Textureffekte, vorher und nachher bitgleich (0,00 %).
* **xemu:** Für Level gibt es keine xemu-Aufnahmen (der Attract-Modus ist ein Film); xemu wurde nicht gestartet. Die Richtung des Toon-Bands folgt exakt der Matrix des Originals (P = Zeilen (0,0,0) (0,−0,5,0) (0,5,0,0), T = (0,5; 0,5; 0) aus dem Lauf; Welt-y ist oben, das Band teilt nach Welt-z).

## Bekannte Grenzen

* **Dark- und Detail-Maps** (2 + 7 Properties in 1-6 und 4-1) sind im Spiel nicht gesehen: Kein Skriptlauf erreichte die Stellen (4-1: die Ratte ertrinkt geradeaus, `snap_to_node` wirkt in den Fahrzeugleveln nicht, die Freecam-Flüge `run\mm\fly\` luden die Abschnitte nicht). Sie laufen durch denselben Code wie die gesehene Glow-Map; Stufen und Ops prüfen die Unit-Tests.
* **Bump-Maps:** Die BUMPENVMAP-Verschiebung wird nicht emuliert (das Spiel hat keine Bump-Map); ein solcher Draw zählt als „approximated“.
* Mehr als 1 + 8 Pässe je Draw werden gezählt („pass overflows“), der letzte Pass ersetzt; das Spiel braucht höchstens 3.
* Schatten und Partikel sind weiter nicht mit xemu verglichen (unverändert seit M8).
