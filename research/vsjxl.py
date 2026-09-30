"""Alternate RasterFold and ffmpeg/libjxl decode (and encode) process runs; report medians.

usage: python research/vsjxl.py [image.bmp] [-n 5]
"""
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXE = REPO / "bin" / "windows-x64" / "bin" / "rasterfold.exe"
TMP = REPO / "build" / "research" / "vs"
TMP.mkdir(parents=True, exist_ok=True)


def wall(cmd):
    t = time.perf_counter()
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode:
        raise SystemExit(f"{cmd} failed: {r.stderr[-300:]}")
    return (time.perf_counter() - t) * 1000


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-n")]
    n = next((int(a[2:]) for a in sys.argv[1:] if a.startswith("-n")), 5)
    src = Path(args[0]) if args else REPO / "measure" / "image.bmp"
    fold, jxl = TMP / "t.fold", TMP / "t.jxl"
    res = {"rf_enc": [], "rf_dec": [], "jxl_enc": [], "jxl_dec": []}
    for i in range(n):
        for f in (fold, TMP / "t.bmp", TMP / "t.fold.partial"):
            f.unlink(missing_ok=True)
        res["rf_enc"].append(wall([str(EXE), "encode", str(src), str(fold)]))
        res["rf_dec"].append(wall([str(EXE), "decode", str(fold), str(TMP / "t.bmp")]))
        jxl.unlink(missing_ok=True)
        res["jxl_enc"].append(wall(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(src), "-c:v", "libjxl",
                                    "-distance", "0", "-effort", "7", "-threads", "1", "-update", "1", str(jxl)]))
        res["jxl_dec"].append(wall(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(jxl), "-frames:v", "1",
                                    "-update", "1", str(TMP / "j.bmp")]))
    for k, v in res.items():
        print(f"{k:8s} median {statistics.median(v):8.1f} ms   min {min(v):8.1f}")
    print(f"size rf={fold.stat().st_size} jxl={jxl.stat().st_size} ratio={fold.stat().st_size / jxl.stat().st_size:.4f}")
    print(f"decode ratio (median) {statistics.median(res['rf_dec']) / statistics.median(res['jxl_dec']):.3f}  "
          f"encode ratio {statistics.median(res['rf_enc']) / statistics.median(res['jxl_enc']):.3f}")


if __name__ == "__main__":
    main()
