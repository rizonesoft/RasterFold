#!/usr/bin/env python3
"""Run target-aware clang-tidy across Windows/POSIX and x64/ARM64 compilation databases."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
COMMON = ('platform', 'api', 'crc32', 'bmp', 'fold', 'cm')
BASELINE_RE = re.compile(r'^count:\s*(\d+)\s*$')
WARNING_RE = re.compile(
    r'^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+):\s+warning:\s+(?P<msg>.+?)\s+\[(?P<check>[^\]]+)\]\s*$'
)
ERROR_RE = re.compile(r'(?::\d+:\d+:\s+(?:fatal\s+)?error:\s+|^(?:error|LLVM ERROR):)', re.M)


def parse_baseline(path: Path) -> int:
    if not path.is_file():
        raise ValueError(f'missing baseline file {path}')
    text = path.read_text(encoding='utf-8')
    match = BASELINE_RE.match(text)
    if not match:
        raise ValueError(f'malformed baseline file {path}; expected "count: <n>"')
    return int(match.group(1))


def committed_baseline(root: Path, rel_path: str = 'todo/.tidy-baseline') -> int | None:
    revs = subprocess.run(
        ['git', 'rev-list', '--reverse', 'HEAD', '--', rel_path],
        cwd=root,
        capture_output=True,
        text=True,
    )
    if revs.returncode != 0:
        return None
    commits = [line.strip() for line in revs.stdout.splitlines() if line.strip()]
    if not commits:
        return None
    prev_val: int | None = None
    prev_sha = ''
    for sha in commits:
        proc = subprocess.run(
            ['git', 'show', f'{sha}:{rel_path}'],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            continue
        match = BASELINE_RE.match(proc.stdout)
        if not match:
            raise ValueError(f'malformed committed baseline {sha}:{rel_path}')
        val = int(match.group(1))
        if prev_val is not None and val > prev_val:
            raise ValueError(
                f'committed baseline in {rel_path} increased from {prev_val} ({prev_sha[:12]}) to {val} ({sha[:12]}); baseline may only shrink'
            )
        prev_val = val
        prev_sha = sha
    return prev_val


def host_targets(root: Path = ROOT, os_name: str = os.name) -> list[dict]:
    suffix = '.exe' if os_name == 'nt' else ''
    if os_name == 'nt':
        tools = root / 'tools/llvm-mingw/bin'
        return [
            {
                'name': 'windows-x64',
                'triple': 'x86_64-w64-mingw32',
                'portable': False,
                'windows': True,
                'freestanding': True,
                'tools': tools,
                'suffix': suffix,
                'sources': [*COMMON, 'mem', 'sys', 'main'],
            },
            {
                'name': 'windows-x64-portable',
                'triple': 'x86_64-w64-mingw32',
                'portable': True,
                'windows': True,
                'freestanding': True,
                'tools': tools,
                'suffix': suffix,
                'sources': [*COMMON, 'mem', 'sys', 'main'],
            },
            {
                'name': 'posix-x64',
                'triple': 'x86_64-linux-gnu',
                'portable': False,
                'windows': False,
                'freestanding': True,
                'tools': tools,
                'suffix': suffix,
                'sources': [*COMMON, 'main'],
            },
            {
                'name': 'posix-arm64',
                'triple': 'aarch64-linux-gnu',
                'portable': True,
                'windows': False,
                'freestanding': True,
                'tools': tools,
                'suffix': suffix,
                'sources': [*COMMON, 'main'],
            },
        ]

    api = json.loads((root / 'toolchain.json').read_text(encoding='utf-8'))['android']['min_api']
    llvm_tools = root / 'tools/llvm-mingw-linux/bin'
    ndk_tools = root / 'tools/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin'
    return [
        {
            'name': 'linux-x64',
            'triple': 'x86_64-linux-gnu',
            'portable': False,
            'windows': False,
            'freestanding': False,
            'tools': llvm_tools,
            'suffix': '',
            'sources': [*COMMON, 'sys_posix', 'main'],
        },
        {
            'name': 'linux-x64-portable',
            'triple': 'x86_64-linux-gnu',
            'portable': True,
            'windows': False,
            'freestanding': False,
            'tools': llvm_tools,
            'suffix': '',
            'sources': [*COMMON, 'sys_posix', 'main'],
        },
        {
            'name': 'windows-x64',
            'triple': 'x86_64-w64-mingw32',
            'portable': False,
            'windows': True,
            'freestanding': True,
            'tools': llvm_tools,
            'suffix': '',
            'sources': [*COMMON, 'mem', 'sys', 'main'],
        },
        {
            'name': 'windows-x64-portable',
            'triple': 'x86_64-w64-mingw32',
            'portable': True,
            'windows': True,
            'freestanding': True,
            'tools': llvm_tools,
            'suffix': '',
            'sources': [*COMMON, 'mem', 'sys', 'main'],
        },
        {
            'name': 'android-arm64',
            'triple': f'aarch64-linux-android{api}',
            'portable': False,
            'windows': False,
            'freestanding': False,
            'tools': ndk_tools,
            'suffix': '',
            'sources': [*COMMON, 'sys_posix', 'main'],
        },
        {
            'name': 'android-x64',
            'triple': f'x86_64-linux-android{api}',
            'portable': False,
            'windows': False,
            'freestanding': False,
            'tools': ndk_tools,
            'suffix': '',
            'sources': [*COMMON, 'sys_posix', 'main'],
        },
    ]


def validate_compdb(compdb_path: Path, root: Path = ROOT) -> list[Path]:
    if not compdb_path.is_file():
        raise ValueError(f'missing compilation database {compdb_path}')
    try:
        entries = json.loads(compdb_path.read_text(encoding='utf-8'))
    except json.JSONDecodeError as exc:
        raise ValueError(f'malformed compilation database {compdb_path}: {exc}') from exc
    if not isinstance(entries, list) or not entries:
        raise ValueError(f'empty compilation database {compdb_path}')
    files: list[Path] = []
    for entry in entries:
        if not isinstance(entry, dict) or not entry.get('file') or not entry.get('arguments'):
            raise ValueError(f'invalid compilation database entry in {compdb_path}')
        src = Path(entry['file'])
        if not src.is_file():
            raise ValueError(f'compilation database {compdb_path} references missing file {src}')
        files.append(src)
    return files


def write_compdb(spec: dict, root: Path = ROOT) -> tuple[Path, Path, list[Path]]:
    tools = Path(spec['tools'])
    suffix = spec['suffix']
    cc = tools / ('clang' + suffix)
    tidy = tools / ('clang-tidy' + suffix)
    for binary in (cc, tidy):
        if not binary.is_file():
            raise ValueError(f'missing {binary}; run tools/bootstrap.py (Windows: tools/bootstrap.ps1)')
    triple = spec['triple']
    flags = [
        f'--target={triple}',
        '-std=c11',
        '-O2',
        '-g',
        '-Wall',
        '-Wextra',
        '-Werror',
        '-Wno-unused-parameter',
        '-fno-builtin',
        '-fno-ident',
        '-fvisibility=hidden',
    ]
    if not triple.startswith('aarch64-'):
        flags += ['-march=x86-64-v2', '-mtune=generic']
    if spec['portable']:
        flags += ['-DRF_PORTABLE_SIMD']
    if spec['windows']:
        flags += ['-gcodeview']
    if spec['freestanding']:
        flags += ['-ffreestanding', '-fno-stack-protector']
    else:
        flags += ['-fPIC', '-pthread']
    work = root / 'build' / spec['name']
    work.mkdir(parents=True, exist_ok=True)
    compdb = []
    for source_name in spec['sources']:
        source = root / 'src' / f'{source_name}.c'
        obj = work / f'{source_name}.o'
        compdb.append({
            'directory': str(root),
            'file': str(source),
            'output': str(obj),
            'arguments': [str(cc), *flags, '-c', str(source), '-o', str(obj)],
        })
    compdb_path = work / 'compile_commands.json'
    compdb_path.write_text(json.dumps(compdb, indent=2) + '\n', encoding='utf-8')
    sources = validate_compdb(compdb_path, root)
    return work, tidy, sources


def normalize_source_path(raw_path: str, root: Path = ROOT) -> str:
    candidate = Path(raw_path)
    try:
        resolved = candidate.resolve()
        root_resolved = root.resolve()
        if resolved.is_relative_to(root_resolved):
            return resolved.relative_to(root_resolved).as_posix()
    except OSError:
        pass
    text = raw_path.replace('\\', '/')
    marker = '/src/'
    if marker in text:
        return 'src/' + text.split(marker, 1)[1]
    return text


def parse_tidy_output(
    output: str,
    returncode: int,
    root: Path = ROOT,
    target_name: str = '',
) -> tuple[set[tuple[str, int, int, str]], dict[tuple[str, int, int, str], str]]:
    if ERROR_RE.search(output):
        prefix = f'{target_name}: ' if target_name else ''
        raise ValueError(f'{prefix}clang-tidy reported error diagnostic:\n{output.strip()}')
    if returncode != 0:
        prefix = f'{target_name}: ' if target_name else ''
        raise ValueError(f'{prefix}clang-tidy exited with code {returncode}:\n{output.strip()}')
    findings: set[tuple[str, int, int, str]] = set()
    messages: dict[tuple[str, int, int, str], str] = {}
    for line in output.splitlines():
        match = WARNING_RE.match(line.strip())
        if not match:
            continue
        rel = normalize_source_path(match.group('file'), root)
        key = (rel, int(match.group('line')), int(match.group('col')), match.group('check'))
        findings.add(key)
        messages.setdefault(key, match.group('msg'))
    return findings, messages


def evaluate_ratchet(
    measured: int,
    baseline_path: Path,
    ratchet: bool = False,
    head_baseline: int | None = None,
) -> str:
    baseline = parse_baseline(baseline_path)
    if head_baseline is not None and baseline > head_baseline:
        raise ValueError(
            f'baseline {baseline} in {baseline_path} exceeds committed baseline {head_baseline}; baseline may only shrink'
        )
    if measured > baseline:
        raise ValueError(f'clang-tidy found {measured} unique warning(s), exceeding baseline {baseline}')
    if measured < baseline:
        if ratchet:
            baseline_path.write_text(f'count: {measured}\n', encoding='utf-8', newline='\n')
            return f'ratcheted baseline from {baseline} to {measured}'
        raise ValueError(
            f'clang-tidy found {measured} unique warning(s), below baseline {baseline}; '
            f'run python3 scripts/tidy.py --ratchet to shrink {baseline_path.name}'
        )
    return f'{measured}/{baseline} unique warning(s)'


def run_tidy(root: Path = ROOT, ratchet: bool = False) -> int:
    config = root / '.clang-tidy'
    if not config.is_file():
        raise ValueError(f'missing {config}')
    baseline_path = root / 'todo/.tidy-baseline'
    head_base = committed_baseline(root)
    parse_baseline(baseline_path)
    if head_base is not None and parse_baseline(baseline_path) > head_base:
        raise ValueError(
            f'baseline {parse_baseline(baseline_path)} in {baseline_path} exceeds committed baseline {head_base}; baseline may only shrink'
        )

    specs = host_targets(root, os.name)
    all_findings: set[tuple[str, int, int, str]] = set()
    all_messages: dict[tuple[str, int, int, str], str] = {}
    target_names: list[str] = []

    for spec in specs:
        work, tidy_bin, sources = write_compdb(spec, root)
        cmd = [
            str(tidy_bin),
            f'-p={work}',
            f'--config-file={config}',
            '--quiet',
            *map(str, sources),
        ]
        proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        combined = (proc.stdout or '') + ('\n' + proc.stderr if proc.stderr else '')
        findings, messages = parse_tidy_output(combined, proc.returncode, root, spec['name'])
        all_findings |= findings
        for key, msg in messages.items():
            all_messages.setdefault(key, msg)
        target_names.append(spec['name'])

    for key in sorted(all_findings):
        rel, line, col, check = key
        print(f'{rel}:{line}:{col}: warning: {all_messages[key]} [{check}]')

    verdict = evaluate_ratchet(len(all_findings), baseline_path, ratchet=ratchet, head_baseline=head_base)
    host_note = (
        'full 6-target matrix'
        if os.name != 'nt'
        else '4-target Windows/POSIX/x64/ARM64 matrix (sys_posix.c requires Linux/Android sysroot)'
    )
    print(f'tidy: {verdict} across {len(target_names)} target databases ({", ".join(target_names)}; {host_note})')
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ratchet', action='store_true', help='shrink todo/.tidy-baseline when warnings decrease')
    args = parser.parse_args(argv)
    try:
        return run_tidy(ROOT, ratchet=args.ratchet)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f'tidy: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
