"""Aggregate build/research/prof_rips.txt into line and function profiles.

Make the samples with: python research/ab.py bench measure, then
build/research/bench.exe -p -r3 build/research/ppm/measure.ppm (run inside build/research).
"""
import collections
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
RES = REPO / "build" / "research"
SYM = REPO / "tools" / "llvm-mingw" / "bin" / "llvm-symbolizer.exe"
exe = sys.argv[2] if len(sys.argv) > 2 else str(RES / "bench.exe")
rips = (RES / "prof_rips.txt").read_text().split()
uniq = sorted(set(rips))
out = subprocess.run([str(SYM), "--obj=" + exe, "--inlining"], input="\n".join(uniq), capture_output=True, text=True).stdout
blocks = out.strip().split("\n\n")
info = {a: [(b.splitlines()[i], b.splitlines()[i + 1]) for i in range(0, len(b.splitlines()) - 1, 2)] for a, b in zip(uniq, blocks)}
cnt = collections.Counter(rips)
by_line, by_func = collections.Counter(), collections.Counter()
for addr, n in cnt.items():
    fn, loc = (info.get(addr) or [("?", "?")])[0]
    parts = loc.split(":")
    line = parts[-2] if len(parts) >= 3 else "?"
    by_line[f"{Path(':'.join(parts[:-2])).name}:{line}  {fn}"] += n
    by_func[fn] += n
total = sum(cnt.values())
top = int(sys.argv[1]) if len(sys.argv) > 1 else 40
print(f"samples {total}")
for k, v in by_func.most_common(15):
    print(f"{100 * v / total:6.2f}%  {k}")
print("---")
for k, v in by_line.most_common(top):
    print(f"{100 * v / total:6.2f}%  {k}")
