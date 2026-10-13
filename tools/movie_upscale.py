"""M6b: build a movie pack - the game's 47 XMV movies upscaled with AI (docs/m6b_report.md, video/movie_pack.h).

For every movie of the disc (\\video\\*.xmv, read from the disc image with tools/xdvdfs.py or from a folder):
  1. decode the WMV2 frames with FFmpeg (its xmv demuxer + wmv2 decoder: same frame count and pts as the port's
     own decoder, docs/m6_report.md V2) to PNG (rgb24, BT.601 limited like the port's YUY2 path),
  2. upscale the folder with Real-ESRGAN ncnn Vulkan you installed yourself (default model realesr-animevideov3 at
     its native 2x: trained on cartoon footage, clean lines, flat colours stay flat, removes the WMV2 blocks),
  3. mix 75 % AI + 25 % a soft Lanczos upscale of the original (--ai-mix; the same recipe as the G2 texture packs:
     keeps small painted details the AI would flatten, "nicht übertrieben"); --ai-mix-for sets it per movie (all 47
     use 0.75 - the burned-in text of the credits, trailers and logos stays clean, docs/m6b_report.md),
  4. encode H.264 (NVENC, constant quality, yuv420p, BT.601 tags, no B-frames, keyframe every --gop frames) with
     EXACTLY the pts of the XMV frames (variable frame rate, mp4 time scale 1000 = ms) - no audio: the port keeps
     playing the XMV's own audio tracks (all 5 languages) and takes only the picture from the pack,
  5. verify the mp4 (frame count, every pts, size) against the XMV and write <pack>/<name>.mp4 + <pack>/index.tsv,
  6. delete the temporary frames (and the extracted XMV) before the next movie.

The port ([render] movie_pack = <pack>) finds a movie by the CRC-32 of the first 0x1000 bytes of its file (the
header the game passes to XMVDecoder_CreateDecoderForPackets), so the pack only applies to the disc it was made from.
Byte-identical movies (Intro = 1-1-00) are upscaled once and share the mp4.

The pack contains derived game artwork: keep it in your user data or under run/, never in the repository.
Everything runs locally and offline (FFmpeg, Real-ESRGAN, Python standard library); nothing is downloaded.

Usage:
  python tools/movie_upscale.py --iso <disc image> --pack <folder> --ai-exe <realesrgan-ncnn-vulkan.exe>
         [--xmv-dir <folder with *.xmv> instead of --iso] [--ai-model realesr-animevideov3] [--ai-mix 0.75]
         [--ai-mix-for NAME=MIX ...] [--scale 2] [--gpu 0] [--codec h264|hevc] [--cq 18] [--gop 30]
         [--only NAME,...] [--overwrite]
         [--work <temp folder>] [--ffmpeg-dir <folder with ffmpeg.exe/ffprobe.exe>] [--reference-json <json>]
Example (the pack of docs/m6b_report.md: RTX 5090, 17 minutes, 555 MB):
  python tools/movie_upscale.py --iso "Y:\\...\\Futurama (Europe).iso" --pack run\\moviepack_ai_x2 ^
         --ai-exe D:\\pop_upscale\\realesrgan\\realesrgan-ncnn-vulkan.exe --ffmpeg-dir "D:\\Program Files (x86)\\ffmpeg\\bin"
--reference-json: the result of "futurama.exe --decode-movies=<iso> --result=<json>" - the pts of every frame as the
port's own decoder sees them are compared with the mp4 as well.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xdvdfs  # noqa: E402  (repository tool: XDVDFS reader)

INDEX_NAME = "index.tsv"
INDEX_HEADER = ("# Futurama PC movie pack v1 (tools/movie_upscale.py; video/movie_pack.h)\n"
                "# name\theader_crc32\txmv_bytes\tframes\twidth\theight\tduration_ms\tfile\n")
HEADER_BYTES = 0x1000  # the game reads this much of the file before XMVDecoder_CreateDecoderForPackets


class Tools:
    def __init__(self, ffmpeg_dir: str | None):
        d = Path(ffmpeg_dir) if ffmpeg_dir else None
        self.ffmpeg = str(d / "ffmpeg.exe") if d else (shutil.which("ffmpeg") or "ffmpeg")
        self.ffprobe = str(d / "ffprobe.exe") if d else (shutil.which("ffprobe") or "ffprobe")


LOG_FILE: Path | None = None


def log(msg: str) -> None:
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    if LOG_FILE is not None:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")


def run(cmd: list[str], what: str) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"{what} failed (exit {r.returncode}): {(r.stderr or r.stdout)[-800:]}")
    return r


def xmv_header(path: Path) -> tuple[int, int, int, int]:
    """(CRC-32 of the first 0x1000 bytes, width, height, duration ms) of an .xmv file."""
    with open(path, "rb") as f:
        head = f.read(HEADER_BYTES)
    if len(head) < 0x24 or head[0x0C:0x10] != b"xobX":
        raise RuntimeError(f"{path.name}: not an XMV file")
    width, height, duration = struct.unpack_from("<III", head, 0x14)
    return zlib.crc32(head) & 0xFFFFFFFF, width, height, duration


def video_pts(tools: Tools, path: Path) -> list[int]:
    """pts in ms of every video packet (display order = decode order: WMV2 and our H.264 have no B-frames)."""
    out = run([tools.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "packet=pts_time",
               "-of", "csv=p=0", str(path)], "ffprobe").stdout.split()
    return [int(round(float(x) * 1000.0)) for x in out if x and x != "N/A"]


def write_concat(path: Path, folder: Path, pts: list[int], last_gap: int) -> None:
    """ffconcat list of the frames with their exact display durations (image2 at 1000 fps = ms time base)."""
    with open(path, "w", encoding="utf-8") as f:
        f.write("ffconcat version 1.0\n")
        for i, p in enumerate(pts):
            dur = (pts[i + 1] - p) if i + 1 < len(pts) else last_gap
            name = (folder / f"{i + 1:06d}.png").as_posix()
            f.write(f"file '{name}'\noption framerate 1000\nduration {dur / 1000.0:.3f}\n")


def read_index(pack: Path) -> dict[str, list[str]]:
    rows: dict[str, list[str]] = {}
    p = pack / INDEX_NAME
    if p.is_file():
        for line in p.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                cols = line.split("\t")
                if len(cols) >= 8:
                    rows[cols[0]] = cols
    return rows


def write_index(pack: Path, rows: dict[str, list[str]]) -> None:
    tmp = pack / (INDEX_NAME + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(INDEX_HEADER)
        for name in sorted(rows, key=str.lower):
            f.write("\t".join(rows[name]) + "\n")
    os.replace(tmp, pack / INDEX_NAME)


def process(name: str, xmv: Path, args, tools: Tools, reference: dict[str, list[int]]) -> list[str]:
    crc, width, height, duration = xmv_header(xmv)
    pts = video_pts(tools, xmv)
    if not pts:
        raise RuntimeError("no video frames")
    if name + ".xmv" in reference and reference[name + ".xmv"] != pts:
        raise RuntimeError("FFmpeg's pts differ from the port's decoder (--reference-json)")
    last_gap = max(1, pts[-1] - pts[-2]) if len(pts) > 1 else max(1, duration)
    work = Path(args.work) / name
    shutil.rmtree(work, ignore_errors=True)
    src, ai = work / "in", work / "ai"
    src.mkdir(parents=True)
    ai.mkdir()
    t0 = time.time()
    # 1. decode (every frame, no frame rate conversion)
    run([tools.ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(xmv), "-map", "0:v:0", "-fps_mode",
         "passthrough", "-pix_fmt", "rgb24", str(src / "%06d.png")], "decode")
    n = len(list(src.glob("*.png")))
    if n != len(pts):
        raise RuntimeError(f"decoded {n} frames, the file has {len(pts)}")
    t1 = time.time()
    # 2. AI upscale of the whole folder (one upscaler run per movie)
    native = (Path(args.ai_models) / f"{args.ai_model}-x{args.scale}.param").is_file()
    cmd = [args.ai_exe, "-i", str(src), "-o", str(ai), "-n", args.ai_model, "-s", str(args.scale if native else 4),
           "-m", str(args.ai_models), "-g", str(args.gpu), "-j", args.ai_threads, "-f", "png"]
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors="replace")
    n_ai = len(list(ai.glob("*.png")))
    if r.returncode != 0 or n_ai != n:
        raise RuntimeError(f"upscaler: {n_ai}/{n} frames (exit {r.returncode}) {r.stderr[-500:]}")
    t2 = time.time()
    # 3.+4. mix with the soft upscale and encode with the XMV's pts
    W, H = width * args.scale, height * args.scale
    write_concat(work / "ai.ffconcat", ai, pts, last_gap)
    write_concat(work / "in.ffconcat", src, pts, last_gap)
    mix = max(0.0, min(1.0, args.mix_for.get(name.lower(), args.ai_mix)))
    graph = (f"[0:v]scale={W}:{H}:flags=lanczos,format=gbrp[a];[1:v]scale={W}:{H}:flags=lanczos,format=gbrp[b];"
             f"[a][b]blend=all_expr='A*{mix:.3f}+B*{1.0 - mix:.3f}',"
             f"scale=out_color_matrix=bt601:out_range=tv,format=yuv420p[v]")
    codec = "h264_nvenc" if args.codec == "h264" else "hevc_nvenc"
    profile = ["-profile:v", "high"] if args.codec == "h264" else ["-profile:v", "main"]
    out = Path(args.pack) / f"{name}.mp4"
    tmp_out = Path(args.pack) / f"{name}.tmp.mp4"
    run([tools.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
         "-f", "concat", "-safe", "0", "-i", str(work / "ai.ffconcat"),
         "-f", "concat", "-safe", "0", "-i", str(work / "in.ffconcat"),
         "-filter_complex", graph, "-map", "[v]", "-fps_mode", "passthrough",
         "-c:v", codec, "-gpu", str(args.gpu), "-preset", "p7", "-tune", "hq", "-rc", "vbr", "-cq", str(args.cq),
         "-b:v", "0", "-bf", "0", "-g", str(args.gop), *profile,
         "-colorspace", "smpte170m", "-color_primaries", "smpte170m", "-color_trc", "smpte170m", "-color_range", "tv",
         "-video_track_timescale", "1000", "-an", "-movflags", "+faststart", str(tmp_out)], "encode")
    t3 = time.time()
    # 5. verify: frame count and every pts
    got = video_pts(tools, tmp_out)
    if got != pts:
        bad = next((i for i, (a, b) in enumerate(zip(got, pts)) if a != b), min(len(got), len(pts)))
        raise RuntimeError(f"mp4 has {len(got)} frames, XMV {len(pts)}; first difference at frame {bad}")
    os.replace(tmp_out, out)
    # 6. temporary frames
    shutil.rmtree(work, ignore_errors=True)
    size = out.stat().st_size
    log(f"{name}: {n} frames {width}x{height} -> {W}x{H}, AI mix {mix:.2f}, {size / 1e6:.1f} MB; decode {t1 - t0:.0f} s, "
        f"AI {t2 - t1:.0f} s ({n / max(t2 - t1, 0.01):.1f} fps), encode {t3 - t2:.0f} s; pts verified")
    return [name, f"{crc:08X}", str(xmv.stat().st_size), str(n), str(W), str(H), str(pts[-1] + last_gap), out.name]


def main(argv: list[str] | None = None) -> int:
    global LOG_FILE
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--iso", help="disc image (XDVDFS); the movies are extracted one at a time")
    ap.add_argument("--xmv-dir", help="folder with the .xmv files instead of --iso")
    ap.add_argument("--pack", required=True, help="output folder (<name>.mp4 + index.tsv)")
    ap.add_argument("--ai-exe", default=os.environ.get("FUT_REALESRGAN"),
                    help="realesrgan-ncnn-vulkan.exe you installed yourself (default: %%FUT_REALESRGAN%%)")
    ap.add_argument("--ai-models", help="models folder (default: <exe folder>/models)")
    ap.add_argument("--ai-model", default="realesr-animevideov3")
    ap.add_argument("--ai-mix", type=float, default=0.75, help="share of the AI result 0..1 (rest: Lanczos)")
    ap.add_argument("--ai-mix-for", action="append", default=[], metavar="NAME=MIX",
                    help="AI share for one movie (repeatable), e.g. Credits-PAL=0.5 if its text should stay softer")
    ap.add_argument("--ai-threads", default="2:4:4", help="realesrgan -j load:proc:save")
    ap.add_argument("--scale", type=int, default=2, choices=(2, 3, 4))
    ap.add_argument("--gpu", type=int, default=0, help="GPU index for Real-ESRGAN (Vulkan) and NVENC")
    ap.add_argument("--codec", default="h264", choices=("h264", "hevc"),
                    help="h264 (default: Windows decodes it without extensions) or hevc")
    ap.add_argument("--cq", type=int, default=18, help="NVENC constant quality (lower = better, larger)")
    ap.add_argument("--gop", type=int, default=30, help="keyframe interval in frames")
    ap.add_argument("--only", help="comma-separated movie names (without .xmv)")
    ap.add_argument("--overwrite", action="store_true", help="rebuild movies already in the pack")
    ap.add_argument("--work", help="temporary folder (default: <pack>/_work, deleted per movie)")
    ap.add_argument("--ffmpeg-dir", help="folder with ffmpeg.exe and ffprobe.exe (default: PATH)")
    ap.add_argument("--reference-json", help="--decode-movies result of the port: compare its pts too")
    args = ap.parse_args(argv)

    args.mix_for = {}
    for item in args.ai_mix_for:
        key, _, value = item.partition("=")
        try:
            args.mix_for[key.strip().lower()] = float(value)
        except ValueError:
            ap.error(f"--ai-mix-for {item}: expected NAME=MIX")
    if not args.iso and not args.xmv_dir:
        ap.error("--iso or --xmv-dir is required")
    if not args.ai_exe or not Path(args.ai_exe).is_file():
        ap.error("--ai-exe <realesrgan-ncnn-vulkan.exe> (or FUT_REALESRGAN) is required")
    args.ai_models = args.ai_models or str(Path(args.ai_exe).parent / "models")
    pack = Path(args.pack).resolve()
    pack.mkdir(parents=True, exist_ok=True)
    args.pack = str(pack)
    args.work = str(Path(args.work).resolve()) if args.work else str(pack / "_work")
    LOG_FILE = pack / "movie_upscale.log"
    tools = Tools(args.ffmpeg_dir)
    reference: dict[str, list[int]] = {}
    if args.reference_json:
        data = json.loads(Path(args.reference_json).read_text(encoding="utf-8"))
        reference = {f["name"]: list(f["pts"]) for f in data.get("files", [])}

    # Movie list: name -> (source path or XDVDFS entry)
    disc = xdvdfs.XDVDFS(args.iso) if args.iso else None
    movies: dict[str, tuple] = {}
    if disc is not None:
        for path, start, size, is_dir in disc.walk():
            if not is_dir and path.lower().endswith(".xmv"):
                movies[Path(path).stem] = ("disc", start, size)
    else:
        for p in Path(args.xmv_dir).glob("*.xmv"):
            movies[p.stem] = ("file", p)
    names = sorted(movies, key=str.lower)
    if args.only:
        wanted = {n.strip().lower() for n in args.only.split(",") if n.strip()}
        names = [n for n in names if n.lower() in wanted]
    rows = read_index(pack)
    log(f"start: {len(names)} movies, model {args.ai_model} x{args.scale}, AI mix {args.ai_mix}, {args.codec} cq "
        f"{args.cq}, gop {args.gop}, GPU {args.gpu}, pack {pack}")
    start = time.time()
    done = failed = reused = 0
    by_file_crc: dict[int, str] = {}  # CRC of the whole file -> name already in the pack (identical movies)
    for name in names:
        xmv_tmp = None
        try:
            if movies[name][0] == "disc":
                _, sector, size = movies[name]
                xmv_tmp = Path(args.work) / f"{name}.xmv"
                xmv_tmp.parent.mkdir(parents=True, exist_ok=True)
                with open(xmv_tmp, "wb") as out:
                    remaining, offset = size, 0
                    while remaining:
                        chunk = disc.read(sector, min(remaining, 1 << 20), offset)
                        out.write(chunk)
                        offset += len(chunk)
                        remaining -= len(chunk)
                xmv = xmv_tmp
            else:
                xmv = movies[name][1]
            file_crc = zlib.crc32(xmv.read_bytes()) & 0xFFFFFFFF
            twin = by_file_crc.get(file_crc)
            if twin is not None and twin in rows:
                row = list(rows[twin])
                row[0] = name
                rows[name] = row
                reused += 1
                log(f"{name}: byte-identical to {twin} - shares {row[7]}")
            elif not args.overwrite and name in rows and (pack / rows[name][7]).is_file() and \
                    rows[name][1] == f"{xmv_header(xmv)[0]:08X}":
                log(f"{name}: already in the pack")
            else:
                rows[name] = process(name, xmv, args, tools, reference)
                done += 1
            by_file_crc.setdefault(file_crc, name)
            write_index(pack, rows)
        except Exception as e:  # keep going with the next movie
            failed += 1
            log(f"FAIL {name}: {e}")
        finally:
            if xmv_tmp is not None:
                xmv_tmp.unlink(missing_ok=True)
    shutil.rmtree(args.work, ignore_errors=True)
    files = {r[7] for r in rows.values() if (pack / r[7]).is_file()}  # shared files once
    total = sum((pack / f).stat().st_size for f in files)
    frames = sum(int(r[3]) for r in rows.values())
    log(f"done: {done} upscaled, {reused} shared, {failed} failed in {(time.time() - start) / 60:.1f} min; pack "
        f"{len(rows)} movies, {frames} frames, {len(files)} files, {total / 1e6:.0f} MB")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
