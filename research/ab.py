"""Speed and size research helpers for src/cm.c. Run from the repository root.
Data (corpus, JPEG XL / WebP sizes, executables) lives in build/research; see research/corpus.py.

  python research/ab.py bench [cm.c] [-r3] images...      time one cm.c (default: src/cm.c)
  python research/ab.py ab A.c B.c [-n 5] images...        alternate two variants, compare minima
  python research/ab.py size [cm.c] SET                    exact sizes vs JPEG XL e7, e9, and WebP, in parallel
Images may be names in build/research/ppm or set names: sub, kodak, ic, synth, photo, all, tp.
"""
import json
import math
import os
import re
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
RES = REPO / "build" / "research"
CLANG = (
    REPO / "tools" / "llvm-mingw" / "bin" / "clang.exe"
    if os.name == "nt"
    else REPO / "tools" / "llvm-mingw-linux" / "bin" / "x86_64-w64-mingw32-clang"
)
PPM = RES / "ppm"
SETS = {
    "sub": ["measure", "kodim01", "kodim13", "kodim23", "ic_deer", "ic_fireworks", "ic_nightshot_iso_1600", "ic_artificial"],
    "kodak": [f"kodim{i:02d}" for i in range(1, 25)],
    "ic": ["ic_artificial", "ic_big_building", "ic_big_tree", "ic_bridge", "ic_cathedral", "ic_deer", "ic_fireworks",
           "ic_flower_foveon", "ic_hdr", "ic_leaves_iso_1600", "ic_leaves_iso_200", "ic_nightshot_iso_100",
           "ic_nightshot_iso_1600", "ic_spider_web"],
    "tp": ["kodim01", "kodim03", "kodim05", "kodim13", "kodim23", "tp_deer_crop", "tp_fireworks_crop", "tp_flower_foveon_crop",
           "tp_nightshot_iso_1600_crop", "tp_spider_web_crop"],
    "synth": ["syn_solid", "syn_checker", "syn_stripes", "syn_gradient", "syn_noise", "syn_pixel", "syn_ui", "syn_text",
              "syn_diagram", "syn_odd", "syn_wide"],
}
SETS["photo"] = ["measure"] + SETS["kodak"] + SETS["ic"]
SETS["all"] = SETS["photo"] + SETS["synth"]


def is_positive_int(value: object) -> bool:
    return type(value) is int and value > 0


def ref_size(refs: dict, name: str, kind: str) -> int:
    if not isinstance(refs, dict):
        raise SystemExit("invalid jxl_refs.json: root must be a JSON object")
    key = f"{name}:{kind}"
    if key not in refs:
        raise SystemExit(f"missing reference in jxl_refs.json: {key}")
    val = refs[key]
    if not is_positive_int(val):
        raise SystemExit(f"invalid reference in jxl_refs.json: {key}={val!r}")
    return val


def summarize_sizes(got: list[tuple[str, int]], refs: dict, verbose: bool = False, prefix: str = "") -> str:
    if not got:
        raise SystemExit("no images to summarize")
    rows: list[tuple[str, int, int, int, int]] = []
    for n, s in got:
        if not is_positive_int(s):
            raise SystemExit(f"invalid measured size for {n}: {s!r}")
        j7 = ref_size(refs, n, "e7")
        j9 = ref_size(refs, n, "e9")
        wb = ref_size(refs, n, "webp")
        rows.append((n, s, j7, j9, wb))
    lines: list[str] = []
    tot = tj7 = tj9 = twb = 0
    logs7: list[float] = []
    logs9: list[float] = []
    logsw: list[float] = []
    worst7 = ("", -1.0)
    worst9 = ("", -1.0)
    worstw = ("", -1.0)
    meas_row: tuple[int, int, int, int] | None = None
    for n, s, j7, j9, wb in rows:
        tot += s
        tj7 += j7
        tj9 += j9
        twb += wb
        r7 = s / j7
        r9 = s / j9
        rw = s / wb
        logs7.append(math.log(r7))
        logs9.append(math.log(r9))
        logsw.append(math.log(rw))
        if r7 > worst7[1]:
            worst7 = (n, r7)
        if r9 > worst9[1]:
            worst9 = (n, r9)
        if rw > worstw[1]:
            worstw = (n, rw)
        if n == "measure":
            meas_row = (s, j7, j9, wb)
        if verbose:
            lines.append(
                f"  {n:28s} {s:9d} jxl={j7:9d} {r7:.4f} e9={j9:9d} {r9:.4f} webp={wb:9d} {rw:.4f}"
            )
    gm7 = math.exp(sum(logs7) / len(logs7))
    gm9 = math.exp(sum(logs9) / len(logs9))
    gmw = math.exp(sum(logsw) / len(logsw))
    ms = ""
    if meas_row is not None:
        ms_s, ms_j7, ms_j9, ms_wb = meas_row
        ms = f" measure={ms_s} ({ms_s / ms_j7:.4f} e9={ms_s / ms_j9:.4f} webp={ms_s / ms_wb:.4f})"
    lines.append(
        f"{prefix}total={tot}"
        f" jxl={tj7} ratio={tot / tj7:.4f} geomean={gm7:.4f} worst={worst7[1]:.4f} ({worst7[0]})"
        f" e9={tj9} e9_ratio={tot / tj9:.4f} e9_geomean={gm9:.4f} e9_worst={worst9[1]:.4f} ({worst9[0]})"
        f" webp={twb} webp_ratio={tot / twb:.4f} webp_geomean={gmw:.4f} webp_worst={worstw[1]:.4f} ({worstw[0]})"
        f"{ms}"
    )
    return "\n".join(lines)


def names(args):
    out = []
    for a in args:
        if a.startswith("-t"):
            continue
        if not a.startswith("-"):
            out += SETS.get(a, [a])
    return out


def build(src, out, flags=()):
    cmd = [str(CLANG), "-O2", "-march=x86-64-v2", "-mtune=generic", "-g", *flags, f"-I{REPO / 'src'}", "-o", str(out),
           str(TOOLS / "bench.c"), str(TOOLS / "benchsys.c"), str(src), str(REPO / "src" / "crc32.c"), "-lwinmm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr)


THREADS = next((a for a in sys.argv if a.startswith("-t")), "-t1")


def run(exe, imgs, reps=1):
    out = subprocess.run([str(exe), f"-r{reps}", THREADS, *[f"ppm/{n}.ppm" for n in imgs]], capture_output=True, text=True, cwd=RES).stdout
    if "MISMATCH" in out or "failed" in out:
        raise SystemExit("bad run:\n" + out)
    return out


def total(out):
    m = re.search(r"TOTAL (\d+)\s+enc=([\d.]+)ms dec=([\d.]+)ms", out)
    return int(m.group(1)), float(m.group(2)), float(m.group(3))


def main():
    mode, rest = sys.argv[1], sys.argv[2:]
    flags = [a for a in rest if a.startswith("-D") or a.startswith("-O") or a.startswith("-m")]
    rest = [a for a in rest if a not in flags]
    if mode == "bench":
        src = rest.pop(0) if rest and rest[0].endswith(".c") else REPO / "src" / "cm.c"
        reps = next((int(a[2:]) for a in rest if a.startswith("-r")), 3)
        imgs = names([a for a in rest if not a.startswith("-r")])
        build(src, RES / "bench.exe", flags)
        print(run(RES / "bench.exe", imgs, reps), end="")
    elif mode == "ab":
        a, b = rest[0], rest[1]
        n = next((int(x[2:]) for x in rest if x.startswith("-n")), 5)
        imgs = names([x for x in rest[2:] if not x.startswith("-n")])
        build(a, RES / "benchA.exe", flags)
        build(b, RES / "benchB.exe", flags)
        res = {"A": [], "B": []}
        for _ in range(n):
            res["A"].append(total(run(RES / "benchA.exe", imgs)))
            res["B"].append(total(run(RES / "benchB.exe", imgs)))
        for k, v in res.items():
            enc = [x[1] for x in v]; dec = [x[2] for x in v]
            print(f"{k}: size={v[0][0]}  enc min={min(enc):7.1f} med={statistics.median(enc):7.1f}   dec min={min(dec):7.1f} med={statistics.median(dec):7.1f}")
        print(f"B/A: enc {min(x[1] for x in res['B']) / min(x[1] for x in res['A']):.3f}  dec {min(x[2] for x in res['B']) / min(x[2] for x in res['A']):.3f}")
    elif mode == "rsize":
        exe, cfg = RES / rest[0], rest[1]
        imgs = names(rest[2:])
        refs = json.loads((RES / "jxl_refs.json").read_text(encoding="utf-8"))

        def one(n):
            out = subprocess.run([str(exe), cfg, f"ppm/{n}.ppm"], capture_output=True, text=True, cwd=RES).stdout
            return n, int(re.search(r"^\S+\s+(\d+)", out, re.M).group(1))
        with ThreadPoolExecutor(10) as pool:
            got = list(pool.map(one, imgs))
        print(summarize_sizes(got, refs, verbose="-v" in sys.argv, prefix=f"{rest[0]} {cfg} "))
    elif mode == "size":
        src = rest.pop(0) if rest and rest[0].endswith(".c") else REPO / "src" / "cm.c"
        imgs = names(rest)
        build(src, RES / "benchS.exe", flags)
        refs = json.loads((RES / "jxl_refs.json").read_text(encoding="utf-8"))

        def one(n):
            for _ in range(3):
                out = run(RES / "benchS.exe", [n])
                m = re.search(r"^\S+\s+(\d+)", out, re.M)
                if m:
                    return n, int(m.group(1))
            raise SystemExit(f"no size for {n}: {out!r}")
        with ThreadPoolExecutor(8) as pool:
            got = list(pool.map(one, imgs))
        print(summarize_sizes(got, refs, verbose="-v" in sys.argv))


if __name__ == "__main__":
    main()
