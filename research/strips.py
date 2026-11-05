#!/usr/bin/env python3
"""Measure the size cost of coding an image as S independent horizontal strips.

Usage:
  python3 research/strips.py --self-test
  python3 research/strips.py <set> <S> [--cm PATH] [--max-height H] [-v]
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import inspect
import json
import math
import re
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
RES = REPO / "build" / "research"

spec = importlib.util.spec_from_file_location("ab", TOOLS / "ab.py")
assert spec is not None and spec.loader is not None
ab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab)

M3_REF = "b181fb1edef63b42d29350a8b24096307a8596ee"
M4_REF = "fb27c317d4cae74e7d5aadd38789e3fbc68fc80c"
M4_GATE_REF = "0c1ec08ab66b2eca8d8ac66a1bce83c9e25cffe8"

EXPECTED_BASELINES: dict[str, dict[str, int]] = {
    "m5": {"photo": 30795260, "tp": 6640275, "synth": 201634},
    "m4": {"photo": 30812732, "tp": 6641390, "synth": 206229},
    "m1": {"photo": 30865211, "tp": 6651583, "synth": 206566},
}

BENCH_LINE_RE = re.compile(
    r"^(?:ppm/)?(\S+)\.ppm\s+(\d+)\s+bpp=\S+\s+enc=\s*([\d.]+)ms\s+dec=\s*([\d.]+)ms\s+\([^)]+\)\s+(\S+)$"
)


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    idx = 0
    n = len(data)
    tokens: list[bytes] = []
    while len(tokens) < 4 and idx < n:
        while idx < n and data[idx] in b" \t\r\n":
            idx += 1
        if idx < n and data[idx] == ord("#"):
            while idx < n and data[idx] != ord("\n"):
                idx += 1
            continue
        start = idx
        while idx < n and data[idx] not in b" \t\r\n#":
            idx += 1
        if idx > start:
            tokens.append(data[start:idx])
    if len(tokens) != 4 or tokens[0] != b"P6" or int(tokens[3]) != 255:
        raise ValueError(f"invalid P6 PPM header in {path}")
    # bench.c uses fscanf(...) followed by a single fgetc(f) whitespace byte
    idx += 1
    w, h = int(tokens[1]), int(tokens[2])
    expected = w * h * 3
    rgb = data[idx : idx + expected]
    if len(rgb) != expected:
        raise ValueError(f"truncated PPM payload in {path}: got {len(rgb)}, expected {expected}")
    return w, h, rgb


def file_sha256(path: Path) -> str:
    if not path.is_file():
        raise FileNotFoundError(f"missing file for sha256: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_ppm(path: Path, w: int, h: int, rgb: bytes) -> str:
    if len(rgb) != w * h * 3:
        raise ValueError(f"RGB length {len(rgb)} != {w * h * 3}")
    header = f"P6\n{w} {h}\n255\n".encode("ascii")
    payload = header + rgb
    if not path.is_file() or path.read_bytes() != payload:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
    return hashlib.sha256(payload).hexdigest()


def strip_row_bounds(height: int, num_strips: int) -> list[tuple[int, int]]:
    if num_strips < 1:
        raise ValueError(f"num_strips must be >= 1, got {num_strips}")
    if height < num_strips:
        raise ValueError(f"height {height} < num_strips {num_strips}")
    bounds: list[tuple[int, int]] = []
    for s in range(num_strips):
        y0 = (s * height) // num_strips
        y1 = ((s + 1) * height) // num_strips
        if y1 <= y0:
            raise ValueError(f"empty strip {s} for height={height}, S={num_strips}")
        bounds.append((y0, y1))
    return bounds


def split_ppm_strips(w: int, h: int, rgb: bytes, num_strips: int) -> list[tuple[int, int, bytes]]:
    row_bytes = w * 3
    bounds = strip_row_bounds(h, num_strips)
    out: list[tuple[int, int, bytes]] = []
    for y0, y1 in bounds:
        sh = y1 - y0
        out.append((w, sh, rgb[y0 * row_bytes : y1 * row_bytes]))
    if b"".join(chunk for _, _, chunk in out) != rgb:
        raise AssertionError("strip concatenation mismatch")
    return out


def parse_bench_rows_with_status(
    stdout: str, expected_imgs: list[str] | tuple[str, ...]
) -> dict[str, tuple[int, float, float, str]]:
    rows: dict[str, tuple[int, float, float, str]] = {}
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("encode failed"):
            raise ValueError(f"bench encode failure: {line}")
        m = BENCH_LINE_RE.match(line)
        if m:
            name, sz_s, enc_s, dec_s, status = m.groups()
            if status not in ("OK", "RAW", "PAL"):
                raise ValueError(f"bench round-trip failure on {name}: {status}")
            val = (int(sz_s), max(float(enc_s), 0.05), max(float(dec_s), 0.05), status)
            rows[name] = val
            rows[f"ppm/{name}"] = val
    missing = [img for img in expected_imgs if img not in rows]
    if missing:
        raise ValueError(f"bench output missing expected images: {missing}")
    return rows


def validate_and_adjust_strip_size(
    set_name: str,
    image_name: str,
    whole_size: int,
    whole_status: str,
    strip_sizes: list[int],
    strip_statuses: list[str],
) -> tuple[int, int, bool]:
    """Validate method statuses and return (raw_sum, adj_sum, is_pure_cm).

    On photographic sets (photo, tp, kodak, ic, sub, tp-only), every whole image
    and every strip crop must be coded by Method 1 (status == 'OK').
    On synth, non-OK crops are rejected from the CM single-container adjustment
    (-(S - 1) * 32 + 1) so palette/raw crops never enter the CM formula.
    """
    num_strips = len(strip_sizes)
    if num_strips < 1 or len(strip_statuses) != num_strips:
        raise ValueError("mismatched strip_sizes and strip_statuses")
    raw_sum = sum(strip_sizes)
    all_ok = whole_status == "OK" and all(st == "OK" for st in strip_statuses)
    if set_name != "synth" and not all_ok:
        raise ValueError(
            f"non-CM status on {set_name}:{image_name}: whole={whole_status}, strips={strip_statuses}"
        )
    if all_ok:
        adj_sum = raw_sum if num_strips == 1 else (raw_sum - (num_strips - 1) * 32 + 1)
        return raw_sum, adj_sum, True
    return raw_sum, raw_sum, False


def git_show_to(ref_path: str, dest: Path) -> None:
    proc = subprocess.run(
        ["git", "show", ref_path],
        cwd=REPO,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        check=True,
    )
    if not dest.is_file() or dest.read_bytes() != proc.stdout:
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(proc.stdout)


def ensure_baseline_sources(cm_path: Path) -> str:
    """Ensure baseline source files exist and match pinned git objects; return baseline key ('m5', 'm4', or 'm1')."""
    name = cm_path.name
    if name == "cm_m1.c":
        m1_dir = RES / "m1_src"
        for fname in ("fold.c", "pal.c", "bmp.c", "crc32.c", "rf.h", "rc.h"):
            git_show_to(f"{M3_REF}:src/{fname}", m1_dir / fname)
        git_show_to(f"{M3_REF}:src/cm.c", cm_path)
        return "m1"
    if name == "cm_m4.c":
        git_show_to(f"{M4_GATE_REF}:src/cm.c", cm_path)
        return "m4"
    if cm_path.resolve() == (REPO / "src" / "cm.c").resolve():
        return "m5"
    raise ValueError(f"unsupported baseline cm path: {cm_path}")


def baseline_source_digest(
    cm_path: Path,
    baseline_key: str,
    force_cm: bool,
    extra_inputs: tuple[Path, ...] = (),
    compiler_path: Path | None = None,
) -> str:
    clang_path = ab.CLANG if compiler_path is None else compiler_path
    src_dir = RES / "m1_src" if baseline_key == "m1" else REPO / "src"
    seen: set[Path] = set()
    files: list[Path] = []
    compiler_companions: list[Path] = [clang_path]
    if clang_path.parent.is_dir():
        for cname in ("clang", "clang.exe", "x86_64-w64-windows-gnu.cfg"):
            compiler_companions.append(clang_path.parent / cname)
    for candidate in [
        cm_path,
        *sorted(src_dir.glob("*.[ch]")),
        *sorted((REPO / "src").glob("*.[ch]")),
        *sorted((REPO / "include").glob("*.h")),
        TOOLS / "bench.c",
        TOOLS / "benchsys.c",
        REPO / "toolchain.json",
        *compiler_companions,
        *extra_inputs,
    ]:
        if candidate.is_file():
            resolved = candidate.resolve()
            if resolved not in seen:
                seen.add(resolved)
                files.append(resolved)
    recipe_str = (
        f"baseline={baseline_key}:force_cm={int(force_cm)}:clang={clang_path}\n"
        + inspect.getsource(build_strips_bench)
        + "\n"
        + inspect.getsource(ab.build)
    )
    h = hashlib.sha256(recipe_str.encode("utf-8"))
    if not clang_path.is_file():
        h.update(f"missing_clang:{clang_path}\0".encode("utf-8"))
    for p in files:
        h.update(p.name.encode("ascii") + b"\0" + file_sha256(p).encode("ascii") + b"\0")
    return h.hexdigest()


def is_valid_cache_entry(entry: object, expected_ppm_sha: str) -> bool:
    return (
        isinstance(entry, list)
        and len(entry) == 5
        and isinstance(entry[0], int)
        and entry[0] > 0
        and isinstance(entry[3], str)
        and entry[3] in ("OK", "RAW", "PAL")
        and entry[4] == expected_ppm_sha
    )


def run_bench_relpaths(exe: Path, rel_no_ext: list[str]) -> dict[str, tuple[int, float, float, str]]:
    out: dict[str, tuple[int, float, float, str]] = {}
    batch_size = 32
    for i in range(0, len(rel_no_ext), batch_size):
        batch = rel_no_ext[i : i + batch_size]
        basenames = [p.rsplit("/", 1)[-1] for p in batch]
        proc = subprocess.run(
            [str(exe), "-r1", "-t3", *[f"{p}.ppm" for p in batch]],
            cwd=RES,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0 or "MISMATCH" in proc.stdout or "failed" in proc.stdout:
            raise RuntimeError(f"bench run failed (rc={proc.returncode}):\n{proc.stdout}\n{proc.stderr}")
        parsed = parse_bench_rows_with_status(proc.stdout, basenames)
        for rel, base in zip(batch, basenames):
            out[rel] = parsed[base]
            out[base] = parsed[base]
    return out


def verify_whole_baselines(
    exe: Path,
    baseline_key: str,
    sets_to_check: tuple[str, ...],
    src_digest: str = "",
) -> dict[str, dict[str, tuple[int, float, float, str]]]:
    exe_sha = file_sha256(exe)
    cache_path = RES / "strips" / f"whole_{baseline_key}.json"
    cached: dict[str, object] = {}
    if cache_path.is_file():
        try:
            loaded = json.loads(cache_path.read_text(encoding="utf-8"))
            if (
                isinstance(loaded, dict)
                and loaded.get("_exe_sha256") == exe_sha
                and (not src_digest or loaded.get("_src_digest") == src_digest)
            ):
                cached = loaded
        except Exception:
            cached = {}
    dirty = False
    if cached.get("_exe_sha256") != exe_sha or (src_digest and cached.get("_src_digest") != src_digest):
        cached = {"_exe_sha256": exe_sha, "_src_digest": src_digest}
        dirty = True
    expected_map = EXPECTED_BASELINES.get(baseline_key, {})
    results: dict[str, dict[str, tuple[int, float, float, str]]] = {}
    for sname in sets_to_check:
        imgs = ab.SETS[sname]
        ppm_shas = {img: file_sha256(ab.PPM / f"{img}.ppm") for img in imgs}
        set_cache = cached.get(sname)
        if isinstance(set_cache, dict) and all(
            is_valid_cache_entry(set_cache.get(img), ppm_shas[img]) for img in imgs
        ):
            rows_map = {
                img: (
                    int(set_cache[img][0]),
                    float(set_cache[img][1]),
                    float(set_cache[img][2]),
                    str(set_cache[img][3]),
                )
                for img in imgs
            }
        else:
            rows = run_bench_relpaths(exe, [f"ppm/{img}" for img in imgs])
            rows_map = {img: rows[f"ppm/{img}"] for img in imgs}
            cached[sname] = {img: [*rows_map[img], ppm_shas[img]] for img in imgs}
            dirty = True
        tot = sum(rows_map[img][0] for img in imgs)
        exp = expected_map.get(sname)
        if exp is not None and tot != exp:
            raise RuntimeError(
                f"baseline {baseline_key} whole-image total mismatch on {sname}: got {tot}, expected {exp}"
            )
        results[sname] = rows_map
    if dirty:
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        cache_path.write_text(json.dumps(cached, indent=2), encoding="utf-8")
    return results


def build_strips_bench(cm_path: Path, exe: Path, baseline_key: str, force_cm: bool) -> str:
    src_digest = baseline_source_digest(cm_path, baseline_key, force_cm)
    stamp_path = RES / "strips" / f"{exe.name}.src_sha256"
    if exe.is_file() and stamp_path.is_file() and stamp_path.read_text(encoding="utf-8").strip() == src_digest:
        return src_digest
    if not force_cm:
        ab.build(cm_path, exe)
        stamp_path.parent.mkdir(parents=True, exist_ok=True)
        stamp_path.write_text(src_digest + "\n", encoding="utf-8")
        return src_digest
    is_m1 = baseline_key == "m1"
    src_dir = RES / "m1_src" if is_m1 else REPO / "src"
    fold_src = (src_dir / "fold.c").read_text(encoding="utf-8")
    fold_cm = (
        fold_src.replace(
            "uint8_t *f = rf_alloc(RF_HEADER_SIZE + raw + 256);",
            "size_t max_cm = 12u + 3u * ((size_t)img->width * img->height * 2u + 1024u);\n    uint8_t *f = rf_alloc(RF_HEADER_SIZE + max_cm);",
        )
        .replace(
            "rc = rf_pal_encode(img->rgb, img->width, img->height, f + RF_HEADER_SIZE, raw - 1, &pal_n, err);",
            "rc = RF_OK; pal_n = 0;",
        )
        .replace(
            "size_t cm_cap = raw - 1;",
            "size_t cm_cap = max_cm;",
        )
    )
    if "size_t cm_cap = max_cm;" not in fold_cm or "rc = RF_OK; pal_n = 0;" not in fold_cm:
        raise RuntimeError(f"failed to patch fold.c for force_cm on baseline {baseline_key}")
    fold_cm_path = RES / "strips" / f"fold_cm_{baseline_key}.c"
    fold_cm_path.parent.mkdir(parents=True, exist_ok=True)
    fold_cm_path.write_text(fold_cm, encoding="utf-8")

    cm_src = cm_path.read_text(encoding="utf-8")
    cm_force = cm_src.replace(
        "size_t each = (size_t)w * h + 64;",
        "size_t each = (size_t)w * h * 2u + 1024u;",
    )
    if "size_t each = (size_t)w * h * 2u + 1024u;" not in cm_force:
        raise RuntimeError(f"failed to patch cm.c per-channel cap for force_cm on baseline {baseline_key}")
    cm_force_path = RES / "strips" / f"cm_force_{baseline_key}.c"
    cm_force_path.write_text(cm_force, encoding="utf-8")

    extra_defs: list[str] = []
    inc_dirs = [f"-I{REPO / 'src'}"]
    if is_m1:
        rf_h_text = (src_dir / "rf.h").read_text(encoding="utf-8", errors="ignore")
        if "RF_METHOD_PAL" not in rf_h_text:
            extra_defs.append("-DRF_METHOD_PAL=2u")
        if "RF_METHOD_PAL2" not in rf_h_text:
            extra_defs.append("-DRF_METHOD_PAL2=3u")
        inc_dirs.insert(0, f"-I{src_dir}")
    cmd = [
        str(ab.CLANG),
        "-O2",
        "-march=x86-64-v2",
        "-mtune=generic",
        "-g",
        *extra_defs,
        *inc_dirs,
        "-o",
        str(exe),
        str(TOOLS / "bench.c"),
        str(TOOLS / "benchsys.c"),
        str(fold_cm_path),
        str(cm_force_path),
        str(src_dir / "pal.c"),
        str(src_dir / "bmp.c"),
        str(src_dir / "crc32.c"),
        "-lwinmm",
    ]
    r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"clang build failed for {exe}:\n{r.stderr}")
    stamp_path.parent.mkdir(parents=True, exist_ok=True)
    stamp_path.write_text(src_digest + "\n", encoding="utf-8")
    return src_digest


def measure_strips_for_set(
    set_name: str,
    num_strips: int,
    cm_path: Path,
    max_height: int | None = None,
    verbose: bool = False,
    force_cm: bool = False,
    ppm_dir: Path | None = None,
    set_images: list[str] | None = None,
) -> dict[str, object]:
    if max_height is not None and max_height <= 0:
        raise ValueError(f"no images in set {set_name!r} match max_height={max_height}")
    src_ppm_dir = ab.PPM if ppm_dir is None else ppm_dir
    candidate_imgs = ab.SETS[set_name] if set_images is None else set_images

    selected_ppms: list[tuple[str, int, int, bytes]] = []
    for img in candidate_imgs:
        ppm_path = src_ppm_dir / f"{img}.ppm"
        w, h, rgb = read_ppm(ppm_path)
        if max_height is not None and h > max_height:
            continue
        selected_ppms.append((img, w, h, rgb))
    if not selected_ppms:
        raise ValueError(f"no images in set {set_name!r} match max_height={max_height}")

    baseline_key = ensure_baseline_sources(cm_path)
    tag = f"{baseline_key}{'_cm' if force_cm else ''}"
    exe = RES / f"bench_strips_{tag}.exe"
    src_digest = build_strips_bench(cm_path, exe, baseline_key, force_cm)
    exe_sha = file_sha256(exe)

    if not force_cm:
        whole_by_set = verify_whole_baselines(exe, baseline_key, (set_name,), src_digest=src_digest)
        whole_rows = whole_by_set[set_name]
    else:
        whole_by_set = verify_whole_baselines(exe, f"{baseline_key}_cm", (set_name,), src_digest=src_digest)
        whole_rows = whole_by_set[set_name]

    imgs: list[str] = []
    dims: dict[str, tuple[int, int]] = {}
    strip_relpaths: list[str] = []
    strip_ppm_shas: dict[str, str] = {}
    for img, w, h, rgb in selected_ppms:
        imgs.append(img)
        dims[img] = (w, h)
        strips = split_ppm_strips(w, h, rgb, num_strips)
        for s_idx, (sw, sh, srgb) in enumerate(strips):
            rel = f"strips/S{num_strips}/{img}_s{s_idx}"
            out_ppm = RES / f"{rel}.ppm"
            strip_ppm_shas[rel] = write_ppm(out_ppm, sw, sh, srgb)
            strip_relpaths.append(rel)

    strip_cache_path = RES / "strips" / f"strips_{tag}_S{num_strips}.json"
    strip_cached: dict[str, object] = {}
    if strip_cache_path.is_file():
        try:
            loaded = json.loads(strip_cache_path.read_text(encoding="utf-8"))
            if (
                isinstance(loaded, dict)
                and loaded.get("_exe_sha256") == exe_sha
                and loaded.get("_src_digest") == src_digest
            ):
                strip_cached = loaded
        except Exception:
            strip_cached = {}
    if strip_cached.get("_exe_sha256") != exe_sha or strip_cached.get("_src_digest") != src_digest:
        strip_cached = {"_exe_sha256": exe_sha, "_src_digest": src_digest}
    missing_relpaths = [
        r for r in strip_relpaths if not is_valid_cache_entry(strip_cached.get(r), strip_ppm_shas[r])
    ]
    if missing_relpaths:
        fresh_rows = run_bench_relpaths(exe, missing_relpaths)
        for r in missing_relpaths:
            strip_cached[r] = [*fresh_rows[r], strip_ppm_shas[r]]
        strip_cache_path.parent.mkdir(parents=True, exist_ok=True)
        strip_cache_path.write_text(json.dumps(strip_cached, indent=2), encoding="utf-8")
    strip_rows = {
        r: (
            int(strip_cached[r][0]),
            float(strip_cached[r][1]),
            float(strip_cached[r][2]),
            str(strip_cached[r][3]),
        )
        for r in strip_relpaths
    }

    whole_total = 0
    raw_total = 0
    adj_total = 0
    raw_logs: list[float] = []
    adj_logs: list[float] = []
    worst_raw = ("", -1.0, 0, 0)
    worst_adj = ("", -1.0, 0, 0)
    non_cm_count = 0
    per_image: list[dict[str, object]] = []

    for img in imgs:
        w_sz, _, _, w_st = whole_rows[img]
        s_sizes = [strip_rows[f"strips/S{num_strips}/{img}_s{s}"][0] for s in range(num_strips)]
        s_sts = [strip_rows[f"strips/S{num_strips}/{img}_s{s}"][3] for s in range(num_strips)]
        raw_sum, adj_sum, is_cm = validate_and_adjust_strip_size(
            set_name, img, w_sz, w_st, s_sizes, s_sts
        )
        if force_cm and not is_cm:
            raise ValueError(f"force_cm expected OK on {img}, got whole={w_st} strips={s_sts}")
        if not is_cm:
            non_cm_count += 1
        whole_total += w_sz
        raw_total += raw_sum
        adj_total += adj_sum
        r_ratio = raw_sum / w_sz
        a_ratio = adj_sum / w_sz
        raw_logs.append(math.log(r_ratio))
        adj_logs.append(math.log(a_ratio))
        if r_ratio > worst_raw[1]:
            worst_raw = (img, r_ratio, w_sz, raw_sum)
        if a_ratio > worst_adj[1]:
            worst_adj = (img, a_ratio, w_sz, adj_sum)
        per_image.append(
            {
                "image": img,
                "width": dims[img][0],
                "height": dims[img][1],
                "whole": w_sz,
                "whole_status": w_st,
                "raw_sum": raw_sum,
                "adj_sum": adj_sum,
                "raw_pct": (r_ratio - 1.0) * 100.0,
                "adj_pct": (a_ratio - 1.0) * 100.0,
                "strip_statuses": s_sts,
                "is_cm": is_cm,
            }
        )

    raw_gm = math.exp(sum(raw_logs) / len(raw_logs))
    adj_gm = math.exp(sum(adj_logs) / len(adj_logs))
    raw_pct = (raw_total / whole_total - 1.0) * 100.0
    adj_pct = (adj_total / whole_total - 1.0) * 100.0

    label = f"{set_name}" + (f"(H<={max_height})" if max_height is not None else "") + (" [pure-CM]" if force_cm else "")
    if verbose:
        for row in per_image:
            st_tag = "CM" if row["is_cm"] else f"MIX({row['whole_status']}->{','.join(row['strip_statuses'])})"
            print(
                f"  {row['image']:28s} {row['width']:4d}x{row['height']:<4d} "
                f"whole={row['whole']:9d} raw={row['raw_sum']:9d} ({row['raw_pct']:+6.3f}%) "
                f"adj={row['adj_sum']:9d} ({row['adj_pct']:+6.3f}%) {st_tag}"
            )
    print(
        f"strips baseline={baseline_key} set={label} n={len(imgs)} S={num_strips} "
        f"whole={whole_total} raw_sum={raw_total} ({raw_pct:+.3f}%, geomean={raw_gm:.6f}, "
        f"worst={worst_raw[0]} {(worst_raw[1]-1.0)*100.0:+.3f}% [{worst_raw[2]}->{worst_raw[3]}]) "
        f"adj_sum={adj_total} ({adj_pct:+.3f}%, geomean={adj_gm:.6f}, "
        f"worst={worst_adj[0]} {(worst_adj[1]-1.0)*100.0:+.3f}% [{worst_adj[2]}->{worst_adj[3]}])"
        + (f" non_cm={non_cm_count}" if non_cm_count else "")
    )
    if force_cm and set_name == "synth" and any(r["image"] == "syn_noise" for r in per_image):
        nn_rows = [r for r in per_image if r["image"] != "syn_noise"]
        if nn_rows:
            nn_whole = sum(int(r["whole"]) for r in nn_rows)
            nn_raw = sum(int(r["raw_sum"]) for r in nn_rows)
            nn_adj = sum(int(r["adj_sum"]) for r in nn_rows)
            nn_raw_gm = math.exp(sum(math.log(int(r["raw_sum"]) / int(r["whole"])) for r in nn_rows) / len(nn_rows))
            nn_adj_gm = math.exp(sum(math.log(int(r["adj_sum"]) / int(r["whole"])) for r in nn_rows) / len(nn_rows))
            nn_w_raw = max(nn_rows, key=lambda r: int(r["raw_sum"]) / int(r["whole"]))
            nn_w_adj = max(nn_rows, key=lambda r: int(r["adj_sum"]) / int(r["whole"]))
            nn_raw_pct = (nn_raw / nn_whole - 1.0) * 100.0
            nn_adj_pct = (nn_adj / nn_whole - 1.0) * 100.0
            nn_label = f"{set_name}" + (f"(H<={max_height})" if max_height is not None else "") + " [pure-CM, no-noise]"
            print(
                f"strips baseline={baseline_key} set={nn_label} n={len(nn_rows)} S={num_strips} "
                f"whole={nn_whole} raw_sum={nn_raw} ({nn_raw_pct:+.3f}%, geomean={nn_raw_gm:.6f}, "
                f"worst={nn_w_raw['image']} {float(nn_w_raw['raw_pct']):+.3f}% [{nn_w_raw['whole']}->{nn_w_raw['raw_sum']}]) "
                f"adj_sum={nn_adj} ({nn_adj_pct:+.3f}%, geomean={nn_adj_gm:.6f}, "
                f"worst={nn_w_adj['image']} {float(nn_w_adj['adj_pct']):+.3f}% [{nn_w_adj['whole']}->{nn_w_adj['adj_sum']}])"
            )
    return {
        "baseline": baseline_key,
        "set": label,
        "n": len(imgs),
        "S": num_strips,
        "whole": whole_total,
        "raw_sum": raw_total,
        "raw_pct": raw_pct,
        "raw_geomean": raw_gm,
        "worst_raw": worst_raw,
        "adj_sum": adj_total,
        "adj_pct": adj_pct,
        "adj_geomean": adj_gm,
        "worst_adj": worst_adj,
        "non_cm": non_cm_count,
    }


def self_test() -> int:
    # 1. Exact floor(s*H/S) .. floor((s+1)*H/S)-1 partitioning and byte-exact reconstruction
    for w, h in ((17, 63), (64, 512), (31, 768), (23, 941)):
        rgb = bytes(((i * 73 + 19) & 0xFF) for i in range(w * h * 3))
        for s in (1, 2, 4):
            bounds = strip_row_bounds(h, s)
            assert bounds[0][0] == 0 and bounds[-1][1] == h
            for idx in range(s):
                assert bounds[idx] == ((idx * h) // s, ((idx + 1) * h) // s)
            strips = split_ppm_strips(w, h, rgb, s)
            assert sum(sh for _, sh, _ in strips) == h
            assert b"".join(chunk for _, _, chunk in strips) == rgb

    # 2. Method-status validation and CM single-container header adjustment
    raw2, adj2, is_cm2 = validate_and_adjust_strip_size(
        "photo", "kodim01", 500000, "OK", [250100, 250200], ["OK", "OK"]
    )
    assert (raw2, adj2, is_cm2) == (500300, 500300 - 32 + 1, True)

    raw4, adj4, is_cm4 = validate_and_adjust_strip_size(
        "photo", "kodim01", 500000, "OK", [125100, 125100, 125100, 125100], ["OK", "OK", "OK", "OK"]
    )
    assert (raw4, adj4, is_cm4) == (500400, 500400 - 96 + 1, True)

    # Non-OK status on photo/tp must raise ValueError
    for bad_set in ("photo", "tp"):
        try:
            validate_and_adjust_strip_size(
                bad_set, "bad_img", 10000, "OK", [5000, 5000], ["OK", "PAL"]
            )
        except ValueError:
            pass
        else:
            raise AssertionError(f"expected ValueError for PAL strip on {bad_set}")

    # Non-OK status on synth must not apply the CM header adjustment
    raw_syn, adj_syn, is_cm_syn = validate_and_adjust_strip_size(
        "synth", "syn_ui", 1410, "PAL", [800, 820], ["PAL", "OK"]
    )
    assert (raw_syn, adj_syn, is_cm_syn) == (1620, 1620, False)

    # 3. Bench output parser checks
    sample_out = (
        "ppm/kodim01.ppm 549519 bpp=8.944 enc=112.4ms dec=114.2ms (305 cyc/sample) OK\n"
        "strips/S2/syn_ui_s0.ppm 812 bpp=0.014 enc=4.1ms dec=1.2ms (12 cyc/sample) PAL\n"
    )
    parsed = parse_bench_rows_with_status(sample_out, ["kodim01", "strips/S2/syn_ui_s0"])
    assert parsed["kodim01"][0] == 549519 and parsed["kodim01"][3] == "OK"
    assert parsed["strips/S2/syn_ui_s0"][0] == 812 and parsed["strips/S2/syn_ui_s0"][3] == "PAL"
    try:
        parse_bench_rows_with_status(
            "ppm/bad.ppm 100 bpp=1.0 enc=1.0ms dec=1.0ms (1 cyc/sample) MISMATCH!\n",
            ["bad"],
        )
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError on MISMATCH!")

    # 4. Cache and crop provenance verification on repeat runs
    assert is_valid_cache_entry([500, 1.0, 1.0, "OK", "abc123"], "abc123")
    assert not is_valid_cache_entry([500, 1.0, 1.0, "OK"], "abc123")
    assert not is_valid_cache_entry([500, 1.0, 1.0, "OK", "wrong_sha"], "abc123")
    try:
        verify_whole_baselines(RES / "nonexistent_bench_exe.exe", "m5", ("photo",))
    except FileNotFoundError:
        pass
    else:
        raise AssertionError("expected FileNotFoundError when exe does not exist")

    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_ppm = Path(tmpdir) / "test.ppm"
        sha1 = write_ppm(tmp_ppm, 2, 2, bytes([10] * 12))
        # Overwrite with same dimensions (2x2) but different pixel bytes
        tmp_ppm.write_bytes(b"P6\n2 2\n255\n" + bytes([20] * 12))
        assert ab.ppm_dimensions(tmp_ppm) == (2, 2)
        sha2 = write_ppm(tmp_ppm, 2, 2, bytes([10] * 12))
        assert sha1 == sha2
        assert tmp_ppm.read_bytes() == b"P6\n2 2\n255\n" + bytes([10] * 12)

        # 5. Build provenance digest includes headers, build recipe, and compiler binary content at same path
        tmp_hdr = Path(tmpdir) / "simd_probe.h"
        tmp_clang = Path(tmpdir) / "x86_64-w64-mingw32-clang"
        tmp_hdr.write_text("#define SIMD_PROBE 1\n", encoding="utf-8")
        tmp_clang.write_bytes(b"fake-compiler-binary-v1")
        d1 = baseline_source_digest(
            REPO / "src" / "cm.c", "m5", False, extra_inputs=(tmp_hdr,), compiler_path=tmp_clang
        )
        tmp_hdr.write_text("#define SIMD_PROBE 2\n", encoding="utf-8")
        d2 = baseline_source_digest(
            REPO / "src" / "cm.c", "m5", False, extra_inputs=(tmp_hdr,), compiler_path=tmp_clang
        )
        assert d1 != d2, "changing an included header must change baseline_source_digest"
        d_force = baseline_source_digest(
            REPO / "src" / "cm.c", "m5", True, extra_inputs=(tmp_hdr,), compiler_path=tmp_clang
        )
        assert d2 != d_force, "force_cm flag must change baseline_source_digest"
        # Replace compiler binary in place at the exact same path
        tmp_clang.write_bytes(b"fake-compiler-binary-v2")
        d_clang_v2 = baseline_source_digest(
            REPO / "src" / "cm.c", "m5", False, extra_inputs=(tmp_hdr,), compiler_path=tmp_clang
        )
        assert d2 != d_clang_v2, "replacing compiler binary at same path must change baseline_source_digest"

        # 6. Empty height-filtered subset raises ValueError in isolation without compiler or corpus
        saved_clang = ab.CLANG
        try:
            ab.CLANG = Path(tmpdir) / "nonexistent_compiler"
            for bad_h in (0, 1):
                try:
                    measure_strips_for_set(
                        "photo",
                        2,
                        REPO / "src" / "cm.c",
                        max_height=bad_h,
                        ppm_dir=Path(tmpdir),
                        set_images=["test"],
                    )
                except ValueError as exc:
                    assert f"max_height={bad_h}" in str(exc)
                else:
                    raise AssertionError(f"expected ValueError on empty height-filtered subset (max_height={bad_h})")
        finally:
            ab.CLANG = saved_clang

    print("strips.py self-test: 10/10 passed")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("set_name", nargs="?", help="corpus set (photo, tp, synth)")
    parser.add_argument("strips", nargs="?", type=int, help="number of horizontal strips (e.g. 2 or 4)")
    parser.add_argument("--cm", type=Path, default=REPO / "src" / "cm.c", help="path to cm.c variant")
    parser.add_argument("--max-height", type=int, default=None, help="only include images with height <= H")
    parser.add_argument("--force-cm", action="store_true", help="disable palette method to measure pure Method 1 CM")
    parser.add_argument("-v", "--verbose", action="store_true", help="print per-image rows")
    parser.add_argument("--self-test", action="store_true", help="run unit self-tests")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.set_name or args.strips is None:
        parser.error("set_name and strips are required unless --self-test is given")
    if args.set_name not in ab.SETS:
        parser.error(f"unknown set {args.set_name!r}")

    measure_strips_for_set(
        args.set_name,
        args.strips,
        args.cm,
        max_height=args.max_height,
        verbose=args.verbose,
        force_cm=args.force_cm,
    )
    if args.set_name == "synth" and not args.force_cm:
        measure_strips_for_set(
            args.set_name,
            args.strips,
            args.cm,
            max_height=args.max_height,
            verbose=args.verbose,
            force_cm=True,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
