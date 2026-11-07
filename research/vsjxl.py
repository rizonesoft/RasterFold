"""Alternate RasterFold and ffmpeg/libjxl (or two RasterFold executables / corpus sets) runs; report medians and mins.

usage: python research/vsjxl.py [image.bmp] [-n 5] [--threads 1|3|6|9] [--exe PATH] [--alt-exe PATH]
       python research/vsjxl.py --corpus photo|tp|synth [-n 3] [--threads 1|3] [--pgo-profile PATH]
       python research/vsjxl.py --webp-photos [-n 3] [--threads 1|3|6|9] [--exe PATH]
"""
import argparse
import math
import os
import re
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
    p.add_argument("--threads", type=int, choices=(1, 3, 6, 9), default=None)
    p.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    p.add_argument("--alt-exe", type=Path, default=None)
    p.add_argument("--corpus", choices=("photo", "tp", "synth", "all"), default=None)
    p.add_argument("--webp-photos", action="store_true", help="compare RasterFold and lossless WebP decode wall time on testdata/photos")
    p.add_argument("--pgo-profile", type=Path, default=None)
    p.add_argument("--lto", action="store_true", help="compare multi-TU non-LTO vs LTO corpus builds")
    return p.parse_args(norm)


def parse_bench_rows(stdout: str, expected_imgs: tuple[str, ...]) -> dict[str, tuple[int, float, float]]:
    rows: dict[str, tuple[int, float, float]] = {}
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("encode failed"):
            raise RuntimeError(f"bench encode failure: {line}")
        m = re.match(
            r"^(?:ppm/)?(\S+)\.ppm\s+(\d+)\s+bpp=\S+\s+enc=\s*([\d.]+)ms\s+dec=\s*([\d.]+)ms\s+\([^)]+\)\s+(\S+)$",
            line,
        )
        if m:
            status = m.group(5)
            if status not in ("OK", "RAW", "PAL"):
                raise RuntimeError(f"bench round-trip failure on {m.group(1)}: {status}")
            rows[m.group(1)] = (int(m.group(2)), max(float(m.group(3)), 0.05), max(float(m.group(4)), 0.05))
    missing = [img for img in expected_imgs if img not in rows]
    if missing:
        raise RuntimeError(f"bench output missing expected images: {missing}")
    return rows


def build_bench_multi_tu(clang: Path, obj_dir: Path, out: Path, lto: bool, extra_cflags: tuple[str, ...] = ()) -> None:
    obj_dir.mkdir(parents=True, exist_ok=True)
    base_cflags = [
        str(clang),
        "-O2",
        "-march=x86-64-v2",
        "-mtune=generic",
        "-g",
        f"-I{REPO / 'src'}",
        *extra_cflags,
        *(["-flto"] if lto else []),
    ]
    units = (
        ("bench", REPO / "research" / "bench.c"),
        ("benchsys", REPO / "research" / "benchsys.c"),
        ("cm", REPO / "src" / "cm.c"),
        ("pal", REPO / "src" / "pal.c"),
        ("fold", REPO / "src" / "fold.c"),
        ("bmp", REPO / "src" / "bmp.c"),
        ("crc32", REPO / "src" / "crc32.c"),
    )
    objs: list[str] = []
    for name, src in units:
        obj = obj_dir / f"{name}.o"
        proc = subprocess.run([*base_cflags, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        if proc.returncode != 0:
            raise SystemExit(proc.stderr)
        objs.append(str(obj))
    link_cmd = [str(clang), "-fuse-ld=lld", *(["-flto"] if lto else []), *objs, "-lwinmm", "-o", str(out)]
    proc = subprocess.run(link_cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(proc.stderr)


def run_corpus_ab(set_name: str, n: int, threads: int, pgo_profile: Path | None, lto: bool = False) -> None:
    if str(REPO / "scripts") not in sys.path:
        sys.path.insert(0, str(REPO / "scripts"))
    if str(REPO / "research") not in sys.path:
        sys.path.insert(0, str(REPO / "research"))
    import ab  # noqa: E402
    import build as build_mod  # noqa: E402

    suffix = ".exe" if os.name == "nt" else ""
    tools = REPO / ("tools/llvm-mingw/bin" if os.name == "nt" else "tools/llvm-mingw-linux/bin")
    base_exe = ab.RES / "bench_base.exe"
    cand_exe = ab.RES / ("bench_lto.exe" if lto else "bench_pgo.exe")
    if lto:
        extra: tuple[str, ...] = ()
        if pgo_profile is not None:
            prof = build_mod.validate_pgo_profile(pgo_profile, tools, suffix)
            extra = (f"-fprofile-instr-use={prof}", "-Wno-profile-instr-unprofiled")
        build_bench_multi_tu(ab.CLANG, ab.RES / "objs_base", base_exe, lto=False, extra_cflags=extra)
        build_bench_multi_tu(ab.CLANG, ab.RES / "objs_lto", cand_exe, lto=True, extra_cflags=extra)
    else:
        prof_path = pgo_profile or (REPO / "build" / "pgo" / "rasterfold.profdata")
        prof = build_mod.validate_pgo_profile(prof_path, tools, suffix)
        ab.build(REPO / "src" / "cm.c", base_exe, ())
        ab.build(REPO / "src" / "cm.c", cand_exe, (f"-fprofile-instr-use={prof}", "-Wno-profile-instr-unprofiled"))
    sets = ("photo", "tp", "synth") if set_name == "all" else (set_name,)
    t_flag = f"-t{threads}"
    for sname in sets:
        imgs = ab.SETS[sname]
        base_runs = []
        pgo_runs = []
        for r in range(n):
            pair = [("base", base_exe, base_runs), ("cand", cand_exe, pgo_runs)]
            if r % 2 == 1:
                pair.reverse()
            for _, exe, lst in pair:
                proc = subprocess.run(
                    [str(exe), "-r1", t_flag, *[f"ppm/{img}.ppm" for img in imgs]],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    cwd=ab.RES,
                    check=True,
                )
                rows = parse_bench_rows(proc.stdout, imgs)
                lst.append(rows)
        base_tot_enc = 0.0
        pgo_tot_enc = 0.0
        base_tot_dec = 0.0
        pgo_tot_dec = 0.0
        enc_ratios = []
        dec_ratios = []
        for img in imgs:
            if base_runs[0][img][0] != pgo_runs[0][img][0]:
                raise SystemExit(f"size mismatch on {img}")
            be = min(x[img][1] for x in base_runs)
            pe = min(x[img][1] for x in pgo_runs)
            bd = min(x[img][2] for x in base_runs)
            pd = min(x[img][2] for x in pgo_runs)
            base_tot_enc += be
            pgo_tot_enc += pe
            base_tot_dec += bd
            pgo_tot_dec += pd
            enc_ratios.append((pe / be, img))
            dec_ratios.append((pd / bd, img))
        g_enc = math.exp(sum(math.log(x[0]) for x in enc_ratios) / len(enc_ratios))
        g_dec = math.exp(sum(math.log(x[0]) for x in dec_ratios) / len(dec_ratios))
        w_enc = max(enc_ratios)
        w_dec = max(dec_ratios)
        print(
            f"{sname:5s} {t_flag} n={len(imgs):2d} | dec min_tot={pgo_tot_dec/base_tot_dec:.3f} "
            f"({pgo_tot_dec:.1f}/{base_tot_dec:.1f}ms) geomean={g_dec:.3f} worst={w_dec[1]}({w_dec[0]:.3f}) | "
            f"enc min_tot={pgo_tot_enc/base_tot_enc:.3f} ({pgo_tot_enc:.1f}/{base_tot_enc:.1f}ms) "
            f"geomean={g_enc:.3f} worst={w_enc[1]}({w_enc[0]:.3f})",
            flush=True,
        )


def _ffmpeg_bin() -> str:
    return (
        shutil.which("ffmpeg")
        or shutil.which("ffmpeg.exe")
        or "/mnt/c/Users/DerickPayne/AppData/Local/Microsoft/WinGet/Links/ffmpeg.exe"
    )


def _ff_path(ffmpeg_bin: str, path: Path) -> str:
    s = str(path.resolve())
    if os.name != "nt" and ffmpeg_bin.lower().endswith(".exe") and s.startswith("/"):
        return subprocess.check_output(["wslpath", "-w", s], text=True).strip()
    return str(path)


def run_webp_photos(exe: Path, n: int, threads: int | None) -> None:
    from PIL import Image

    if str(REPO / "tests") not in sys.path:
        sys.path.insert(0, str(REPO / "tests"))
    import benchmark

    ffmpeg = _ffmpeg_bin()
    photos = sorted((REPO / "testdata" / "photos").glob("*.png"))
    tag = str(threads) if threads else "default"

    def _run_in(work_dir: Path) -> None:
        if os.name == "nt":
            local_exe = work_dir / "rasterfold.exe"
            shutil.copy2(exe, local_exe)
        else:
            local_exe = exe
        rf_totals: list[int] = []
        webp_totals: list[int] = []
        ratios: list[float] = []
        for p in photos:
            stem = p.stem
            bmp = work_dir / f"{stem}.bmp"
            fold = work_dir / f"{stem}.fold"
            webp = work_dir / f"{stem}.webp"
            out_rf = work_dir / f"{stem}.rf.bmp"
            out_webp = work_dir / f"{stem}.webp.bmp"
            with Image.open(p) as im:
                im.convert("RGB").save(bmp, "BMP")
            rc_enc, _, _ = benchmark.run([str(local_exe), "encode", str(bmp), str(fold)])
            if rc_enc != 0:
                raise SystemExit(f"encode failed on {stem}")
            rc_wenc, _, _ = benchmark.run(
                [ffmpeg, "-y", "-v", "error", "-i", _ff_path(ffmpeg, bmp), "-c:v", "libwebp", "-lossless", "1", "-quality", "100", "-compression_level", "6", "-threads", "1", _ff_path(ffmpeg, webp)]
            )
            if rc_wenc != 0:
                raise SystemExit(f"webp encode failed on {stem}")
            cmd_rf = [str(local_exe)]
            if threads:
                cmd_rf += ["--threads", str(threads)]
            cmd_rf += ["decode", str(fold), str(out_rf)]
            cmd_webp = [ffmpeg, "-y", "-v", "error", "-i", _ff_path(ffmpeg, webp), _ff_path(ffmpeg, out_webp)]
            rf_times: list[int] = []
            webp_times: list[int] = []
            for r in range(n):
                if r % 2 == 0:
                    out_rf.unlink(missing_ok=True)
                    rc, dt_rf, _ = benchmark.run(cmd_rf)
                    if rc != 0:
                        raise SystemExit(f"rf decode failed on {stem}")
                    rf_times.append(dt_rf)
                    out_webp.unlink(missing_ok=True)
                    rc, dt_w, _ = benchmark.run(cmd_webp)
                    if rc != 0:
                        raise SystemExit(f"webp decode failed on {stem}")
                    webp_times.append(dt_w)
                else:
                    out_webp.unlink(missing_ok=True)
                    rc, dt_w, _ = benchmark.run(cmd_webp)
                    if rc != 0:
                        raise SystemExit(f"webp decode failed on {stem}")
                    webp_times.append(dt_w)
                    out_rf.unlink(missing_ok=True)
                    rc, dt_rf, _ = benchmark.run(cmd_rf)
                    if rc != 0:
                        raise SystemExit(f"rf decode failed on {stem}")
                    rf_times.append(dt_rf)
            rf_med = sorted(rf_times)[len(rf_times) // 2]
            w_med = sorted(webp_times)[len(webp_times) // 2]
            rf_totals.append(rf_med)
            webp_totals.append(w_med)
            ratios.append(rf_med / w_med)
            print(f"{stem:28s} rf={rf_med:8.0f} us  webp={w_med:8.0f} us  ratio={rf_med/w_med:.3f}", flush=True)
        tot_rf = sum(rf_totals)
        tot_w = sum(webp_totals)
        geomean = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
        print(
            f"TOTAL threads={tag}: rf={tot_rf:.0f} ({tot_rf/1e6:.2f}s) webp={tot_w:.0f} ({tot_w/1e6:.2f}s) ratio={tot_rf/tot_w:.3f} geomean={geomean:.3f}",
            flush=True,
        )

    if os.name == "nt":
        with tempfile.TemporaryDirectory(prefix="rf_vsjxl_webp_") as td:
            _run_in(Path(td))
    else:
        work = TMP / f"webp_photos_{tag}"
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True, exist_ok=True)
        _run_in(work)


def main():
    args = parse_args(sys.argv[1:])
    if args.webp_photos:
        run_webp_photos(args.exe, args.n, args.threads)
        return
    if args.corpus is not None:
        run_corpus_ab(args.corpus, args.n, args.threads or 1, args.pgo_profile, lto=args.lto)
        return
    src = Path(args.image)
    n = args.n
    exe = args.exe
    alt = args.alt_exe
    t_args = ["--threads", str(args.threads)] if args.threads else []

    if os.name == "nt":
        ctx = tempfile.TemporaryDirectory(prefix="rf_vsjxl_")
    else:
        ctx = tempfile.TemporaryDirectory(prefix="rf_vsjxl_", dir=TMP)
    ffmpeg = _ffmpeg_bin()
    with ctx as td:
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
            res["jxl_enc"].append(wall([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", _ff_path(ffmpeg, local_src), "-c:v", "libjxl",
                                        "-distance", "0", "-effort", "7", "-threads", jxl_threads, "-update", "1", _ff_path(ffmpeg, jxl)]))
            res["jxl_dec"].append(wall([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-threads", jxl_threads, "-i", _ff_path(ffmpeg, jxl), "-frames:v", "1",
                                        "-update", "1", _ff_path(ffmpeg, work / "j.bmp")]))
        for k, v in res.items():
            print(f"{k:8s} median {statistics.median(v):8.1f} ms   min {min(v):8.1f}")
        print(f"size rf={fold.stat().st_size} jxl={jxl.stat().st_size} ratio={fold.stat().st_size / jxl.stat().st_size:.4f}")
        print(f"decode ratio (median) {statistics.median(res['rf_dec']) / statistics.median(res['jxl_dec']):.3f}  "
              f"encode ratio {statistics.median(res['rf_enc']) / statistics.median(res['jxl_enc']):.3f}")


if __name__ == "__main__":
    main()
