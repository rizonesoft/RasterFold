"""Size, speed, and memory: RasterFold against PNG, lossless WebP, and lossless JPEG XL.

  python tests/benchmark.py [-n REPEATS] [SET ...]      sets: measure photos synthetic (default all)

Writes benchmarks/results.md and benchmarks/results.csv. Every time is a
process wall time (median of the repeats) and every memory figure a process
peak working set (maximum of the repeats), so RasterFold and ffmpeg are
measured the same way, startup included. A result counts only when the decoded
pixels equal the source. JPEG XL is decoded twice at effort 7: with ffmpeg's
default thread count, as tests/measure_final.py does, and with -threads 1;
jxl-e9 encodes at effort 9 and decodes with ffmpeg's default thread count.
"""

from __future__ import annotations

import csv
import ctypes
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
from ctypes import wintypes
from datetime import date
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_correctness import checker, draw_ui, gradient, noise, solid, stripes  # noqa: E402

EXE = ROOT / "bin" / "windows-x64" / "bin" / "rasterfold.exe"
OUT = ROOT / "benchmarks"
WORK = ROOT / "build" / "benchmark"
ALL_SETS = ("measure", "photos", "synthetic")
CSV_FIELDS = (
    "set",
    "image",
    "width",
    "height",
    "raw",
    "codec",
    "bytes",
    "encode_us",
    "decode_us",
    "encode_peak",
    "decode_peak",
    "pixels_match",
)

CODECS = {
    "rasterfold": {"ext": "fold"},
    "rasterfold-1t": {"ext": "fold", "extra": ["--threads", "1"]},
    "jxl": {"ext": "jxl", "args": ["-c:v", "libjxl", "-distance", "0", "-effort", "7", "-threads", "1"]},
    "jxl-1t": {"ext": "jxl", "reuse": "jxl", "dec": ["-threads", "1"]},
    "jxl-e9": {"ext": "jxl", "args": ["-c:v", "libjxl", "-distance", "0", "-effort", "9", "-threads", "1"]},
    "webp": {"ext": "webp", "args": ["-c:v", "libwebp", "-lossless", "1", "-quality", "100", "-compression_level", "6", "-threads", "1"]},
    "png": {"ext": "png", "args": ["-c:v", "png", "-compression_level", "9", "-pred", "mixed", "-threads", "1"]},
}


class PMC(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


_meminfo = ctypes.WinDLL("kernel32", use_last_error=True).K32GetProcessMemoryInfo if os.name == "nt" else None
if _meminfo is not None:
    _meminfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
    _meminfo.restype = wintypes.BOOL


def run(cmd: list[str]) -> tuple[int, int, int]:
    """Exit code, wall time in microseconds, peak working set in bytes."""
    started = time.perf_counter_ns()
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    proc.communicate()
    wall = (time.perf_counter_ns() - started) // 1000
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    peak = (
        int(pmc.PeakWorkingSetSize)
        if _meminfo is not None and _meminfo(wintypes.HANDLE(int(proc._handle)), ctypes.byref(pmc), pmc.cb)
        else 0
    )
    return proc.returncode, wall, peak


def pixels_of(path: Path) -> bytes:
    with Image.open(path) as image:
        return image.convert("RGB").tobytes()


def synthetic() -> list[tuple[str, int, int, bytes]]:
    return [
        ("solid", 640, 480, solid(640, 480, (240, 240, 240))),
        ("checker", 512, 512, checker(512, 512, 16)),
        ("stripes", 640, 360, stripes(640, 360)),
        ("gradient", 640, 480, gradient(640, 480)),
        ("noise", 256, 256, noise(256, 256, 1000)),
        ("pixel", 128, 128, checker(128, 128, 1)),
        ("ui", 1280, 720, draw_ui(1280, 720, 11)),
        ("text", 800, 600, draw_ui(800, 600, 12)),
        ("diagram", 900, 600, draw_ui(900, 600, 13)),
        ("odd", 65, 63, gradient(65, 63)),
        ("wide", 1919, 200, stripes(1919, 200)),
    ]


def sources(name: str, folder: Path) -> list[tuple[str, Path]]:
    folder.mkdir(parents=True, exist_ok=True)
    out = []
    if name == "measure":
        out.append(("image", ROOT / "measure" / "image.bmp"))
    elif name == "photos":
        for png in sorted((ROOT / "testdata" / "photos").glob("*.png")):
            bmp = folder / f"{png.stem}.bmp"
            if not bmp.exists():
                Image.open(png).convert("RGB").save(bmp, format="BMP")
            out.append((png.stem, bmp))
    else:
        for img, w, h, rgb in synthetic():
            bmp = folder / f"{img}.bmp"
            if not bmp.exists():
                Image.frombytes("RGB", (w, h), rgb).save(bmp, format="BMP")
            out.append((img, bmp))
    return out


def _parse_num(raw: str) -> int | float:
    text = raw.strip()
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    return float(text)


def parse_csv_row(raw: dict[str, str]) -> dict:
    match_str = (raw.get("pixels_match") or "").strip()
    if match_str == "True":
        pixels_match = True
    elif match_str == "False":
        pixels_match = False
    else:
        raise ValueError(f"invalid pixels_match value in results.csv: {match_str!r}")
    bytes_str = (raw.get("bytes") or "").strip()
    return {
        "set": (raw.get("set") or "").strip(),
        "image": (raw.get("image") or "").strip(),
        "width": int(raw["width"]),
        "height": int(raw["height"]),
        "raw": int(raw["raw"]),
        "codec": (raw.get("codec") or "").strip(),
        "bytes": int(bytes_str) if bytes_str != "" else None,
        "encode_us": _parse_num(raw["encode_us"]),
        "decode_us": _parse_num(raw["decode_us"]),
        "encode_peak": int(raw["encode_peak"]),
        "decode_peak": int(raw["decode_peak"]),
        "pixels_match": pixels_match,
    }


def load_existing_rows(csv_path: Path) -> list[dict]:
    if not csv_path.is_file():
        return []
    with csv_path.open("r", newline="", encoding="utf-8") as fh:
        return [parse_csv_row(r) for r in csv.DictReader(fh)]


def write_csv_rows(csv_path: Path, rows: list[dict]) -> None:
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(CSV_FIELDS))
        writer.writeheader()
        writer.writerows(rows)


def merge_rows(existing: list[dict], measured: list[dict], measured_sets: list[str]) -> tuple[list[dict], list[str]]:
    measured_set_names = set(measured_sets)
    retained = [r for r in existing if r["set"] not in measured_set_names]
    retained_sets = [s for s in ALL_SETS if s not in measured_set_names and any(r["set"] == s for r in retained)]
    for r in retained:
        if r["set"] not in retained_sets:
            retained_sets.append(r["set"])
    ordered_sets: list[str] = []
    for s in (*ALL_SETS, *measured_sets, *retained_sets):
        if s not in ordered_sets and (s in measured_set_names or s in retained_sets):
            ordered_sets.append(s)
    by_set: dict[str, list[dict]] = {s: [] for s in ordered_sets}
    for r in measured:
        by_set.setdefault(r["set"], []).append(r)
    for r in retained:
        by_set.setdefault(r["set"], []).append(r)
    combined: list[dict] = []
    seen: set[tuple[str, str, str]] = set()
    for s in ordered_sets:
        for r in by_set.get(s, []):
            key = (r["set"], r["image"], r["codec"])
            if key in seen:
                continue
            seen.add(key)
            combined.append(r)
    return combined, retained_sets


def measure(codec: str, src: Path, folder: Path, stem: str, repeats: int, done: dict) -> dict:
    spec = CODECS[codec]
    enc_file = folder / f"{stem}.{spec.get('reuse', codec)}.{spec['ext']}"
    dec_file = folder / f"{stem}.{codec}.out.bmp"
    encs, decs = [], []
    ok = True
    for _ in range(repeats):
        for f in (dec_file, Path(str(dec_file) + ".partial")):
            f.unlink(missing_ok=True)
        if "reuse" in spec:
            base = done[spec["reuse"]]
            e = (0, base["encode_us"], base["encode_peak"]) if base["pixels_match"] else (1, 0, 0)
            d = run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", *spec["dec"], "-i", str(enc_file),
                     "-frames:v", "1", "-update", "1", str(dec_file)]) if e[0] == 0 else (1, 0, 0)
        elif codec.startswith("rasterfold"):
            for f in (enc_file, Path(str(enc_file) + ".partial")):
                f.unlink(missing_ok=True)
            e = run([str(EXE), "encode", *spec.get("extra", []), str(src), str(enc_file)])
            d = run([str(EXE), "decode", *spec.get("extra", []), str(enc_file), str(dec_file)]) if e[0] == 0 else (1, 0, 0)
        else:
            enc_file.unlink(missing_ok=True)
            e = run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(src), *spec["args"], "-update", "1", str(enc_file)])
            d = run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", *spec.get("dec", []), "-i", str(enc_file),
                     "-frames:v", "1", "-update", "1", str(dec_file)]) if e[0] == 0 else (1, 0, 0)
        ok = ok and e[0] == 0 and d[0] == 0
        encs.append(e)
        decs.append(d)
    match = ok and pixels_of(dec_file) == pixels_of(src)
    return {
        "bytes": enc_file.stat().st_size if ok else None,
        "encode_us": statistics.median(x[1] for x in encs),
        "decode_us": statistics.median(x[1] for x in decs),
        "encode_peak": max(x[2] for x in encs),
        "decode_peak": max(x[2] for x in decs),
        "pixels_match": match,
    }


def geomean(values: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in values) / len(values)) if values else float("nan")


def main() -> int:
    args = sys.argv[1:]
    repeats = 3
    if "-n" in args:
        i = args.index("-n")
        repeats = int(args[i + 1])
        del args[i:i + 2]
    sets = args or list(ALL_SETS)
    if not EXE.exists():
        print(f"missing {EXE}; run build.ps1")
        return 1
    OUT.mkdir(exist_ok=True)
    csv_path = OUT / "results.csv"
    existing = load_existing_rows(csv_path)
    measured = []
    for set_name in sets:
        folder = WORK / set_name
        for stem, src in sources(set_name, folder):
            w, h = Image.open(src).size
            print(f"{set_name}/{stem} {w}x{h}", flush=True)
            done: dict = {}
            for codec in CODECS:
                r = done[codec] = measure(codec, src, folder, stem, repeats, done)
                measured.append({"set": set_name, "image": stem, "width": w, "height": h, "raw": w * h * 3, "codec": codec, **r})
    rows, retained_sets = merge_rows(existing, measured, sets)
    failures = [f"{r['codec']} {r['set']}/{r['image']}" for r in rows if not r["pixels_match"]]
    write_csv_rows(csv_path, rows)
    report(rows, failures, sets, repeats, retained_sets=retained_sets)
    print(f"failures: {len(failures)}")
    for f in failures:
        print(" ", f)
    return 1 if failures else 0


def report(
    rows: list[dict],
    failures: list[str],
    sets: list[str],
    repeats: int,
    retained_sets: list[str] | tuple[str, ...] = (),
    out_dir: Path = OUT,
) -> None:
    toolchain = ROOT / "build" / "toolchain.txt"
    ff_exe = shutil.which("ffmpeg") or shutil.which("ffmpeg.exe") or "ffmpeg"
    try:
        ff_proc = subprocess.run([ff_exe, "-version"], capture_output=True, text=True)
        ffv = ff_proc.stdout.splitlines()[0] if ff_proc.returncode == 0 and ff_proc.stdout.splitlines() else "ffmpeg unknown"
    except OSError:
        ffv = "ffmpeg unknown"
    if retained_sets:
        intro_line = (
            "Generated by `tests/benchmark.py`. Measured sets come from this run; retained sets keep previously recorded rows from "
            "`benchmarks/results.csv` (prior run repeat count and host metadata are not stored in CSV). The numbers are measurements, not targets."
        )
        times_bullet = (
            f"- Times: median process wall time of {repeats} runs for measured sets ({', '.join(sets)}), startup included; "
            f"retained sets ({', '.join(retained_sets)}) keep their previously recorded measurements from `benchmarks/results.csv` (prior run metadata unavailable). "
            "Memory: maximum process peak working set."
        )
    else:
        intro_line = "Generated by `tests/benchmark.py`. One machine, one run; the numbers are measurements, not targets."
        times_bullet = f"- Times: median process wall time of {repeats} runs, startup included. Memory: maximum process peak working set."
    lines = [
        "# RasterFold measurements",
        "",
        intro_line,
        "",
        f"- Date: {date.today().isoformat()}",
        f"- Machine: {platform.platform()}, {os.cpu_count()} logical processors",
        f"- Python {platform.python_version()}, Pillow {Image.__version__}",
        f"- {ffv}",
        f"- RasterFold toolchain: {toolchain.read_text(encoding='utf-8').splitlines()[0] if toolchain.exists() else 'unknown'}",
        times_bullet,
        "- `rasterfold` uses its default three threads (one per channel); `rasterfold-1t` passes `--threads 1`.",
        "- JPEG XL: ffmpeg libjxl `-distance 0 -effort 7` (`jxl`, `jxl-1t`) and `-effort 9` (`jxl-e9`), encoded with `-threads 1`. `jxl` and `jxl-e9` decode with ffmpeg's default",
        "  threads (as `tests/measure_final.py` does), `jxl-1t` with `-threads 1`. WebP: `-lossless 1 -quality 100",
        "  -compression_level 6 -threads 1`. PNG: `-compression_level 9 -pred mixed -threads 1`.",
        "",
    ]
    if failures:
        lines += ["## Failures", ""] + [f"- {f}" for f in failures] + [""]
    codecs = list(CODECS)
    report_sets: list[str] = []
    for s in (*ALL_SETS, *sets, *retained_sets):
        if s not in report_sets and any(r["set"] == s for r in rows):
            report_sets.append(s)
    for set_name in report_sets:
        srows = [r for r in rows if r["set"] == set_name]
        images = list(dict.fromkeys(r["image"] for r in srows))
        by = {(r["image"], r["codec"]): r for r in srows}
        lines += [f"## {set_name}", ""]
        if set_name in retained_sets:
            lines += ["Retained from existing `benchmarks/results.csv` (not re-measured in this run).", ""]
        lines += ["Totals and geometric means of the per-image ratio to `jxl` (lower is better):", ""]
        lines += ["| codec | total bytes | size | encode time | decode time | encode memory | decode memory |", "|---|---:|---:|---:|---:|---:|---:|"]
        for c in codecs:
            ok = [
                i
                for i in images
                if (i, c) in by
                and (i, "jxl") in by
                and by[(i, c)]["pixels_match"]
                and by[(i, "jxl")]["pixels_match"]
            ]
            if not images or len(ok) < len(images):
                lines.append(f"| {c} | -- | -- | -- | -- | -- | -- |")
                continue
            tot = sum(by[(i, c)]["bytes"] or 0 for i in ok)

            def g(key: str) -> str:
                vals = [by[(i, c)][key] / by[(i, "jxl")][key] for i in ok if by[(i, "jxl")][key]]
                return f"{geomean(vals):.3f}" if vals else "--"

            lines.append(f"| {c} | {tot} | {g('bytes')} | {g('encode_us')} | {g('decode_us')} | {g('encode_peak')} | {g('decode_peak')} |")
        lines += ["", "Bytes per image:", "", "| image | size | raw | " + " | ".join(codecs) + " |", "|---|---|---:|" + "---:|" * len(codecs)]
        for i in images:
            r0 = next(r for r in srows if r["image"] == i)
            cells = [
                "--"
                if (i, c) not in by
                else (str(by[(i, c)]["bytes"]) if by[(i, c)]["pixels_match"] else "FAIL")
                for c in codecs
            ]
            lines.append(f"| {i} | {r0['width']}x{r0['height']} | {r0['raw']} | " + " | ".join(cells) + " |")
        lines += ["", "Decode wall time, microseconds:", "", "| image | " + " | ".join(codecs) + " |", "|---|" + "---:|" * len(codecs)]
        for i in images:
            cells = ["--" if (i, c) not in by else str(int(round(float(by[(i, c)]["decode_us"])))) for c in codecs]
            lines.append(f"| {i} | " + " | ".join(cells) + " |")
        lines += ["", "Encode wall time, microseconds:", "", "| image | " + " | ".join(codecs) + " |", "|---|" + "---:|" * len(codecs)]
        for i in images:
            cells = ["--" if (i, c) not in by else str(int(round(float(by[(i, c)]["encode_us"])))) for c in codecs]
            lines.append(f"| {i} | " + " | ".join(cells) + " |")
        lines += ["", "Decode peak working set, bytes:", "", "| image | " + " | ".join(codecs) + " |", "|---|" + "---:|" * len(codecs)]
        for i in images:
            cells = ["--" if (i, c) not in by else str(by[(i, c)]["decode_peak"]) for c in codecs]
            lines.append(f"| {i} | " + " | ".join(cells) + " |")
        lines.append("")
    (out_dir / "results.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
