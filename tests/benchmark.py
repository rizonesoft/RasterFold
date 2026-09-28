"""Repeatable size, time, and memory comparison.

RasterFold times come from the tool itself and from the process wall clock.
PNG, lossless WebP, and lossless JPEG XL times are ffmpeg process wall times,
which include process startup, format conversion, and muxing. They are not
split into codec-only time. Every image is listed. A decode that does not
reproduce the source pixels is a failure, not a size result.
"""

from __future__ import annotations

import csv
import hashlib
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

import ctypes
from ctypes import wintypes

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_correctness import checker, draw_ui, gradient, noise, solid, stripes  # noqa: E402

EXE = ROOT / "build" / "rasterfold.exe"
OUT = ROOT / "benchmarks"
WORK = OUT / "work"
FFMPEG = "ffmpeg"

PNG_ARGS = ["-c:v", "png", "-compression_level", "9", "-pred", "mixed", "-threads", "1"]
WEBP_ARGS = ["-c:v", "libwebp", "-lossless", "1", "-quality", "100", "-compression_level", "6", "-threads", "1"]
JXL_ARGS = ["-c:v", "libjxl", "-distance", "0", "-effort", "7", "-threads", "1"]


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


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_meminfo = _k32.K32GetProcessMemoryInfo
_meminfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
_meminfo.restype = wintypes.BOOL


def peak_working_set(handle: int) -> int | None:
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    if not _meminfo(wintypes.HANDLE(int(handle)), ctypes.byref(pmc), pmc.cb):
        return None
    return int(pmc.PeakWorkingSetSize)


def run_timed(cmd: list[str]) -> dict:
    started = time.perf_counter_ns()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout, stderr = proc.communicate()
    elapsed_us = (time.perf_counter_ns() - started) // 1000
    return {
        "code": proc.returncode,
        "stdout": stdout,
        "stderr": stderr,
        "wall_us": elapsed_us,
        "peak_ws": peak_working_set(proc._handle),
    }


def cpu_name() -> str:
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_Processor | Select-Object -First 1).Name"],
            text=True,
        )
        return " ".join(out.split())
    except (OSError, subprocess.CalledProcessError):
        return platform.processor()


def memory_bytes() -> str:
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory"],
            text=True,
        )
        return out.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def ffmpeg_version() -> str:
    out = subprocess.check_output([FFMPEG, "-version"], text=True, stderr=subprocess.STDOUT)
    return out.splitlines()[0].strip()


def parse_stats(text: str) -> dict[str, int]:
    stats = {}
    for line in text.splitlines():
        if line.startswith("rf_stat ") and "=" in line:
            key, value = line[len("rf_stat ") :].split("=", 1)
            stats[key] = int(value)
    return stats


def save_bmp(path: Path, width: int, height: int, rgb: bytes) -> None:
    Image.frombytes("RGB", (width, height), rgb).save(path, format="BMP")


def pixels_of(path: Path) -> bytes:
    with Image.open(path) as image:
        return image.convert("RGB").tobytes()


def dev_images() -> list[tuple[str, int, int, bytes]]:
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
    ]


def holdout_images() -> list[tuple[str, int, int, bytes]]:
    return [
        ("solid", 100, 50, solid(100, 50, (12, 80, 160))),
        ("checker", 300, 180, checker(300, 180, 7)),
        ("stripes", 800, 450, stripes(800, 450)),
        ("gradient", 333, 200, gradient(333, 200)),
        ("noise", 320, 180, noise(320, 180, 9099)),
        ("pixel", 48, 48, checker(48, 48, 1)),
        ("ui", 1024, 768, draw_ui(1024, 768, 21)),
        ("text", 640, 400, draw_ui(640, 400, 22)),
        ("diagram", 700, 500, draw_ui(700, 500, 23)),
        ("wide", 1919, 200, stripes(1919, 200)),
    ]


def ffmpeg_encode(src: Path, dest: Path, codec_args: list[str]) -> dict:
    if dest.exists():
        dest.unlink()
    cmd = [FFMPEG, "-y", "-hide_banner", "-i", str(src), *codec_args, "-update", "1", str(dest)]
    result = run_timed(cmd)
    result["bytes"] = dest.stat().st_size if dest.exists() and result["code"] == 0 else None
    return result


def ffmpeg_decode(src: Path, dest: Path) -> dict:
    if dest.exists():
        dest.unlink()
    cmd = [FFMPEG, "-y", "-hide_banner", "-i", str(src), "-frames:v", "1", "-update", "1", str(dest)]
    return run_timed(cmd)


def rasterfold_convert(mode: str, src: Path, dest: Path) -> dict:
    if dest.exists():
        dest.unlink()
    result = run_timed([str(EXE), mode, "--time", str(src), str(dest)])
    stats = parse_stats(result["stdout"].decode("utf-8", "replace"))
    result["stats"] = stats
    result["bytes"] = dest.stat().st_size if dest.exists() and result["code"] == 0 else None
    return result


def blank_row(group: str, name: str, width: int, height: int, digest: str, raw: int, bmp_bytes: int) -> dict:
    return {
        "set": group,
        "image": name,
        "width": width,
        "height": height,
        "sha256": digest,
        "raw_rgb_bytes": raw,
        "bmp_bytes": bmp_bytes,
    }


def main() -> int:
    if not EXE.exists():
        print(f"missing {EXE}")
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    failures = []
    for group, factory in (("dev", dev_images), ("holdout", holdout_images)):
        folder = WORK / group
        folder.mkdir(parents=True, exist_ok=True)
        for name, width, height, rgb in factory():
            bmp = folder / f"{name}.bmp"
            save_bmp(bmp, width, height, rgb)
            source = pixels_of(bmp)
            if source != rgb:
                failures.append(f"{group}/{name}: Pillow BMP did not preserve pixels")
            digest = hashlib.sha256(bmp.read_bytes()).hexdigest()
            raw = width * height * 3
            base = blank_row(group, name, width, height, digest, raw, bmp.stat().st_size)
            print(f"{group}/{name} {width}x{height}", flush=True)

            rf_enc = rasterfold_convert("encode", bmp, folder / f"{name}.rfi")
            rf_dec = rasterfold_convert("decode", folder / f"{name}.rfi", folder / f"{name}.rf.bmp")
            rf_match = rf_dec["code"] == 0 and pixels_of(folder / f"{name}.rf.bmp") == source
            if rf_enc["code"] != 0 or rf_dec["code"] != 0 or not rf_match:
                failures.append(f"rasterfold {group}/{name} enc={rf_enc['code']} dec={rf_dec['code']} match={rf_match}")
            enc_stats = rf_enc.get("stats", {})
            dec_stats = rf_dec.get("stats", {})
            rows.append({
                **base,
                "codec": "rasterfold",
                "setting": "experimental v1, 1 thread",
                "encode_wall_us": rf_enc["wall_us"],
                "decode_wall_us": rf_dec["wall_us"],
                "encode_peak_ws": rf_enc["peak_ws"],
                "decode_peak_ws": rf_dec["peak_ws"],
                "compressed_bytes": rf_enc["bytes"],
                "pixels_match": rf_match,
                "rf_encode_read_us": enc_stats.get("read_us"),
                "rf_encode_parse_us": enc_stats.get("parse_us"),
                "rf_encode_codec_us": enc_stats.get("codec_us"),
                "rf_encode_write_us": enc_stats.get("write_us"),
                "rf_encode_total_us": enc_stats.get("total_us"),
                "rf_encode_alloc_peak": enc_stats.get("alloc_peak"),
                "rf_decode_read_us": dec_stats.get("read_us"),
                "rf_decode_codec_us": dec_stats.get("codec_us"),
                "rf_decode_write_us": dec_stats.get("write_us"),
                "rf_decode_total_us": dec_stats.get("total_us"),
                "rf_decode_alloc_peak": dec_stats.get("alloc_peak"),
                "mode_raw": enc_stats.get("mode_raw"),
                "mode_packbits": enc_stats.get("mode_packbits"),
                "mode_predict": enc_stats.get("mode_predict"),
                "mode_palette": enc_stats.get("mode_palette"),
            })

            for codec, args, ext in (
                ("png", PNG_ARGS, "png"),
                ("webp", WEBP_ARGS, "webp"),
                ("jxl", JXL_ARGS, "jxl"),
            ):
                encoded = folder / f"{name}.{ext}"
                decoded = folder / f"{name}.{codec}.bmp"
                enc = ffmpeg_encode(bmp, encoded, args)
                dec = ffmpeg_decode(encoded, decoded) if enc["code"] == 0 else {"code": None, "wall_us": None, "peak_ws": None, "stderr": b""}
                match = False
                if dec.get("code") == 0 and decoded.exists():
                    match = pixels_of(decoded) == source
                if enc["code"] != 0 or dec.get("code") != 0 or not match:
                    err = (enc["stderr"] or dec.get("stderr") or b"")[-400:]
                    failures.append(f"{codec} {group}/{name} enc={enc['code']} dec={dec.get('code')} match={match} {err!r}")
                setting = " ".join(args)
                rows.append({
                    **base,
                    "codec": codec,
                    "setting": setting,
                    "encode_wall_us": enc["wall_us"],
                    "decode_wall_us": dec.get("wall_us"),
                    "encode_peak_ws": enc["peak_ws"],
                    "decode_peak_ws": dec.get("peak_ws"),
                    "compressed_bytes": enc.get("bytes"),
                    "pixels_match": match,
                    "rf_encode_read_us": "",
                    "rf_encode_parse_us": "",
                    "rf_encode_codec_us": "",
                    "rf_encode_write_us": "",
                    "rf_encode_total_us": "",
                    "rf_encode_alloc_peak": "",
                    "rf_decode_read_us": "",
                    "rf_decode_codec_us": "",
                    "rf_decode_write_us": "",
                    "rf_decode_total_us": "",
                    "rf_decode_alloc_peak": "",
                    "mode_raw": "",
                    "mode_packbits": "",
                    "mode_predict": "",
                    "mode_palette": "",
                })

    csv_path = OUT / "baseline.csv"
    fields = list(rows[0].keys())
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    write_report(rows, failures)
    print(f"wrote {csv_path}")
    print(f"failures: {len(failures)}")
    for item in failures:
        print(item)
    return 1 if failures else 0


def write_report(rows: list[dict], failures: list[str]) -> None:
    toolchain = ROOT / "build" / "toolchain.txt"
    tool_text = toolchain.read_text(encoding="utf-8") if toolchain.exists() else "toolchain.txt missing"
    lines = [
        "# RasterFold baseline measurements",
        "",
        "Generated by `tests/benchmark.py`. These numbers are measurements from this run, not targets.",
        "",
        f"- Date: 2026-09-28",
        f"- Machine: {platform.platform()}",
        f"- CPU: {cpu_name()}",
        f"- Logical processors: {os.cpu_count()}",
        f"- Physical memory bytes: {memory_bytes()}",
        f"- Python: {platform.python_version()}",
        f"- Pillow: {Image.__version__}",
        f"- ffmpeg: {ffmpeg_version()}",
        "- RasterFold: experimental version 1, one thread, no AVX2 path.",
        "- PNG: ffmpeg `png`, `-compression_level 9 -pred mixed -threads 1`.",
        "- WebP: ffmpeg `libwebp`, `-lossless 1 -quality 100 -compression_level 6 -threads 1`.",
        "- JPEG XL: ffmpeg `libjxl`, `-distance 0 -effort 7 -threads 1`.",
        "",
        "RasterFold `codec_us` is only the block encode or block decode. `total_us` adds BMP parsing or writing and file reads or writes, including the final flush. `encode_wall_us` is the whole process, including startup.",
        "ffmpeg times are whole-process wall times only. This ffmpeg build does not report a separate codec-only timer, and its process peak working set includes the ffmpeg binary.",
        "A row is a successful size only when `pixels_match` is true.",
        "",
        "Toolchain:",
        "",
        "```",
        tool_text.rstrip(),
        "```",
        "",
    ]
    if failures:
        lines.append("## Failures")
        lines.append("")
        lines.extend(f"- {item}" for item in failures)
        lines.append("")

    for group in ("dev", "holdout"):
        group_rows = [row for row in rows if row["set"] == group]
        images = []
        for row in group_rows:
            if row["image"] not in images:
                images.append(row["image"])
        lines.append(f"## {group}")
        lines.append("")
        lines.append("| image | raw | rasterfold | png | webp | jxl | rf codec enc us | rf total enc us | rf wall enc us | png wall enc us | webp wall enc us | jxl wall enc us |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for image in images:
            by_codec = {row["codec"]: row for row in group_rows if row["image"] == image}
            rf = by_codec["rasterfold"]
            lines.append(
                "| {image} | {raw} | {rfb} | {png} | {webp} | {jxl} | {codec} | {total} | {wall} | {pw} | {ww} | {jw} |".format(
                    image=image,
                    raw=rf["raw_rgb_bytes"],
                    rfb=cell(rf),
                    png=cell(by_codec["png"]),
                    webp=cell(by_codec["webp"]),
                    jxl=cell(by_codec["jxl"]),
                    codec=rf["rf_encode_codec_us"],
                    total=rf["rf_encode_total_us"],
                    wall=rf["encode_wall_us"],
                    pw=by_codec["png"]["encode_wall_us"],
                    ww=by_codec["webp"]["encode_wall_us"],
                    jw=by_codec["jxl"]["encode_wall_us"],
                )
            )
        lines.append("")
        lines.append("| image | rf dec codec us | rf dec wall us | png dec wall us | webp dec wall us | jxl dec wall us | rf enc peak ws | png enc peak ws | webp enc peak ws | jxl enc peak ws | modes r/p/pr/pa |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|")
        for image in images:
            by_codec = {row["codec"]: row for row in group_rows if row["image"] == image}
            rf = by_codec["rasterfold"]
            modes = f"{rf['mode_raw']}/{rf['mode_packbits']}/{rf['mode_predict']}/{rf['mode_palette']}"
            lines.append(
                "| {image} | {dc} | {dw} | {pd} | {wd} | {jd} | {rp} | {pp} | {wp} | {jp} | {modes} |".format(
                    image=image,
                    dc=rf["rf_decode_codec_us"],
                    dw=rf["decode_wall_us"],
                    pd=by_codec["png"]["decode_wall_us"],
                    wd=by_codec["webp"]["decode_wall_us"],
                    jd=by_codec["jxl"]["decode_wall_us"],
                    rp=rf["encode_peak_ws"],
                    pp=by_codec["png"]["encode_peak_ws"],
                    wp=by_codec["webp"]["encode_peak_ws"],
                    jp=by_codec["jxl"]["encode_peak_ws"],
                    modes=modes,
                )
            )
        lines.append("")
        lines.append("Modes are raw / packbits / predict / palette block counts from the encoder.")
        lines.append("")
    text = "\n".join(lines) + "\n"
    (OUT / "baseline.md").write_text(text, encoding="utf-8")


def cell(row: dict) -> str:
    if not row["pixels_match"] or row["compressed_bytes"] is None:
        return "FAIL"
    return str(row["compressed_bytes"])


if __name__ == "__main__":
    raise SystemExit(main())
