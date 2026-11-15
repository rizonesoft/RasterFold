#!/usr/bin/env python3
"""Paired alternating default-thread RasterFold vs libjxl e7 tail-pool benchmark.

Runs on Windows via `python3 scripts/windows.py research/jxl_alt.py`.
Logs per-round JSONL (including Windows CPU % via GetSystemTimes and WSL 1-minute loadavg)
and verifies contiguous tail-pool ([N-10:N-5] and [N-5:N]) speed and PeakWorkingSetSize
predicates across measure, photos, and synthetic.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def _load_benchmark():
    existing = sys.modules.get("benchmark")
    if existing is not None:
        return existing
    bench_path = ROOT / "tests" / "benchmark.py"
    spec = importlib.util.spec_from_file_location("benchmark", bench_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"unable to load {bench_path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["benchmark"] = mod
    spec.loader.exec_module(mod)
    return mod


bm = _load_benchmark()

VALID_SETS = ("measure", "photos", "synthetic")
POOL_RATIO_KEYS = (
    "enc_sum_ratio",
    "enc_gm_ratio",
    "dec_sum_ratio",
    "dec_gm_ratio",
    "enc_ws_sum_ratio",
    "enc_ws_max_ratio",
    "dec_ws_sum_ratio",
    "dec_ws_max_ratio",
)


class FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]


_get_sys_times = ctypes.WinDLL("kernel32", use_last_error=True).GetSystemTimes if os.name == "nt" else None
if _get_sys_times is not None:
    _get_sys_times.argtypes = [ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME), ctypes.POINTER(FILETIME)]
    _get_sys_times.restype = wintypes.BOOL


def _ft64(ft: FILETIME) -> int:
    return (int(ft.dwHighDateTime) << 32) | int(ft.dwLowDateTime)


def sample_win_cpu_pct(window_s: float = 0.15) -> float:
    """Measure Windows host CPU busy percentage over window_s, or -1.0 off Windows."""
    if _get_sys_times is None:
        return -1.0
    i0, k0, u0 = FILETIME(), FILETIME(), FILETIME()
    i1, k1, u1 = FILETIME(), FILETIME(), FILETIME()
    if not _get_sys_times(ctypes.byref(i0), ctypes.byref(k0), ctypes.byref(u0)):
        return -1.0
    time.sleep(window_s)
    if not _get_sys_times(ctypes.byref(i1), ctypes.byref(k1), ctypes.byref(u1)):
        return -1.0
    idle = _ft64(i1) - _ft64(i0)
    total = (_ft64(k1) - _ft64(k0)) + (_ft64(u1) - _ft64(u0))
    if total <= 0:
        return 0.0
    busy = max(0, total - idle)
    return round(100.0 * busy / total, 1)


def _parse_load(raw: str) -> float | None:
    parts = raw.strip().split()
    if parts and re.fullmatch(r"\d+(?:\.\d+)?", parts[0]):
        return round(float(parts[0]), 2)
    return None


def sample_wsl_load(load_file: Path | None = None) -> float | None:
    """Read 1-minute host load average from load_file, /proc/loadavg, or wsl.exe."""
    for path in (load_file, Path("/proc/loadavg")):
        if path is not None and path.is_file():
            val = _parse_load(path.read_text(encoding="utf-8"))
            if val is not None:
                return val
    if os.name == "nt":
        wsl_bin = shutil.which("wsl.exe")
        if wsl_bin:
            try:
                proc = subprocess.run(
                    [wsl_bin, "-e", "cat", "/proc/loadavg"],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    timeout=3,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                )
            except (OSError, subprocess.SubprocessError):
                return None
            if proc.returncode == 0:
                return _parse_load(proc.stdout)
    return None


def _ffmpeg_bin() -> str:
    return (
        shutil.which("ffmpeg")
        or shutil.which("ffmpeg.exe")
        or "/mnt/c/Users/DerickPayne/AppData/Local/Microsoft/WinGet/Links/ffmpeg.exe"
    )


def _ff_path(ffmpeg_bin: str, path: Path) -> str:
    s = str(path)
    if os.name != "nt" and ffmpeg_bin.lower().endswith(".exe") and s.startswith("/"):
        return subprocess.check_output(["wslpath", "-w", s], text=True).strip()
    return s


def geomean(vals: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in vals) / len(vals))


def run_one_codec(
    codec: str,
    src: Path,
    work: Path,
    stem: str,
    exe: Path = bm.EXE,
    ffmpeg: str | None = None,
) -> dict[str, int]:
    """Encode and decode one image with default threads and measure microseconds and working set."""
    if codec == "rf":
        enc_file = work / f"{stem}.rf.fold"
        dec_file = work / f"{stem}.rf.out.bmp"
        for p in (enc_file, Path(str(enc_file) + ".partial"), dec_file, Path(str(dec_file) + ".partial")):
            p.unlink(missing_ok=True)
        rc_e, us_e, ws_e = bm.run([str(exe), "encode", str(src), str(enc_file)])
        if rc_e != 0:
            raise RuntimeError(f"rasterfold encode failed rc={rc_e} on {stem}")
        rc_d, us_d, ws_d = bm.run([str(exe), "decode", str(enc_file), str(dec_file)])
        if rc_d != 0:
            raise RuntimeError(f"rasterfold decode failed rc={rc_d} on {stem}")
    elif codec == "jxl":
        ff = ffmpeg or _ffmpeg_bin()
        enc_file = work / f"{stem}.jxl.jxl"
        dec_file = work / f"{stem}.jxl.out.bmp"
        enc_file.unlink(missing_ok=True)
        dec_file.unlink(missing_ok=True)
        rc_e, us_e, ws_e = bm.run(
            [
                ff,
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                _ff_path(ff, src),
                "-c:v",
                "libjxl",
                "-distance",
                "0",
                "-effort",
                "7",
                "-update",
                "1",
                _ff_path(ff, enc_file),
            ]
        )
        if rc_e != 0:
            raise RuntimeError(f"ffmpeg libjxl encode failed rc={rc_e} on {stem}")
        rc_d, us_d, ws_d = bm.run(
            [
                ff,
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                _ff_path(ff, enc_file),
                "-frames:v",
                "1",
                "-update",
                "1",
                _ff_path(ff, dec_file),
            ]
        )
        if rc_d != 0:
            raise RuntimeError(f"ffmpeg libjxl decode failed rc={rc_d} on {stem}")
    else:
        raise ValueError(f"unsupported codec: {codec}")
    return {
        "bytes": enc_file.stat().st_size,
        "enc_us": us_e,
        "dec_us": us_d,
        "enc_ws": ws_e,
        "dec_ws": ws_d,
    }


def summarize_pool(rounds_slice: list[dict], stems: list[str]) -> dict[str, object]:
    """Aggregate per-image minimum wall time and maximum peak working set across a 5-round pool."""
    rf_enc_min, jxl_enc_min = {}, {}
    rf_dec_min, jxl_dec_min = {}, {}
    rf_enc_ws, jxl_enc_ws = {}, {}
    rf_dec_ws, jxl_dec_ws = {}, {}
    for s in stems:
        rf_enc_min[s] = min(r["images"][s]["rf"]["enc_us"] for r in rounds_slice)
        jxl_enc_min[s] = min(r["images"][s]["jxl"]["enc_us"] for r in rounds_slice)
        rf_dec_min[s] = min(r["images"][s]["rf"]["dec_us"] for r in rounds_slice)
        jxl_dec_min[s] = min(r["images"][s]["jxl"]["dec_us"] for r in rounds_slice)
        rf_enc_ws[s] = max(r["images"][s]["rf"]["enc_ws"] for r in rounds_slice)
        jxl_enc_ws[s] = max(r["images"][s]["jxl"]["enc_ws"] for r in rounds_slice)
        rf_dec_ws[s] = max(r["images"][s]["rf"]["dec_ws"] for r in rounds_slice)
        jxl_dec_ws[s] = max(r["images"][s]["jxl"]["dec_ws"] for r in rounds_slice)

    enc_sum_rf = sum(rf_enc_min.values())
    enc_sum_jxl = sum(jxl_enc_min.values())
    dec_sum_rf = sum(rf_dec_min.values())
    dec_sum_jxl = sum(jxl_dec_min.values())
    enc_ws_sum_rf = sum(rf_enc_ws.values())
    enc_ws_sum_jxl = sum(jxl_enc_ws.values())
    dec_ws_sum_rf = sum(rf_dec_ws.values())
    dec_ws_sum_jxl = sum(jxl_dec_ws.values())
    enc_ws_max_rf = max(rf_enc_ws.values())
    enc_ws_max_jxl = max(jxl_enc_ws.values())
    dec_ws_max_rf = max(rf_dec_ws.values())
    dec_ws_max_jxl = max(jxl_dec_ws.values())

    return {
        "enc_sum_rf_us": enc_sum_rf,
        "enc_sum_jxl_us": enc_sum_jxl,
        "enc_sum_ratio": enc_sum_rf / enc_sum_jxl,
        "enc_gm_ratio": geomean([rf_enc_min[s] / jxl_enc_min[s] for s in stems]),
        "dec_sum_rf_us": dec_sum_rf,
        "dec_sum_jxl_us": dec_sum_jxl,
        "dec_sum_ratio": dec_sum_rf / dec_sum_jxl,
        "dec_gm_ratio": geomean([rf_dec_min[s] / jxl_dec_min[s] for s in stems]),
        "enc_ws_sum_rf": enc_ws_sum_rf,
        "enc_ws_sum_jxl": enc_ws_sum_jxl,
        "enc_ws_sum_ratio": enc_ws_sum_rf / enc_ws_sum_jxl,
        "enc_ws_max_rf": enc_ws_max_rf,
        "enc_ws_max_jxl": enc_ws_max_jxl,
        "enc_ws_max_ratio": enc_ws_max_rf / enc_ws_max_jxl,
        "dec_ws_sum_rf": dec_ws_sum_rf,
        "dec_ws_sum_jxl": dec_ws_sum_jxl,
        "dec_ws_sum_ratio": dec_ws_sum_rf / dec_ws_sum_jxl,
        "dec_ws_max_rf": dec_ws_max_rf,
        "dec_ws_max_jxl": dec_ws_max_jxl,
        "dec_ws_max_ratio": dec_ws_max_rf / dec_ws_max_jxl,
        "win_cpu_min": min(r["win_cpu_pct"] for r in rounds_slice),
        "win_cpu_max": max(r["win_cpu_pct"] for r in rounds_slice),
        "wsl_loads": [r["wsl_load"] for r in rounds_slice if r["wsl_load"] is not None],
    }


def rel_diff(a: float, b: float) -> float:
    return abs(a - b) / min(a, b)


def pool_predicates_hold(p1: dict, p2: dict, max_diff: float = 0.10) -> bool:
    """Return True when both pools beat libjxl (< 1.0) on all 8 ratios and agree within max_diff."""
    both_under_1 = all(p[k] < 1.0 for p in (p1, p2) for k in POOL_RATIO_KEYS)
    ed = rel_diff(float(p1["enc_sum_ratio"]), float(p2["enc_sum_ratio"]))
    dd = rel_diff(float(p1["dec_sum_ratio"]), float(p2["dec_sum_ratio"]))
    return both_under_1 and ed <= max_diff and dd <= max_diff


def warm_binaries(warm_dir: Path, exe: Path, ffmpeg: str) -> None:
    """Run one untimed encode+decode pair so cold binary paging does not skew round 0."""
    warm_dir.mkdir(parents=True, exist_ok=True)
    warm_bmp = warm_dir / "warm.bmp"
    bm.Image.frombytes("RGB", (64, 64), bm.solid(64, 64, (120, 140, 160))).save(warm_bmp, format="BMP")
    run_one_codec("jxl", warm_bmp, warm_dir, "warm", exe=exe, ffmpeg=ffmpeg)
    run_one_codec("rf", warm_bmp, warm_dir, "warm", exe=exe, ffmpeg=ffmpeg)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sets", nargs="*", help=f"benchmark sets from {VALID_SETS}")
    parser.add_argument("--rounds", type=int, default=10, help="initial round count (>= 10, multiple of 5)")
    parser.add_argument("--max-rounds", type=int, default=30, help="maximum round cap before failing")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "build" / "research" / "jxl_alt")
    parser.add_argument("--load-file", type=Path, default=None)
    parser.add_argument("--exe", type=Path, default=bm.EXE)
    args = parser.parse_args(argv)
    if not args.sets:
        args.sets = list(VALID_SETS)
    for name in args.sets:
        if name not in VALID_SETS:
            parser.error(f"unknown set {name!r}: expected one of {', '.join(VALID_SETS)}")
    if args.rounds < 10 or args.rounds % 5 != 0:
        parser.error("--rounds must be >= 10 and a multiple of 5")
    if args.max_rounds < args.rounds:
        parser.error("--max-rounds must be >= --rounds")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    out_dir = args.out_dir if args.out_dir.is_absolute() else ROOT / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = args.exe if args.exe.is_absolute() else ROOT / args.exe
    work_root = out_dir / "jxl_alt_work"
    jsonl_path = out_dir / "jxl_alt_pools.jsonl"
    summary_path = out_dir / "jxl_alt_summary.json"
    jsonl_path.unlink(missing_ok=True)
    ffmpeg = _ffmpeg_bin()

    warm_binaries(work_root / "_warm", exe, ffmpeg)

    all_summary: dict[str, object] = {}
    all_passed = True

    for set_name in args.sets:
        folder = work_root / set_name
        items = bm.sources(set_name, folder)
        stems = [stem for stem, _ in items]
        ref_pixels = {stem: bm.pixels_of(src) for stem, src in items}
        rounds: list[dict] = []
        target_rounds = args.rounds
        r_idx = 0
        while r_idx < target_rounds:
            win_cpu = sample_win_cpu_pct(0.15)
            wsl_ld = sample_wsl_load(args.load_file)
            order = ("jxl", "rf") if r_idx % 2 == 0 else ("rf", "jxl")
            img_res: dict[str, dict[str, dict[str, int]]] = {}
            for stem, src in items:
                pair: dict[str, dict[str, int]] = {}
                for c in order:
                    pair[c] = run_one_codec(c, src, folder, stem, exe=exe, ffmpeg=ffmpeg)
                    if r_idx == 0:
                        dec_path = folder / (f"{stem}.rf.out.bmp" if c == "rf" else f"{stem}.jxl.out.bmp")
                        if bm.pixels_of(dec_path) != ref_pixels[stem]:
                            raise RuntimeError(f"pixel mismatch on {set_name}/{stem} ({c})")
                img_res[stem] = pair
            rec = {
                "set": set_name,
                "round": r_idx,
                "order": list(order),
                "win_cpu_pct": win_cpu,
                "wsl_load": wsl_ld,
                "images": img_res,
            }
            rounds.append(rec)
            with jsonl_path.open("a", encoding="utf-8", newline="\n") as fh:
                fh.write(json.dumps(rec) + "\n")
            r_idx += 1

            if r_idx == target_rounds:
                p1 = summarize_pool(rounds[target_rounds - 10 : target_rounds - 5], stems)
                p2 = summarize_pool(rounds[target_rounds - 5 : target_rounds], stems)
                if not pool_predicates_hold(p1, p2) and target_rounds + 5 <= args.max_rounds:
                    ed = rel_diff(float(p1["enc_sum_ratio"]), float(p2["enc_sum_ratio"]))
                    dd = rel_diff(float(p1["dec_sum_ratio"]), float(p2["dec_sum_ratio"]))
                    both_u1 = all(p[k] < 1.0 for p in (p1, p2) for k in POOL_RATIO_KEYS)
                    print(
                        f"[{set_name}] extending {target_rounds} -> {target_rounds + 5} "
                        f"(enc_diff={ed*100:.2f}% dec_diff={dd*100:.2f}% under_1={both_u1})",
                        flush=True,
                    )
                    target_rounds += 5

        n = len(rounds)
        p1 = summarize_pool(rounds[n - 10 : n - 5], stems)
        p2 = summarize_pool(rounds[n - 5 : n], stems)
        ed = rel_diff(float(p1["enc_sum_ratio"]), float(p2["enc_sum_ratio"]))
        dd = rel_diff(float(p1["dec_sum_ratio"]), float(p2["dec_sum_ratio"]))
        passed = pool_predicates_hold(p1, p2)
        if not passed:
            all_passed = False
        rf_bytes = sum(rounds[0]["images"][s]["rf"]["bytes"] for s in stems)
        jxl_bytes = sum(rounds[0]["images"][s]["jxl"]["bytes"] for s in stems)
        all_summary[set_name] = {
            "passed": passed,
            "rounds": n,
            "tail_slices": [f"[{n-10}:{n-5}]", f"[{n-5}:{n}]"],
            "rf_bytes": rf_bytes,
            "jxl_bytes": jxl_bytes,
            "bytes_ratio": rf_bytes / jxl_bytes,
            "pool1": p1,
            "pool2": p2,
            "enc_diff_pct": round(ed * 100.0, 3),
            "dec_diff_pct": round(dd * 100.0, 3),
        }
        verdict = "PASS" if passed else "FAIL"
        print(
            f"{set_name} {verdict} (rounds={n}, slices=[{n-10}:{n-5}] & [{n-5}:{n}], bytes={rf_bytes}/{jxl_bytes}={rf_bytes/jxl_bytes:.4f}):\n"
            f"  [{n-10}:{n-5}] enc_sum={p1['enc_sum_rf_us']}/{p1['enc_sum_jxl_us']}={p1['enc_sum_ratio']:.4f} "
            f"enc_gm={p1['enc_gm_ratio']:.4f} dec_sum={p1['dec_sum_rf_us']}/{p1['dec_sum_jxl_us']}={p1['dec_sum_ratio']:.4f} "
            f"dec_gm={p1['dec_gm_ratio']:.4f} enc_ws_sum={p1['enc_ws_sum_ratio']:.4f} enc_ws_max={p1['enc_ws_max_ratio']:.4f} "
            f"dec_ws_sum={p1['dec_ws_sum_ratio']:.4f} dec_ws_max={p1['dec_ws_max_ratio']:.4f} "
            f"win_cpu={p1['win_cpu_min']}..{p1['win_cpu_max']}% wsl_load={p1['wsl_loads']}\n"
            f"  [{n-5}:{n}] enc_sum={p2['enc_sum_rf_us']}/{p2['enc_sum_jxl_us']}={p2['enc_sum_ratio']:.4f} "
            f"enc_gm={p2['enc_gm_ratio']:.4f} dec_sum={p2['dec_sum_rf_us']}/{p2['dec_sum_jxl_us']}={p2['dec_sum_ratio']:.4f} "
            f"dec_gm={p2['dec_gm_ratio']:.4f} enc_ws_sum={p2['enc_ws_sum_ratio']:.4f} enc_ws_max={p2['enc_ws_max_ratio']:.4f} "
            f"dec_ws_sum={p2['dec_ws_sum_ratio']:.4f} dec_ws_max={p2['dec_ws_max_ratio']:.4f} "
            f"win_cpu={p2['win_cpu_min']}..{p2['win_cpu_max']}% wsl_load={p2['wsl_loads']}\n"
            f"  diff: enc={ed*100:.3f}% dec={dd*100:.3f}%",
            flush=True,
        )

    summary_path.write_text(json.dumps(all_summary, indent=2) + "\n", encoding="utf-8")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
