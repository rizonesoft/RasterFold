"""Judge RasterFold on measure/image.bmp.

This image is the scoreboard, not a special case. The codec must use the same
path it would use for any other 24-bit BMP.

Exit 0 only when the decoded pixels match, the .fold file is smaller than
lossless JPEG XL, and decode wall time and peak working set are less than or
equal to JPEG XL. WebP and AVIF are measured and must decode exactly, but
they are not the pass bar. Exit 1 when the measurement is valid but the bar
is missed. Exit 2 when a tool fails or a decode is not exact.

Writes measure/verdict.json.
"""

from __future__ import annotations

import ctypes
import json
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import rfi_ref as rf  # noqa: E402

BMP = ROOT / "measure" / "image.bmp"
EXE = ROOT / "bin" / "windows-x64" / "bin" / "rasterfold.exe"
WORK = ROOT / "measure"
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


_meminfo = ctypes.WinDLL("kernel32", use_last_error=True).K32GetProcessMemoryInfo
_meminfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
_meminfo.restype = wintypes.BOOL


def peak_ws(handle: int) -> int | None:
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    if not _meminfo(wintypes.HANDLE(int(handle)), ctypes.byref(pmc), pmc.cb):
        return None
    return int(pmc.PeakWorkingSetSize)


def run(cmd: list[str]) -> dict:
    started = time.perf_counter_ns()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout, stderr = proc.communicate()
    return {
        "code": proc.returncode,
        "stdout": stdout.decode("utf-8", "replace"),
        "stderr": stderr.decode("utf-8", "replace"),
        "wall_us": (time.perf_counter_ns() - started) // 1000,
        "peak_ws": peak_ws(proc._handle),
    }


def stats(text: str) -> dict[str, int]:
    found = {}
    for line in text.splitlines():
        if line.startswith("rf_stat ") and "=" in line:
            key, value = line[len("rf_stat ") :].split("=", 1)
            found[key] = int(value)
    return found


def ffmpeg_encode(src: Path, dest: Path, args: list[str]) -> dict:
    dest.unlink(missing_ok=True)
    update = [] if dest.suffix.lower() == ".avif" else ["-update", "1"]
    result = run(["ffmpeg", "-y", "-hide_banner", "-i", str(src), *args, *update, str(dest)])
    result["bytes"] = dest.stat().st_size if result["code"] == 0 and dest.exists() else None
    return result


def ffmpeg_decode(src: Path, dest: Path) -> dict:
    dest.unlink(missing_ok=True)
    return run(["ffmpeg", "-y", "-hide_banner", "-i", str(src), "-frames:v", "1", "-update", "1", str(dest)])


def same_pixels(path: Path, rgb: bytes) -> bool:
    try:
        _w, _h, got = rf.read_bmp24(path.read_bytes())
        return got == rgb
    except Exception:
        with Image.open(path) as image:
            got = image.convert("RGB").tobytes()
        return got == rgb


def main() -> int:
    if not BMP.exists():
        print(f"missing final image {BMP}")
        return 2
    if not EXE.exists():
        print(f"missing {EXE}; run build.ps1")
        return 2
    WORK.mkdir(parents=True, exist_ok=True)
    width, height, rgb = rf.read_bmp24(BMP.read_bytes())
    failures: list[str] = []
    record: dict = {
        "image": BMP.relative_to(ROOT).as_posix(),   # repository-relative: a host path would name the machine
        "width": width,
        "height": height,
        "raw_rgb_bytes": len(rgb),
        "bmp_bytes": BMP.stat().st_size,
    }

    rfi = WORK / "image.fold"
    decoded = WORK / "rasterfold.bmp"
    rfi.unlink(missing_ok=True)
    decoded.unlink(missing_ok=True)
    enc = run([str(EXE), "encode", "--time", str(BMP), str(rfi)])
    dec = run([str(EXE), "decode", "--time", str(rfi), str(decoded)])
    enc_stats = stats(enc["stdout"])
    dec_stats = stats(dec["stdout"])
    rf_ok = enc["code"] == 0 and dec["code"] == 0 and rfi.exists() and same_pixels(decoded, rgb)
    if not rf_ok:
        failures.append("rasterfold pixels do not match measure/image.bmp")
    record["rasterfold"] = {
        "bytes": rfi.stat().st_size if rfi.exists() else None,
        "encode_wall_us": enc["wall_us"],
        "decode_wall_us": dec["wall_us"],
        "encode_peak_ws": enc["peak_ws"],
        "decode_peak_ws": dec["peak_ws"],
        "encode_codec_us": enc_stats.get("codec_us"),
        "decode_codec_us": dec_stats.get("codec_us"),
        "pixels_match": rf_ok,
    }

    codecs = {
        "webp": [
            "-c:v", "libwebp", "-lossless", "1", "-quality", "100",
            "-compression_level", "6", "-threads", "1",
        ],
        "avif": [
            "-c:v", "libaom-av1", "-pix_fmt", "gbrp", "-still-picture", "1",
            "-cpu-used", "4", "-threads", "1", "-aom-params", "lossless=1",
        ],
        "jxl": [
            "-c:v", "libjxl", "-distance", "0", "-effort", "7", "-threads", "1",
        ],
    }
    extensions = {"webp": "webp", "avif": "avif", "jxl": "jxl"}
    for name, args in codecs.items():
        blob = WORK / f"image.{extensions[name]}"
        back = WORK / f"{name}.bmp"
        encoded = ffmpeg_encode(BMP, blob, args)
        decoded_run = ffmpeg_decode(blob, back) if encoded["code"] == 0 else None
        ok = bool(decoded_run and decoded_run["code"] == 0 and same_pixels(back, rgb))
        if not ok:
            failures.append(f"{name} is not an exact decode of measure/image.bmp")
        record[name] = {
            "bytes": encoded.get("bytes"),
            "encode_wall_us": encoded["wall_us"],
            "decode_wall_us": None if decoded_run is None else decoded_run["wall_us"],
            "encode_peak_ws": encoded["peak_ws"],
            "decode_peak_ws": None if decoded_run is None else decoded_run["peak_ws"],
            "pixels_match": ok,
            "settings": " ".join(args),
        }

    rf_bytes = record["rasterfold"]["bytes"]
    rf_dec_us = record["rasterfold"]["decode_wall_us"]
    rf_mem = record["rasterfold"]["decode_peak_ws"]
    jxl = record["jxl"]
    if rf_ok and jxl["pixels_match"] and rf_bytes is not None and jxl["bytes"] is not None:
        if rf_bytes >= jxl["bytes"]:
            failures.append(f"size {rf_bytes} is not smaller than jxl {jxl['bytes']}")
    if rf_ok and jxl["pixels_match"] and rf_dec_us is not None and jxl["decode_wall_us"] is not None:
        if rf_dec_us > jxl["decode_wall_us"]:
            failures.append(f"decode wall {rf_dec_us} us is slower than jxl {jxl['decode_wall_us']} us")
    if rf_ok and jxl["pixels_match"] and rf_mem is not None and jxl["decode_peak_ws"] is not None:
        if rf_mem > jxl["decode_peak_ws"]:
            failures.append(f"decode peak {rf_mem} is heavier than jxl {jxl['decode_peak_ws']}")

    record["passed"] = not failures
    record["failures"] = failures
    record["summary"] = "passed" if not failures else "; ".join(failures)
    verdict = WORK / "verdict.json"
    # LF on every host: the file is tracked with eol=lf, and Windows Python would write CRLF.
    with open(verdict, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(record, indent=2) + "\n")

    print(f"final image {width}x{height} raw {len(rgb)} bytes")
    for name in ("rasterfold", "jxl", "webp", "avif"):
        item = record[name]
        print(
            f"{name:12} bytes={item['bytes']} decode_wall_us={item['decode_wall_us']} "
            f"decode_peak_ws={item['decode_peak_ws']} pixels={item['pixels_match']}"
        )
    if record["rasterfold"].get("decode_codec_us") is not None:
        print(f"rasterfold decode_codec_us={record['rasterfold']['decode_codec_us']}")
    print("PASS" if record["passed"] else "FAIL")
    for item in failures:
        print(f"- {item}")
    print(verdict)
    if not rf_ok or any(not record[name]["pixels_match"] for name in ("jxl", "webp", "avif")):
        return 2
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
