"""Alternate RasterFold and ffmpeg/libjxl (or two RasterFold executables) process runs; report medians and mins.

usage: python research/vsjxl.py [image.bmp] [-n 5] [--threads 1|3] [--exe PATH] [--alt-exe PATH]
"""
import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_EXE = REPO / "bin" / ("windows-x64/bin/rasterfold.exe" if os.name == "nt" else "linux-x64/bin/rasterfold")
TMP = REPO / "build" / "research" / "vs"
TMP.mkdir(parents=True, exist_ok=True)


def wall(cmd):
    t = time.perf_counter()
    r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True)
    if r.returncode:
        raise SystemExit(f"{cmd} failed: {r.stderr[-300:]}")
    return (time.perf_counter() - t) * 1000


def parse_args(argv):
    norm = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("-n") and len(a) > 2 and a[2:].isdigit():
            norm.extend(["-n", a[2:]])
        else:
            norm.append(a)
        i += 1
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image", nargs="?", default=str(REPO / "measure" / "image.bmp"))
    p.add_argument("-n", type=int, default=5)
    p.add_argument("--threads", type=int, choices=(1, 3), default=None)
    p.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    p.add_argument("--alt-exe", type=Path, default=None)
    return p.parse_args(norm)


def main():
    args = parse_args(sys.argv[1:])
    src = Path(args.image)
    n = args.n
    exe = args.exe
    alt = args.alt_exe
    t_args = ["--threads", str(args.threads)] if args.threads else []

    with tempfile.TemporaryDirectory(prefix="rf_vsjxl_") as td:
        work = Path(td)
        local_src = work / "in.bmp"
        shutil.copy2(src, local_src)
        fold = work / "t.fold"
        if alt is not None:
            local_exe = work / "base.exe"
            local_alt = work / "alt.exe"
            shutil.copy2(exe, local_exe)
            shutil.copy2(alt, local_alt)
            fold_alt = work / "t_alt.fold"
            res = {"base_enc": [], "base_dec": [], "alt_enc": [], "alt_dec": []}
            for i in range(n):
                pairs = [("base", local_exe, fold), ("alt", local_alt, fold_alt)]
                if i % 2 == 1:
                    pairs.reverse()
                for tag, bin_path, f_out in pairs:
                    bmp_out = work / f"{tag}.bmp"
                    for f in (f_out, bmp_out, Path(str(f_out) + ".partial"), Path(str(bmp_out) + ".partial")):
                        f.unlink(missing_ok=True)
                    res[f"{tag}_enc"].append(wall([str(bin_path), "encode", *t_args, str(local_src), str(f_out)]))
                    res[f"{tag}_dec"].append(wall([str(bin_path), "decode", *t_args, str(f_out), str(bmp_out)]))
            if fold.read_bytes() != fold_alt.read_bytes():
                raise SystemExit("base and alt produced different .fold bytes")
            for k, v in res.items():
                print(f"{k:8s} median {statistics.median(v):8.1f} ms   min {min(v):8.1f}")
            print(f"size base={fold.stat().st_size} alt={fold_alt.stat().st_size} identical=True")
            print(f"decode ratio (min) {min(res['alt_dec']) / min(res['base_dec']):.3f}  "
                  f"(median) {statistics.median(res['alt_dec']) / statistics.median(res['base_dec']):.3f}  "
                  f"encode ratio (min) {min(res['alt_enc']) / min(res['base_enc']):.3f}  "
                  f"(median) {statistics.median(res['alt_enc']) / statistics.median(res['base_enc']):.3f}")
            return

        jxl = work / "t.jxl"
        res = {"rf_enc": [], "rf_dec": [], "jxl_enc": [], "jxl_dec": []}
        jxl_threads = str(args.threads) if args.threads else "1"
        for i in range(n):
            for f in (fold, work / "t.bmp", work / "t.fold.partial"):
                f.unlink(missing_ok=True)
            res["rf_enc"].append(wall([str(exe), "encode", *t_args, str(local_src), str(fold)]))
            res["rf_dec"].append(wall([str(exe), "decode", *t_args, str(fold), str(work / "t.bmp")]))
            jxl.unlink(missing_ok=True)
            res["jxl_enc"].append(wall(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(local_src), "-c:v", "libjxl",
                                        "-distance", "0", "-effort", "7", "-threads", jxl_threads, "-update", "1", str(jxl)]))
            res["jxl_dec"].append(wall(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-threads", jxl_threads, "-i", str(jxl), "-frames:v", "1",
                                        "-update", "1", str(work / "j.bmp")]))
        for k, v in res.items():
            print(f"{k:8s} median {statistics.median(v):8.1f} ms   min {min(v):8.1f}")
        print(f"size rf={fold.stat().st_size} jxl={jxl.stat().st_size} ratio={fold.stat().st_size / jxl.stat().st_size:.4f}")
        print(f"decode ratio (median) {statistics.median(res['rf_dec']) / statistics.median(res['jxl_dec']):.3f}  "
              f"encode ratio {statistics.median(res['rf_enc']) / statistics.median(res['jxl_enc']):.3f}")


if __name__ == "__main__":
    main()
