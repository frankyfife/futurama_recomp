# FuturamaPC

A PC port of **Futurama** for the original Xbox (Vivendi Universal / Unique Development Studios, 2003), built on reverse
engineering of the European retail disc.

The Xbox CPU is a Pentium III, so the game code is already 32-bit x86. FuturamaPC runs the original `default.xbe`
natively on Windows: a loader maps it at its original base address, a host DLL replaces the Xbox kernel and the XDK
libraries (Direct3D 8, DirectSound, XInput, XMV video, saves) with PC implementations, and game functions can be hooked
or replaced one at a time. The renderer is a new NetImmerse renderer on Direct3D 11. Vulkan and D3D12 back ends are
planned.

**No game data is included.** You need an image (ISO) of your own European Futurama Xbox disc.

## Status

The game runs on PC with menus, movies, sound and saves, and all 22 levels can be reached and played. A complete
playthrough without cheats has not been confirmed yet.

| Area | What you get |
|---|---|
| Graphics | Direct3D 11. Render resolution up to the full display height or beyond, MSAA 2x/4x/8x with alpha-to-coverage, supersampling, gamma-correct resolve, anisotropic filtering. No FXAA or TAA. "Classic" mode (640x480, no AA) is bit-identical to the Xbox output as captured in xemu |
| Frame rate | Game logic at 60 Hz (NTSC timing with PAL content). Display at 60 Hz, 120 Hz, or the monitor rate capped at 120 Hz, with interpolation of the camera, objects and skinned meshes between logic ticks |
| Widescreen | 4:3 (original), 16:9, 16:10, 21:9 or the monitor's aspect. Hor+ field of view; the HUD is anchored to the screen edges, menus and movies stay 4:3 |
| Renderer extras | Render to texture, projected shadows, particles and the complete multi-pass texture pipeline of the original renderer (the toon-shading environment map on characters and props, glow, dark and detail maps). Adjustable outlines (colour, opacity, thickness) |
| Texture packs | Replace any game texture by content hash (`[render] texture_pack`); dump all textures with `[render] texture_pack_dump`. `tools/texture_upscale.py` builds an AI-upscaled pack (waifu2x recommended) |
| Movie packs | All 47 movies from an AI-upscaled H.264 pack (`[render] movie_pack`), frame-synced to the original audio. Built with `tools/movie_upscale.py` |
| Audio | DirectSound HLE on XAudio2: Xbox ADPCM, streamed music, 3D sound with the exact DirectSound distance model, I3DL2 reverb, device hot-switching |
| Video | All 47 XMV movies in the selected language (own XMV demuxer, WMV2 via the Windows decoder). Every movie can be skipped |
| Input | Controller (XInput), keyboard and mouse; all 24 Xbox buttons can be rebound; rumble; hot-plugging |
| Camera | Optional precise right-stick camera; optional playable first-person mode (experimental); free camera (L3+R3 or F6) in levels, cut scenes and the 3D main menu |
| Saves | The game's own save code on the kernel HLE, in the Xbox hard-disk layout. `tools/verify_save.py` checks and re-signs saves |
| Game options | Start level (any of the 22 levels), play as Fry, Bender or Leela (experimental), movie skipping, logic rate |
| Cheats | Optional, all off by default. See below |
| Launcher | Qt launcher with a setup wizard, disc check, all settings and a result screen after the game; German and English |

Still open: an in-game check of the dark and detail maps (two levels, not reached by the automated runs), the bump-map
perturbation (unused by the game), a comparison of shadows and particles against xemu, and the Vulkan/D3D12 back ends.
See [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Quick start

Requirements: Windows 10 or 11 (64-bit), a Direct3D 11 GPU, and an ISO of your own **European** Futurama Xbox disc
(other versions are detected and rejected).

1. Build the package (see below) or unpack it, and start **`dist\FuturamaPC\FuturamaLauncher.exe`**.
2. The first-start wizard asks for the UI language, the disc image (checked immediately), the game language, the
   display and the image quality (Classic, Standard or Maximum). Everything can be changed later in the tabs.
3. Press **Play**. The launcher hides and shows the result when the game exits, with the cause and the end of the log
   if something went wrong.

In game: **Alt+Enter** toggles fullscreen, **F12** takes a screenshot.

Settings, logs, screenshots and saves live in `%LOCALAPPDATA%\FuturamaPC\`. An empty `portable.txt` next to
`futurama.exe` keeps them in the program folder instead.

### Saves

As on the Xbox, the game saves only at the end of a level (statistics screen, "Save"). Saves are stored in
`%LOCALAPPDATA%\FuturamaPC\hdd\partition1\UDATA\46490002\`, one folder per save. To back them up, copy that folder
while the game is not running. Saves are signed; hand-edited files are reported as damaged by the game.

### Cheats

All cheats are on the launcher's **Cheats** tab (`[cheats]` in `futurama.ini`). With every cheat off the game runs
unmodified. With **"Toggle cheats in game with the D-pad"** enabled, each cheat can also be switched on and off during
play with its D-pad sequence (green flash = on, red = off).

| Cheat | Use | D-pad sequence |
|---|---|---|
| Level skip | Press A A B B A A B B A A in a level (enabled by default) | – |
| Invulnerable | No damage, no lives lost | ↑ ↑ ↓ ↓ ← → ← → |
| One-hit kill | Enemies die from one hit; bosses lose a quarter per hit | ← → ← → ↑ ↓ ↑ ↓ |
| Enemies ignore me | Enemies hold fire | ↓ ↑ ↓ ↑ → ← → ← |
| Teleport to the free camera | In the free camera press RB (or T): the character lands on the ground below the camera | → → ← ← ↑ ↑ ↓ ↓ |
| Moon jump | Hold LB + B to rise (keyboard: Space + 1); works on Zoidberg's rat in 4-1 | ↑ → ↓ ← ↑ → ↓ ← |
| Longer jumps | Adds 0.0–2.0 m to running jumps, in 0.1 m steps | – |
| Infinite ammo | Weapons, Chicken Walker, cannon, Bender's and Leela's charge | ↓ ↓ ↑ ↑ ← ← → → |
| Freeze timer | Stops every countdown: the swamp clock in 4-1, the power console in 2-4, room 9 in 3-5, the time lamps in 5-1 to 5-3 | ↑ ← ↓ → ↑ ← ↓ → |

Details and limits: [`docs/g8_cheats_report.md`](docs/g8_cheats_report.md).

### Texture and movie packs

Packs are derived from your own disc and must never be committed or shared with the repository.

```
rem 1. dump the textures: set [render] texture_pack_dump = 1, play a while; the PNGs land in <userdata>\texture_dump
rem 2. upscale them 2x with waifu2x-ncnn-vulkan (installed by you)
python tools\texture_upscale.py <userdata>\texture_dump <pack folder> --scale 2 --method waifu2x --w2x-exe <waifu2x-ncnn-vulkan.exe>
rem 3. movies: Real-ESRGAN ncnn Vulkan and FFmpeg (installed by you)
python tools\movie_upscale.py --iso <disc image> --pack <movie pack folder> --ai-exe <realesrgan-ncnn-vulkan.exe>
```

Then set the pack folders on the **Graphics** tab ("Texture pack", "Movie pack"). Details:
[`docs/g2_report.md`](docs/g2_report.md), [`docs/m6b_report.md`](docs/m6b_report.md).

## Building

Tools: Visual Studio 2022 Build Tools (MSVC), CMake, Ninja, Python 3. The launcher needs Qt 6.8.3 for MSVC 2022 x64
(default location `D:\Qt\6.8.3\msvc2022_64`, override with `FUT_QT_DIR`). Always call the build scripts by their full
path.

```
rem game (x86): futurama.exe, futurama_host.dll, fut_tests.exe
cmd /c "D:\FuturamaDecomp\build.cmd" build\x86-relwithdebinfo RelWithDebInfo
rem launcher (x64), with unit tests
cmd /c "D:\FuturamaDecomp\build_launcher.cmd" build\launcher-rel Release test
rem player package in dist\FuturamaPC\
python tools\package.py --game-build build\x86-relwithdebinfo --launcher-build build\launcher-rel
rem unit tests
build\x86-relwithdebinfo\bin\fut_tests.exe
```

Set `FUT_CMAKE_ARGS=-DFUT_WERROR=ON` to treat warnings as errors. A clang-cl build is available with
`build.cmd <dir> Debug fut_tests clang`.

`futurama.exe` can also be started directly: without `--config` it reads `%LOCALAPPDATA%\FuturamaPC\futurama.ini`.
Tool modes used by the launcher: `--print-config-schema=`, `--check-iso= --result=`, `--query-gpu=`, `--print-paths=`
([`src/INTERFACES.md`](src/INTERFACES.md) 4.21). The `tools\m*_check.py` scripts run automated checks with input
scripts (`tools\input_scripts\`).

## Repository layout

```
src/            loader and host DLL: core, kernel HLE, vfs, xapi (game hooks, saves, cheats), gfx (renderer),
                audio, video, input; INTERFACES.md describes the interfaces between the parts
launcher/       Qt launcher (FuturamaLauncher.exe), builds its tabs from the game's config schema
tests/          unit tests (doctest), some of them check hook sites against the retail XBE
tools/          Python tools: disc and archive extraction, XBE info, Ghidra helpers, checks, pack builders
analysis/       reverse-engineering notes per topic (analysis/loader/*.md)
docs/           design (loader_design.md), roadmap and one report per milestone
ghidra_scripts/ Ghidra scripts used by tools/build_ghidra.py
third_party/    bundled third-party code
```

Not in the repository (git-ignored): the disc images, `orig/` (extracted XBEs), `ghidra/` (the Ghidra project),
`build*/`, `dist/` and `run/` (test-run data, dumps and packs).

## Documentation

- [`docs/ROADMAP.md`](docs/ROADMAP.md): what is done and what is next
- [`docs/loader_design.md`](docs/loader_design.md): design decisions and every deviation from the original
- [`src/INTERFACES.md`](src/INTERFACES.md): interfaces between the host components, config keys, changelog
- [`src/host/gfx/GFX_CONTRACT.md`](src/host/gfx/GFX_CONTRACT.md): renderer contract
- [`launcher/README.md`](launcher/README.md): the launcher
- Milestone reports in `docs/`: M1–M3 (loader, first image), M3b (60 Hz), M3c (launcher, image quality), M3d
  (120 Hz), M4 (input), M4b (free camera), M5 (audio), M6/M6b (movies, movie packs), M7 (saves), M8 (renderer P2:
  render to texture, shadows, particles), G1–G8 (outlines, texture packs, widescreen, level select, camera,
  first person, level skip, cheats)

The reports and most notes are written in German.

## Reverse-engineering notes

| | Prototype (`Xbox - Futurama.bin`) | Retail EU (`Futurama (Europe) ….iso`) |
|---|---|---|
| XBE build | 2003-06-17 12:41 UTC | 2003-07-01 16:47 UTC |
| Signature | debug keys (devkit) | retail |
| Title ID | VU-021 | FI-002 |
| Region / video | NTSC | PAL, 5 languages |

- XDK 5233 (QFE 1), statically linked: XAPILIB, D3D8, DSOUND, XMV, LIBCMT, XGRAPHC.
- Engine: NetImmerse 4.2.1.0 with UDS extensions (`NiUDSNode`, `NiUDSObject`, …).
- No PDB and no MSVC RTTI. The NetImmerse classes are recovered from NiRTTI (130 classes with name, size and parent),
  their vtables and the stream loader registrations. The level scripts (`scripts/levelX-Y.ful`) come with `.dbg`
  symbol files on both discs.
- `python tools/build_ghidra.py retail_eu` rebuilds the Ghidra database reproducibly (ghidra-xbe with
  XbSymbolDatabase, CRT initialisers, NetImmerse classes). Permanent names belong in a symbol TSV, because manual work
  in the Ghidra UI is lost on a rebuild.

```
python tools/xdvdfs.py ls <image>              # list the files of an Xbox disc image
python tools/xdvdfs.py x  <image> <out> [...]  # extract
python tools/futimg.py ls <file.img>           # list an .img archive
python tools/xbeinfo.py   <default.xbe>        # header, libraries, sections
```

## License

Copyright (C) 2026 frankyfife - <https://github.com/frankyfife/futurama_recomp>

FuturamaPC is free software under the **GNU General Public License version 3 or later** ([`LICENSE`](LICENSE)).
You may use, study, change and share it; anyone who distributes it, changed or not, must do so under the same
license with the complete source code. As an additional term under section 7(b) of the GPL-3.0
([`NOTICE`](NOTICE)), every copy and every work based on it must keep this attribution unchanged:

> Based on FuturamaPC by frankyfife - https://github.com/frankyfife/futurama_recomp

Third-party components keep their own licenses: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

This project contains no game code, data or artwork from the disc and grants no rights to them. It loads everything
at run time from an image of a disc you own. Futurama is a trademark of its respective owners; this is an unofficial
fan project.
