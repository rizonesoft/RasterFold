#!/usr/bin/env python3
"""Generate mirrored documentation pages and stage the allowlisted Jekyll site.

Uses only the Python standard library. Never stages raw docs/ or repository root.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
SITE_CONTENT = 'docs/site-content.json'
FORBIDDEN_SITE_TOKENS = (
    'todo/',
    '.agents/',
    '.claude/',
    '.codex/',
    '.conclave/',
    '.intelligence/',
    'docs/reviews/',
    'docs/phase-runs/',
    'reviews/',
    'phase-runs/',
    'agents.md',
    'gemini.md',
    'claude.md',
)
MD_LINK_RE = re.compile(r'(!?\[[^\]\n]*\]\()(?P<url><[^>\n]+>|[^\s)]+)(?P<tail>(?:\s+["\'][^\n]*?["\'])?\))')
MD_REF_RE = re.compile(r'^(\s*\[(?!\^)[^\]]+\]:\s*)(?P<url><[^>\n]+>|[^\s>]+)(?P<tail>.*)$')
HTML_ATTR_RE = re.compile(
    r'(\b(?:href|src|poster)\s*=\s*)(?:(["\'])(?P<qurl>[^"\']*)(\2)|(?P<uurl>[^\s"\'=<>`]+))',
    re.I,
)
README_SECTIONS = (
    '## What is here',
    '## Build in WSL or Linux',
    '## Encode, decode, inspect',
    '## Embed the codec',
)
RESEARCH_SECTIONS = (
    '## 1. Where things stand',
    '## 2. Measurement protocol',
    '## 3. Already tried (do not repeat unchanged)',
    '## 8. Prior art',
)
README_BLOCK_RE = re.compile(
    r'<!-- drift:begin readme-capabilities -->\r?\n(?P<body>.*?)\r?\n<!-- drift:end readme-capabilities -->',
    re.S,
)


def safe_repo_path(root: Path, rel: str) -> Path:
    if not isinstance(rel, str) or not rel or '\\' in rel:
        raise ValueError(f'invalid repository path: {rel!r}')
    parts = Path(rel).parts
    if Path(rel).is_absolute() or '..' in parts:
        raise ValueError(f'path escapes repository: {rel}')
    resolved_root = root.resolve()
    candidate = resolved_root / rel
    if not candidate.resolve().is_relative_to(resolved_root):
        raise ValueError(f'path escapes repository: {rel}')
    cur = resolved_root
    for part in parts:
        cur /= part
        if cur.is_symlink():
            raise ValueError(f'symlink is not allowed: {rel}')
    return candidate


def is_forbidden_path(rel: str, extra_prefixes: list[str] | tuple[str, ...] = ()) -> bool:
    norm = rel.strip('/').lower()
    for token in (*FORBIDDEN_SITE_TOKENS, *(p.lower() for p in extra_prefixes)):
        clean = token.strip('/')
        if not clean:
            continue
        if norm == clean or norm.startswith(clean + '/') or ('/' + clean + '/') in ('/' + norm + '/'):
            return True
    return False


def load_site_content(root: Path) -> dict:
    path = safe_repo_path(root, SITE_CONTENT)
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('schema') != 1:
        raise ValueError('unsupported site-content.json schema')
    base_path = data.get('base_path', '')
    if not isinstance(base_path, str) or not base_path.startswith('/') or not base_path.endswith('/'):
        raise ValueError(f'invalid base_path in {SITE_CONTENT}: {base_path!r}')
    extra_forbidden = data.get('forbidden_prefixes', [])
    if not isinstance(extra_forbidden, list) or not all(isinstance(x, str) for x in extra_forbidden):
        raise ValueError('forbidden_prefixes must be a list of strings')
    seen_staged = set()
    for group in ('config', 'pages', 'assets'):
        entries = data.get(group)
        if not isinstance(entries, list) or not entries:
            raise ValueError(f'{SITE_CONTENT}: {group} must be a non-empty list')
        for entry in entries:
            if not isinstance(entry, dict):
                raise ValueError(f'{SITE_CONTENT}: {group} entry must be an object')
            src = entry.get('source')
            staged = entry.get('staged')
            out = entry.get('output')
            for label, rel in (('source', src), ('staged', staged), *((('output', out),) if out else ())):
                if not isinstance(rel, str) or not rel or '\\' in rel or Path(rel).is_absolute() or '..' in Path(rel).parts:
                    raise ValueError(f'{SITE_CONTENT}: invalid {label} path {rel!r}')
                if is_forbidden_path(rel, extra_forbidden):
                    raise ValueError(f'{SITE_CONTENT}: forbidden {label} path {rel!r}')
            safe_repo_path(root, src)
            if out:
                safe_repo_path(root, out)
            if staged in seen_staged:
                raise ValueError(f'{SITE_CONTENT}: duplicate staged path {staged!r}')
            seen_staged.add(staged)
    return data


def split_h2_sections(markdown_text: str) -> tuple[str, dict[str, str]]:
    lines = markdown_text.splitlines(keepends=True)
    preamble: list[str] = []
    sections: dict[str, list[str]] = {}
    current: str | None = None
    fence: str | None = None
    for line in lines:
        fmatch = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
        if fmatch and fence is None:
            fence = fmatch.group(1)
        elif fence and re.match(r'^\s{0,3}' + re.escape(fence[0]) + '{' + str(len(fence)) + r',}\s*$', line):
            fence = None
        if fence is None and line.startswith('## '):
            current = line.strip()
            sections[current] = [line]
        elif current is None:
            preamble.append(line)
        else:
            sections[current].append(line)
    return ''.join(preamble), {k: ''.join(v).rstrip() + '\n' for k, v in sections.items()}


def render_format_page(root: Path, page_meta: dict) -> str:
    source_rel = page_meta['source']
    spec_text = safe_repo_path(root, source_rel).read_text(encoding='utf-8')
    title = page_meta['title']
    nav_order = page_meta['nav_order']
    header = (
        '---\n'
        'layout: default\n'
        f'title: "{title}"\n'
        f'nav_order: {nav_order}\n'
        f'source_path: {source_rel}\n'
        '---\n\n'
        f'<!-- GENERATED by scripts/build-docs.py from {source_rel}; do not edit by hand. -->\n\n'
    )
    return header + spec_text.lstrip()


def render_usage_page(root: Path, page_meta: dict) -> str:
    source_rel = page_meta['source']
    readme_text = safe_repo_path(root, source_rel).read_text(encoding='utf-8')
    _, sections = split_h2_sections(readme_text)
    selected: list[str] = []
    for heading in README_SECTIONS:
        if heading not in sections:
            raise ValueError(f'{source_rel}: missing required section {heading!r}')
        selected.append(sections[heading].rstrip())
    block_match = README_BLOCK_RE.search(readme_text)
    if not block_match:
        raise ValueError(f'{source_rel}: missing readme-capabilities block')
    capabilities_line = block_match.group('body').strip()
    body = '\n\n'.join(selected) + '\n\n## Current Capabilities and Limits\n\n' + capabilities_line + '\n'
    link_map = {
        'docs/measurements.md': 'measurements.md',
        'docs/building.md': 'building.md',
        'docs/library.md': 'library.md',
        'docs/vision.md': 'vision.md',
        'include/rasterfold.h': '../include/rasterfold.h',
        'examples/': '../examples/',
    }
    for old_target, new_target in link_map.items():
        body = body.replace(f']({old_target})', f']({new_target})')
    title = page_meta['title']
    nav_order = page_meta['nav_order']
    header = (
        '---\n'
        'layout: default\n'
        f'title: "{title}"\n'
        f'nav_order: {nav_order}\n'
        f'source_path: {source_rel}\n'
        '---\n\n'
        f'<!-- GENERATED by scripts/build-docs.py from {source_rel}; do not edit by hand. -->\n\n'
        f'# {title}\n\n'
        'RasterFold is a lossless RGB image codec written in C11. It turns 24-bit BMP images into `.fold` files and restores every pixel exactly.\n\n'
    )
    return header + body


def sanitize_public_research(text: str) -> str:
    # Strip internal TODO references while preserving all measured figures and prose.
    text = re.sub(r'\s*\(`D\d{2}\s+T\d{2}(?:\s+§\d+)?(?:`,\s*`D\d{2}\s+T\d{2}(?:\s+§\d+)?)*`(?:,\s*milestone\s+\d+)?\)', '', text)
    text = re.sub(r',\s*`D\d{2}\s+T\d{2}(?:\s+§\d+)?`', '', text)
    text = re.sub(r'\s+of\s+`D\d{2}\s+T\d{2}(?:\s+§\d+)?`', '', text)
    text = re.sub(r'\s*\(backlog\s+`B-\d+`\)', '', text)
    text = text.replace('(`spec/FOLD.md`)', '([Format Specification](format.md))')
    text = text.replace('[`spec/FOLD.md`](../spec/FOLD.md)', '[Format Specification](format.md)')
    return text


def render_research_summary_page(root: Path, page_meta: dict) -> str:
    source_rel = page_meta['source']
    research_text = safe_repo_path(root, source_rel).read_text(encoding='utf-8')
    _, sections = split_h2_sections(research_text)
    selected: list[str] = []
    for heading in RESEARCH_SECTIONS:
        if heading not in sections:
            raise ValueError(f'{source_rel}: missing required section {heading!r}')
        chunk = sections[heading].rstrip()
        chunk = re.sub(r'\n---\s*$', '', chunk)
        selected.append(sanitize_public_research(chunk))
    title = page_meta['title']
    nav_order = page_meta['nav_order']
    header = (
        '---\n'
        'layout: default\n'
        f'title: "{title}"\n'
        f'nav_order: {nav_order}\n'
        f'source_path: {source_rel}\n'
        '---\n\n'
        f'<!-- GENERATED by scripts/build-docs.py from {source_rel}; do not edit by hand. -->\n\n'
        '# RasterFold Research Summary\n\n'
        'Status date: 2026-09-29 (historical baseline record; see [Benchmarks](benchmarks.md) for current committed measurements).\n\n'
        'This summary excerpts the public measurement baseline, protocol, experiment ledger, and prior-art references from the research notes. '
        'The normative bitstream is specified in [Format Specification](format.md); the measurement scripts are documented in [`research/README.md`](../research/README.md).\n\n'
    )
    return header + '\n\n---\n\n'.join(selected) + '\n'


def render_generated_pages(root: Path, site_content: dict | None = None) -> dict[str, str]:
    if site_content is None:
        site_content = load_site_content(root)
    outputs: dict[str, str] = {}
    renderers = {
        'format': render_format_page,
        'usage': render_usage_page,
        'research-summary': render_research_summary_page,
    }
    for page in site_content['pages']:
        if page.get('kind') != 'mirrored':
            continue
        page_id = page.get('id')
        output_rel = page.get('output')
        if page_id not in renderers or not output_rel:
            raise ValueError(f'unsupported mirrored page entry: {page!r}')
        outputs[output_rel] = renderers[page_id](root, page)
    return outputs


def atomic_write_text(path: Path, content: str) -> None:
    if path.is_symlink():
        raise ValueError(f'refusing to write through symlink: {path}')
    path.parent.mkdir(parents=True, exist_ok=True)
    data = content.encode('utf-8')
    fd, tmp_name = tempfile.mkstemp(prefix=f'.{path.name}.', suffix='.tmp', dir=path.parent)
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, 'wb') as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        if path.exists():
            os.chmod(tmp_path, path.stat().st_mode)
        else:
            os.chmod(tmp_path, 0o644)
        os.replace(tmp_path, path)
    finally:
        if tmp_path.exists():
            tmp_path.unlink()


def write_generated_pages(root: Path) -> list[str]:
    site_content = load_site_content(root)
    generated = render_generated_pages(root, site_content)
    written: list[str] = []
    for rel, content in sorted(generated.items()):
        path = safe_repo_path(root, rel)
        current = path.read_text(encoding='utf-8') if path.is_file() else None
        if current != content:
            atomic_write_text(path, content)
            written.append(rel)
    return written


def check_generated_pages(root: Path) -> list[str]:
    site_content = load_site_content(root)
    generated = render_generated_pages(root, site_content)
    errors: list[str] = []
    for rel, expected in sorted(generated.items()):
        path = safe_repo_path(root, rel)
        if not path.is_file():
            errors.append(f'{rel}: missing generated file (run python3 scripts/build-docs.py)')
            continue
        actual = path.read_text(encoding='utf-8')
        if actual != expected:
            errors.append(f'{rel}: generated file differs from source (run python3 scripts/build-docs.py)')
    for group in ('config', 'pages', 'assets'):
        for entry in site_content[group]:
            check_rel = entry.get('output') or entry['source']
            path = safe_repo_path(root, check_rel)
            if not path.is_file():
                errors.append(f'{check_rel}: allowlisted source file is missing')
                continue
            if group == 'pages':
                text = path.read_text(encoding='utf-8')
                expected_source_line = f'source_path: {entry["source"]}'
                if expected_source_line not in text.split('---', 2)[1] if text.startswith('---\n') and text.count('---') >= 2 else True:
                    errors.append(f'{check_rel}: missing {expected_source_line!r} in front matter')
    return errors


def validate_stage_destination(root: Path, stage_arg: str | Path) -> Path:
    if isinstance(stage_arg, Path):
        raw = str(stage_arg)
    else:
        raw = stage_arg
    if not isinstance(raw, str) or not raw.strip() or '\\' in raw:
        raise ValueError(f'invalid staging destination: {raw!r}')
    rel_parts = Path(raw).parts
    if '..' in rel_parts:
        raise ValueError(f'staging destination cannot contain .. traversal: {raw}')
    resolved_root = root.resolve()
    build_root = resolved_root / 'build'
    if build_root.is_symlink():
        raise ValueError('build directory cannot be a symlink')
    candidate = Path(raw) if Path(raw).is_absolute() else (resolved_root / raw)
    lexical_dest = Path(os.path.abspath(str(candidate)))
    if lexical_dest == build_root or not lexical_dest.is_relative_to(build_root):
        raise ValueError(f'staging destination must be a subdirectory inside build/: {raw}')
    cur = build_root
    for part in lexical_dest.relative_to(build_root).parts:
        cur /= part
        if cur.is_symlink():
            raise ValueError(f'staging destination ancestor or target is a symlink: {cur}')
    resolved_dest = lexical_dest.resolve()
    if resolved_dest == build_root or not resolved_dest.is_relative_to(build_root):
        raise ValueError(f'staging destination must resolve inside build/: {raw}')
    if resolved_dest.exists() and not resolved_dest.is_dir():
        raise ValueError(f'staging destination exists and is not a directory: {raw}')
    return resolved_dest


def rebase_staged_links(
    text: str,
    source_doc_rel: str,
    root: Path,
    site_content: dict,
    doc_to_staged_html: dict[str, str],
    asset_to_staged_url: dict[str, str],
) -> str:
    base_path = site_content['base_path'].rstrip('/')
    repo_slug = site_content['repository']
    extra_forbidden = site_content.get('forbidden_prefixes', [])
    source_dir = Path(source_doc_rel).parent

    def rewrite_url(raw_url: str) -> str:
        bracketed = raw_url.startswith('<') and raw_url.endswith('>')
        clean_url = raw_url[1:-1] if bracketed else raw_url
        parts = urlsplit(clean_url)
        if parts.scheme or parts.netloc or clean_url.startswith('#') or clean_url.startswith('{{'):
            return raw_url
        target_path = parts.path
        query = ('?' + parts.query) if parts.query else ''
        fragment = ('#' + parts.fragment) if parts.fragment else ''
        suffix = f'{query}{fragment}'
        if not target_path:
            return raw_url
        if target_path.startswith('/'):
            if not target_path.startswith(base_path + '/'):
                raise ValueError(f'{source_doc_rel}: root-relative link outside {base_path}/: {clean_url}')
            return raw_url
        norm_rel = os.path.normpath((source_dir / target_path).as_posix()).replace('\\', '/')
        if norm_rel.startswith('../') or norm_rel == '..':
            raise ValueError(f'{source_doc_rel}: link escapes repository: {clean_url}')
        if is_forbidden_path(norm_rel, extra_forbidden):
            raise ValueError(f'{source_doc_rel}: link targets forbidden private/internal path {norm_rel!r}')
        if norm_rel in doc_to_staged_html:
            rebased = f'{base_path}/{doc_to_staged_html[norm_rel]}{suffix}'
        elif norm_rel in asset_to_staged_url:
            rebased = f'{base_path}/{asset_to_staged_url[norm_rel]}{suffix}'
        else:
            repo_target = safe_repo_path(root, norm_rel)
            if not repo_target.exists():
                raise ValueError(f'{source_doc_rel}: link target does not exist: {norm_rel}')
            kind = 'tree' if repo_target.is_dir() else 'blob'
            rebased = f'https://github.com/{repo_slug}/{kind}/main/{norm_rel}{suffix}'
        return f'<{rebased}>' if bracketed else rebased

    def rewrite_html_attr(match: re.Match[str]) -> str:
        prefix = match.group(1)
        qurl = match.group('qurl')
        if qurl is not None:
            quote = match.group(2)
            return f'{prefix}{quote}{rewrite_url(qurl)}{quote}'
        return f'{prefix}{rewrite_url(match.group("uurl"))}'

    lines: list[str] = []
    fence: str | None = None
    for line in text.splitlines(keepends=True):
        fmatch = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
        if fmatch and fence is None:
            fence = fmatch.group(1)
            lines.append(line)
            continue
        if fence and re.match(r'^\s{0,3}' + re.escape(fence[0]) + '{' + str(len(fence)) + r',}\s*$', line):
            fence = None
            lines.append(line)
            continue
        if fence is not None:
            lines.append(line)
            continue
        line = MD_LINK_RE.sub(lambda m: m.group(1) + rewrite_url(m.group('url')) + m.group('tail'), line)
        line = MD_REF_RE.sub(lambda m: m.group(1) + rewrite_url(m.group('url')) + m.group('tail'), line)
        line = HTML_ATTR_RE.sub(rewrite_html_attr, line)
        lines.append(line)
    return ''.join(lines)


def stage_site(root: Path, stage_arg: str | Path) -> list[str]:
    dest = validate_stage_destination(root, stage_arg)
    errors = check_generated_pages(root)
    if errors:
        raise ValueError('; '.join(errors))
    site_content = load_site_content(root)
    extra_forbidden = site_content.get('forbidden_prefixes', [])

    doc_to_staged_html: dict[str, str] = {}
    for page in site_content['pages']:
        doc_rel = page.get('output') or page['source']
        staged_rel = page['staged']
        html_rel = re.sub(r'\.md$', '.html', staged_rel)
        doc_to_staged_html[doc_rel] = html_rel

    asset_to_staged_url: dict[str, str] = {}
    for asset in site_content['assets']:
        asset_to_staged_url[asset['source']] = asset['staged']

    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp_dir = Path(tempfile.mkdtemp(prefix=f'.{dest.name}.', dir=dest.parent))
    backup_dir: Path | None = None
    try:
        expected_staged: set[str] = set()
        for entry in site_content['config']:
            src = safe_repo_path(root, entry['source'])
            staged_rel = entry['staged']
            out_path = tmp_dir / staged_rel
            out_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, out_path, follow_symlinks=False)
            expected_staged.add(staged_rel)

        for page in site_content['pages']:
            doc_rel = page.get('output') or page['source']
            staged_rel = page['staged']
            src = safe_repo_path(root, doc_rel)
            raw_text = src.read_text(encoding='utf-8')
            staged_text = rebase_staged_links(
                raw_text,
                doc_rel,
                root,
                site_content,
                doc_to_staged_html,
                asset_to_staged_url,
            )
            out_path = tmp_dir / staged_rel
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_text(staged_text, encoding='utf-8', newline='\n')
            expected_staged.add(staged_rel)

        for asset in site_content['assets']:
            src = safe_repo_path(root, asset['source'])
            staged_rel = asset['staged']
            out_path = tmp_dir / staged_rel
            out_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, out_path, follow_symlinks=False)
            expected_staged.add(staged_rel)

        actual_staged: set[str] = set()
        for item in sorted(tmp_dir.rglob('*')):
            if item.is_symlink():
                raise ValueError(f'symlink found in staged tree: {item}')
            if item.is_file():
                rel = item.relative_to(tmp_dir).as_posix()
                if is_forbidden_path(rel, extra_forbidden):
                    raise ValueError(f'forbidden path leaked into staged tree: {rel}')
                actual_staged.add(rel)
        if actual_staged != expected_staged:
            raise ValueError(f'staged file set mismatch: expected {sorted(expected_staged)}, got {sorted(actual_staged)}')

        if dest.exists():
            backup_dir = Path(tempfile.mkdtemp(prefix=f'.{dest.name}.old.', dir=dest.parent))
            backup_dir.rmdir()
            os.replace(dest, backup_dir)
        os.replace(tmp_dir, dest)
        if backup_dir and backup_dir.exists():
            shutil.rmtree(backup_dir)
        return sorted(actual_staged)
    except Exception:
        if backup_dir and backup_dir.exists() and not dest.exists():
            os.replace(backup_dir, dest)
        if tmp_dir.exists():
            shutil.rmtree(tmp_dir, ignore_errors=True)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--write', action='store_true', help='Write generated pages (default when no mode flag is given)')
    parser.add_argument('--check', action='store_true', help='Verify generated pages and site allowlist without modifying files')
    parser.add_argument('--stage', metavar='DIR', help='Stage the allowlisted Jekyll source tree into DIR (must be inside build/)')
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        if args.check and (args.write or args.stage):
            raise ValueError('--check cannot be combined with --write or --stage')
        if args.check:
            errors = check_generated_pages(root)
            for err in errors:
                print(f'build-docs: {err}', file=sys.stderr)
            if errors:
                return 1
            print('build-docs: current')
            return 0
        if args.stage:
            if args.write:
                write_generated_pages(root)
            staged = stage_site(root, args.stage)
            print(f'build-docs: staged {len(staged)} file(s) into {args.stage}')
            return 0
        written = write_generated_pages(root)
        print(f'build-docs: wrote {len(written)} generated file(s)')
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f'build-docs: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
