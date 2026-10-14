# Third-party notices

FuturamaPC itself is licensed under the GNU General Public License version 3 or later (see [`LICENSE`](LICENSE)
and [`NOTICE`](NOTICE)). The components below are not covered by that license; each keeps its own license. All of
them are compatible with the GPL-3.0.

## Included in this repository

| Component | Where | License | Notes |
|---|---|---|---|
| doctest 2.4.11 (Viktor Kirilov) | `third_party/doctest/doctest.h` | MIT, [`third_party/doctest/LICENSE.txt`](third_party/doctest/LICENSE.txt) | Unit-test framework, only linked into `fut_tests` and the tests; not part of the player package. Contains parts of Catch2 and lest under the Boost Software License 1.0, [`third_party/doctest/LICENSE-BSL-1.0.txt`](third_party/doctest/LICENSE-BSL-1.0.txt) |
| GNU GPL 3.0 and LGPL 3.0 texts (Free Software Foundation) | `LICENSE`, `launcher/licenses/` | verbatim copying permitted, changing not allowed | |

## Shipped in the player package (`dist\FuturamaPC\`), not in the repository

| Component | License | Notes |
|---|---|---|
| Qt 6.8.3 (Core, Gui, Widgets and the platform/style plugins), The Qt Company Ltd. and other contributors | GNU LGPL 3.0 | Used by `FuturamaLauncher.exe`, linked dynamically; the DLLs may be replaced. The package contains the license texts and a note on the Qt source code in `LICENSES\Qt\` (written by `tools/package.py`). Qt source: <https://download.qt.io/archive/qt/6.8/6.8.3/single/> |
| Microsoft Visual C++ runtime (`vcruntime140*.dll`, `msvcp140*.dll`) | Microsoft Visual Studio redistributable terms | Copied next to the launcher by `windeployqt` for Release builds. A system library in the sense of GPL-3.0 section 1 |

## Used at run time, part of Windows (not distributed)

Direct3D 11, DXGI, XAudio2 2.9, X3DAudio, XInput 1.4, Raw Input, Media Foundation and the Windows Media Video
decoder are components of Windows and are only called through their system interfaces.

## Optional tools installed by the user (not distributed)

The helper scripts in `tools/` can call programs the user installs; none of them is included or downloaded by this
project:

| Tool | License | Used by |
|---|---|---|
| Python 3, numpy, Pillow | PSF License, BSD-3-Clause, MIT-CMU (HPND) | all Python tools |
| waifu2x-ncnn-vulkan (nihui) | MIT | `tools/texture_upscale.py --method waifu2x` |
| Real-ESRGAN ncnn Vulkan (xinntao) | BSD-3-Clause | `tools/texture_upscale.py --method ai`, `tools/movie_upscale.py` |
| FFmpeg | LGPL 2.1+ / GPL 2+ depending on the build | `tools/movie_upscale.py`, `tools/m6_check.py` |
| Ghidra (NSA), ghidra-xbe, XbSymbolDatabase | Apache 2.0, MIT, BSD-style; see each project | `tools/build_ghidra.py`, `ghidra_scripts/` |
| xemu | GPL 2+ | reference captures for the checks, run separately |

## Interoperability references

The Xbox kernel and XDK interfaces are reimplemented from public NT documentation, the observable behaviour of the
original game and the public interface descriptions also used by Cxbx-Reloaded and xemu. No source code from those
projects is included.

## The game

"Futurama" is a trademark of its respective owners. The game's program code, data, artwork, audio and video are
not part of this repository or the player package and are not covered by any license here; they are read at run
time from an image of a disc the user owns.
