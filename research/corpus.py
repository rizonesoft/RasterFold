"""Build the research corpus in build/research/ppm and its JPEG XL e7, e9, and WebP reference sizes.

  python research/corpus.py

Downloads the 24 Kodak images (r0k.us) and the imagecompression.info 8-bit RGB
set (346 MB, once), cuts 1280x960 center crops of the latter, converts
measure/image.bmp, the test pack in testdata/photos and the generated images of
tests/benchmark.py to PPM, then encodes every image with ffmpeg libjxl (effort 7
and effort 9) and lossless libwebp and records the sizes in jxl_refs.json.
Everything lands in build/research, which git ignores.
"""

from __future__ import annotations

import io
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
DATA = REPO / "build" / "research"
PPM = DATA / "ppm"
sys.path.insert(0, str(REPO / "tests"))

REFS_SPEC = (
    ("e7", ".e7.jxl", ["-c:v", "libjxl", "-distance", "0", "-effort", "7", "-threads", "1", "-update", "1"]),
    ("e9", ".e9.jxl", ["-c:v", "libjxl", "-distance", "0", "-effort", "9", "-threads", "1", "-update", "1"]),
    ("webp", ".webp", ["-c:v", "libwebp", "-lossless", "1", "-quality", "100", "-compression_level", "6", "-threads", "1", "-update", "1"]),
)


def is_positive_int(value: object) -> bool:
    return type(value) is int and value > 0


def fetch(url: str, dest: Path) -> None:
    if not dest.exists():
        print("download", url, flush=True)
        dest.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, dest)


def _ffmpeg_arg(exe: str, path: Path, runner=subprocess.run) -> str:
    if runner is subprocess.run and os.name != "nt" and exe.lower().endswith(".exe"):
        return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()
    return str(path)


def encode_ref(
    ppm_dir: Path,
    jxl_dir: Path,
    name: str,
    kind: str,
    ext: str,
    args: list[str],
    runner=subprocess.run,
) -> tuple[str, int]:
    src = ppm_dir / f"{name}.ppm"
    dest = jxl_dir / f"{name}{ext}"
    exe = shutil.which("ffmpeg") or shutil.which("ffmpeg.exe") or "ffmpeg"
    cmd = [
        exe,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        _ffmpeg_arg(exe, src, runner),
        *args,
        _ffmpeg_arg(exe, dest, runner),
    ]
    r = runner(cmd, capture_output=True)
    if r.returncode != 0 or not dest.is_file():
        err = (r.stderr or b"").decode("utf-8", "replace").strip() if isinstance(r.stderr, bytes) else str(r.stderr or "").strip()
        raise RuntimeError(f"ffmpeg failed for {name}:{kind} (exit {r.returncode}): {err}")
    size = dest.stat().st_size
    if not is_positive_int(size):
        raise RuntimeError(f"ffmpeg wrote non-positive size for {name}:{kind}: {size}")
    return f"{name}:{kind}", size


def validate_refs(refs: dict, stems: list[str]) -> None:
    if not isinstance(refs, dict):
        raise ValueError("jxl_refs.json root must be a JSON object")
    for stem in stems:
        for kind, _ext, _args in REFS_SPEC:
            key = f"{stem}:{kind}"
            if key not in refs:
                raise ValueError(f"missing reference in jxl_refs.json: {key}")
            if not is_positive_int(refs[key]):
                raise ValueError(f"invalid reference in jxl_refs.json: {key}={refs[key]!r}")


def update_refs(
    ppm_dir: Path = PPM,
    jxl_dir: Path = DATA / "jxl",
    refs_path: Path = DATA / "jxl_refs.json",
    runner=subprocess.run,
) -> dict[str, int]:
    jxl_dir.mkdir(parents=True, exist_ok=True)
    raw = json.loads(refs_path.read_text(encoding="utf-8")) if refs_path.exists() else {}
    if not isinstance(raw, dict):
        raise SystemExit("jxl_refs.json root must be a JSON object")
    refs: dict[str, int] = {k: v for k, v in raw.items() if isinstance(k, str) and is_positive_int(v)}
    stems = sorted(p.stem for p in ppm_dir.glob("*.ppm"))
    if not stems:
        raise SystemExit(f"no PPM images in {ppm_dir}")
    todo = [
        (stem, kind, ext, list(args))
        for stem in stems
        for kind, ext, args in REFS_SPEC
        if not is_positive_int(refs.get(f"{stem}:{kind}"))
    ]

    def _one(task: tuple[str, str, str, list[str]]) -> tuple[str, int]:
        stem, kind, ext, args = task
        return encode_ref(ppm_dir, jxl_dir, stem, kind, ext, args, runner=runner)

    if todo:
        try:
            with ThreadPoolExecutor(6) as pool:
                for key, size in pool.map(_one, todo):
                    refs[key] = size
                    print(key, size, flush=True)
        except Exception as exc:
            raise SystemExit(str(exc)) from exc
    try:
        validate_refs(refs, stems)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    refs_path.parent.mkdir(parents=True, exist_ok=True)
    refs_path.write_text(json.dumps(refs, indent=1, sort_keys=True), encoding="utf-8")
    reloaded = json.loads(refs_path.read_text(encoding="utf-8"))
    try:
        validate_refs(reloaded, stems)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    return reloaded


def main() -> None:
    PPM.mkdir(parents=True, exist_ok=True)
    Image.open(REPO / "measure" / "image.bmp").convert("RGB").save(PPM / "measure.ppm")
    for i in range(1, 25):
        png = DATA / "corpus" / "kodak" / f"kodim{i:02d}.png"
        fetch(f"https://r0k.us/graphics/kodak/kodak/kodim{i:02d}.png", png)
        Image.open(png).convert("RGB").save(PPM / f"kodim{i:02d}.ppm")
    for png in sorted((REPO / "testdata" / "photos").glob("*_crop.png")):
        Image.open(png).convert("RGB").save(PPM / f"tp_{png.stem}.ppm")
    zpath = DATA / "corpus" / "icinfo" / "rgb8bit.zip"
    fetch("http://imagecompression.info/test_images/rgb8bit.zip", zpath)
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            if info.filename.endswith(".ppm"):
                im = Image.open(io.BytesIO(z.read(info))).convert("RGB")
                w, h = im.size
                cw, ch = min(1280, w), min(960, h)
                x0, y0 = (w - cw) // 2, (h - ch) // 2
                im.crop((x0, y0, x0 + cw, y0 + ch)).save(PPM / f"ic_{Path(info.filename).stem}.ppm")
    from benchmark import synthetic  # noqa: E402
    for name, w, h, rgb in synthetic():
        Image.frombytes("RGB", (w, h), rgb).save(PPM / f"syn_{name}.ppm")

    update_refs(PPM, DATA / "jxl", DATA / "jxl_refs.json")
    print(f"{len(list(PPM.glob('*.ppm')))} images in {PPM}")


if __name__ == "__main__":
    main()
