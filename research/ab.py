"""Speed and size research helpers for src/cm.c. Run from the repository root.
Data (corpus, JPEG XL sizes, executables) lives in build/research; see research/corpus.py.

  python research/ab.py bench [cm.c] [-r3] images...      time one cm.c (default: src/cm.c)
  python research/ab.py ab A.c B.c [-n 5] images...        alternate two variants, compare minima
  python research/ab.py size [cm.c] SET                    exact sizes vs JPEG XL e7, in parallel
Images may be names in build/research/ppm or set names: sub, kodak, ic, synth, photo, all, tp.
"""
import json
import math
import re
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
RES = REPO / "build" / "research"
CLANG = REPO / "tools" / "llvm-mingw" / "bin" / "clang.exe"
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
    out = subprocess.run([str(exe), f"-r{reps}", THREADS, *[str(PPM / f"{n}.ppm") for n in imgs]], capture_output=True, text=True, cwd=RES).stdout
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
        refs = json.loads((RES / "jxl_refs.json").read_text())

        def one(n):
            out = subprocess.run([str(exe), cfg, str(PPM / f"{n}.ppm")], capture_output=True, text=True).stdout
            return n, int(re.search(r"^\S+\s+(\d+)", out, re.M).group(1))
        with ThreadPoolExecutor(10) as pool:
            got = list(pool.map(one, imgs))
        tot = tj = 0; logs = []
        for n, s in got:
            j = refs[f"{n}:e7"]; tot += s; tj += j; logs.append(math.log(s / j))
            if "-v" in sys.argv:
                print(f"  {n:28s} {s:9d} jxl={j:9d} {s / j:.4f}")
        meas = [s for n, s in got if n == "measure"]
        ms = f" measure={meas[0]} ({meas[0] / refs['measure:e7']:.4f})" if meas else ""
        print(f"{rest[0]} {cfg} total={tot} ratio={tot / tj:.4f} geomean={math.exp(sum(logs) / len(logs)):.4f} worst={math.exp(max(logs)):.4f}{ms}")
    elif mode == "size":
        src = rest.pop(0) if rest and rest[0].endswith(".c") else REPO / "src" / "cm.c"
        imgs = names(rest)
        build(src, RES / "benchS.exe", flags)
        refs = json.loads((RES / "jxl_refs.json").read_text())

        def one(n):
            for _ in range(3):
                out = run(RES / "benchS.exe", [n])
                m = re.search(r"^\S+\s+(\d+)", out, re.M)
                if m:
                    return n, int(m.group(1))
            raise SystemExit(f"no size for {n}: {out!r}")
        with ThreadPoolExecutor(8) as pool:
            got = list(pool.map(one, imgs))
        tot = tj = 0; logs = []
        for n, s in got:
            j = refs[f"{n}:e7"]; tot += s; tj += j; logs.append(math.log(s / j))
            if "-v" in sys.argv:
                print(f"  {n:28s} {s:9d} jxl={j:9d} {s / j:.4f}")
        meas = [s for n, s in got if n == "measure"]
        ms = f" measure={meas[0]} ({meas[0] / refs['measure:e7']:.4f})" if meas else ""
        print(f"total={tot} jxl={tj} ratio={tot / tj:.4f} geomean={math.exp(sum(logs) / len(logs)):.4f} worst={math.exp(max(logs)):.4f}{ms}")


if __name__ == "__main__":
    main()
