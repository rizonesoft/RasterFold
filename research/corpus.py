"""Build the research corpus in build/research/ppm and its JPEG XL e7 reference sizes.

  python research/corpus.py

Downloads the 24 Kodak images (r0k.us) and the imagecompression.info 8-bit RGB
set (346 MB, once), cuts 1280x960 center crops of the latter, converts
measure/image.bmp, the test pack in testdata/photos and the generated images of
tests/benchmark.py to PPM, then encodes every image with ffmpeg libjxl at the
settings of tests/measure_final.py and records the sizes in jxl_refs.json.
Everything lands in build/research, which git ignores.
"""

from __future__ import annotations

import io
import json
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


def fetch(url: str, dest: Path) -> None:
    if not dest.exists():
        print("download", url, flush=True)
        dest.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, dest)


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

    refs_path = DATA / "jxl_refs.json"
    refs = json.loads(refs_path.read_text()) if refs_path.exists() else {}
    jxl = DATA / "jxl"
    jxl.mkdir(exist_ok=True)

    def one(name: str):
        dest = jxl / f"{name}.e7.jxl"
        r = subprocess.run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(PPM / f"{name}.ppm"), "-c:v", "libjxl",
                            "-distance", "0", "-effort", "7", "-threads", "1", "-update", "1", str(dest)], capture_output=True)
        return name, dest.stat().st_size if r.returncode == 0 else None

    todo = [p.stem for p in sorted(PPM.glob("*.ppm")) if f"{p.stem}:e7" not in refs]
    with ThreadPoolExecutor(6) as pool:
        for name, size in pool.map(one, todo):
            if size:
                refs[f"{name}:e7"] = size
                print(name, size, flush=True)
    refs_path.write_text(json.dumps(refs, indent=1, sort_keys=True))
    print(f"{len(list(PPM.glob('*.ppm')))} images in {PPM}")


if __name__ == "__main__":
    main()
