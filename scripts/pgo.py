#!/usr/bin/env python3
"""Generate and validate the reproducible PGO profile build/pgo/rasterfold.profdata."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))
import build as build_mod  # noqa: E402

DEFAULT_OUT = ROOT / 'build' / 'pgo' / 'rasterfold.profdata'
PGO_EXE = ROOT / 'bin' / 'windows-x64-pgo' / 'bin' / 'rasterfold.exe'
TRACKED_SOURCES = build_mod.PGO_TRACKED_SOURCES


def has_wsl_bridge() -> bool:
    return (
        os.name != 'nt'
        and shutil.which('wslpath') is not None
        and Path('/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe').is_file()
    )


def write_bmp24(width: int, height: int, rgb: bytes) -> bytes:
    stride = (width * 3 + 3) & ~3
    image_size = stride * height
    out = bytearray(54 + image_size)
    struct.pack_into('<2sIHHI', out, 0, b'BM', 54 + image_size, 0, 0, 54)
    struct.pack_into('<IiiHHIIiiII', out, 14, 40, width, height, 1, 24, 0, image_size, 3780, 3780, 0, 0)
    for y in range(height):
        src_row = rgb[(height - 1 - y) * width * 3 : (height - y) * width * 3]
        dst = 54 + y * stride
        out[dst : dst + width * 3 : 3] = src_row[2::3]
        out[dst + 1 : dst + width * 3 : 3] = src_row[1::3]
        out[dst + 2 : dst + width * 3 : 3] = src_row[0::3]
    return bytes(out)


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    idx = 0
    tokens: list[bytes] = []
    while len(tokens) < 4:
        while idx < len(data) and data[idx] in b' \t\r\n':
            idx += 1
        if idx < len(data) and data[idx] == ord('#'):
            while idx < len(data) and data[idx] not in b'\r\n':
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b' \t\r\n#':
            idx += 1
        tokens.append(data[start:idx])
    if tokens[0] != b'P6' or int(tokens[3]) != 255:
        raise ValueError(f'unsupported PPM header in {path}')
    w, h = int(tokens[1]), int(tokens[2])
    idx += 1
    rgb = data[idx : idx + w * h * 3]
    if len(rgb) != w * h * 3:
        raise ValueError(f'truncated PPM payload in {path}')
    return w, h, rgb


def synthetic_training_bmps(work: Path) -> list[Path]:
    """Deterministic non-held-out training images covering CM branch regimes when build/research/ppm is absent."""
    specs = [
        ('grad_256x192', 256, 192, 'gradient'),
        ('photo_like_320x240', 320, 240, 'photo_like'),
        ('photo_like_240x320', 240, 320, 'photo_like2'),
        ('checker_192x192', 192, 192, 'checker'),
        ('stripes_256x160', 256, 160, 'stripes'),
        ('noise_160x120', 160, 120, 'noise'),
        ('edges_256x192', 256, 192, 'edges'),
        ('solid_128x128', 128, 128, 'solid'),
        ('mixed_300x200', 300, 200, 'mixed'),
        ('ramps_257x193', 257, 193, 'ramps'),
    ]
    paths: list[Path] = []
    for idx, (name, w, h, kind) in enumerate(specs):
        rng = random.Random(1000 + idx)
        rgb = bytearray(w * h * 3)
        pos = 0
        for y in range(h):
            for x in range(w):
                if kind == 'gradient':
                    r, g, b = (x * 3) & 255, (y * 5) & 255, (x + y * 2) & 255
                elif kind in ('photo_like', 'photo_like2'):
                    base = ((x * 5 + y * 3) // 4) & 255
                    jit = rng.randint(-6, 6)
                    g = max(0, min(255, base + jit))
                    r = max(0, min(255, g + ((x - y) // 8) + rng.randint(-3, 3)))
                    b = max(0, min(255, g - ((x + y) // 10) + rng.randint(-3, 3)))
                elif kind == 'checker':
                    c = 32 if ((x // 8) ^ (y // 8)) & 1 else 224
                    r, g, b = c, (c + 12) & 255, (c + 24) & 255
                elif kind == 'stripes':
                    r, g, b = (y * 17) & 255, 40, (200 - y * 3) & 255
                elif kind == 'noise':
                    r, g, b = rng.randrange(256), rng.randrange(256), rng.randrange(256)
                elif kind == 'edges':
                    edge = 240 if (x % 32 < 2 or y % 32 < 2) else 24
                    r, g, b = edge, (edge + (x & 15)) & 255, (edge ^ (y & 15)) & 255
                elif kind == 'solid':
                    r, g, b = 120, 140, 160
                elif kind == 'mixed':
                    if x < w // 3:
                        r, g, b = (x * 2) & 255, (y * 2) & 255, ((x + y) * 2) & 255
                    elif x < (2 * w) // 3:
                        g = max(0, min(255, 128 + ((x * 7 - y * 5) & 31) - 16 + rng.randint(-2, 2)))
                        r, b = g, g
                    else:
                        r, g, b = (255 if (x ^ y) & 4 else 16), 80, 192
                else:
                    r, g, b = (x * 9 + y) & 255, (x + y * 7) & 255, (x * 3 ^ y * 5) & 255
                rgb[pos] = r
                rgb[pos + 1] = g
                rgb[pos + 2] = b
                pos += 3
        bmp_path = work / f'{name}.bmp'
        bmp_path.write_bytes(write_bmp24(w, h, bytes(rgb)))
        paths.append(bmp_path)
    return paths


def prepare_training_bmps(work: Path) -> tuple[list[Path], str]:
    """Use 10 non-held-out Kodak images from photo when present, else deterministic synthetic training BMPs."""
    ppm_dir = ROOT / 'build' / 'research' / 'ppm'
    tp_kodak = {'kodim01', 'kodim03', 'kodim05', 'kodim13', 'kodim23'}
    kodak = sorted(p for p in ppm_dir.glob('kodim*.ppm') if p.stem not in tp_kodak)[:10] if ppm_dir.is_dir() else []
    if len(kodak) == 10:
        paths: list[Path] = []
        for ppm in kodak:
            w, h, rgb = read_ppm(ppm)
            bmp = work / f'{ppm.stem}.bmp'
            bmp.write_bytes(write_bmp24(w, h, rgb))
            paths.append(bmp)
        return paths, 'photo:non-tp-kodak-10'
    return synthetic_training_bmps(work), 'synthetic:10-regime'


def publish_profile_pair(
    staged_prof: Path,
    out_path: Path,
    meta: dict[str, object],
    replace_fn=None,
) -> None:
    if replace_fn is None:
        replace_fn = lambda src, dst: src.replace(dst)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path = Path(str(out_path) + '.json')
    partial = Path(str(out_path) + '.partial')
    meta_partial = Path(str(meta_path) + '.partial')
    bak_prof = Path(str(out_path) + '.bak')
    bak_meta = Path(str(meta_path) + '.bak')
    bak_prof_tmp = Path(str(bak_prof) + '.tmp')
    bak_meta_tmp = Path(str(bak_meta) + '.tmp')
    for p in (partial, meta_partial, bak_prof, bak_meta, bak_prof_tmp, bak_meta_tmp):
        p.unlink(missing_ok=True)
    shutil.copy2(staged_prof, partial)
    meta_partial.write_text(json.dumps(meta, indent=2) + '\n', encoding='utf-8')
    compiler = str(meta['compiler'])
    expected_hashes = dict(meta['sources'])
    had_valid_backup = False
    try:
        if build_mod.is_consistent_profile_pair(out_path, meta_path, compiler, expected_hashes):
            shutil.copy2(out_path, bak_prof_tmp)
            shutil.copy2(meta_path, bak_meta_tmp)
            replace_fn(bak_prof_tmp, bak_prof)
            replace_fn(bak_meta_tmp, bak_meta)
            had_valid_backup = build_mod.is_consistent_profile_pair(bak_prof, bak_meta, compiler, expected_hashes)
        replace_fn(partial, out_path)
        replace_fn(meta_partial, meta_path)
    except BaseException:
        for p in (partial, meta_partial, bak_prof_tmp, bak_meta_tmp):
            p.unlink(missing_ok=True)
        if (
            had_valid_backup
            and not build_mod.is_consistent_profile_pair(out_path, meta_path, compiler, expected_hashes)
            and build_mod.is_consistent_profile_pair(bak_prof, bak_meta, compiler, expected_hashes)
        ):
            res_prof = Path(str(out_path) + '.restore')
            res_meta = Path(str(meta_path) + '.restore')
            shutil.copy2(bak_prof, res_prof)
            shutil.copy2(bak_meta, res_meta)
            res_prof.replace(out_path)
            res_meta.replace(meta_path)
        raise
    finally:
        for p in (bak_prof, bak_meta, bak_prof_tmp, bak_meta_tmp):
            p.unlink(missing_ok=True)


def generate_profile(
    out_path: Path,
    build_instrumented: bool = True,
    pgo_exe: Path = PGO_EXE,
    max_images: int | None = None,
) -> Path:
    suffix = '.exe' if os.name == 'nt' else ''
    tools = ROOT / ('tools/llvm-mingw/bin' if os.name == 'nt' else 'tools/llvm-mingw-linux/bin')
    profdata_tool = tools / ('llvm-profdata' + suffix)
    clang_tool = tools / ('clang' + suffix)
    if not profdata_tool.is_file() or not clang_tool.is_file():
        raise FileNotFoundError(f'missing {profdata_tool}; run tools/bootstrap.py')
    if not build_instrumented:
        build_mod.validate_pgo_exe_provenance(pgo_exe, clang_tool)
    if os.name != 'nt' and pgo_exe.suffix.lower() == '.exe':
        if not has_wsl_bridge():
            raise RuntimeError('Windows PGO training executable requires a Windows host or WSL interop')
        cmd = [sys.executable, str(ROOT / 'scripts' / 'windows.py'), 'scripts/pgo.py', '--out',
               out_path.resolve().relative_to(ROOT.resolve()).as_posix()]
        if not build_instrumented:
            cmd.append('--no-build')
        if pgo_exe != PGO_EXE:
            cmd.extend(['--pgo-exe', pgo_exe.resolve().relative_to(ROOT.resolve()).as_posix()])
        if max_images is not None:
            cmd.extend(['--max-images', str(max_images)])
        subprocess.run(cmd, cwd=ROOT, stdin=subprocess.DEVNULL, check=True)
        return out_path
    if build_instrumented:
        subprocess.run([sys.executable, str(ROOT / 'scripts' / 'build.py'), '--pgo-gen'], cwd=ROOT, check=True)
    if not pgo_exe.is_file():
        raise FileNotFoundError(f'missing PGO executable: {pgo_exe}')
    exe_meta = build_mod.validate_pgo_exe_provenance(pgo_exe, clang_tool)

    with tempfile.TemporaryDirectory(prefix='rf_pgo_') as tmp:
        tmp_dir = Path(tmp)
        raw_dir = tmp_dir / 'profraw'
        img_dir = tmp_dir / 'images'
        raw_dir.mkdir(parents=True, exist_ok=True)
        img_dir.mkdir(parents=True, exist_ok=True)
        bmps, dataset_label = prepare_training_bmps(img_dir)
        if max_images is not None:
            bmps = bmps[:max_images]
        run_index = 0
        raw_files: list[Path] = []
        for bmp in bmps:
            for threads in ('1', '3'):
                fold_out = img_dir / f'{bmp.stem}_t{threads}.fold'
                bmp_out = img_dir / f'{bmp.stem}_t{threads}.out.bmp'
                for cmd_args in (
                    ['encode', '--threads', threads, str(bmp), str(fold_out)],
                    ['decode', '--threads', threads, str(fold_out), str(bmp_out)],
                ):
                    run_index += 1
                    profraw = raw_dir / f'run_{run_index:03d}.profraw'
                    env = os.environ.copy()
                    env['LLVM_PROFILE_FILE'] = str(profraw)
                    proc = subprocess.run([str(pgo_exe), *cmd_args], env=env, capture_output=True, text=True)
                    if proc.returncode != 0:
                        raise RuntimeError(f'PGO training run failed ({cmd_args}): {proc.stderr.strip()}')
                    if not profraw.is_file() or profraw.stat().st_size == 0:
                        raise RuntimeError(f'PGO training run did not emit non-empty profile: {profraw}')
                    raw_files.append(profraw)
                fold_out.unlink(missing_ok=True)
                bmp_out.unlink(missing_ok=True)

        staged = tmp_dir / 'rasterfold.profdata'
        merge_proc = subprocess.run(
            [str(profdata_tool), 'merge', '-o', str(staged), *map(str, raw_files)],
            capture_output=True,
            text=True,
        )
        if merge_proc.returncode != 0 or not staged.is_file() or staged.stat().st_size == 0:
            raise RuntimeError(f'llvm-profdata merge failed: {merge_proc.stderr.strip()}')
        show_proc = subprocess.run([str(profdata_tool), 'show', str(staged)], capture_output=True, text=True)
        if show_proc.returncode != 0:
            raise RuntimeError(f'llvm-profdata show validation failed: {show_proc.stderr.strip()}')

        verified_exe_meta = build_mod.validate_pgo_exe_provenance(pgo_exe, clang_tool)
        meta = {
            'schema': 1,
            'dataset': dataset_label,
            'runs': len(raw_files),
            'compiler': verified_exe_meta['compiler'],
            'exe_sha256': verified_exe_meta['exe_sha256'],
            'profdata_sha256': hashlib.sha256(staged.read_bytes()).hexdigest(),
            'sources': dict(verified_exe_meta['sources']),
        }
        publish_profile_pair(staged, out_path, meta)

    print(f'generated {out_path.relative_to(ROOT).as_posix()} ({len(raw_files)} runs, {dataset_label})', flush=True)
    return out_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--no-build', action='store_true', help='use existing bin/windows-x64-pgo/bin/rasterfold.exe')
    parser.add_argument('--pgo-exe', type=Path, default=PGO_EXE, help='override instrumented executable path')
    parser.add_argument('--max-images', type=int, default=None, help='limit training images (for fast lifecycle tests)')
    args = parser.parse_args()
    out = args.out if args.out.is_absolute() else (ROOT / args.out)
    pgo_exe = args.pgo_exe if args.pgo_exe.is_absolute() else (ROOT / args.pgo_exe)
    try:
        generate_profile(out, build_instrumented=not args.no_build, pgo_exe=pgo_exe, max_images=args.max_images)
        return 0
    except Exception as exc:
        print(f'pgo: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
