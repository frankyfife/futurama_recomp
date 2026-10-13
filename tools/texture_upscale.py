"""G2: build a texture pack from a texture dump (docs/g2_report.md, src/host/gfx/texture_pack.h).

The game writes its textures with [render] texture_pack_dump = 1 to <userdata>/texture_dump/<hash>.png (+ index.tsv).
This tool upscales them and writes <pack>/<hash>.dds (or .png); [render] texture_pack = <pack> then loads them.
Everything runs locally and offline: numpy + Pillow, nothing is downloaded. The pack contains derived game artwork:
keep it in your user data or under run/, never in the repository (.gitignore excludes texture_dump/ and texture_pack*/).

Methods (--method):
  cartoon   (default) art-style-aware 2x/4x for the flat comic colours of the game, "not too crisp":
              1. colours of fully transparent texels are filled from their neighbours (no dark seams at alpha edges),
              2. a light edge-preserving smoothing at the original size removes DXT block noise in flat areas,
              3. Lanczos-3 upscale of the colour,
              4. edge tightening: every texel moves part of the way (--strength, default 0.5) towards the darker or
                 brighter colour of its neighbourhood, whichever it is closer to - only where the neighbourhood has
                 an edge (luma range > --edge), never beyond the neighbourhood's own colours (no halos, no ringing,
                 flat areas stay flat),
              5. alpha on its own path (never through an AI): binary alpha (alpha-tested textures) Lanczos
                 thresholded at 50 % = smooth clean contours instead of stairs; soft alpha (glows, lamps, light
                 cones, smoke) Lanczos clamped to the surrounding source texels plus back-projection - never
                 thresholded, box-averaged back it matches the original alpha,
              6. under (nearly) transparent texels the ORIGINAL colour is kept (plain Lanczos of it), not the filled
                 colour of step 1: the game draws some alpha textures additively (ONE/ONE), where that colour shows.
  lanczos   plain Lanczos-3 with steps 1, 5 (soft reference).
  external  an AI upscaler YOU installed yourself, e.g. Real-ESRGAN ncnn Vulkan:
              --external-exe C:\\tools\\realesrgan-ncnn-vulkan.exe --external-args "-n realesr-animevideov3 -s {scale}"
            The tool runs "<exe> -i <in.png> -o <out.png> <args>" on the colour (step 1 applied), resizes the result
            to the target size if needed, mixes it with the cartoon result (--external-mix, default 0.7 = 70 % AI)
            and applies step 5 to the alpha. No model or program is downloaded by this tool.
  waifu2x   the recommended AI preset (since v3): waifu2x-ncnn-vulkan you installed yourself (--w2x-exe or the
            environment variable FUT_WAIFU2X), model cunet (--w2x-model), denoise level 1 (--w2x-noise), -s 2. Keeps
            the painted detail and the original look; calibrated against the waifu2x PS2 pack the user liked
            (docs/g2_report.md). 100 % waifu2x by default (--w2x-mix), no back-projection (--ai-detail 0). One
            upscaler run for all textures; colour step 1 before, alpha steps 5 and 6 after.
  ai        the former AI preset (Real-ESRGAN ncnn Vulkan you installed yourself, --ai-exe or the environment
            variable FUT_REALESRGAN): model realesr-animevideov3 (trained on cartoon footage: clean lines, flat colours
            stay flat, no invented photo detail), mixed 75 % AI + 25 % cartoon (--ai-mix) so the painted grain of the
            walls survives. One upscaler run for all textures (fast); colour step 1 before, alpha step 5 after.
            Compared on the game's textures against realesrgan-x4plus(-anime), 4x_NMKD-Siax, 4xNomos8kSC and
            4xLSDIRplusC: those invent cracks and scratches or flatten the walls completely (docs/g2_report.md).
            Then one back-projection pass (--ai-detail 1, also for external): the result, box-downscaled to the
            original size, must match the original again - brings back the faint painted detail (cracks, grime)
            the AI smooths away, keeps its clean edges.
  trace     redraws text instead of filtering it (fonts, signs, name plates): signed distance field of the coverage
            (alpha or the ink of a two-colour split) at the original size, bicubic to the target size, re-thresholded
            with a one-pixel ramp; inside every glyph a second two-colour split (white core / dark outline) is traced
            the same way; flat colours from the clear texels of their class; a part that is not two-coloured (the
            coloured button icons in the font atlas) keeps a smooth colour inside its traced contour. Written as PNG
            (lossless like the 16-bit originals). Crisp, but the soft outer half of the font's dark outline becomes
            a solid band: letters look bolder than the original.
Text textures: whatever --method says, the game's font atlas and text textures (TEXT_HASHES, --trace-list) never go
through an AI. --text-method conservative (default): a clean 2x with the original stroke weight (text_upscale);
trace: the redraw above; original: left out of the pack.
Text in pictures (--text-overlay builtin, default none since v3): in the loading and front-end screens the letters
inside the rectangles of TEXT_RECTS ("LADEVORGANG, BITTE WARTEN...", slogans, copyright lines) are redrawn with the
trace method on top of the picture's upscale. waifu2x keeps that print clean by itself.

Output (--format): dds (default; BC1 for opaque textures and binary alpha with black transparent texels, BC3 for the
rest, full mip chain of plain 2x2 box averages like the originals - same memory
class as the original DXT textures, about 4x / 16x the original for 2x / 4x) or png (RGBA8, the game generates the
mips and uploads 32 bit per texel - 8x the memory of a DXT1 original before scaling; fine for small subsets).

Usage:
  python tools/texture_upscale.py <dump folder> <pack folder> [--scale 2|4]
         [--method cartoon|lanczos|external|ai|waifu2x|trace] [--format dds|png] [--strength 0.5] [--min-size 16]
         [--max-size 2048] [--only HASH,...] [--exclude FILE] [--limit N] [--jobs N] [--overwrite]
         [--w2x-exe EXE] [--w2x-model cunet] [--w2x-noise 1] [--w2x-mix 1.0]
         [--ai-exe EXE] [--ai-model NAME] [--ai-mix 0.75] [--ai-detail N] [--trace-list builtin|none|FILE]
         [--text-method conservative|trace|original|ai] [--text-sharpen 0.8] [--text-overlay builtin|auto|none]
Example:
  python tools/texture_upscale.py run/g2/dump/texture_dump run/g2/pack --scale 2
  python tools/texture_upscale.py <dump> <pack> --scale 2 --method waifu2x --w2x-exe D:\\tools\\waifu2x-ncnn-vulkan.exe
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as e:  # pragma: no cover
    sys.exit(f"texture_upscale: needs numpy and Pillow ({e}); install them yourself if you want to use this tool")

HEX = set("0123456789abcdefABCDEF")
VERSION = 3


def parse_hash(stem: str) -> str | None:
    if len(stem) < 16 or any(c not in HEX for c in stem[:16]) or (len(stem) > 16 and stem[16] in HEX):
        return None
    return stem[:16].lower()


# ---- image helpers (float32 arrays, H x W x C, 0..1) ------------------------------------------------------------------

def shifted(a: np.ndarray, dy: int, dx: int) -> np.ndarray:
    """a shifted by (dy, dx) with edge replication (out[y, x] = a[y + dy, x + dx])."""
    h, w = a.shape[:2]
    ys = np.clip(np.arange(h) + dy, 0, h - 1)
    xs = np.clip(np.arange(w) + dx, 0, w - 1)
    return a[ys][:, xs]


def fill_transparent(rgb: np.ndarray, alpha: np.ndarray, iterations: int = 24) -> np.ndarray:
    """Colours of alpha == 0 texels from the mean of known neighbours (grown ring by ring)."""
    known = alpha > (1.0 / 255.0)
    if known.all() or not known.any():
        return rgb
    rgb = rgb.copy()
    for _ in range(iterations):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                k = shifted(known, dy, dx)
                acc += shifted(rgb, dy, dx) * k[..., None]
                cnt += k
        new = (~known) & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        known = known | new
    return rgb


def bilateral3(rgb: np.ndarray, sigma_r: float) -> np.ndarray:
    """3x3 bilateral filter (spatial sigma 1): removes small block noise, keeps edges."""
    acc = np.zeros_like(rgb)
    wsum = np.zeros(rgb.shape[:2], np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            s = shifted(rgb, dy, dx)
            d2 = ((s - rgb) ** 2).sum(axis=2)
            w = np.exp(-(dy * dy + dx * dx) / 2.0 - d2 / (2.0 * sigma_r * sigma_r)).astype(np.float32)
            acc += s * w[..., None]
            wsum += w
    return acc / wsum[..., None]


def resize(a: np.ndarray, w: int, h: int, resample=Image.LANCZOS) -> np.ndarray:
    """Per-channel float resize (no 8-bit rounding in between)."""
    if a.ndim == 2:
        return np.asarray(Image.fromarray(a.astype(np.float32), "F").resize((w, h), resample), np.float32)
    return np.stack([resize(a[..., c], w, h, resample) for c in range(a.shape[2])], axis=2)


def anti_ringing(up: np.ndarray, src: np.ndarray, w: int, h: int) -> np.ndarray:
    """Clamps the Lanczos result per channel to the range of the 2x2 source texels around each target texel (no
    overshoot halos next to the black outlines)."""
    lo = np.minimum(np.minimum(src, shifted(src, 0, 1)), np.minimum(shifted(src, 1, 0), shifted(src, 1, 1)))
    hi = np.maximum(np.maximum(src, shifted(src, 0, 1)), np.maximum(shifted(src, 1, 0), shifted(src, 1, 1)))
    sh, sw = src.shape[:2]
    # target texel centre in source coordinates; the 2x2 window starts at floor(centre - 0.5)
    ys = np.clip(np.floor((np.arange(h) + 0.5) * sh / h - 0.5).astype(int), 0, sh - 1)
    xs = np.clip(np.floor((np.arange(w) + 0.5) * sw / w - 0.5).astype(int), 0, sw - 1)
    return np.clip(up, lo[ys][:, xs], hi[ys][:, xs])


def box_down(a: np.ndarray, w: int, h: int) -> np.ndarray:
    """Box (area) downscale by an integer factor per axis - what the GPU's next mip level / minification sees."""
    return a.reshape(h, a.shape[0] // h, w, a.shape[1] // w, a.shape[2]).mean(axis=(1, 3))


def back_project(up: np.ndarray, src: np.ndarray, iterations: int) -> np.ndarray:
    """Iterative back-projection: adds the bilinear-upscaled difference between the source and the box-downscaled
    result, so the result seen at the original size matches the source again. Restores the low-contrast painted
    detail (cracks, grime, mottling) an AI upscaler smooths away, keeps its clean edges; clamped like the Lanczos
    path (no halos)."""
    h, w = src.shape[:2]
    H, W = up.shape[:2]
    if H % h or W % w:
        return up
    for _ in range(iterations):
        up = up + resize(src - box_down(up, w, h), W, H, Image.BILINEAR)
        up = anti_ringing(up, src, W, H)
    return up


def luma(rgb: np.ndarray) -> np.ndarray:
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def tighten_edges(rgb: np.ndarray, radius: int, strength: float, edge: float) -> np.ndarray:
    """Moves each texel towards the darker or brighter colour of its (2r+1)^2 neighbourhood (whichever is closer in
    luma), by `strength` times the edge weight. The result stays between the neighbourhood's own colours."""
    y = luma(rgb)
    ymin, ymax = y.copy(), y.copy()
    cmin, cmax = rgb.copy(), rgb.copy()
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if dy == 0 and dx == 0:
                continue
            ys = shifted(y, dy, dx)
            cs = shifted(rgb, dy, dx)
            lo = ys < ymin
            hi = ys > ymax
            ymin = np.where(lo, ys, ymin)
            cmin = np.where(lo[..., None], cs, cmin)
            ymax = np.where(hi, ys, ymax)
            cmax = np.where(hi[..., None], cs, cmax)
    span = ymax - ymin
    t = np.where(span > 1e-6, (y - ymin) / np.maximum(span, 1e-6), 0.5)
    target = np.where((t < 0.5)[..., None], cmin, cmax)
    # Edge weight: 0 below `edge`, 1 from 2 * edge on (flat or gently shaded areas stay untouched).
    wedge = np.clip((span - edge) / max(edge, 1e-6), 0.0, 1.0)
    # Texels in the middle of a ramp move less (keeps a 1-texel anti-aliased transition: "not too crisp").
    wmid = 1.0 - (1.0 - np.abs(t - 0.5) * 2.0) ** 2 * 0.5
    k = (strength * wedge * wmid)[..., None]
    return rgb + (target - rgb) * k


def classify_alpha(alpha8: np.ndarray) -> str:
    if (alpha8 == 255).all():
        return "none"
    if np.isin(alpha8, (0, 255)).all():
        return "binary"
    return "smooth"


def upscale_alpha(alpha: np.ndarray, w: int, h: int, kind: str) -> np.ndarray:
    """Alpha on its own path (never through an AI upscaler). Binary alpha (alpha-tested: 0/255 only): Lanczos
    thresholded at 50 % = smooth contours. Soft alpha (blended glows, lamps, light cones, smoke, glass): Lanczos
    clamped to the surrounding source texels (no overshoot rings around a lamp) plus four back-projection passes, so
    the result box-averaged to the original size matches the original alpha (mean error < 3/255) - never thresholded, the soft gradient and
    the overall opacity stay exactly as drawn."""
    if kind == "none":
        return np.ones((h, w), np.float32)
    if kind == "binary":
        return (np.clip(resize(alpha, w, h), 0.0, 1.0) >= 0.5).astype(np.float32)
    src = alpha[..., None]
    a = anti_ringing(resize(src, w, h), src, w, h)
    a = back_project(a, src, 4)
    return np.clip(a[..., 0], 0.0, 1.0)


def keep_transparent_colour(up: np.ndarray, rgb: np.ndarray, a: np.ndarray, W: int, H: int) -> np.ndarray:
    """Colour under (nearly) transparent texels from the ORIGINAL colour, not from the filled-in colour the
    upscaler saw. The game draws some alpha textures additively (lamps, glows, light cones) where the colour under
    alpha 0 can be visible; filling it from the neighbours (needed for clean visible edges) made whole lamp quads
    glow. Where the upscaled alpha is at least 25 % the upscaler's colour is used, below that it fades to a plain
    Lanczos of the original colour (clamped, no rings)."""
    plain = anti_ringing(resize(rgb, W, H), rgb, W, H)
    k = np.clip(a * 4.0, 0.0, 1.0)[..., None]
    return plain + (up - plain) * k


def run_external(rgb: np.ndarray, w: int, h: int, scale: int, exe: str, args: str) -> np.ndarray:
    with tempfile.TemporaryDirectory(prefix="fut_upscale_") as tmp:
        src = Path(tmp) / "in.png"
        dst = Path(tmp) / "out.png"
        Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").save(src)
        cmd = [exe, "-i", str(src), "-o", str(dst)] + args.format(scale=scale).split()
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = np.asarray(Image.open(dst).convert("RGB"), np.float32) / 255.0
    if out.shape[0] != h or out.shape[1] != w:
        out = resize(out, w, h)
    return out


def upscale(rgba8: np.ndarray, scale: int, method: str, strength: float, edge: float, deblock: float,
            external: tuple[str, str, float] | None = None,
            ai: tuple[Path, float] | None = None, detail: int = 0) -> tuple[np.ndarray, str]:
    """RGBA8 (H x W x 4) -> upscaled RGBA8 and the alpha class. Colour and alpha take separate paths: the colour
    (transparent texels filled from their neighbours) goes through the filter / AI, the alpha through
    upscale_alpha; under transparent texels the original colour is kept (keep_transparent_colour)."""
    h, w = rgba8.shape[:2]
    a8 = rgba8[..., 3]
    kind = classify_alpha(a8)
    orig = rgba8[..., :3].astype(np.float32) / 255.0
    alpha = a8.astype(np.float32) / 255.0
    rgb = fill_transparent(orig, alpha)
    W, H = w * scale, h * scale
    src = bilateral3(rgb, deblock) if deblock > 0 else rgb
    if method == "lanczos":
        up = resize(rgb, W, H)
    elif method in ("ai", "waifu2x") and ai is not None and ai[1] >= 1.0:
        up = None  # the AI result alone (no cartoon share)
    else:
        up = anti_ringing(resize(src, W, H), src, W, H)
        up = tighten_edges(up, max(1, scale // 2), strength, edge)
    if method == "external":
        exe, args, mix = external
        out = run_external(rgb, W, H, scale, exe, args)
        up = out * mix + up * (1.0 - mix)
    elif method in ("ai", "waifu2x"):
        ai_file, mix = ai
        out = np.asarray(Image.open(ai_file).convert("RGB"), np.float32) / 255.0
        if out.shape[0] != H or out.shape[1] != W:
            out = resize(out, W, H)
        up = out if up is None else out * mix + up * (1.0 - mix)
    if method in ("external", "ai", "waifu2x") and detail > 0:
        up = back_project(up, src, detail)
    up = np.clip(up, 0.0, 1.0)
    a = upscale_alpha(alpha, W, H, kind)
    if kind != "none":
        up = np.clip(keep_transparent_colour(up, orig, a, W, H), 0.0, 1.0)
    out = np.concatenate([up, a[..., None]], axis=2)
    return (out * 255.0 + 0.5).astype(np.uint8), kind


def text_upscale(rgba8: np.ndarray, scale: int, sharpen: float = 0.8) -> tuple[np.ndarray, str]:
    """--text-method conservative (default for the font atlas and the text textures): a clean 2x of the original
    letters with the SAME stroke weight - Lanczos of colour and coverage, clamped to the surrounding source texels
    (no rings), one back-projection pass on the coverage (box-averaged back to the original size it equals the
    original: not bolder, not thinner), then a gentle unsharp mask (3x3 box, `sharpen`), clamped again. The soft
    dark outline keeps its soft outer half (the trace method makes it a solid black band: the letters look bold).
    Colour under transparent texels: the original (keep_transparent_colour)."""
    h, w = rgba8.shape[:2]
    W, H = w * scale, h * scale
    kind = classify_alpha(rgba8[..., 3])
    orig = rgba8[..., :3].astype(np.float32) / 255.0
    alpha = rgba8[..., 3].astype(np.float32) / 255.0
    rgb = fill_transparent(orig, alpha)

    def sharp(img: np.ndarray, src: np.ndarray) -> np.ndarray:
        if sharpen <= 0:
            return img
        blur = np.stack([box_blur(img[..., c], 1) for c in range(img.shape[2])], axis=2)
        return anti_ringing(img + sharpen * (img - blur), src, W, H)

    up = sharp(anti_ringing(resize(rgb, W, H), rgb, W, H), rgb)
    if kind == "none":
        a = np.ones((H, W), np.float32)
    else:
        src = alpha[..., None]
        a = back_project(anti_ringing(resize(src, W, H), src, W, H), src, 1)
        a = sharp(a, src)[..., 0]
        if kind == "binary":
            a = (a >= 0.5).astype(np.float32)
        a = np.clip(a, 0.0, 1.0)
        up = keep_transparent_colour(up, orig, a, W, H)
    out = np.concatenate([np.clip(up, 0.0, 1.0), a[..., None]], axis=2)
    return (out * 255.0 + 0.5).astype(np.uint8), kind


# ---- trace (fonts and text) ---------------------------------------------------------------------------------------

# Text textures of the EU retail game (FTX1 hashes from a dump of all 22 levels and the front end, docs/g2_report.md
# "Schriften"). Hashes are numbers, not artwork. --trace-list replaces this list, --trace-list none switches it off.
TEXT_HASHES = {
    "1b29f9843b56fd49": "fonts/latin1.nif - the game's only font atlas (menus, HUD counters, hints, dialogs)",
    "684736283da5d540": "copyright screen, left half",
    "da595f0f469225ed": "copyright screen, right half",
    "40dbf373555b807e": "'This game uses NDL graphics technology' (handwriting)",
    "db2fdf19f78765d5": "credits list ART / CODE (level 5)",
    "18d5aea7ffae8f80": "sign 'Closed till Dark God appeased' (level 3)",
    "c4ffe9de8637ef9e": "street sign 'W. 4th St' (level 1)",
    "cddbb855f7cace5a": "number plate '27831769'",
    "eeeb7e98836a77d8": "Planet Express name plates 'Dr. Zoidberg / Bureaucrat / Hermes Conrad'",
    "3da2b52472bf6e5a": "name plate 'Zoidberg'",
    "8b50b6fc02be0891": "name plate 'Professor'",
    "2a1e67e9e3a1afb2": "name plate 'Leela'",
    "a1403dfc150bb2c1": "name plate 'Bender'",
    "8595cb6c3c24d5cd": "name plate 'Hermes'",
    "34be2fad3496c9e8": "name plate 'Fry'",
    "7d39892595509ab6": "name plate 'Amy'",
    "5337acf412eeba09": "label 'Mail'",
    "b18fecb4dc22d862": "label 'In / Out'",
}


# Printed text in the loading and front-end screens of the EU retail game (FTX1 hash -> rectangles in texels of the
# original, x0, y0, x1, y1). text_overlay redraws the letters it finds inside these rectangles (--text-overlay
# builtin). Chosen once by looking at every letter group find_letters reports in a full dump (510 groups, 213 are
# text; the others are drawing details such as teeth, eyes, shines, waves or logo pieces) and then at the result:
# 24 text groups on striped or shaded paper (flag stripes, white letters with drop shadow, the Gamebryo logo) came
# out worse than the plain AI result and are left out. Big headline lettering and logos are not listed either: the
# AI keeps them clean (docs/g2_report.md, "Text in Bildern").
TEXT_RECTS = {
    "09532098ee28512e": [(405, 388, 543, 405), (406, 407, 535, 421)],
    "0ca50b138e747175": [(133, 144, 255, 159), (133, 161, 247, 174)],
    "19342f2b8a75d07e": [(169, 135, 255, 147), (169, 152, 253, 164), (10, 1, 163, 18), (173, 1, 257, 15)],
    "1bff13ccbf27f7a9": [(154, 373, 368, 388), (388, 373, 521, 388), (363, 413, 486, 428), (363, 430, 477, 443)],
    "1cfc91c71732b7ca": [(189, 64, 257, 80)],
    "217b00e5b1a57d6f": [(158, 227, 255, 243), (65, 228, 129, 243)],
    "2442afbc57c9a5ba": [(105, 131, 257, 147), (104, 155, 256, 171)],
    "24ad7c2a244bed11": [(143, 133, 257, 147), (144, 152, 257, 166), (116, 19, 257, 34)],
    "25d64947fad7e413": [
        (508, 268, 542, 290), (477, 291, 526, 315), (515, 307, 564, 331), (361, 387, 522, 406), (360, 411, 512,
        427)],
    "2f49359843142e92": [(388, 389, 526, 406), (389, 408, 518, 422)],
    "3284abc3bf305067": [(137, 133, 257, 147), (138, 152, 254, 166)],
    "39c5ae39983a35aa": [
        (414, 227, 566, 248), (321, 228, 385, 243), (382, 251, 430, 271), (393, 389, 531, 406), (394, 408, 523,
        422)],
    "3abe9af0adfd72ec": [(154, 117, 257, 132)],
    "3bdefa8711cbb3da": [(301, 274, 598, 290), (301, 297, 580, 317), (393, 389, 531, 406), (394, 408, 523, 422)],
    "3c829a4243b39e9b": [(389, 400, 511, 415), (389, 417, 503, 430)],
    "3d0910b325ecd6eb": [(105, 134, 257, 150), (104, 153, 256, 169)],
    "4345971192c95ec6": [(145, 126, 223, 142), (103, 147, 257, 173)],
    "46b377f45b17415b": [(147, 141, 257, 154), (147, 158, 254, 171)],
    "4e201fa32b39bcba": [
        (370, 388, 394, 403), (469, 388, 483, 394), (513, 388, 528, 394), (512, 394, 531, 406), (370, 411, 442,
        427), (466, 411, 521, 427), (342, 294, 537, 313), (283, 317, 598, 337)],
    "4f5acd5a37a4ee36": [
        (114, 132, 138, 147), (213, 132, 227, 138), (114, 155, 186, 171), (210, 155, 256, 171), (86, 38, 257, 57),
        (27, 61, 257, 81)],
    "54f9ad1461b50eff": [(374, 282, 465, 309), (391, 389, 529, 405), (392, 408, 521, 422), (352, 280, 565, 307)],
    "55ba6b728b9d529c": [(74, 147, 236, 166), (73, 170, 225, 187)],
    "5618af6b05f65019": [(-1, 146, 257, 179)],
    "5bacea8330b20a7d": [(132, 133, 255, 148), (133, 152, 257, 166)],
    "62527caac1be262d": [(129, 120, 183, 144), (57, 121, 114, 141), (1, 165, 257, 213)],
    "661906eaf2c4f312": [(315, 315, 543, 332), (349, 334, 510, 350), (395, 389, 533, 406), (396, 408, 525, 422)],
    "66e19f0abd8dd389": [(403, 397, 530, 412), (403, 414, 521, 427)],
    "695c98f603798f29": [
        (425, 391, 547, 405), (425, 408, 539, 420), (303, 236, 590, 253), (575, 256, 618, 271), (266, 257, 419,
        274), (429, 257, 565, 271)],
    "6a20afce51abd498": [(361, 390, 522, 409), (360, 409, 512, 425), (145, 126, 223, 142), (103, 146, 544, 179)],
    "6e650f7b5a7eeaad": [(399, 389, 537, 406), (400, 408, 529, 422), (372, 275, 571, 291)],
    "6eb4c7eddd9e1ca9": [(113, 131, 256, 147), (112, 155, 257, 171)],
    "71e192115946bb6f": [
        (438, 269, 466, 280), (401, 271, 431, 282), (364, 273, 387, 285), (409, 391, 531, 405), (409, 408, 523,
        420)],
    "893634f53197ac72": [
        (182, 13, 210, 24), (145, 15, 175, 26), (108, 17, 131, 29), (153, 135, 257, 147), (153, 152, 257, 164)],
    "8c783cc7b947956d": [(47, 236, 253, 253)],
    "8fecb4eca17daa5c": [(59, 59, 257, 76), (93, 78, 254, 94), (139, 133, 257, 147), (140, 152, 257, 166)],
    "9200d0cbba61a215": [(138, 199, 220, 217), (115, 199, 257, 215)],
    "930e52f0befe0625": [(118, 26, 209, 53), (135, 133, 257, 147), (136, 152, 257, 166), (96, 24, 257, 51)],
    "98cb648da2018834": [(394, 199, 476, 217), (490, 199, 532, 213), (371, 199, 550, 215)],
    "a3056c4c0a56d8dd": [(149, 132, 257, 147), (150, 151, 257, 165)],
    "a5a89c339a4a09e5": [(371, 400, 511, 417), (376, 426, 509, 442)],
    "ae61ebfbd83f0e6b": [(369, 387, 530, 406), (368, 411, 520, 427)],
    "b2755b4d6086ad78": [(430, 138, 568, 166), (451, 169, 559, 198), (366, 387, 528, 406), (365, 411, 517, 427)],
    "b28d66188e7b7915": [(385, 120, 439, 144), (313, 121, 370, 141), (257, 165, 497, 201)],
    "bf4350b033e83d2d": [(-1, 64, 190, 80)],
    "c5e487a25ccfedab": [(387, 387, 548, 406), (386, 411, 538, 427)],
    "cb5d96527891f090": [(45, 18, 252, 34), (45, 41, 251, 57), (137, 133, 257, 147), (138, 152, 254, 166)],
    "cf0308043cb23fe7": [(131, 131, 257, 147), (130, 155, 257, 171), (145, 73, 246, 103)],
    "cfcf13ffe6d21b54": [(115, 144, 255, 161), (120, 170, 253, 186)],
    "db5ef46f87fbfe7a": [(4, 117, 112, 132), (132, 117, 249, 132), (107, 157, 230, 172), (107, 174, 221, 187)],
    "e09289d8ddb4dc75": [(396, 389, 534, 406), (397, 408, 526, 422)],
    "e4c12e40665debf0": [
        (53, 249, 125, 259), (121, 274, 282, 289), (306, 274, 342, 286), (363, 274, 398, 289), (405, 274, 460, 286),
        (468, 277, 504, 289), (274, 291, 331, 306), (346, 291, 376, 303), (235, 308, 300, 320), (375, 308, 414,
        320), (442, 308, 481, 320), (316, 309, 368, 323), (237, 325, 276, 337), (281, 325, 315, 340), (351, 325,
        422, 340), (242, 342, 302, 357), (309, 342, 417, 357), (156, 359, 186, 371), (196, 359, 222, 371), (232,
        359, 317, 374), (358, 359, 398, 371), (410, 359, 450, 371), (460, 361, 483, 371), (250, 376, 302, 388),
        (330, 376, 425, 388), (219, 393, 246, 405), (261, 393, 348, 408), (356, 393, 410, 405), (105, 410, 144,
        422), (152, 410, 206, 422), (216, 410, 265, 422), (297, 410, 376, 425), (439, 410, 524, 425)],
    "ebe7b27616890d97": [(110, 131, 253, 147), (109, 155, 257, 171)],
    "f5d2a6a217ca20a5": [(189, 64, 446, 80), (330, 403, 492, 422), (329, 426, 481, 443)],
    "fb8466b2e5f502d7": [(140, 133, 257, 147), (141, 152, 257, 166)],
}


def near_distance(mask: np.ndarray, radius: int) -> np.ndarray:
    """Euclidean distance (texels) from every texel to the nearest True texel, capped at radius + 1."""
    d = np.where(mask, 0.0, float(radius + 1)).astype(np.float32)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            r = (dy * dy + dx * dx) ** 0.5
            if 0 < r <= radius:
                d = np.where(shifted(mask, dy, dx), np.minimum(d, r), d)
    return d


def signed_distance(cov: np.ndarray, radius: int = 6) -> np.ndarray:
    """Signed distance field (texels, > 0 inside) of a coverage map 0..1. The contour lies where the coverage crosses
    0.5; texels on the contour use their coverage as sub-texel position (anti-aliased originals keep their shape)."""
    inside = cov >= 0.5
    d_in = near_distance(~inside, radius) - 0.5    # inside: distance to the outside
    d_out = near_distance(inside, radius) - 0.5    # outside: distance to the inside
    sdf = np.where(inside, d_in, -d_out)
    edge = np.abs(sdf) <= 0.5
    return np.where(edge, np.clip(cov - 0.5, -0.5, 0.5), sdf).astype(np.float32)


def trace_coverage(cov: np.ndarray, W: int, H: int, scale: int, limit: np.ndarray) -> np.ndarray:
    """SDF of the coverage at the original size, bicubic to W x H, re-thresholded with a one-pixel ramp (smooth,
    sharp contours instead of blur or stairs); never outside `limit` (the original coverage grown by one texel, so a
    contour cannot bleed into the next glyph cell)."""
    sdf = resize(signed_distance(cov), W, H, Image.BICUBIC)
    out = np.clip(sdf * scale + 0.5, 0.0, 1.0)
    return out * resize(limit.astype(np.float32), W, H, Image.NEAREST)


def otsu(values: np.ndarray) -> float:
    """Threshold 0..1 that splits the values into two classes with the largest between-class variance."""
    hist, edges = np.histogram(values, bins=64, range=(0.0, 1.0))
    p = hist.astype(np.float64) / max(1, hist.sum())
    centers = (edges[:-1] + edges[1:]) / 2
    w0 = np.cumsum(p)
    m = np.cumsum(p * centers)
    mt = m[-1]
    with np.errstate(divide="ignore", invalid="ignore"):
        var = (mt * w0 - m) ** 2 / (w0 * (1.0 - w0))
    var[~np.isfinite(var)] = 0
    return float(edges[int(np.argmax(var)) + 1])


def class_colours(rgb: np.ndarray, member: np.ndarray, reach: int = 0) -> np.ndarray:
    """Colour field: every texel gets the colour of the nearest texel of the class (flat colours, no blending)."""
    if not member.any():
        return rgb
    # Mean of the class texels in a 5x5 window: evens out 16-bit/DXT quantisation noise inside a glyph.
    acc = np.zeros_like(rgb)
    cnt = np.zeros(member.shape, np.float32)
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            m = shifted(member, dy, dx)
            acc += shifted(rgb, dy, dx) * m[..., None]
            cnt += m
    flat = np.where(member[..., None], acc / np.maximum(cnt, 1.0)[..., None], rgb)
    return fill_transparent(flat, member.astype(np.float32), iterations=reach or max(rgb.shape[:2]))


def components(mask: np.ndarray) -> np.ndarray:
    """8-connected component labels (-1 outside the mask; a label is the flat index of one texel of the component).
    Min-label propagation with pointer jumping (pure numpy, a few dozen passes even for long outlines)."""
    h, w = mask.shape
    big = h * w
    lab = np.where(mask, np.arange(big).reshape(h, w), big)
    while True:
        m = lab
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy or dx:
                    m = np.minimum(m, shifted(lab, dy, dx))
        m = np.where(mask, m, big)
        flat = m.ravel()
        for _ in range(4):  # pointer jumping: follow the label of the texel a label points at
            sel = flat < big
            flat[sel] = flat[flat[sel]]
        m = flat.reshape(h, w)
        if (m == lab).all():
            break
        lab = m
    return np.where(mask, lab, -1)


def two_tone(y: np.ndarray, mask: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray] | None:
    """Two-colour split of the luma inside `mask`: (coverage of the brighter colour 0..1, texels clearly of the
    brighter colour, texels clearly of the darker colour) or None if the area is not two-coloured. Only the clear
    texels give the flat colours (an anti-aliased in-between texel would tint the contour)."""
    vals = y[mask]
    if vals.size < 4:
        return None
    t = otsu(vals)
    lo_v, hi_v = vals[vals < t], vals[vals >= t]
    if lo_v.size == 0 or hi_v.size == 0:
        return None
    # the pure colours (not the class means, which include the anti-aliased in-between texels)
    lo, hi = float(np.percentile(lo_v, 10)), float(np.percentile(hi_v, 90))
    if hi - lo < 0.2:
        return None
    cov = np.clip((y - lo) / (hi - lo), 0.0, 1.0) * mask
    bright, dark = (cov >= 0.85) & mask, (cov <= 0.15) & mask
    if not bright.any() or not dark.any():
        bright, dark = (y >= t) & mask, (y < t) & mask
    return cov, bright, dark


def two_tone_error(rgb: np.ndarray, cov: np.ndarray, bright: np.ndarray, dark: np.ndarray,
                   mask: np.ndarray) -> float:
    """RMS colour error of the model "dark colour + (bright - dark) * coverage" over the mask (0 = pure two-colour
    text; icons with gradients or several colours give large values)."""
    cb = rgb[bright].mean(axis=0)
    cd = rgb[dark].mean(axis=0)
    model = cd + (cb - cd) * cov[mask][:, None]
    return float(np.sqrt(((rgb[mask] - model) ** 2).sum(axis=1).mean()))


def trace_upscale(rgba8: np.ndarray, scale: int) -> tuple[np.ndarray | None, str]:
    """method trace: redraws text instead of filtering it.
    Coverage = alpha (font atlas, text decals) or, for opaque two-colour textures, the ink colour of a luma split.
    Inside each glyph (8-connected part of the alpha) a second two-colour split (white core / dark outline) is traced
    the same way; a part that is not two-coloured (the coloured button icons in the font atlas) keeps a smooth colour
    (Lanczos, clamped) inside its traced alpha contour. Flat colours come from the clear texels of their class."""
    h, w = rgba8.shape[:2]
    W, H = w * scale, h * scale
    a8 = rgba8[..., 3]
    kind = classify_alpha(a8)
    rgb = rgba8[..., :3].astype(np.float32) / 255.0
    alpha = a8.astype(np.float32) / 255.0
    y = luma(rgb)

    if kind == "none":
        everything = np.ones((h, w), bool)
        split = two_tone(y, everything)
        if split is None or two_tone_error(rgb, *split, everything) > 0.12:
            return None, kind
        cov, bright, dark = split
        f = trace_coverage(cov, W, H, scale, everything)
        cb = resize(class_colours(rgb, bright), W, H, Image.BILINEAR)
        cd = resize(class_colours(rgb, dark), W, H, Image.BILINEAR)
        up = cd + (cb - cd) * f[..., None]
        a = np.ones((H, W), np.float32)
    else:
        seen = alpha > 1.0 / 255.0
        grown = seen.copy()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                grown = grown | shifted(seen, dy, dx)
        a = trace_coverage(alpha, W, H, scale, grown)
        if kind == "binary":
            a = (a >= 0.5).astype(np.float32)
        solid = alpha > 0.25
        cov = np.zeros((h, w), np.float32)
        bright = np.zeros((h, w), bool)
        dark = np.zeros((h, w), bool)
        smooth = np.zeros((h, w), bool)  # parts that keep a smooth colour
        labels = components(grown)
        for lab in np.unique(labels[solid]):
            part = labels == lab
            core = part & solid
            split = two_tone(y, core)
            if split is None or two_tone_error(rgb, *split, core) > 0.12:
                smooth |= part
                continue
            cov += split[0]
            bright |= split[1]
            dark |= split[2]
        filled = fill_transparent(rgb, alpha)
        soft = anti_ringing(resize(filled, W, H), filled, W, H)
        if bright.any() and dark.any():
            f = trace_coverage(cov, W, H, scale, grown & ~smooth)
            cb = resize(class_colours(rgb, bright), W, H, Image.BILINEAR)
            cd = resize(class_colours(rgb, dark), W, H, Image.BILINEAR)
            up = cd + (cb - cd) * f[..., None]
        else:
            up = soft
        if smooth.any():
            k = resize(smooth.astype(np.float32), W, H, Image.NEAREST)[..., None]
            up = up * (1.0 - k) + soft * k
    out = np.concatenate([np.clip(up, 0.0, 1.0), a[..., None]], axis=2)
    return (out * 255.0 + 0.5).astype(np.uint8), kind


def box_blur(a: np.ndarray, r: int) -> np.ndarray:
    """Mean over a (2r+1)^2 window with edge replication (2D)."""
    p = np.pad(a, r, mode="edge").astype(np.float64)
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    k = 2 * r + 1
    h, w = a.shape
    s = c[k:k + h, k:k + w] - c[0:h, k:k + w] - c[k:k + h, 0:w] + c[0:h, 0:w]
    return (s / (k * k)).astype(np.float32)


def grow(mask: np.ndarray, r: int) -> np.ndarray:
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dy or dx:
                out |= shifted(mask, dy, dx)
    return out


def find_letters(rgb: np.ndarray, rects: list | None = None,
                 groups: list | None = None) -> list[tuple[np.ndarray, np.ndarray, bool]]:
    """Printed text in a picture (loading screens): connected parts that are much darker (or brighter) than their
    surroundings, letter-sized (4..26 texels high, at most 34 wide) and standing in a row of at least three such
    parts (a part of touching letters counts by its width). Returns (letter mask, loose ink mask of that polarity,
    dark ink) per polarity that has text. Big logo lettering with outlines and the line
    art of the picture are not matched (too large / not in rows) and stay with the picture method."""
    y = luma(rgb)
    bg = box_blur(y, 7)
    found = []
    for dark in (True, False):
        ink = (bg - y > 0.22) if dark else (y - bg > 0.22)
        if ink.sum() < 12:
            continue
        lab = components(ink)
        ids, inv = np.unique(lab[ink], return_inverse=True)
        ys, xs = np.nonzero(ink)
        n = ids.size
        y0 = np.full(n, 1 << 30); y1 = np.full(n, -1); x0 = np.full(n, 1 << 30); x1 = np.full(n, -1)
        np.minimum.at(y0, inv, ys); np.maximum.at(y1, inv, ys)
        np.minimum.at(x0, inv, xs); np.maximum.at(x1, inv, xs)
        area = np.bincount(inv, minlength=n)
        bh, bw = y1 - y0 + 1, x1 - x0 + 1
        fill = area / (bh * bw)
        # print sits on paper: the texels around a part (2 texels out) are one plain colour, unlike the shadow and
        # outline pieces of logo lettering or of the drawing, which border two or more colours
        near_lab = np.full(y.shape, -1)
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                near_lab = np.maximum(near_lab, shifted(lab, dy, dx))
        loose = (bg - y > 0.08) if dark else (y - bg > 0.08)
        ring = (near_lab >= 0) & ~grow(ink | loose, 1)  # without the anti-aliased fringe
        ring_id = np.searchsorted(ids, near_lab[ring])
        ry = y[ring].astype(np.float64)
        rn = np.bincount(ring_id, minlength=n)
        rs = np.bincount(ring_id, ry, minlength=n)
        rq = np.bincount(ring_id, ry * ry, minlength=n)
        ring_std = np.sqrt(np.maximum(rq / np.maximum(rn, 1) - (rs / np.maximum(rn, 1)) ** 2, 0.0))
        cand = (bh >= 4) & (bh <= 26) & (bw <= 12 * bh) & (bw <= 220) & (area >= 4) & (fill <= 0.85) & \
            (rn >= bh + bw) & (ring_std < 0.12)
        if not cand.any():
            continue
        # rows: candidate boxes grown sideways by about half a letter height must chain into groups of >= 3
        boxes = np.zeros(y.shape, bool)
        for i in np.nonzero(cand)[0]:
            g = max(2, int(bh[i]) // 2)
            boxes[y0[i]:y1[i] + 1, max(0, x0[i] - g):x1[i] + 1 + g] = True
        glab = components(boxes)
        group_of = glab[y0, x0]  # top-left texel of each box lies in its own (grown) box
        keep = np.zeros(n, bool)
        for gid in np.unique(group_of[cand]):
            members = cand & (group_of == gid)
            # touching letters of a word form one part: count letters by width (letters are about 0.7 high wide)
            if np.maximum(1, np.round(bw[members] / (0.7 * bh[members]))).sum() < 3:
                continue
            gw = x1[members].max() - x0[members].min() + 1
            gh = y1[members].max() - y0[members].min() + 1
            hs = bh[members]
            if gw >= 2.5 * np.median(hs) and gh <= 2.2 * hs.max():
                box = (int(x0[members].min()), int(y0[members].min()), int(x1[members].max()) + 1,
                       int(y1[members].max()) + 1)
                if groups is not None:
                    groups.append((box, dark))
                cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
                if rects is None or any(r[0] <= cx < r[2] and r[1] <= cy < r[3] for r in rects):
                    keep |= members
        if keep.any():
            letters = np.zeros(y.shape, bool)
            letters[ys[keep[inv]], xs[keep[inv]]] = True
            found.append((letters, loose, dark))
    return found


def envelope(rgb: np.ndarray, y: np.ndarray, r: int, brightest: bool) -> np.ndarray:
    """Per texel the mean colour of the texels within a (2r+1)^2 window whose luma is within 0.06 of the window's
    brightest (or darkest) texel: the clean paper colour around dark print (or the print colour), following the
    gradients of the picture without the bright bias of a plain maximum."""
    sign = 1.0 if brightest else -1.0
    ext = y * sign
    shifts = [(dy, dx) for dy in range(-r, r + 1) for dx in range(-r, r + 1)]
    for dy, dx in shifts:
        ext = np.maximum(ext, shifted(y, dy, dx) * sign)
    acc = np.zeros_like(rgb)
    cnt = np.zeros(y.shape, np.float32)
    for dy, dx in shifts:
        take = (shifted(y, dy, dx) * sign >= ext - 0.06).astype(np.float32)
        acc += shifted(rgb, dy, dx) * take[..., None]
        cnt += take
    return acc / np.maximum(cnt, 1.0)[..., None]


def text_overlay(rgba8: np.ndarray, up8: np.ndarray, scale: int, rects: list | None = None) -> tuple[np.ndarray, int]:
    """Redraws the printed text of a picture on top of its upscale (AI or cartoon). Around the letters (4 texels,
    feathered) the picture is replaced by: paper colour (envelope of the original) + letters traced from the
    original's coverage (SDF, one-pixel ramp) in the print colour. Returns the image and the number of letter texels
    (0 = no text found)."""
    h, w = rgba8.shape[:2]
    H, W = up8.shape[:2]
    if H != h * scale or W != w * scale:
        return up8, 0
    if rects:
        # work on the part of the picture around the rectangles only (12 texels margin for the paper estimate)
        x0 = max(0, min(r[0] for r in rects) - 12)
        y0 = max(0, min(r[1] for r in rects) - 12)
        x1 = min(w, max(r[2] for r in rects) + 12)
        y1 = min(h, max(r[3] for r in rects) + 12)
        if (x0, y0, x1, y1) != (0, 0, w, h):
            part, n = text_overlay(rgba8[y0:y1, x0:x1], up8[y0 * scale:y1 * scale, x0 * scale:x1 * scale], scale,
                                   [(r[0] - x0, r[1] - y0, r[2] - x0, r[3] - y0) for r in rects])
            res = up8.copy()
            res[y0 * scale:y1 * scale, x0 * scale:x1 * scale] = part
            return res, n
    rgb = rgba8[..., :3].astype(np.float32) / 255.0
    out = up8[..., :3].astype(np.float32) / 255.0
    y = luma(rgb)
    total = 0
    for letters, _loose, dark in find_letters(rgb, rects):
        region = grow(letters, 4)
        # paper: the picture's own colour next to the print, filled in under the letters and lightly smoothed
        paper = class_colours(rgb, ~grow(_loose | letters, 1), reach=8)
        paper = np.stack([box_blur(paper[..., c], 1) for c in range(3)], axis=2)
        ink = envelope(rgb, y, 2, brightest=not dark)
        yp, yi = luma(paper), luma(ink)
        span = yp - yi
        ok = np.abs(span) > 0.1
        cov = np.where(ok, np.clip((yp - y) / np.where(ok, span, 1.0), 0.0, 1.0), 0.0) * region
        f = trace_coverage(cov.astype(np.float32), W, H, scale, region)[..., None]
        drawn = resize(paper, W, H, Image.BILINEAR) * (1.0 - f) + resize(ink, W, H, Image.BILINEAR) * f
        weight = box_blur(grow(letters, 2).astype(np.float32), 2)  # 1 near the letters, 0 from 5 texels away
        n = np.clip(resize(weight, W, H, Image.BILINEAR), 0.0, 1.0)[..., None]
        out = out * (1.0 - n) + drawn * n
        total += int(letters.sum())
    res = up8.copy()
    res[..., :3] = (np.clip(out, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    return res, total


# ---- mips and BC1/BC3 encoding --------------------------------------------------------------------------------------

def next_mip(rgba8: np.ndarray, binary_alpha: bool) -> np.ndarray:
    """2x2 box (odd axis: the last row/column is repeated); binary alpha stays binary (alpha test)."""
    a = rgba8.astype(np.float32)
    h, w = a.shape[:2]
    if h > 1 and h % 2:
        a = np.concatenate([a, a[-1:]], axis=0)
    if w > 1 and w % 2:
        a = np.concatenate([a, a[:, -1:]], axis=1)
    h2, w2 = max(1, a.shape[0] // 2), max(1, a.shape[1] // 2)
    fy, fx = a.shape[0] // h2, a.shape[1] // w2
    blocks = a.reshape(h2, fy, w2, fx, 4)
    al = blocks[..., 3]
    asum = al.sum(axis=(1, 3))
    # Plain box for the colour, like the mip levels of the original textures: an alpha-weighted mean would move
    # the visible colour into the transparent texels, which additively drawn lamps and glows show.
    col = blocks[..., :3].mean(axis=(1, 3))
    alpha = asum / (fy * fx)
    if binary_alpha:
        alpha = np.where(alpha >= 127.5, 255.0, 0.0)
    return np.clip(np.concatenate([col, alpha[..., None]], axis=2) + 0.5, 0, 255).astype(np.uint8)


def to_blocks(rgba8: np.ndarray) -> np.ndarray:
    h, w = rgba8.shape[:2]
    bh, bw = (h + 3) // 4, (w + 3) // 4
    pad = np.pad(rgba8, ((0, bh * 4 - h), (0, bw * 4 - w), (0, 0)), mode="edge")
    return pad.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, 4).astype(np.float32)


def pack565(c: np.ndarray) -> np.ndarray:
    r = np.clip(np.round(c[..., 0] * 31 / 255), 0, 31).astype(np.uint32)
    g = np.clip(np.round(c[..., 1] * 63 / 255), 0, 63).astype(np.uint32)
    b = np.clip(np.round(c[..., 2] * 31 / 255), 0, 31).astype(np.uint32)
    return (r << 11) | (g << 5) | b


def unpack565(v: np.ndarray) -> np.ndarray:
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1).astype(np.float32)


def color_endpoints(px: np.ndarray, mask: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Principal-axis endpoints of the masked texels of each block (N x 16 x 3, mask N x 16)."""
    m = mask[..., None].astype(np.float32)
    n = np.maximum(m.sum(axis=1), 1.0)
    mean = (px * m).sum(axis=1) / n
    d = (px - mean[:, None, :]) * m
    cov = np.einsum("nki,nkj->nij", d, d)
    axis = np.ones((px.shape[0], 3), np.float32) / np.sqrt(3.0)
    for _ in range(6):
        axis = np.einsum("nij,nj->ni", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    t = np.einsum("nki,ni->nk", px - mean[:, None, :], axis)
    tmin = np.where(mask, t, np.inf).min(axis=1)
    tmax = np.where(mask, t, -np.inf).max(axis=1)
    tmin = np.where(np.isfinite(tmin), tmin, 0.0)
    tmax = np.where(np.isfinite(tmax), tmax, 0.0)
    return mean + axis * tmax[:, None], mean + axis * tmin[:, None]


def nearest(px: np.ndarray, palette: np.ndarray) -> np.ndarray:
    d = ((px[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)
    return d.argmin(axis=2)


def color_candidate(px: np.ndarray, c0: np.ndarray, c1: np.ndarray, three_color: np.ndarray,
                    transparent: np.ndarray):
    """565 endpoints in the order the mode needs, indices and squared error of each block for endpoints c0/c1."""
    e0, e1 = pack565(c0), pack565(c1)
    # 4-colour mode needs e0 > e1, 3-colour mode e0 <= e1.
    swap = np.where(three_color, e0 > e1, e0 < e1)
    e0, e1 = np.where(swap, e1, e0), np.where(swap, e0, e1)
    q0, q1 = unpack565(e0), unpack565(e1)
    pal4 = np.stack([q0, q1, (2 * q0 + q1) / 3, (q0 + 2 * q1) / 3], axis=1)
    pal3 = np.stack([q0, q1, (q0 + q1) / 2, np.full_like(q0, 1e6)], axis=1)
    equal = (e0 == e1) & ~three_color  # e0 == e1 decodes in 3-colour mode: only index 0 is safe
    pal = np.where(three_color[:, None, None], pal3, pal4)
    pal = np.where(equal[:, None, None], np.stack([q0, q0, q0, q0], axis=1), pal)
    idx = nearest(px, pal)
    idx = np.where(equal[:, None], 0, idx)
    idx = np.where(transparent, 3, idx)
    chosen = np.take_along_axis(pal, np.minimum(idx, 2)[..., None].repeat(3, axis=2), axis=1)
    err = (((chosen - px) ** 2).sum(axis=2) * ~transparent).sum(axis=1)
    return e0, e1, q0, q1, idx, err


def encode_color(blocks: np.ndarray, three_color: np.ndarray, transparent: np.ndarray) -> bytes:
    """BC1 colour blocks. three_color: blocks that need punch-through alpha (index 3 = transparent). Per block the
    best (squared error) of: principal-axis endpoints, bounding-box endpoints, and a least-squares refinement of the
    better one."""
    px = blocks[..., :3]
    opaque = ~transparent
    cands = [color_endpoints(px, opaque)]
    lo = np.where(opaque[..., None], px, np.inf).min(axis=1)
    hi = np.where(opaque[..., None], px, -np.inf).max(axis=1)
    lo = np.where(np.isfinite(lo), lo, 0.0)
    hi = np.where(np.isfinite(hi), hi, 0.0)
    cands.append((hi, lo))
    best = None
    w4 = np.array([1.0, 0.0, 2.0 / 3.0, 1.0 / 3.0], np.float32)
    w3 = np.array([1.0, 0.0, 0.5, 0.0], np.float32)
    for round_ in range(3):
        c0, c1 = cands[round_] if round_ < 2 else refined
        e0, e1, q0, q1, idx, err = color_candidate(px, c0, c1, three_color, transparent)
        if best is None:
            best = [e0, e1, idx, err, q0, q1]
        else:
            better = err < best[3]
            best = [np.where(better, e0, best[0]), np.where(better, e1, best[1]),
                    np.where(better[:, None], idx, best[2]), np.where(better, err, best[3]),
                    np.where(better[:, None], q0, best[4]), np.where(better[:, None], q1, best[5])]
        if round_ == 1:
            # one least-squares step from the best so far: the endpoints that reproduce the opaque texels best
            idx, q0, q1 = best[2], best[4], best[5]
            weights = np.where(three_color[:, None], w3[idx], w4[idx])
            m = (opaque & (idx != 3)).astype(np.float32)
            wa = weights * m
            wb = (1.0 - weights) * m
            a00, a01, a11 = (wa * wa).sum(1), (wa * wb).sum(1), (wb * wb).sum(1)
            b0, b1 = (wa[..., None] * px).sum(1), (wb[..., None] * px).sum(1)
            det = a00 * a11 - a01 * a01
            ok = np.abs(det) > 1e-3
            sdet = np.where(ok, det, 1.0)
            n0 = (a11[:, None] * b0 - a01[:, None] * b1) / sdet[:, None]
            n1 = (a00[:, None] * b1 - a01[:, None] * b0) / sdet[:, None]
            refined = (np.where(ok[:, None], np.clip(n0, 0, 255), q0), np.where(ok[:, None], np.clip(n1, 0, 255), q1))
    e0, e1, idx = best[0], best[1], best[2]
    bits = np.zeros(len(px), np.uint32)
    for i in range(16):
        bits |= idx[:, i].astype(np.uint32) << (2 * i)
    out = np.zeros((len(px), 2), np.uint32)
    out[:, 0] = e0 | (e1 << 16)
    out[:, 1] = bits
    return out.astype("<u4").tobytes()


def encode_alpha(blocks: np.ndarray) -> bytes:
    """BC3 alpha blocks (8-value mode, a0 > a1)."""
    a = blocks[..., 3]
    a0 = np.round(a.max(axis=1))
    a1 = np.round(a.min(axis=1))
    equal = a0 == a1
    steps = np.array([0, 7, 1, 2, 3, 4, 5, 6], np.float32)  # index -> weight of a1 in sevenths
    pal = (a0[:, None] * (7 - steps) + a1[:, None] * steps) / 7.0
    idx = np.abs(a[:, :, None] - pal[:, None, :]).argmin(axis=2)
    idx = np.where(equal[:, None], 0, idx)
    out = bytearray()
    a0b = a0.astype(np.uint8)
    a1b = a1.astype(np.uint8)
    bits = np.zeros(len(a), np.uint64)
    for i in range(16):
        bits |= idx[:, i].astype(np.uint64) << np.uint64(3 * i)
    arr = np.zeros((len(a), 8), np.uint8)
    arr[:, 0] = a0b
    arr[:, 1] = a1b
    for k in range(6):
        arr[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(0xFF)).astype(np.uint8)
    out += arr.tobytes()
    return bytes(out)


def encode_level(rgba8: np.ndarray, fmt: str) -> bytes:
    blocks = to_blocks(rgba8)
    if fmt == "DXT1":
        transparent = blocks[..., 3] < 128
        three = transparent.any(axis=1)
        return encode_color(blocks, three, transparent)
    color = encode_color(blocks, np.zeros(len(blocks), bool), np.zeros(blocks.shape[:2], bool))
    alpha = encode_alpha(blocks)
    c = np.frombuffer(color, np.uint8).reshape(-1, 8)
    a = np.frombuffer(alpha, np.uint8).reshape(-1, 8)
    return np.concatenate([a, c], axis=1).tobytes()


def write_dds(path: Path, levels: list[bytes], width: int, height: int, fmt: str) -> None:
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
    caps = 0x1000 | (0x8 | 0x400000 if len(levels) > 1 else 0)
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, height, width, len(levels[0]), 0, len(levels))
    pf = struct.pack("<II4sIIIII", 32, 0x4, fmt.encode(), 0, 0, 0, 0, 0)
    tail = struct.pack("<IIIII", caps, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(header + pf + tail)
        for lv in levels:
            f.write(lv)


def save_pack_file(rgba8: np.ndarray, kind: str, out_base: Path, fmt: str) -> Path:
    h, w = rgba8.shape[:2]
    if fmt == "png" or w % 4 or h % 4:
        path = out_base.with_suffix(".png")
        Image.fromarray(rgba8, "RGBA").save(path, optimize=False, compress_level=6)
        out_base.with_suffix(".dds").unlink(missing_ok=True)  # the game prefers .dds: drop a stale one
        return path
    # BC1 only where its punch-through black under transparent texels is what the original has: opaque textures
    # and binary alpha whose transparent texels are black. Everything else BC3 (keeps the original colour under
    # alpha 0 for additive drawing; DXT3/ARGB4444 originals often have white or coloured texels there).
    bc = "DXT1"
    if kind == "smooth" or (kind == "binary" and (rgba8[rgba8[..., 3] < 128][:, :3] > 8).any()):
        bc = "DXT5"
    levels = []
    level = rgba8
    while True:
        levels.append(encode_level(level, bc))
        if level.shape[0] == 1 and level.shape[1] == 1:
            break
        level = next_mip(level, kind == "binary")
    path = out_base.with_suffix(".dds")
    write_dds(path, levels, w, h, bc)
    out_base.with_suffix(".png").unlink(missing_ok=True)
    return path


# ---- AI batch (method ai) ------------------------------------------------------------------------------------------

def run_ai_batch(sources: list[Path], opts: dict, args, tmp: Path) -> Path:
    """Writes the alpha-filled colour of every texture to upscale into tmp/in, runs the upscaler once over the folder
    and returns tmp/out (<hash>.png). Models without a native 2x/3x (all but realesr-animevideov3) run at 4x; the
    result is resized to the target size per texture."""
    src_dir, out_dir = tmp / "in", tmp / "out"
    src_dir.mkdir()
    out_dir.mkdir()
    n = 0
    for p in sources:
        name = parse_hash(p.stem)
        if name in opts["trace"] and opts["text_method"] != "ai":
            continue
        rgba = np.asarray(Image.open(p).convert("RGBA"), np.uint8)
        if skip_reason(rgba, opts)[0] is not None:
            continue
        rgb = fill_transparent(rgba[..., :3].astype(np.float32) / 255.0, rgba[..., 3].astype(np.float32) / 255.0)
        Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").save(src_dir / f"{name}.png")
        n += 1
    if args.method == "waifu2x":
        # waifu2x-ncnn-vulkan: -n noise level (-1..3), -s 2/4, -m model folder (models-cunet, ...)
        scale = args.scale
        label = f"waifu2x {args.w2x_model} noise {args.w2x_noise}"
        cmd = [args.w2x_exe, "-i", str(src_dir), "-o", str(out_dir), "-n", str(args.w2x_noise), "-s", str(scale),
               "-m", str(args.w2x_models), "-f", "png"]
    else:
        native = (args.ai_models / f"{args.ai_model}-x{args.scale}.param").is_file()
        scale = args.scale if native else 4
        label = f"AI {args.ai_model}"
        cmd = [args.ai_exe, "-i", str(src_dir), "-o", str(out_dir), "-n", args.ai_model, "-s", str(scale),
               "-m", str(args.ai_models), "-f", "png"]
    print(f"texture_upscale: {label} x{scale} on {n} textures ...", flush=True)
    start = time.time()
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors="replace")
    done = len(list(out_dir.glob("*.png")))
    print(f"texture_upscale: {label} done: {done}/{n} in {time.time() - start:.1f} s (exit {r.returncode})",
          flush=True)
    if r.returncode != 0:
        print(r.stderr[-1500:], file=sys.stderr)
    return out_dir


# ---- driver ----------------------------------------------------------------------------------------------------

def skip_reason(rgba: np.ndarray, opts: dict) -> tuple[str | None, int]:
    """Why a texture stays original (None = upscale) and its scale (reduced by --max-size)."""
    h, w = rgba.shape[:2]
    if max(w, h) < opts["min_size"]:
        return f"{w}x{h} smaller than --min-size", 0
    if (rgba == rgba[0, 0]).all():
        return "single colour", 0
    scale = opts["scale"]
    while scale > 1 and max(w, h) * scale > opts["max_size"]:
        scale //= 2
    if scale < 2:
        return f"{w}x{h} already at --max-size", 0
    return None, scale


def process(job: tuple) -> tuple[str, str, str]:
    src, out_dir, opts = job
    name = parse_hash(src.stem)
    try:
        img = Image.open(src).convert("RGBA")
        rgba = np.asarray(img, np.uint8)
        h, w = rgba.shape[:2]
        reason, scale = skip_reason(rgba, opts)
        if reason is not None:
            return name, "skip", reason
        text_method = "trace" if opts["method"] == "trace" else opts["text_method"]
        if opts["method"] == "trace" or name in opts["trace"]:
            if text_method == "original":
                return name, "skip", "text texture kept original (--text-method original)"
            if text_method == "conservative":
                up, kind = text_upscale(rgba, scale, opts["text_sharpen"])
                path = save_pack_file(up, kind, out_dir / name, "png")
                return name, "ok", f"{w}x{h} -> {up.shape[1]}x{up.shape[0]} {kind} alpha text 2x -> {path.name}"
            up, kind = trace_upscale(rgba, scale)
            if up is not None:
                # PNG: lossless like the uncompressed 16-bit font originals (BC would smear the thin outlines)
                path = save_pack_file(up, kind, out_dir / name, "png")
                return name, "ok", f"{w}x{h} -> {up.shape[1]}x{up.shape[0]} {kind} alpha traced -> {path.name}"
            if opts["method"] == "trace":
                return name, "skip", "not text (no alpha and not two-coloured)"
        ai = None
        if opts["method"] in ("ai", "waifu2x"):
            ai_file = opts["ai_dir"] / f"{name}.png"
            if not ai_file.is_file():
                return name, "error", "no AI result (see the upscaler output above)"
            ai = (ai_file, opts["ai_mix"])
        up, kind = upscale(rgba, scale, opts["method"], opts["strength"], opts["edge"], opts["deblock"],
                           opts.get("external"), ai, opts["detail"])
        note = ""
        if name in opts["screens"] and kind == "none":
            up, letters = text_overlay(rgba, up, scale, TEXT_RECTS.get(name))
            note = f", text redrawn ({letters} letter texels)" if letters else ""
        path = save_pack_file(up, kind, out_dir / name, opts["format"])
        return name, "ok", f"{w}x{h} -> {up.shape[1]}x{up.shape[0]} {kind} alpha{note} -> {path.name}"
    except Exception as e:  # noqa: BLE001 - reported per file
        return name, "error", str(e)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dump", type=Path, help="texture dump folder (<userdata>/texture_dump)")
    ap.add_argument("pack", type=Path, help="output pack folder ([render] texture_pack)")
    ap.add_argument("--scale", type=int, choices=(2, 4), default=2)
    ap.add_argument("--method", choices=("cartoon", "lanczos", "external", "ai", "waifu2x", "trace"),
                    default="cartoon")
    ap.add_argument("--format", choices=("dds", "png"), default="dds")
    ap.add_argument("--strength", type=float, default=0.5, help="edge tightening 0..1 (cartoon; 0 = Lanczos look)")
    ap.add_argument("--edge", type=float, default=0.06, help="luma range that counts as an edge (cartoon)")
    ap.add_argument("--deblock", type=float, default=0.05, help="range sigma of the DXT noise smoothing (0 = off)")
    ap.add_argument("--min-size", type=int, default=16, help="skip textures whose larger side is below this")
    ap.add_argument("--max-size", type=int, default=2048, help="largest output side (the scale is reduced)")
    ap.add_argument("--only", default="", help="comma-separated hashes to process")
    ap.add_argument("--exclude", type=Path, help="file with hashes (one per line) to leave original")
    ap.add_argument("--limit", type=int, default=0, help="process at most N textures (tests)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--overwrite", action="store_true", help="replace existing pack files")
    ap.add_argument("--external-exe", help="AI upscaler you installed yourself (method external)")
    ap.add_argument("--external-args", default="-s {scale}", help="extra arguments, {scale} is replaced")
    ap.add_argument("--external-mix", type=float, default=0.7, help="share of the AI result 0..1")
    ap.add_argument("--ai-exe", default=os.environ.get("FUT_REALESRGAN", ""),
                    help="realesrgan-ncnn-vulkan.exe you installed yourself (method ai; default: %%FUT_REALESRGAN%%)")
    ap.add_argument("--ai-model", default="realesr-animevideov3", help="model name in the models folder (method ai)")
    ap.add_argument("--ai-models", type=Path, help="models folder (default: 'models' next to --ai-exe)")
    ap.add_argument("--ai-mix", type=float, default=0.75, help="share of the AI result 0..1 (method ai)")
    ap.add_argument("--w2x-exe", default=os.environ.get("FUT_WAIFU2X", ""),
                    help="waifu2x-ncnn-vulkan.exe you installed yourself (method waifu2x; default: %%FUT_WAIFU2X%%)")
    ap.add_argument("--w2x-model", default="cunet",
                    help="model folder next to --w2x-exe without 'models-': cunet (default), "
                         "upconv_7_anime_style_art_rgb, upconv_7_photo")
    ap.add_argument("--w2x-models", type=Path, help="model folder (default: models-<--w2x-model> next to --w2x-exe)")
    ap.add_argument("--w2x-noise", type=int, choices=(-1, 0, 1, 2, 3), default=1,
                    help="waifu2x denoise level (-1 off .. 3 strong; default 1)")
    ap.add_argument("--w2x-mix", type=float, default=1.0,
                    help="share of the waifu2x result 0..1 (method waifu2x; the rest is the cartoon method)")
    ap.add_argument("--text-method", choices=("conservative", "trace", "original", "ai"), default="conservative",
                    help="font atlas and text textures (--trace-list): conservative = clean 2x with the original "
                         "stroke weight (default), trace = redrawn from a distance field (crisper, bolder outline), "
                         "original = left out of the pack, ai = like every other texture")
    ap.add_argument("--text-sharpen", type=float, default=0.8, help="unsharp amount of --text-method conservative")
    ap.add_argument("--trace-list", default="builtin",
                    help="hashes (file, one per line) redrawn with method trace whatever --method says; 'builtin' = "
                         "the game's font atlas and text textures (TEXT_HASHES), 'none' = off")
    ap.add_argument("--text-overlay", choices=("builtin", "auto", "none"), default="none",
                    help="redraw the printed text of screen pictures with the trace method on top of the picture "
                         "upscale: builtin = the game's loading and front-end screens (TEXT_RECTS), auto = every "
                         "16-bit RGB texture of at least 256x256 in index.tsv (other dumps), none = off "
                         "(default: waifu2x keeps the printed text clean by itself, the redraw made it bolder)")
    ap.add_argument("--ai-detail", type=int, default=None,
                    help="back-projection passes that bring back the original's fine painted detail (methods ai, "
                         "waifu2x and external; default 1 for ai/external, 0 for waifu2x, which keeps it itself)")
    args = ap.parse_args(argv)

    if not args.dump.is_dir():
        print(f"texture_upscale: dump folder {args.dump} not found", file=sys.stderr)
        return 2
    if args.method == "external":
        if not args.external_exe or not Path(args.external_exe).is_file():
            print("texture_upscale: --method external needs --external-exe <your installed upscaler .exe>",
                  file=sys.stderr)
            return 2
    if args.ai_detail is None:
        args.ai_detail = 0 if args.method == "waifu2x" else 1
    if args.method == "waifu2x":
        if not args.w2x_exe or not Path(args.w2x_exe).is_file():
            print("texture_upscale: --method waifu2x needs --w2x-exe <your waifu2x-ncnn-vulkan.exe> (or FUT_WAIFU2X)",
                  file=sys.stderr)
            return 2
        if args.w2x_models is None:
            args.w2x_models = Path(args.w2x_exe).parent / f"models-{args.w2x_model}"
        if not args.w2x_models.is_dir():
            print(f"texture_upscale: waifu2x model folder {args.w2x_models} not found", file=sys.stderr)
            return 2
    if args.method == "ai":
        if not args.ai_exe or not Path(args.ai_exe).is_file():
            print("texture_upscale: --method ai needs --ai-exe <your realesrgan-ncnn-vulkan.exe> (or FUT_REALESRGAN)",
                  file=sys.stderr)
            return 2
        if args.ai_models is None:
            args.ai_models = Path(args.ai_exe).parent / "models"
        if not (args.ai_models / f"{args.ai_model}-x4.param").is_file() and \
                not (args.ai_models / f"{args.ai_model}.param").is_file():
            print(f"texture_upscale: model {args.ai_model} not found in {args.ai_models}", file=sys.stderr)
            return 2
    repo = Path(__file__).resolve().parent.parent
    try:
        rel = args.pack.resolve().relative_to(repo)
        if rel.parts and rel.parts[0] not in ("run", "build", "out", "dist"):
            print(f"texture_upscale: warning: {args.pack} lies inside the repository - packs contain game artwork "
                  "and must never be committed (use run/ or your user data)", file=sys.stderr)
    except ValueError:
        pass
    args.pack.mkdir(parents=True, exist_ok=True)

    only = {h.strip().lower() for h in args.only.split(",") if h.strip()}
    if args.trace_list == "builtin":
        trace = set(TEXT_HASHES)
    elif args.trace_list == "none":
        trace = set()
    else:
        trace = {parse_hash(l.strip()) for l in Path(args.trace_list).read_text().splitlines() if parse_hash(l.strip())}
    # Screen pictures (loading screens and their 256x256 tiles, front-end screens): uncompressed 16-bit RGB in the
    # dump index, at least 256 x 256. Their printed text is redrawn on top of the picture upscale (text_overlay).
    screens = set(TEXT_RECTS) - trace if args.text_overlay == "builtin" else set()
    index = args.dump / "index.tsv"
    if args.text_overlay == "auto" and index.is_file():
        for line in index.read_text(encoding="utf-8").splitlines()[1:]:
            f = line.split("	")
            if len(f) >= 6 and f[3] == "RGB16" and min(int(f[1]), int(f[2])) >= 256 and f[0] not in trace:
                screens.add(f[0])
    exclude = set()
    if args.exclude:
        exclude = {parse_hash(l.strip()) for l in args.exclude.read_text().splitlines() if parse_hash(l.strip())}
    sources = []
    for p in sorted(args.dump.glob("*.png")):
        h = parse_hash(p.stem)
        if h is None or (only and h not in only) or h in exclude:
            continue
        if not args.overwrite and ((args.pack / f"{h}.dds").exists() or (args.pack / f"{h}.png").exists()):
            continue
        sources.append(p)
    if args.limit:
        sources = sources[: args.limit]
    opts = {"scale": args.scale, "method": args.method, "format": args.format, "strength": args.strength,
            "edge": args.edge, "deblock": args.deblock, "min_size": args.min_size, "max_size": args.max_size,
            "detail": max(0, args.ai_detail), "trace": trace, "screens": screens, "text_method": args.text_method,
            "text_sharpen": args.text_sharpen}
    if args.text_method == "ai":
        opts["trace"] = set()
    if args.method == "external":
        opts["external"] = (args.external_exe, args.external_args, args.external_mix)
    ai_tmp = None
    if args.method in ("ai", "waifu2x") and sources:
        ai_tmp = tempfile.TemporaryDirectory(prefix="fut_ai_")
        opts["ai_dir"] = run_ai_batch(sources, opts, args, Path(ai_tmp.name))
        opts["ai_mix"] = args.ai_mix if args.method == "ai" else args.w2x_mix
    print(f"texture_upscale: {len(sources)} textures, {args.method} x{args.scale} -> {args.pack} ({args.format}, "
          f"{args.jobs} jobs)")
    start = time.time()
    counts = {"ok": 0, "skip": 0, "error": 0}
    jobs = [(p, args.pack, opts) for p in sources]
    if args.jobs > 1 and len(jobs) > 1:
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as ex:
            results = ex.map(process, jobs, chunksize=4)
            for name, status, text in results:
                counts[status] += 1
                if status != "skip":
                    print(f"  {name}: {status} {text}")
    else:
        for job in jobs:
            name, status, text = process(job)
            counts[status] += 1
            if status != "skip":
                print(f"  {name}: {status} {text}")
    if ai_tmp is not None:
        ai_tmp.cleanup()
    ai_info = f"ai model {args.ai_model}\nai mix {args.ai_mix}\n" if args.method == "ai" else ""
    if args.method == "waifu2x":
        ai_info = f"waifu2x model {args.w2x_model}\nwaifu2x noise {args.w2x_noise}\nwaifu2x mix {args.w2x_mix}\n"
    if args.method in ("ai", "external", "waifu2x"):
        ai_info += f"ai detail {max(0, args.ai_detail)}\n"
    ai_info += f"text list {args.trace_list} ({len(trace)}), text method {args.text_method}\n"
    ai_info += f"text overlay {args.text_overlay} ({len(screens)})\n"
    (args.pack / "pack_info.txt").write_text(
        f"texture_upscale.py v{VERSION}\nmethod {args.method}\nscale {args.scale}\nformat {args.format}\n"
        f"strength {args.strength}\nedge {args.edge}\ndeblock {args.deblock}\n{ai_info}", encoding="utf-8")
    print(f"texture_upscale: {counts['ok']} written, {counts['skip']} skipped, {counts['error']} errors "
          f"in {time.time() - start:.1f} s")
    return 1 if counts["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
