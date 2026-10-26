"""Speed and size research helpers for src/cm.c. Run from the repository root.
Data (corpus, JPEG XL / WebP sizes, executables) lives in build/research; see research/corpus.py.

  python research/ab.py bench [cm.c] [-r3] images...      time one cm.c (default: src/cm.c)
  python research/ab.py ab A.c B.c [-n 5] images...        alternate two variants, compare minima
  python research/ab.py size [cm.c] SET [--csv PATH]       exact sizes vs JPEG XL e7, e9, and WebP, in parallel
  python research/ab.py compat [REF]                       build baseline REF into build/research/vs/rasterfold-m1.exe and cm_m1.c, verify 55-image .fold byte identity and decode
Images may be names in build/research/ppm or set names: sub, kodak, ic, synth, photo, all, tp, tp-only.
"""
import ast
import csv
import hashlib
import io
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from datetime import date
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
SETS["tp-only"] = [n for n in SETS["tp"] if n not in SETS["photo"]]
SETS["all"] = SETS["photo"] + SETS["tp-only"] + SETS["synth"]
CORPUS_CSV_FIELDS = ("set", "image", "width", "height", "rasterfold", "jxl_e7", "jxl_e9", "webp")
HEADER_COMMENT_RE = re.compile(
    r"^# codec_revision=(?P<codec_revision>[0-9a-f]{40}) kat_fold_sha256=(?P<kat_fold_sha256>[0-9a-f]{64}) date=(?P<date>\d{4}-\d{2}-\d{2})$"
)
PROVENANCE_PATHS = ("src", "include", "research/bench.c", "research/benchsys.c", "tests/test_correctness.py")


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


def ppm_dimensions(ppm_path: Path) -> tuple[int, int]:
    if not ppm_path.is_file():
        raise SystemExit(f"missing PPM file: {ppm_path}")
    data = ppm_path.read_bytes()
    if not data.startswith(b"P6"):
        raise SystemExit(f"invalid PPM magic in {ppm_path}")
    pos = 2
    tokens: list[bytes] = []
    n = len(data)
    while len(tokens) < 3 and pos < n:
        while pos < n and data[pos:pos + 1] in (b" ", b"\t", b"\r", b"\n"):
            pos += 1
        if pos < n and data[pos:pos + 1] == b"#":
            while pos < n and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < n and data[pos:pos + 1] not in (b" ", b"\t", b"\r", b"\n", b"#"):
            pos += 1
        if pos > start:
            tokens.append(data[start:pos])
    if len(tokens) != 3 or pos >= n or data[pos:pos + 1] not in (b" ", b"\t", b"\r", b"\n"):
        raise SystemExit(f"invalid PPM header in {ppm_path}")
    pos += 1
    try:
        width = int(tokens[0].decode("ascii"))
        height = int(tokens[1].decode("ascii"))
        maxval = int(tokens[2].decode("ascii"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise SystemExit(f"invalid PPM header numbers in {ppm_path}: {exc}") from exc
    if width <= 0 or height <= 0 or maxval != 255:
        raise SystemExit(f"invalid PPM dimensions/maxval in {ppm_path}: {width}x{height} maxval={maxval}")
    expected_pixels = width * height * 3
    if len(data) - pos != expected_pixels:
        raise SystemExit(
            f"invalid PPM payload length in {ppm_path}: got {len(data) - pos}, expected {expected_pixels}"
        )
    return width, height


def image_set_label(name: str) -> str:
    if name in SETS["photo"]:
        return "photo"
    if name in SETS["tp"]:
        return "tp"
    if name in SETS["synth"]:
        return "synth"
    raise SystemExit(f"unknown corpus set for image: {name}")


def kat_fold_sha256(repo: Path = REPO) -> str:
    tc_path = repo / "tests" / "test_correctness.py"
    if not tc_path.is_file():
        raise SystemExit(f"missing {tc_path}")
    text = tc_path.read_text(encoding="utf-8")
    try:
        tree = ast.parse(text, filename=str(tc_path))
    except SyntaxError as exc:
        raise SystemExit(f"syntax error in {tc_path}: {exc}") from exc
    matches = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name) and node.targets[0].id == "KAT_FOLD":
            matches.append(node.value)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.target.id == "KAT_FOLD" and node.value is not None:
            matches.append(node.value)
    if len(matches) != 1:
        raise SystemExit(f"unable to locate unique KAT_FOLD in {tc_path}")
    call = matches[0]
    if (
        not isinstance(call, ast.Call)
        or not isinstance(call.func, ast.Attribute)
        or not isinstance(call.func.value, ast.Name)
        or call.func.value.id != "bytes"
        or call.func.attr != "fromhex"
        or len(call.args) != 1
        or call.keywords
        or not isinstance(call.args[0], ast.Constant)
        or not isinstance(call.args[0].value, str)
    ):
        raise SystemExit(f"unsupported KAT_FOLD definition in {tc_path}")
    try:
        raw = bytes.fromhex(call.args[0].value)
    except ValueError as exc:
        raise SystemExit(f"invalid KAT_FOLD hex literal in {tc_path}: {exc}") from exc
    if not raw:
        raise SystemExit(f"empty KAT_FOLD in {tc_path}")
    return hashlib.sha256(raw).hexdigest()


def _provenance_digests(repo: Path = REPO) -> dict[str, str]:
    digests: dict[str, str] = {}
    for rel in PROVENANCE_PATHS:
        target = repo / rel
        if target.is_dir():
            for child in sorted(target.rglob("*")):
                if child.is_file():
                    digests[child.relative_to(repo).as_posix()] = hashlib.sha256(child.read_bytes()).hexdigest()
        elif target.is_file():
            digests[target.relative_to(repo).as_posix()] = hashlib.sha256(target.read_bytes()).hexdigest()
        else:
            raise SystemExit(f"missing provenance path: {target}")
    return digests


def provenance_snapshot(
    src: Path,
    flags: list[str] | tuple[str, ...] = (),
    repo: Path = REPO,
    runner=subprocess.run,
) -> tuple[str, str, dict[str, str]]:
    head_sha, kat_hash = codec_provenance(src, flags, repo=repo, runner=runner)
    return head_sha, kat_hash, _provenance_digests(repo)


def codec_provenance(
    src: Path,
    flags: list[str] | tuple[str, ...] = (),
    repo: Path = REPO,
    runner=subprocess.run,
) -> tuple[str, str]:
    canonical_cm = (repo / "src" / "cm.c").resolve()
    if Path(src).resolve() != canonical_cm:
        raise SystemExit(f"--csv requires canonical {canonical_cm}, got {src}")
    if flags:
        raise SystemExit(f"--csv refuses custom compiler flags: {list(flags)}")
    rev_proc = runner(
        ["git", "rev-parse", "--verify", "HEAD"],
        cwd=repo,
        capture_output=True,
        text=True,
    )
    head_sha = (rev_proc.stdout or "").strip() if rev_proc.returncode == 0 else ""
    if not re.fullmatch(r"[0-9a-f]{40}", head_sha):
        raise SystemExit("unable to determine clean 40-char git HEAD for --csv")
    status_proc = runner(
        ["git", "status", "--porcelain", "--", *PROVENANCE_PATHS],
        cwd=repo,
        capture_output=True,
        text=True,
    )
    if status_proc.returncode != 0:
        raise SystemExit("unable to check git status for --csv provenance")
    if (status_proc.stdout or "").strip():
        raise SystemExit(
            f"--csv refuses modified codec/bench sources:\n{(status_proc.stdout or '').strip()}"
        )
    return head_sha, kat_fold_sha256(repo)


def read_corpus_csv(csv_path: Path) -> tuple[dict[str, str], list[dict]]:
    if not csv_path.is_file():
        raise SystemExit(f"missing corpus CSV: {csv_path}")
    raw_lines = csv_path.read_text(encoding="utf-8").splitlines()
    if len(raw_lines) < 2:
        raise SystemExit(f"invalid corpus CSV (too few lines): {csv_path}")
    m = HEADER_COMMENT_RE.fullmatch(raw_lines[0].strip())
    if not m:
        raise SystemExit(f"invalid corpus CSV header comment in {csv_path}: {raw_lines[0]!r}")
    meta = dict(m.groupdict())
    reader = csv.DictReader(io.StringIO("\n".join(raw_lines[1:]) + "\n"))
    if tuple(reader.fieldnames or ()) != CORPUS_CSV_FIELDS:
        raise SystemExit(f"invalid corpus CSV columns in {csv_path}: {reader.fieldnames!r}")
    rows: list[dict] = []
    seen_images: set[str] = set()
    for r in reader:
        img = (r.get("image") or "").strip()
        set_name = (r.get("set") or "").strip()
        if not img or img in seen_images:
            raise SystemExit(f"duplicate or empty image row in {csv_path}: {img!r}")
        seen_images.add(img)
        if set_name not in ("photo", "tp", "synth") or image_set_label(img) != set_name:
            raise SystemExit(f"invalid set label for {img} in {csv_path}: {set_name!r}")
        try:
            row = {
                "set": set_name,
                "image": img,
                "width": int(r["width"]),
                "height": int(r["height"]),
                "rasterfold": int(r["rasterfold"]),
                "jxl_e7": int(r["jxl_e7"]),
                "jxl_e9": int(r["jxl_e9"]),
                "webp": int(r["webp"]),
            }
        except (KeyError, TypeError, ValueError) as exc:
            raise SystemExit(f"invalid numeric field for {img} in {csv_path}: {exc}") from exc
        if any(row[k] <= 0 for k in ("width", "height", "rasterfold", "jxl_e7", "jxl_e9", "webp")):
            raise SystemExit(f"non-positive numeric field for {img} in {csv_path}: {row}")
        rows.append(row)
    if not rows:
        raise SystemExit(f"empty corpus CSV: {csv_path}")
    return meta, rows


def write_corpus_csv(
    csv_path: Path,
    got: list[tuple[str, int]],
    refs: dict,
    *,
    src: Path | None = None,
    flags: list[str] | tuple[str, ...] = (),
    ppm_dir: Path = PPM,
    repo: Path = REPO,
    runner=subprocess.run,
    today: str | None = None,
    expected_snapshot: tuple[str, str, dict[str, str]] | None = None,
) -> tuple[dict[str, str], list[dict]]:
    if not got:
        raise SystemExit("no images to write to corpus CSV")
    csv_path = Path(csv_path)
    if csv_path.is_symlink() or csv_path.is_dir():
        raise SystemExit(f"refusing to write corpus CSV to symlink or directory: {csv_path}")
    post_snapshot = provenance_snapshot(
        src if src is not None else repo / "src" / "cm.c",
        flags,
        repo=repo,
        runner=runner,
    )
    if expected_snapshot is not None and post_snapshot != expected_snapshot:
        raise SystemExit("--csv refused: codec/bench provenance changed during measurement")
    head_sha, kat_hash, _ = post_snapshot
    stamp_date = today or date.today().isoformat()
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", stamp_date):
        raise SystemExit(f"invalid date for corpus CSV: {stamp_date!r}")
    seen: set[str] = set()
    rows: list[dict] = []
    for name, s in got:
        if not name or name in seen:
            raise SystemExit(f"duplicate or empty image in corpus CSV export: {name!r}")
        seen.add(name)
        if not is_positive_int(s):
            raise SystemExit(f"invalid measured size for {name}: {s!r}")
        j7 = ref_size(refs, name, "e7")
        j9 = ref_size(refs, name, "e9")
        wb = ref_size(refs, name, "webp")
        w, h = ppm_dimensions(ppm_dir / f"{name}.ppm")
        rows.append(
            {
                "set": image_set_label(name),
                "image": name,
                "width": w,
                "height": h,
                "rasterfold": s,
                "jxl_e7": j7,
                "jxl_e9": j9,
                "webp": wb,
            }
        )
    buf = io.StringIO()
    buf.write(f"# codec_revision={head_sha} kat_fold_sha256={kat_hash} date={stamp_date}\n")
    writer = csv.DictWriter(buf, fieldnames=list(CORPUS_CSV_FIELDS), lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    payload = buf.getvalue().encode("utf-8")

    csv_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path: Path | None = None
    try:
        fd, tmp_str = tempfile.mkstemp(
            prefix=f".{csv_path.name}.",
            suffix=".tmp",
            dir=csv_path.parent,
        )
        tmp_path = Path(tmp_str)
        with os.fdopen(fd, "wb") as fh:
            fh.write(payload)
            fh.flush()
            os.fsync(fh.fileno())
        verified = read_corpus_csv(tmp_path)
        os.replace(tmp_path, csv_path)
        tmp_path = None
    except OSError as exc:
        if tmp_path is not None:
            tmp_path.unlink(missing_ok=True)
        raise SystemExit(f"failed writing corpus CSV {csv_path}: {exc}") from exc
    except BaseException:
        if tmp_path is not None:
            tmp_path.unlink(missing_ok=True)
        raise
    return verified


def reconstruct_set_summary(csv_path: Path, set_name: str) -> dict:
    if set_name not in SETS:
        raise SystemExit(f"unknown set name: {set_name!r}")
    _, rows = read_corpus_csv(csv_path)
    by_image = {r["image"]: r for r in rows}
    members = SETS[set_name]
    missing = [n for n in members if n not in by_image]
    if missing:
        raise SystemExit(f"missing {set_name} image(s) in {csv_path}: {missing}")
    selected = [by_image[n] for n in members]
    rf_tot = sum(r["rasterfold"] for r in selected)
    e7_tot = sum(r["jxl_e7"] for r in selected)
    e9_tot = sum(r["jxl_e9"] for r in selected)
    wb_tot = sum(r["webp"] for r in selected)
    e7_ratios = [r["rasterfold"] / r["jxl_e7"] for r in selected]
    e9_ratios = [r["rasterfold"] / r["jxl_e9"] for r in selected]
    wb_ratios = [r["rasterfold"] / r["webp"] for r in selected]
    worst_row = max(selected, key=lambda r: r["rasterfold"] / r["jxl_e7"])
    return {
        "set": set_name,
        "images": [r["image"] for r in selected],
        "count": len(selected),
        "rasterfold": rf_tot,
        "jxl_e7": e7_tot,
        "jxl_e9": e9_tot,
        "webp": wb_tot,
        "e7_ratio": rf_tot / e7_tot,
        "e9_ratio": rf_tot / e9_tot,
        "webp_ratio": rf_tot / wb_tot,
        "e7_geomean": math.exp(sum(math.log(x) for x in e7_ratios) / len(e7_ratios)),
        "e9_geomean": math.exp(sum(math.log(x) for x in e9_ratios) / len(e9_ratios)),
        "webp_geomean": math.exp(sum(math.log(x) for x in wb_ratios) / len(wb_ratios)),
        "worst_image": worst_row["image"],
        "worst_e7_ratio": worst_row["rasterfold"] / worst_row["jxl_e7"],
        "below_e9": sum(1 for r in selected if r["rasterfold"] < r["jxl_e9"]),
    }


def extract_csv_arg(args: list[str]) -> tuple[Path | None, list[str]]:
    csv_path: Path | None = None
    cleaned: list[str] = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--csv":
            if csv_path is not None or i + 1 >= len(args) or not args[i + 1].strip():
                raise SystemExit("--csv requires a single path argument")
            csv_path = Path(args[i + 1])
            i += 2
            continue
        if arg.startswith("--csv="):
            val = arg.split("=", 1)[1].strip()
            if csv_path is not None or not val:
                raise SystemExit("--csv requires a single path argument")
            csv_path = Path(val)
            i += 1
            continue
        cleaned.append(arg)
        i += 1
    return csv_path, cleaned


COMPAT_SETS = ("photo", "tp", "tp-only", "synth")


def extract_allow_diff_args(args: list[str]) -> tuple[set[str], list[str]]:
    allowed: set[str] = set()
    cleaned: list[str] = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--allow-diff":
            if i + 1 >= len(args) or not args[i + 1].strip() or args[i + 1].startswith("-"):
                raise SystemExit("--allow-diff requires a set name")
            raw_val = args[i + 1].strip()
            i += 2
        elif arg.startswith("--allow-diff="):
            raw_val = arg.split("=", 1)[1].strip()
            if not raw_val:
                raise SystemExit("--allow-diff requires a set name")
            i += 1
        elif arg.startswith("--"):
            raise SystemExit(f"unknown option: {arg}")
        else:
            cleaned.append(arg)
            i += 1
            continue
        parts = [p.strip() for p in raw_val.split(",") if p.strip()]
        if not parts:
            raise SystemExit("--allow-diff requires a set name")
        for p in parts:
            if p not in COMPAT_SETS:
                raise SystemExit(f"unknown compat set for --allow-diff: {p!r}")
            allowed.add(p)
    return allowed, cleaned


def names(args):
    out = []
    for a in args:
        if a.startswith("-t"):
            continue
        if not a.startswith("-"):
            out += SETS.get(a, [a])
    return out


def is_palette_source(src_path: Path) -> bool:
    if src_path.is_file():
        src_text = src_path.read_text(encoding="utf-8", errors="ignore")
        has_pal = bool(re.search(r"\brf_pal_encode\b", src_text))
        has_cm = bool(re.search(r"\brf_cm_encode\b", src_text))
        if has_pal != has_cm:
            return has_pal
    name = src_path.name.lower()
    return "pal" in name and "cm" not in name


def is_m1_baseline_source(src_path: Path) -> bool:
    m1_dir = RES / "m1_src"
    if not (m1_dir / "fold.c").is_file():
        return False
    try:
        resolved = src_path.resolve()
        if resolved == (RES / "cm_m1.c").resolve():
            return True
        if resolved.parent == m1_dir.resolve():
            return True
    except OSError:
        pass
    return src_path.name == "cm_m1.c"


def build(src, out, flags=()):
    src_path = Path(src)
    if is_m1_baseline_source(src_path):
        m1_dir = RES / "m1_src"
        rf_h_text = (m1_dir / "rf.h").read_text(encoding="utf-8", errors="ignore") if (m1_dir / "rf.h").is_file() else ""
        extra_defs = []
        if "RF_METHOD_PAL" not in rf_h_text:
            extra_defs.append("-DRF_METHOD_PAL=2u")
        if "RF_METHOD_PAL2" not in rf_h_text:
            extra_defs.append("-DRF_METHOD_PAL2=3u")
        pal_srcs = [str(m1_dir / "pal.c")] if (m1_dir / "pal.c").is_file() else []
        cmd = [
            str(CLANG), "-O2", "-march=x86-64-v2", "-mtune=generic", "-g", *extra_defs, *flags,
            f"-I{m1_dir}", f"-I{REPO / 'src'}", "-o", str(out),
            str(TOOLS / "bench.c"), str(TOOLS / "benchsys.c"),
            str(m1_dir / "fold.c"), str(src_path), *pal_srcs,
            str(m1_dir / "bmp.c"), str(m1_dir / "crc32.c"), "-lwinmm",
        ]
    else:
        is_pal_variant = is_palette_source(src_path)
        cm_src = REPO / "src" / "cm.c" if is_pal_variant else src_path
        pal_src = src_path if is_pal_variant else REPO / "src" / "pal.c"
        cmd = [
            str(CLANG), "-O2", "-march=x86-64-v2", "-mtune=generic", "-g", *flags,
            f"-I{REPO / 'src'}", "-o", str(out),
            str(TOOLS / "bench.c"), str(TOOLS / "benchsys.c"),
            str(REPO / "src" / "fold.c"), str(cm_src), str(pal_src),
            str(REPO / "src" / "bmp.c"), str(REPO / "src" / "crc32.c"), "-lwinmm",
        ]
    r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr)


THREADS = next((a for a in sys.argv if a.startswith("-t")), "-t1")


def run(exe, imgs, reps=1):
    out = subprocess.run([str(exe), f"-r{reps}", THREADS, *[f"ppm/{n}.ppm" for n in imgs]], stdin=subprocess.DEVNULL, capture_output=True, text=True, cwd=RES).stdout
    if "MISMATCH" in out or "failed" in out:
        raise SystemExit("bad run:\n" + out)
    return out


def total(out):
    m = re.search(r"TOTAL (\d+)\s+enc=([\d.]+)ms dec=([\d.]+)ms", out)
    return int(m.group(1)), float(m.group(2)), float(m.group(3))


def parse_bench_rows(stdout: str, expected_imgs: list[str] | tuple[str, ...]) -> dict[str, tuple[int, float, float]]:
    rows: dict[str, tuple[int, float, float]] = {}
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("encode failed"):
            raise SystemExit(f"bench encode failure: {line}")
        m = re.match(
            r"^(?:ppm/)?(\S+)\.ppm\s+(\d+)\s+bpp=\S+\s+enc=\s*([\d.]+)ms\s+dec=\s*([\d.]+)ms\s+\([^)]+\)\s+(\S+)$",
            line,
        )
        if m:
            status = m.group(5)
            if status not in ("OK", "RAW", "PAL"):
                raise SystemExit(f"bench round-trip failure on {m.group(1)}: {status}")
            rows[m.group(1)] = (int(m.group(2)), max(float(m.group(3)), 0.05), max(float(m.group(4)), 0.05))
    missing = [img for img in expected_imgs if img not in rows]
    if missing:
        raise SystemExit(f"bench output missing expected images: {missing}")
    return rows


def verify_compat_sets(
    sets_map: dict[str, list[str]],
    load_image,
    encode_blob,
    decode_blob,
    allow_diff_sets: set[str],
    bench_m1_sizes: dict[str, int] | None = None,
) -> list[str]:
    cache: dict[str, tuple[bytes, bytes]] = {}
    lines: list[str] = []
    for sname in COMPAT_SETS:
        imgs = sets_map[sname]
        m1_h = hashlib.sha256()
        cand_h = hashlib.sha256()
        m1_total = 0
        cand_total = 0
        changed = 0
        for img in imgs:
            if img not in cache:
                w, h, rgb = load_image(img)
                m1_t1 = encode_blob("m1", "1", img, w, h, rgb)
                m1_t3 = encode_blob("m1", "3", img, w, h, rgb)
                if m1_t3 != m1_t1:
                    raise SystemExit(f"MISMATCH on {img} (m1 t=3)")
                cand_t1 = encode_blob("cand", "1", img, w, h, rgb)
                cand_t3 = encode_blob("cand", "3", img, w, h, rgb)
                if cand_t3 != cand_t1:
                    raise SystemExit(f"MISMATCH on {img} (cand t=3)")
                if bench_m1_sizes is not None:
                    if img not in bench_m1_sizes or bench_m1_sizes[img] != len(m1_t1):
                        got_sz = bench_m1_sizes.get(img)
                        raise SystemExit(
                            f"BENCH MISMATCH on {img}: bench_m1={got_sz} vs m1_cli={len(m1_t1)}"
                        )
                for src_tag, blob in (("m1", m1_t1), ("cand", cand_t1)):
                    for t in ("1", "3"):
                        if decode_blob("cand", t, img, src_tag, blob) != (w, h, rgb):
                            raise SystemExit(f"DECODE MISMATCH on {img} ({src_tag}->cand t={t})")
                cache[img] = (m1_t1, cand_t1)
            m1_b, cand_b = cache[img]
            if cand_b != m1_b:
                if sname not in allow_diff_sets:
                    raise SystemExit(f"MISMATCH on {img} in set {sname} (cand vs m1)")
                changed += 1
            m1_total += len(m1_b)
            cand_total += len(cand_b)
            m1_h.update(m1_b)
            cand_h.update(cand_b)
        bench_flag = " bench_m1_verified=True" if bench_m1_sizes is not None else ""
        if changed == 0:
            lines.append(
                f"{sname:7s} n={len(imgs):2d} total_bytes={cand_total} "
                f"fold_stream_sha256={cand_h.hexdigest()} identical=True m1_decoded=True cand_decoded=True{bench_flag}"
            )
        else:
            lines.append(
                f"{sname:7s} n={len(imgs):2d} total_bytes={cand_total} m1_total_bytes={m1_total} "
                f"fold_stream_sha256={cand_h.hexdigest()} m1_fold_stream_sha256={m1_h.hexdigest()} "
                f"identical=False ({changed}/{len(imgs)} changed) m1_decoded=True cand_decoded=True{bench_flag}"
            )
    return lines


def main():
    mode, rest = sys.argv[1], sys.argv[2:]
    csv_path, rest = extract_csv_arg(rest)
    if csv_path is not None and mode != "size":
        raise SystemExit(f"--csv is only supported in size mode, not {mode!r}")
    allow_diff_sets, rest = extract_allow_diff_args(rest)
    if allow_diff_sets and mode != "compat":
        raise SystemExit(f"--allow-diff is only supported in compat mode, not {mode!r}")
    verbose = "-v" in sys.argv
    rest = [a for a in rest if a != "-v"]
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
        row_runs = {"A": [], "B": []}
        for i in range(n):
            if i % 2 == 0:
                out_a = run(RES / "benchA.exe", imgs)
                out_b = run(RES / "benchB.exe", imgs)
            else:
                out_b = run(RES / "benchB.exe", imgs)
                out_a = run(RES / "benchA.exe", imgs)
            rows_a = parse_bench_rows(out_a, imgs)
            rows_b = parse_bench_rows(out_b, imgs)
            res["A"].append(total(out_a))
            res["B"].append(total(out_b))
            if verbose:
                row_runs["A"].append(rows_a)
                row_runs["B"].append(rows_b)
        for k, v in res.items():
            enc = [x[1] for x in v]; dec = [x[2] for x in v]
            print(f"{k}: size={v[0][0]}  enc min={min(enc):7.1f} med={statistics.median(enc):7.1f}   dec min={min(dec):7.1f} med={statistics.median(dec):7.1f}")
        print(f"B/A: enc {min(x[1] for x in res['B']) / min(x[1] for x in res['A']):.3f}  dec {min(x[2] for x in res['B']) / min(x[2] for x in res['A']):.3f}")
        if verbose:
            a_tot_enc = b_tot_enc = a_tot_dec = b_tot_dec = 0.0
            enc_ratios = []
            dec_ratios = []
            for img in imgs:
                ae = min(x[img][1] for x in row_runs["A"])
                be = min(x[img][1] for x in row_runs["B"])
                ad = min(x[img][2] for x in row_runs["A"])
                bd = min(x[img][2] for x in row_runs["B"])
                a_tot_enc += ae
                b_tot_enc += be
                a_tot_dec += ad
                b_tot_dec += bd
                enc_ratios.append((be / ae, img))
                dec_ratios.append((bd / ad, img))
            g_enc = math.exp(sum(math.log(x[0]) for x in enc_ratios) / len(enc_ratios))
            g_dec = math.exp(sum(math.log(x[0]) for x in dec_ratios) / len(dec_ratios))
            w_enc = max(enc_ratios)
            w_dec = max(dec_ratios)
            print(
                f"min_tot (n={len(imgs)}): dec={b_tot_dec/a_tot_dec:.3f} ({b_tot_dec:.1f}/{a_tot_dec:.1f}ms) "
                f"geomean={g_dec:.3f} worst={w_dec[1]}({w_dec[0]:.3f}) | "
                f"enc={b_tot_enc/a_tot_enc:.3f} ({b_tot_enc:.1f}/{a_tot_enc:.1f}ms) "
                f"geomean={g_enc:.3f} worst={w_enc[1]}({w_enc[0]:.3f})"
            )
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
        src = Path(rest.pop(0)) if rest and rest[0].endswith(".c") else REPO / "src" / "cm.c"
        pre_snapshot = provenance_snapshot(src, flags) if csv_path is not None else None
        imgs = names(rest)
        build(src, RES / "benchS.exe", flags)
        refs = json.loads((RES / "jxl_refs.json").read_text(encoding="utf-8"))

        def one(n):
            out = ""
            for _ in range(3):
                out = run(RES / "benchS.exe", [n])
                rows = parse_bench_rows(out, [n])
                if n in rows:
                    return n, rows[n][0]
            raise SystemExit(f"no size for {n}: {out!r}")
        with ThreadPoolExecutor(8) as pool:
            got = list(pool.map(one, imgs))
        summary = summarize_sizes(got, refs, verbose="-v" in sys.argv)
        if csv_path is not None:
            write_corpus_csv(
                csv_path,
                got,
                refs,
                src=src,
                flags=flags,
                expected_snapshot=pre_snapshot,
            )
        print(summary)
    elif mode == "compat":
        baseline_ref = rest[0] if rest else "fbf1036de2cfd0eeaee1a55399563e2d2200328c"
        if str(REPO / "tests") not in sys.path:
            sys.path.insert(0, str(REPO / "tests"))
        import rfi_ref as rf  # noqa: E402

        m1_dir = RES / "m1_src"
        m1_dir.mkdir(parents=True, exist_ok=True)
        for old in m1_dir.iterdir():
            if old.is_file():
                old.unlink()
        src_entries = set(
            subprocess.check_output(
                ["git", "ls-tree", "--name-only", f"{baseline_ref}:src"],
                cwd=REPO,
                text=True,
            ).splitlines()
        )
        c_stems = [
            s
            for s in ("platform", "api", "crc32", "bmp", "fold", "cm", "pal", "mem", "sys", "main")
            if f"{s}.c" in src_entries
        ]
        h_files = sorted(f for f in src_entries if f.endswith(".h"))
        for fname in [*(f"{s}.c" for s in c_stems), *h_files]:
            txt = subprocess.check_output(["git", "show", f"{baseline_ref}:src/{fname}"], cwd=REPO)
            (m1_dir / fname).write_bytes(txt)
        inc_dir = m1_dir / "include"
        inc_dir.mkdir(exist_ok=True)
        (inc_dir / "rasterfold.h").write_bytes(
            subprocess.check_output(["git", "show", f"{baseline_ref}:include/rasterfold.h"], cwd=REPO)
        )
        cc = (
            REPO / "tools" / "llvm-mingw" / "bin" / "clang.exe"
            if os.name == "nt"
            else REPO / "tools" / "llvm-mingw-linux" / "bin" / "clang"
        )
        objs = []
        for s in c_stems:
            obj = m1_dir / f"{s}.o"
            subprocess.run(
                [
                    str(cc),
                    "--target=x86_64-w64-mingw32",
                    "-std=c11",
                    "-O2",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-parameter",
                    "-fno-builtin",
                    "-fno-ident",
                    "-fvisibility=hidden",
                    "-march=x86-64-v2",
                    "-mtune=generic",
                    "-gcodeview",
                    "-ffreestanding",
                    "-fno-stack-protector",
                    f"-I{m1_dir}",
                    f"-I{inc_dir}",
                    "-c",
                    str(m1_dir / f"{s}.c"),
                    "-o",
                    str(obj),
                ],
                check=True,
            )
            objs.append(str(obj))
        vs_dir = RES / "vs"
        vs_dir.mkdir(parents=True, exist_ok=True)
        m1_exe = vs_dir / "rasterfold-m1.exe"
        subprocess.run(
            [
                str(cc),
                "--target=x86_64-w64-mingw32",
                "-fuse-ld=lld",
                "-nostdlib",
                *objs,
                "-o",
                str(m1_exe),
                "-Wl,--entry=rf_start",
                "-Wl,--subsystem,console",
                "-lkernel32",
                "-lshell32",
            ],
            check=True,
        )
        (RES / "cm_m1.c").write_bytes((m1_dir / "cm.c").read_bytes())
        bench_m1_exe = RES / "bench_m1.exe"
        build(RES / "cm_m1.c", bench_m1_exe)
        all_imgs = list(dict.fromkeys(img for sname in COMPAT_SETS for img in SETS[sname]))
        bench_m1_rows = parse_bench_rows(run(bench_m1_exe, all_imgs, reps=1), all_imgs)
        bench_m1_sizes = {img: row[0] for img, row in bench_m1_rows.items()}
        cand_exe = REPO / "bin" / "windows-x64" / "bin" / "rasterfold.exe"
        print(f"m1_exe_sha256={hashlib.sha256(m1_exe.read_bytes()).hexdigest()}")
        print(f"cand_exe_sha256={hashlib.sha256(cand_exe.read_bytes()).hexdigest()}")
        print(f"bench_m1_exe_sha256={hashlib.sha256(bench_m1_exe.read_bytes()).hexdigest()}")
        with tempfile.TemporaryDirectory(prefix="rf_m1_eq_") as td:
            work = Path(td)
            exe_map = {"m1": m1_exe, "cand": cand_exe}

            def load_image(img: str) -> tuple[int, int, bytes]:
                ppm_path = PPM / f"{img}.ppm"
                w, h = ppm_dimensions(ppm_path)
                raw_len = w * h * 3
                rgb = ppm_path.read_bytes()[-raw_len:]
                bmp_path = work / f"{img}.bmp"
                bmp_path.write_bytes(rf.write_bmp24(w, h, rgb))
                return w, h, rgb

            def encode_blob(tag: str, t: str, img: str, _w: int, _h: int, _rgb: bytes) -> bytes:
                out_fold = work / f"{img}_{tag}_t{t}.fold"
                out_fold.unlink(missing_ok=True)
                subprocess.run(
                    [str(exe_map[tag]), "encode", "--threads", t, str(work / f"{img}.bmp"), str(out_fold)],
                    stdin=subprocess.DEVNULL,
                    check=True,
                    capture_output=True,
                )
                return out_fold.read_bytes()

            def decode_blob(tag: str, t: str, img: str, src_tag: str, blob: bytes) -> tuple[int, int, bytes]:
                src_fold = work / f"{img}_{src_tag}_t1.fold"
                if not src_fold.is_file() or src_fold.read_bytes() != blob:
                    src_fold.write_bytes(blob)
                dec_bmp = work / f"{img}_{src_tag}_to_{tag}_dec_t{t}.bmp"
                dec_bmp.unlink(missing_ok=True)
                subprocess.run(
                    [str(exe_map[tag]), "decode", "--threads", t, str(src_fold), str(dec_bmp)],
                    stdin=subprocess.DEVNULL,
                    check=True,
                    capture_output=True,
                )
                return rf.read_bmp24(dec_bmp.read_bytes())

            for line in verify_compat_sets(
                SETS,
                load_image,
                encode_blob,
                decode_blob,
                allow_diff_sets,
                bench_m1_sizes=bench_m1_sizes,
            ):
                print(line)


if __name__ == "__main__":
    main()
