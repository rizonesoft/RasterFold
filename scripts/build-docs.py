#!/usr/bin/env python3
"""Generate mirrored documentation pages and stage the allowlisted Jekyll site.

Uses only the Python standard library. Never stages raw docs/ or repository root.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
from html import escape as xml_escape
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from urllib.parse import quote, unquote, urlsplit
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SITE_CONTENT = 'docs/site-content.json'
REQUIRED_PAGE_IDS = (
    'overview',
    'getting-started',
    'installation',
    'usage',
    'c-api',
    'api',
    'cli',
    'format',
    'examples',
    'benchmarks',
    'research-summary',
    'limitations',
    'troubleshooting',
    'contributing',
    'security',
    'licenses',
)
DEFAULT_MIRRORED_MAP = {
    'spec/FOLD.md': 'docs/format.md',
    'README.md': 'docs/usage.md',
    'docs/research.md': 'docs/research-summary.md',
    'docs/building.md': 'docs/installation.md',
    'docs/library.md': 'docs/c-api.md',
    'CONTRIBUTING.md': 'docs/contributing.md',
    'SECURITY.md': 'docs/security.md',
}
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
    canonical_source_paths: dict[Path, str] = {}
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
            src_path = safe_repo_path(root, src)
            canonical_source_paths.setdefault(src_path, src)
            if out:
                safe_repo_path(root, out)
            if staged in seen_staged:
                raise ValueError(f'{SITE_CONTENT}: duplicate staged path {staged!r}')
            seen_staged.add(staged)
    seen_page_ids: set[str] = set()
    seen_page_sources: set[Path] = set()
    seen_output_paths: set[Path] = set()
    for page in data['pages']:
        page_id = page.get('id')
        kind = page.get('kind')
        out = page.get('output')
        src = page.get('source')
        if not isinstance(page_id, str) or not page_id:
            raise ValueError(f'{SITE_CONTENT}: page entry missing valid id: {page!r}')
        if page_id in seen_page_ids:
            raise ValueError(f'{SITE_CONTENT}: duplicate page id {page_id!r}')
        seen_page_ids.add(page_id)
        src_path = safe_repo_path(root, src)
        if src_path in seen_page_sources:
            raise ValueError(f'{SITE_CONTENT}: duplicate page source {src!r}')
        seen_page_sources.add(src_path)
        if kind not in ('authored', 'mirrored', 'generated'):
            raise ValueError(f'{SITE_CONTENT}: page {page_id!r} has invalid kind {kind!r}')
        if kind in ('mirrored', 'generated'):
            if not isinstance(out, str) or not out:
                raise ValueError(f'{SITE_CONTENT}: {kind} page {page_id!r} requires an output path')
            out_path = safe_repo_path(root, out)
            if out == src or out_path in canonical_source_paths:
                target_src = canonical_source_paths.get(out_path, src)
                raise ValueError(
                    f'{SITE_CONTENT}: mirrored page {page_id!r} output {out!r} cannot overwrite source {target_src!r}'
                )
            if out_path in seen_output_paths:
                raise ValueError(f'{SITE_CONTENT}: duplicate mirrored output path {out!r}')
            seen_output_paths.add(out_path)
        elif out is not None:
            raise ValueError(f'{SITE_CONTENT}: authored page {page_id!r} cannot specify output {out!r}')
    missing_ids = [pid for pid in REQUIRED_PAGE_IDS if pid not in seen_page_ids]
    if missing_ids:
        raise ValueError(f'{SITE_CONTENT}: missing required page id(s): {", ".join(missing_ids)}')
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


def relocate_mirrored_links(
    text: str,
    source_rel: str,
    output_rel: str,
    mirrored_source_to_output: dict[str, str] | None = None,
) -> str:
    source_dir = Path(source_rel).parent
    output_dir = Path(output_rel).parent
    mirrored_map = mirrored_source_to_output or DEFAULT_MIRRORED_MAP

    def rewrite_url(raw_url: str) -> str:
        bracketed = raw_url.startswith('<') and raw_url.endswith('>')
        clean_url = raw_url[1:-1] if bracketed else raw_url
        parts = urlsplit(clean_url)
        if parts.scheme or parts.netloc or clean_url.startswith('#') or clean_url.startswith('{{') or not parts.path or parts.path.startswith('/'):
            return raw_url
        trailing_slash = '/' if parts.path.endswith('/') and parts.path != '/' else ''
        norm_rel = os.path.normpath((source_dir / parts.path).as_posix()).replace('\\', '/')
        if norm_rel.startswith('../') or norm_rel == '..':
            raise ValueError(f'{source_rel}: link escapes repository: {clean_url}')
        target_repo_rel = mirrored_map.get(norm_rel, norm_rel)
        rel_from_out = os.path.relpath(target_repo_rel, output_dir.as_posix()).replace('\\', '/')
        if trailing_slash and not rel_from_out.endswith('/'):
            rel_from_out += '/'
        query = ('?' + parts.query) if parts.query else ''
        fragment = ('#' + parts.fragment) if parts.fragment else ''
        rebased = f'{rel_from_out}{query}{fragment}'
        return f'<{rebased}>' if bracketed else rebased

    return map_markdown_urls(text, rewrite_url)


def map_markdown_urls(text: str, rewrite_url, on_prose_line=None) -> str:
    """Apply `rewrite_url` to every Markdown link, reference definition and HTML `href`/`src` outside code fences.
    `on_prose_line`, when given, sees each line outside a fence after its URLs were rewritten."""

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
        if on_prose_line is not None:
            on_prose_line(line)
        lines.append(line)
    return ''.join(lines)


# ---------------------------------------------------------------- offline documentation (release packages)

REPOSITORY_URL = 'https://github.com/rizonesoft/RasterFold'
OFFLINE_ENTRY = 'docs/index.md'
OFFLINE_README = 'README.md'           # the package's first page (PACKAGE.md in the repository), when present
# Repository files a release package ships outside docs/, at the same relative path.
OFFLINE_SHIPPED_FILES = ('LICENSE', 'CHANGELOG.md', 'include/rasterfold.h', 'README.md')
# Repository files a release package ships under another name.
OFFLINE_RENAMED_FILES = {'PACKAGE.md': 'README.md'}
_OFFLINE_INCLUDE_RE = re.compile(r'\{%\s*include\s+generated/(?P<kind>charts|tables)/(?P<name>[a-z0-9-]+)\.(?P<ext>svg|md)\s*%\}')
_FRONT_MATTER_RE = re.compile(r'\A---\n.*?\n---\n', re.S)


def _is_external_url(clean_url: str) -> bool:
    parts = urlsplit(clean_url)
    return bool(parts.scheme or parts.netloc) or clean_url.startswith('#') or not parts.path


def offline_link_report(files: dict[str, bytes], shipped: tuple[str, ...] = OFFLINE_SHIPPED_FILES) -> dict:
    """Check an offline documentation set (package-relative path -> bytes) without any network: every relative
    link of every `docs/*.md` page must name a file of the set or a shipped file, and every page must be
    reachable from `docs/index.md` by following links."""
    pages = sorted(name for name in files if name.startswith('docs/') and name.endswith('.md'))
    checked = pages + ([OFFLINE_README] if OFFLINE_README in files else [])
    known = set(files) | set(shipped)
    counts = {'links': 0}
    unresolved: list[str] = []
    edges: dict[str, set[str]] = {page: set() for page in checked}
    leftovers: list[str] = []
    for page in checked:
        text = files[page].decode('utf-8')

        def visit(raw_url: str, page: str = page) -> str:
            clean = raw_url[1:-1] if raw_url.startswith('<') and raw_url.endswith('>') else raw_url
            if _is_external_url(clean):
                return raw_url
            counts['links'] += 1
            path = unquote(urlsplit(clean).path)
            target = os.path.normpath((Path(page).parent / path).as_posix()).replace('\\', '/')
            if path.startswith('/') or target.startswith('../') or target not in known:
                unresolved.append(f'{page}: {clean}')
            elif target in edges:
                edges[page].add(target)
            return raw_url

        def prose(line: str, page: str = page) -> None:
            if '{%' in line or '{{' in line:
                leftovers.append(f'{page}: template tag left in: {line.strip()[:80]}')
            if '/RasterFold/' in line.replace(REPOSITORY_URL + '/', ''):
                leftovers.append(f'{page}: hosted path left in: {line.strip()[:80]}')

        map_markdown_urls(text, visit, prose)
        if text.startswith('---\n'):
            leftovers.append(f'{page}: Jekyll front matter left in')
    if OFFLINE_README in edges and OFFLINE_ENTRY in edges and OFFLINE_ENTRY not in edges[OFFLINE_README]:
        leftovers.append(f'{OFFLINE_README}: does not link {OFFLINE_ENTRY}')
    reached: set[str] = set()
    todo = [OFFLINE_ENTRY] if OFFLINE_ENTRY in edges else []
    while todo:
        page = todo.pop()
        if page in reached:
            continue
        reached.add(page)
        todo.extend(edges[page] - reached)
    return {
        'pages': pages,
        'links': counts['links'],
        'unresolved': sorted(unresolved),
        'unreachable': sorted(set(pages) - reached),
        'leftovers': sorted(leftovers),
    }


def render_offline_docs(root: Path, revision: str) -> dict[str, bytes]:
    """The documentation a release package ships: every published page as Markdown that reads and links
    correctly with no network and no Jekyll. Returns package-relative path -> bytes.

    Front matter is removed, each generated table is inlined and each chart include becomes an image link to
    the chart file beside the page, and every relative link is rewritten: to the packaged copy when its target
    is a published page, an asset or a shipped file, otherwise to that path at `revision` on GitHub."""
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError(f'offline docs need the full commit id, got {revision!r}')
    errors = check_generated_pages(root)
    if errors:
        raise ValueError('; '.join(errors))
    site_content = load_site_content(root)
    page_docs = [page.get('output') or page['source'] for page in site_content['pages']]
    for doc_rel in page_docs:
        if not (doc_rel.startswith('docs/') and doc_rel.endswith('.md') and doc_rel.count('/') == 1):
            raise ValueError(f'{SITE_CONTENT}: page {doc_rel!r} is not a docs/*.md file')
    if OFFLINE_ENTRY not in page_docs:
        raise ValueError(f'{SITE_CONTENT}: no {OFFLINE_ENTRY} page')
    asset_files = [entry['source'] for entry in site_content.get('assets', [])]
    packaged = set(page_docs) | set(asset_files) | set(OFFLINE_SHIPPED_FILES)
    out: dict[str, bytes] = {}
    for asset_rel in asset_files:
        out[asset_rel] = safe_repo_path(root, asset_rel).read_bytes()
    for doc_rel in page_docs:
        text = safe_repo_path(root, doc_rel).read_text(encoding='utf-8')
        text, stripped = _FRONT_MATTER_RE.subn('', text, count=1)
        if not stripped:
            raise ValueError(f'{doc_rel}: no Jekyll front matter to remove')
        text = text.lstrip('\n')

        def include(match: re.Match[str], doc_rel: str = doc_rel) -> str:
            kind, name, ext = match.group('kind'), match.group('name'), match.group('ext')
            if (kind, ext) not in (('charts', 'svg'), ('tables', 'md')):
                raise ValueError(f'{doc_rel}: unsupported include {match.group(0)}')
            source = safe_repo_path(root, f'docs/_includes/generated/{kind}/{name}.{ext}')
            if kind == 'tables':
                return source.read_text(encoding='utf-8').strip('\n')
            out[f'docs/charts/{name}.svg'] = source.read_bytes()
            return f'![{name} chart](charts/{name}.svg)'

        text = _OFFLINE_INCLUDE_RE.sub(include, text)
        page_dir = Path(doc_rel).parent

        def rewrite(raw_url: str, doc_rel: str = doc_rel, page_dir: Path = page_dir) -> str:
            bracketed = raw_url.startswith('<') and raw_url.endswith('>')
            clean = raw_url[1:-1] if bracketed else raw_url
            if _is_external_url(clean):
                return raw_url
            parts = urlsplit(clean)
            if parts.path.startswith('/'):
                raise ValueError(f'{doc_rel}: site-absolute link cannot be packaged: {clean}')
            # A URL path is percent-encoded; the file it names is not. Decode for the lookup, encode what is emitted.
            target = os.path.normpath((page_dir / unquote(parts.path)).as_posix()).replace('\\', '/')
            if target.startswith('../') or target == '..':
                raise ValueError(f'{doc_rel}: link escapes repository: {clean}')
            suffix = (('?' + parts.query) if parts.query else '') + (('#' + parts.fragment) if parts.fragment else '')
            if target in OFFLINE_RENAMED_FILES:
                safe_repo_path(root, target).read_bytes()        # the repository file must exist
                target = OFFLINE_RENAMED_FILES[target]
            if target in packaged or target in out:
                rebased = quote(os.path.relpath(target, page_dir.as_posix()).replace('\\', '/'), safe='/') + suffix
            else:
                on_disk = safe_repo_path(root, target)
                if on_disk.is_dir():
                    rebased = f'{REPOSITORY_URL}/tree/{revision}/{quote(target, safe="/")}'
                elif on_disk.is_file():
                    rebased = f'{REPOSITORY_URL}/blob/{revision}/{quote(target, safe="/")}{suffix}'
                else:
                    raise ValueError(f'{doc_rel}: link target does not exist: {clean}')
            return f'<{rebased}>' if bracketed else rebased

        out[doc_rel] = map_markdown_urls(text, rewrite).encode('utf-8')
    # Site-only assets (stylesheets, scripts) that no offline page links to are not shipped.
    referenced: set[str] = set()
    for doc_rel in page_docs:

        def note(raw_url: str, doc_rel: str = doc_rel) -> str:
            clean = raw_url[1:-1] if raw_url.startswith('<') and raw_url.endswith('>') else raw_url
            if not _is_external_url(clean):
                referenced.add(os.path.normpath((Path(doc_rel).parent / unquote(urlsplit(clean).path)).as_posix()).replace('\\', '/'))
            return raw_url

        map_markdown_urls(out[doc_rel].decode('utf-8'), note)
    out = {name: data for name, data in out.items() if name in page_docs or name in referenced}
    report = offline_link_report(out)
    problems = report['unresolved'] + report['leftovers'] + [f'{page}: not reachable from {OFFLINE_ENTRY}' for page in report['unreachable']]
    if problems:
        raise ValueError('offline docs: ' + '; '.join(problems))
    return out


def check_offline_tree(directory: Path) -> dict:
    """`offline_link_report` for an extracted release package directory (its top directory)."""
    files: dict[str, bytes] = {}
    for path in sorted(directory.rglob('*')):
        if path.is_symlink():
            raise ValueError(f'{path}: symbolic link in a package tree')
        if path.is_file():
            files[path.relative_to(directory).as_posix()] = path.read_bytes()
    missing = [name for name in OFFLINE_SHIPPED_FILES if name not in files]
    if missing:
        raise ValueError(f'{directory}: package tree lacks {", ".join(missing)}')
    return offline_link_report(files)


def _mirrored_front_matter(page_meta: dict) -> str:
    source_rel = page_meta['source']
    title = page_meta['title']
    nav_order = page_meta['nav_order']
    return (
        '---\n'
        'layout: default\n'
        f'title: "{title}"\n'
        f'nav_order: {nav_order}\n'
        f'source_path: {source_rel}\n'
        '---\n\n'
        f'<!-- GENERATED by scripts/build-docs.py from {source_rel}; do not edit by hand. -->\n\n'
    )


def render_format_page(root: Path, page_meta: dict, mirrored_map: dict[str, str] | None = None) -> str:
    source_rel = page_meta['source']
    output_rel = page_meta['output']
    spec_text = safe_repo_path(root, source_rel).read_text(encoding='utf-8')
    body = relocate_mirrored_links(spec_text.lstrip(), source_rel, output_rel, mirrored_map)
    return _mirrored_front_matter(page_meta) + body


def _strip_drift_block_markers(text: str) -> str:
    return re.sub(r'^[ \t]*<!--\s*drift:(?:begin|end)\s+[A-Za-z0-9_.-]+\s*-->[ \t]*\r?\n?', '', text, flags=re.M)


def render_installation_page(root: Path, page_meta: dict, mirrored_map: dict[str, str] | None = None) -> str:
    source_rel = page_meta['source']
    output_rel = page_meta['output']
    build_text = _strip_drift_block_markers(safe_repo_path(root, source_rel).read_text(encoding='utf-8'))
    body = relocate_mirrored_links(build_text.lstrip(), source_rel, output_rel, mirrored_map)
    return _mirrored_front_matter(page_meta) + body


def render_c_api_page(root: Path, page_meta: dict, mirrored_map: dict[str, str] | None = None) -> str:
    source_rel = page_meta['source']
    output_rel = page_meta['output']
    lib_text = _strip_drift_block_markers(safe_repo_path(root, source_rel).read_text(encoding='utf-8'))
    body = relocate_mirrored_links(lib_text.lstrip(), source_rel, output_rel, mirrored_map)
    return _mirrored_front_matter(page_meta) + body


def _header_comment_text(raw: str) -> str:
    lines = []
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith('*'):
            line = line[1:].lstrip()
        lines.append(line)
    return '\n'.join(lines).strip()


def parse_api_header(root: Path, header_rel: str, def_rel: str) -> dict:
    """Parse the documented subset of the public header.

    Supported: `#define` constants under a shared comment, `typedef struct`
    types (single-line or multi-line, comma-separated and array fields), and
    `RF_API` function declarations (possibly multi-line), each preceded by a
    doc comment. Anything else public fails explicitly rather than being
    dropped from the reference silently.
    """
    text = safe_repo_path(root, header_rel).read_text(encoding='utf-8')
    def_text = safe_repo_path(root, def_rel).read_text(encoding='utf-8')
    exports: dict[str, int] = {}
    in_exports = False
    for raw_line in def_text.splitlines():
        entry = raw_line.strip()
        if entry == 'EXPORTS':
            in_exports = True
            continue
        if not in_exports or not entry:
            continue
        m = re.fullmatch(r'([A-Za-z_][A-Za-z0-9_]*) @([0-9]+)', entry)
        if not m:
            raise ValueError(f'{def_rel}: unsupported export entry: {entry}')
        if m.group(1) in exports:
            raise ValueError(f'{def_rel}: duplicate export entry: {entry}')
        exports[m.group(1)] = int(m.group(2))
    if not exports:
        raise ValueError(f'{def_rel}: no exports parsed')
    intro = None
    constants: list[dict] = []
    types: list[dict] = []
    functions: list[dict] = []
    comment: str | None = None
    comment_used = False
    lines = text.splitlines()
    i = 0

    def take_statement(start: int, struct: bool = False) -> tuple[str, int]:
        parts = []
        j = start
        while j < len(lines):
            parts.append(lines[j].strip())
            joined = ' '.join(parts)
            if '//' in joined or '/*' in joined:
                raise ValueError(f'{header_rel}:{start + 1}: unsupported comment inside a declaration')
            if any(part.startswith('#') for part in parts):
                raise ValueError(f'{header_rel}:{start + 1}: unsupported directive inside a declaration')
            if re.search(r'\}\s*[a-z_][a-z0-9_]*\s*;', joined) if struct else ';' in lines[j]:
                break
            j += 1
        return ' '.join(parts), j

    while i < len(lines):
        line = lines[i].strip()
        if line.startswith('/*'):
            raw = line[2:]
            while '*/' not in raw and i + 1 < len(lines):
                i += 1
                raw += '\n' + lines[i]
            raw, _, rest = raw.partition('*/')
            if intro is None:
                intro = _header_comment_text(raw)
            else:
                comment = _header_comment_text(raw)
                comment_used = False
            if rest.strip():
                # A declaration may share the line with the comment's end;
                # it still has to be parsed, never discarded.
                lines[i] = rest
                line = rest.strip()
            else:
                i += 1
                continue
        if not line or line.startswith('#include') or line in ('#endif', '}') or line.startswith('extern "C"'):
            i += 1
            continue
        if line == '#ifndef RASTERFOLD_H' or line.startswith('#define RASTERFOLD_H'):
            i += 1
            continue
        if line.startswith('#ifndef'):
            raise ValueError(f'{header_rel}:{i + 1}: unsupported conditional: {line}')
        if line == '#ifdef __cplusplus':
            i += 1
            continue
        if line == '#if defined(_WIN32) && defined(RF_SHARED)':
            # The one conditional block in the supported subset: RF_API
            # visibility selection. Only its own directives may appear.
            i += 1
            while i < len(lines) and lines[i].strip() != '#endif':
                inner = lines[i].strip()
                if inner.startswith('#define RF_API') or inner in ('#elif !defined(_WIN32)', '#else'):
                    i += 1
                    continue
                raise ValueError(f'{header_rel}:{i + 1}: unsupported declaration in the RF_API visibility block: {inner}')
            i += 1
            continue
        if line.startswith('#if') or line.startswith('#elif') or line == '#else':
            raise ValueError(f'{header_rel}:{i + 1}: unsupported conditional: {line}')
        if line.startswith('#define'):
            if line == '#define RF_API' or line.startswith('#define RF_API '):
                i += 1
                continue
            m = re.fullmatch(r'#define\s+([A-Z_][A-Z0-9_]*)\s+(.+)', line)
            if not m:
                raise ValueError(f'{header_rel}:{i + 1}: unsupported #define: {line}')
            if m.group(1) == 'RF_API':
                i += 1
                continue
            if comment is None:
                raise ValueError(f'{header_rel}:{i + 1}: undocumented constant {m.group(1)}')
            constants.append({'name': m.group(1), 'value': m.group(2).strip(), 'doc': comment})
            comment_used = True
            i += 1
            continue
        if line.startswith('typedef struct'):
            if comment is None or comment_used:
                raise ValueError(f'{header_rel}:{i + 1}: undocumented type: {line}')
            stmt, j = take_statement(i, struct=True)
            if stmt.count('typedef') != 1:
                raise ValueError(f'{header_rel}:{i + 1}: unsupported multiple declarations: {stmt}')
            # Parse structurally, not by one match: the body is the text
            # up to the first closing brace (no nested brace allowed), and
            # everything after it must be exactly the type name and its
            # semicolon. Any second declaration sharing the statement (a
            # typedef or a bare struct, with any spacing) then refuses.
            # The prefix must be exactly the typedef keyword and struct:
            # a declaration before the first brace (for example a forward
            # typedef sharing the statement) is refused, not skipped.
            if stmt[:stmt.index('{')].strip() != 'typedef struct':
                raise ValueError(f'{header_rel}:{i + 1}: unsupported typedef shape: {stmt}')
            open_brace = stmt.index('{')
            close_brace = stmt.index('}', open_brace)
            body = stmt[open_brace + 1:close_brace]
            rest = stmt[close_brace + 1:].strip()
            m = re.fullmatch(r'([a-z_][a-z0-9_]*);', rest)
            if '{' in body or not m:
                raise ValueError(f'{header_rel}:{i + 1}: unsupported typedef shape: {stmt}')
            fields = [f.strip() for f in body.split(';') if f.strip()]
            types.append({'name': m.group(1), 'fields': fields, 'doc': comment, 'decl': stmt})
            comment = None
            comment_used = False
            i = j + 1
            continue
        if line.startswith('RF_API'):
            if comment is None or comment_used:
                raise ValueError(f'{header_rel}:{i + 1}: undocumented function: {line}')
            stmt, j = take_statement(i)
            if stmt.split(';', 1)[1].strip() or stmt.count('RF_API') != 1:
                raise ValueError(f'{header_rel}:{i + 1}: unsupported multiple declarations on one line: {stmt}')
            m = re.fullmatch(r'RF_API\s+(.+?)\s+(rf_\w+)\s*\((.*)\)\s*;', stmt)
            if not m:
                raise ValueError(f'{header_rel}:{i + 1}: unsupported function declaration: {stmt}')
            functions.append({
                'name': m.group(2),
                'decl': f'{m.group(1)} {m.group(2)}({m.group(3)});',
                'doc': comment,
            })
            comment = None
            comment_used = False
            i = j + 1
            continue
        raise ValueError(f'{header_rel}:{i + 1}: unsupported public declaration: {line}')
    if intro is None:
        raise ValueError(f'{header_rel}: header contract comment not found')
    documented = {f['name'] for f in functions}
    missing = sorted(set(exports) - documented)
    if missing:
        raise ValueError(f'{header_rel}: exported function(s) without documentation: {", ".join(missing)}')
    extra = sorted(documented - set(exports))
    if extra:
        raise ValueError(f'{header_rel}: documented function(s) not exported in {def_rel}: {", ".join(extra)}')
    return {'intro': intro, 'constants': constants, 'types': types, 'functions': functions, 'exports': exports}


def render_api_page(root: Path, page_meta: dict) -> str:
    header_rel = page_meta['source']
    api = parse_api_header(root, header_rel, 'src/rasterfold.def')
    out = [f'# C API Reference\n\n{api["intro"]}\n']
    out.append('## Functions\n')
    for fn in api['functions']:
        out.append(f'### `{fn["name"]}`\n\n```c\n{fn["decl"]}\n```\n\n{fn["doc"]}\n')
    out.append('## Types\n')
    for ty in api['types']:
        fields = '\n'.join(f'- `{field};`' for field in ty['fields'])
        out.append(f'### `{ty["name"]}`\n\n```c\n{ty["decl"]}\n```\n\n{fields}\n\n{ty["doc"]}\n')
    out.append('## Constants\n')
    for const in api['constants']:
        out.append(f'### `{const["name"]}`\n\n```c\n#define {const["name"]} {const["value"]}\n```\n\n{const["doc"]}\n')
    return _mirrored_front_matter(page_meta) + '\n'.join(out)


_C_HELP_DECL_RE = re.compile(r'^static const char (?P<name>[A-Za-z_][A-Za-z0-9_]*)\[\] =', re.M)
_C_HELP_DISPATCH_RE = re.compile(r'wide_eq\(pos\[1\], "(?P<command>[a-z]+)"\)\) rf_print\(0, (?P<name>help_[a-z]+)\);')


_C_COND_RE = re.compile(r'[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b')


def c_inactive_offsets(text: str) -> list[bool]:
    """For each character of a C source, whether it lies inside a comment or under any preprocessor conditional.

    The help reference is read from source without compiling it, so it must not read text the compiler would
    not: a declaration or a dispatch line inside `/* */`, after `//`, or between `#if`/`#ifdef`/`#ifndef` and
    `#endif` (whatever the condition) is treated as inactive and refused by the caller."""
    # Translation phases 1 and 2 run before comments are recognised: a backslash-newline splices two lines
    # (so `/` and `*` on either side of one open a comment) and the trigraph `??/` spells that backslash.
    # This scanner does not model either, so a source that uses them anywhere is refused outright.
    spliced = re.search(r'\\[ \t]*\r?\n|\?\?/', text)
    if spliced:
        line = text.count('\n', 0, spliced.start()) + 1
        raise ValueError(f'line {line}: a backslash-newline line splice or the trigraph ??/ is not supported')
    inactive = [False] * len(text)
    depth = 0
    pos = 0
    state = 'code'          # code | block | line | string | char
    line_start = True
    while pos < len(text):
        ch = text[pos]
        nxt = text[pos + 1] if pos + 1 < len(text) else ''
        if state == 'code':
            if line_start:
                m = _C_COND_RE.match(text, pos)
                if m:
                    if m.group(1) in ('if', 'ifdef', 'ifndef'):
                        depth += 1
                    end = text.find('\n', pos)
                    end = len(text) if end < 0 else end
                    for i in range(pos, end):
                        inactive[i] = True
                    if m.group(1) == 'endif':
                        depth -= 1
                        if depth < 0:
                            raise ValueError('unbalanced #endif')
                    pos = end
                    continue
            if ch == '/' and nxt == '*':
                state = 'block'
                inactive[pos] = inactive[pos + 1] = True
                pos += 2
                line_start = False
                continue
            if ch == '/' and nxt == '/':
                state = 'line'
                inactive[pos] = True
                pos += 1
                line_start = False
                continue
            if ch == '"':
                state = 'string'
            elif ch == "'":
                state = 'char'
            inactive[pos] = depth > 0
        elif state == 'block':
            inactive[pos] = True
            if ch == '*' and nxt == '/':
                inactive[pos + 1] = True
                pos += 2
                state = 'code'
                continue
        elif state == 'line':
            inactive[pos] = True
            if ch == '\n':
                state = 'code'
        else:                                   # string or char literal
            inactive[pos] = depth > 0
            if ch == '\\':
                if pos + 1 < len(text):
                    inactive[pos + 1] = depth > 0
                pos += 2
                line_start = False
                continue
            if (state == 'string' and ch == '"') or (state == 'char' and ch == "'") or ch == '\n':
                state = 'code'
        line_start = ch == '\n' or (line_start and ch in ' \t')
        pos += 1
    if depth != 0:
        raise ValueError('unbalanced preprocessor conditional')
    return inactive


def extract_c_help_constant(text: str, name: str, source_rel: str) -> str:
    """The value of `static const char <name>[] = ...;` in a C source, with `RF_EOL` read as one LF.

    The initializer may hold only double-quoted string literals and the `RF_EOL` token; a literal may hold
    only the escapes `\\"` and `\\\\`. Anything else is refused, so an initializer this parser does not
    fully understand can never become part of the reference."""
    decls = [m for m in _C_HELP_DECL_RE.finditer(text) if m.group('name') == name]
    if len(decls) != 1:
        raise ValueError(f'{source_rel}: expected exactly one `static const char {name}[] =`, found {len(decls)}')
    inactive = c_inactive_offsets(text)
    if inactive[decls[0].start()]:
        raise ValueError(f'{source_rel}: {name} is declared inside a comment or a preprocessor conditional')
    pos = decls[0].end()
    out: list[str] = []
    while True:
        while pos < len(text) and text[pos] in ' \t\r\n':
            pos += 1
        if pos >= len(text):
            raise ValueError(f'{source_rel}: {name}: initializer has no terminating `;`')
        if inactive[pos]:
            raise ValueError(f'{source_rel}: {name}: a comment or preprocessor line inside the initializer')
        ch = text[pos]
        if ch == ';':
            break
        if ch == '"':
            pos += 1
            while True:
                if pos >= len(text) or text[pos] == '\n':
                    raise ValueError(f'{source_rel}: {name}: unterminated string literal')
                c = text[pos]
                if c == '"':
                    pos += 1
                    break
                if c == '\\':
                    nxt = text[pos + 1] if pos + 1 < len(text) else ''
                    if nxt not in ('"', '\\'):
                        raise ValueError(f'{source_rel}: {name}: unsupported escape \\{nxt} in a help string')
                    out.append(nxt)
                    pos += 2
                    continue
                out.append(c)
                pos += 1
            continue
        if text.startswith('RF_EOL', pos) and not (text[pos + 6:pos + 7].isalnum() or text[pos + 6:pos + 7] == '_'):
            out.append('\n')
            pos += 6
            continue
        token = re.match(r'\S+', text[pos:])
        raise ValueError(f'{source_rel}: {name}: unsupported initializer token {token.group(0) if token else text[pos]!r}')
    value = ''.join(out)
    if not value or not value.endswith('\n'):
        raise ValueError(f'{source_rel}: {name}: help text is empty or does not end with RF_EOL')
    return value


def parse_cli_help(root: Path, source_rel: str) -> dict:
    """The command summary and every command's help text, in the order the `help` dispatcher names them."""
    text = safe_repo_path(root, source_rel).read_text(encoding='utf-8')
    inactive = c_inactive_offsets(text)
    dispatch_matches = list(_C_HELP_DISPATCH_RE.finditer(text))
    for m in dispatch_matches:
        if any(inactive[m.start():m.end()]):
            raise ValueError(
                f'{source_rel}: the `help {m.group("command")}` dispatch is inside a comment or a preprocessor conditional'
            )
    dispatched = [(m.group('command'), m.group('name')) for m in dispatch_matches]
    if not dispatched:
        raise ValueError(f'{source_rel}: no `help <command>` dispatch found')
    # The dispatch lines must be one unbroken chain, so no other branch (compiled or not) sits between them.
    chain_start = dispatch_matches[0].start()
    chain_end = dispatch_matches[-1].end()
    if any(inactive[chain_start:chain_end]):
        raise ValueError(f'{source_rel}: a comment or preprocessor line inside the `help <command>` dispatch chain')
    for command, name in dispatched:
        if name != f'help_{command}':
            raise ValueError(f'{source_rel}: `help {command}` prints {name}, expected help_{command}')
    commands = [command for command, _ in dispatched]
    if len(set(commands)) != len(commands):
        raise ValueError(f'{source_rel}: a command is dispatched twice by `help`')
    declared = {m.group('name') for m in _C_HELP_DECL_RE.finditer(text) if m.group('name').startswith('help_')}
    orphans = sorted(declared - {name for _, name in dispatched})
    if orphans:
        raise ValueError(f'{source_rel}: help text never printed by `help <command>`: {", ".join(orphans)}')
    summary = extract_c_help_constant(text, 'usage_text', source_rel)
    helps = [(command, extract_c_help_constant(text, name, source_rel)) for command, name in dispatched]
    for command, body in helps:
        if not body.startswith(f'usage: rasterfold {command}'):
            raise ValueError(f'{source_rel}: help_{command} does not start with its usage line')
        if f'  rasterfold {command}' not in summary:
            raise ValueError(f'{source_rel}: usage_text has no usage line for {command}')
    return {'summary': summary, 'commands': helps}


def render_cli_page(root: Path, page_meta: dict) -> str:
    cli = parse_cli_help(root, page_meta['source'])
    out = [
        '# Command-Line Reference\n\n'
        'Every block on this page is text the program prints, taken from its source, so the page cannot describe '
        'a command or an option that `rasterfold` does not have. `rasterfold help` prints the summary and '
        '`rasterfold help <command>` prints one command.\n',
        '## Summary\n\n```text\n' + cli['summary'] + '```\n',
    ]
    for command, body in cli['commands']:
        out.append(f'## `rasterfold {command}`\n\n```text\n{body}```\n')
    return _mirrored_front_matter(page_meta) + '\n'.join(out)


def render_contributing_page(root: Path, page_meta: dict, mirrored_map: dict[str, str] | None = None) -> str:
    source_rel = page_meta['source']
    output_rel = page_meta['output']
    contrib_text = safe_repo_path(root, source_rel).read_text(encoding='utf-8')
    sanitized = (
        contrib_text.replace('(AGENTS.md)', '(https://github.com/rizonesoft/RasterFold/blob/main/AGENTS.md)')
        .replace('(todo/README.md)', '(https://github.com/rizonesoft/RasterFold/blob/main/todo/README.md)')
    )
    body = relocate_mirrored_links(sanitized.lstrip(), source_rel, output_rel, mirrored_map)
    return _mirrored_front_matter(page_meta) + body


def render_security_page(root: Path, page_meta: dict, mirrored_map: dict[str, str] | None = None) -> str:
    source_rel = page_meta['source']
    output_rel = page_meta['output']
    sec_text = safe_repo_path(root, source_rel).read_text(encoding='utf-8')
    body = relocate_mirrored_links(sec_text.lstrip(), source_rel, output_rel, mirrored_map)
    return _mirrored_front_matter(page_meta) + body


def render_usage_page(root: Path, page_meta: dict, mirrored_map: dict[str, str] | None = None) -> str:
    source_rel = page_meta['source']
    output_rel = page_meta['output']
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
    body = relocate_mirrored_links(body, source_rel, output_rel, mirrored_map)
    title = page_meta['title']
    return (
        _mirrored_front_matter(page_meta)
        + f'# {title}\n\n'
        + 'RasterFold is a lossless RGB image codec written in C11. It turns 24-bit BMP images into `.fold` files and restores every pixel exactly.\n\n'
        + body
    )


def sanitize_public_research(text: str) -> str:
    # Strip internal TODO references while preserving all measured figures and prose.
    text = re.sub(r'\s*\(`D\d{2}\s+T\d{2}(?:\s+§\d+)?(?:`,\s*`D\d{2}\s+T\d{2}(?:\s+§\d+)?)*`(?:,\s*milestone\s+\d+)?\)', '', text)
    text = re.sub(r',\s*`D\d{2}\s+T\d{2}(?:\s+§\d+)?`', '', text)
    text = re.sub(r'\s+of\s+`D\d{2}\s+T\d{2}(?:\s+§\d+)?`', '', text)
    text = re.sub(r'\s*\(backlog\s+`B-\d+`\)', '', text)
    text = text.replace('(`spec/FOLD.md`)', '([Format Specification](format.md))')
    return text


def render_research_summary_page(root: Path, page_meta: dict, mirrored_map: dict[str, str] | None = None) -> str:
    source_rel = page_meta['source']
    output_rel = page_meta['output']
    research_text = safe_repo_path(root, source_rel).read_text(encoding='utf-8')
    _, sections = split_h2_sections(research_text)
    selected: list[str] = []
    for heading in RESEARCH_SECTIONS:
        if heading not in sections:
            raise ValueError(f'{source_rel}: missing required section {heading!r}')
        chunk = sections[heading].rstrip()
        chunk = re.sub(r'\n---\s*$', '', chunk)
        chunk = sanitize_public_research(chunk)
        chunk = relocate_mirrored_links(chunk, source_rel, output_rel, mirrored_map)
        selected.append(chunk)
    return (
        _mirrored_front_matter(page_meta)
        + '# RasterFold Research Summary\n\n'
        + 'Status date: 2026-09-29 (historical baseline record; see [Benchmarks](benchmarks.md) for current committed measurements).\n\n'
        + 'This summary excerpts the public measurement baseline, protocol, experiment ledger, and prior-art references from the research notes. '
        + 'The normative bitstream is specified in [Format Specification](format.md); the measurement scripts are documented in [`research/README.md`](../research/README.md).\n\n'
        + '\n\n---\n\n'.join(selected)
        + '\n'
    )


CHART_NAMES = (
    'corpus-size-ratio',
    'benchmark-size-ratio',
    'encode-time',
    'decode-time',
    'peak-memory',
    'scatter-e9',
    'size-vs-decode',
)
CORPUS_CSV = 'benchmarks/corpus.csv'
RESULTS_CSV = 'benchmarks/results.csv'
CORPUS_CODECS = ('rasterfold', 'jxl_e7', 'jxl_e9', 'webp')
BENCHMARK_SETS = ('measure', 'photos', 'synthetic')
BENCHMARK_CODECS = ('rasterfold', 'rasterfold-1t', 'jxl', 'jxl-1t', 'jxl-e9', 'webp', 'png')
TP_OVERLAP_PHOTO = (
    'kodim01',
    'kodim03',
    'kodim05',
    'kodim13',
    'kodim23',
)
CODEC_COLORS = {
    'rasterfold': '#2563eb',
    'rasterfold-1t': '#60a5fa',
    'jxl': '#059669',
    'jxl_e7': '#059669',
    'jxl-1t': '#34d399',
    'jxl-e9': '#7c3aed',
    'jxl_e9': '#7c3aed',
    'webp': '#d97706',
    'png': '#64748b',
}
SET_COLORS = {
    'photo': '#2563eb',
    'tp': '#059669',
    'synth': '#d97706',
    'measure': '#2563eb',
    'photos': '#059669',
    'synthetic': '#d97706',
}
CODEC_THREAD_LABELS = {
    'rasterfold': 'enc default (up to 9T) / dec default (up to 9T)',
    'rasterfold-1t': 'enc 1T / dec 1T',
    'jxl': 'enc 1T / dec default',
    'jxl-1t': 'enc 1T / dec 1T',
    'jxl-e9': 'enc 1T / dec default',
    'webp': 'enc 1T / dec 1T',
    'png': 'enc 1T / dec 1T',
}


def _geomean(values: list[float]) -> float:
    if not values:
        raise ValueError('cannot compute geometric mean of empty sequence')
    return math.exp(sum(math.log(v) for v in values) / len(values))


def _fmt_us_sum(values: list[int | float | str]) -> str:
    from decimal import Decimal

    tot = sum((Decimal(str(v)) for v in values), Decimal(0))
    return str(int(tot)) if tot == int(tot) else format(tot.normalize(), 'f')


def load_csv_with_meta(root: Path, rel: str) -> tuple[dict[str, str], list[dict[str, str]]]:
    path = safe_repo_path(root, rel)
    if not path.is_file():
        raise ValueError(f'{rel}: missing measurement CSV')
    raw_lines = path.read_text(encoding='utf-8').splitlines(keepends=True)
    meta: dict[str, str] = {}
    idx = 0
    while idx < len(raw_lines) and raw_lines[idx].lstrip().startswith('#'):
        comment = raw_lines[idx].lstrip()[1:].strip()
        for token in comment.split():
            if '=' in token:
                k, v = token.split('=', 1)
                meta[k.strip()] = v.strip()
        idx += 1
    for req_key, pattern in (
        ('codec_revision', r'^[0-9a-f]{40}$'),
        ('kat_fold_sha256', r'^[0-9a-f]{64}$'),
        ('date', r'^\d{4}-\d{2}-\d{2}$'),
    ):
        val = meta.get(req_key, '')
        if not re.fullmatch(pattern, val):
            raise ValueError(f'{rel}: missing or invalid header {req_key}={val!r}')
    if rel == RESULTS_CSV:
        for req_key in ('measured_sets', 'retained_sets', 'retained_revision', 'retained_date', 'set_revisions', 'set_dates'):
            if not meta.get(req_key):
                raise ValueError(f'{rel}: missing required header {req_key}')
        if meta['retained_sets'] != 'none':
            for tok in meta['retained_revision'].split(','):
                rev_val = tok.split(':', 1)[1] if ':' in tok else tok
                if not re.fullmatch(r'^[0-9a-f]{40}$', rev_val):
                    raise ValueError(f'{rel}: invalid retained_revision={meta["retained_revision"]!r}')
            for tok in meta['retained_date'].split(','):
                date_val = tok.split(':', 1)[1] if ':' in tok else tok
                if not re.fullmatch(r'^\d{4}-\d{2}-\d{2}$', date_val):
                    raise ValueError(f'{rel}: invalid retained_date={meta["retained_date"]!r}')
        rev_pairs = dict(item.split(':', 1) for item in meta['set_revisions'].split(',') if ':' in item)
        date_pairs = dict(item.split(':', 1) for item in meta['set_dates'].split(',') if ':' in item)
        for s in BENCHMARK_SETS:
            if not re.fullmatch(r'^[0-9a-f]{40}$', rev_pairs.get(s, '')):
                raise ValueError(f'{rel}: invalid or missing set_revisions entry for {s} in {meta["set_revisions"]!r}')
            if not re.fullmatch(r'^\d{4}-\d{2}-\d{2}$', date_pairs.get(s, '')):
                raise ValueError(f'{rel}: invalid or missing set_dates entry for {s} in {meta["set_dates"]!r}')
    body = ''.join(raw_lines[idx:])
    rows = list(csv.DictReader(io.StringIO(body)))
    if not rows:
        raise ValueError(f'{rel}: CSV contains no data rows')
    return meta, rows


def compute_chart_datasets(root: Path) -> dict[str, dict]:
    corpus_meta, corpus_raw = load_csv_with_meta(root, CORPUS_CSV)
    results_meta, results_raw = load_csv_with_meta(root, RESULTS_CSV)

    corpus_rows: list[dict] = []
    seen_corpus_images: set[str] = set()
    for r in corpus_raw:
        img = (r.get('image') or '').strip()
        set_name = (r.get('set') or '').strip()
        if not img or set_name not in ('photo', 'tp', 'synth') or img in seen_corpus_images:
            raise ValueError(f'{CORPUS_CSV}: invalid or duplicate row {r!r}')
        seen_corpus_images.add(img)
        w = int(r['width'])
        h = int(r['height'])
        rf = int(r['rasterfold'])
        e7 = int(r['jxl_e7'])
        e9 = int(r['jxl_e9'])
        wp = int(r['webp'])
        if min(w, h, rf, e7, e9, wp) <= 0:
            raise ValueError(f'{CORPUS_CSV}: non-positive value in row {r!r}')
        corpus_rows.append(
            {
                'set': set_name,
                'image': img,
                'width': w,
                'height': h,
                'raw': w * h * 3,
                'rasterfold': rf,
                'jxl_e7': e7,
                'jxl_e9': e9,
                'webp': wp,
            }
        )

    results_rows: list[dict] = []
    seen_results: set[tuple[str, str, str]] = set()
    for r in results_raw:
        set_name = (r.get('set') or '').strip()
        img = (r.get('image') or '').strip()
        codec = (r.get('codec') or '').strip()
        key = (set_name, img, codec)
        if set_name not in BENCHMARK_SETS or codec not in BENCHMARK_CODECS or not img or key in seen_results:
            raise ValueError(f'{RESULTS_CSV}: invalid or duplicate row {r!r}')
        seen_results.add(key)
        if (r.get('pixels_match') or '').strip() != 'True':
            raise ValueError(f'{RESULTS_CSV}: pixels_match is not True for {key}')
        w = int(r['width'])
        h = int(r['height'])
        raw_b = int(r['raw'])
        b = int(r['bytes'])
        enc_us = float(r['encode_us'])
        dec_us = float(r['decode_us'])
        enc_pk = int(r['encode_peak'])
        dec_pk = int(r['decode_peak'])
        if min(w, h, raw_b, b, enc_us, dec_us, enc_pk, dec_pk) <= 0:
            raise ValueError(f'{RESULTS_CSV}: non-positive measurement in {key}')
        results_rows.append(
            {
                'set': set_name,
                'image': img,
                'width': w,
                'height': h,
                'raw': raw_b,
                'codec': codec,
                'bytes': b,
                'encode_us': int(enc_us) if enc_us.is_integer() else enc_us,
                'decode_us': int(dec_us) if dec_us.is_integer() else dec_us,
                'encode_peak': enc_pk,
                'decode_peak': dec_pk,
            }
        )

    corpus_prov = {
        'source': CORPUS_CSV,
        'codec_revision': corpus_meta['codec_revision'],
        'kat_fold_sha256': corpus_meta['kat_fold_sha256'],
        'date': corpus_meta['date'],
        'threads': 'rasterfold: default (up to 9T); jxl_e7, jxl_e9, webp: -threads 1',
        'limitations': 'Size-only research corpus (55 images: photo 39, tp 10 overlapping / tp-only 5 exclusive, synth 11); PNG, wall time, and memory are measured in benchmarks/results.csv.',
    }
    set_rev_map = dict(item.split(':', 1) for item in results_meta['set_revisions'].split(',') if ':' in item)
    set_date_map = dict(item.split(':', 1) for item in results_meta['set_dates'].split(',') if ':' in item)
    results_prov = {
        'source': RESULTS_CSV,
        'codec_revision': results_meta['codec_revision'],
        'kat_fold_sha256': results_meta['kat_fold_sha256'],
        'date': results_meta['date'],
        'measured_sets': results_meta['measured_sets'],
        'retained_sets': results_meta['retained_sets'],
        'retained_revision': results_meta['retained_revision'],
        'retained_date': results_meta['retained_date'],
        'set_revisions': results_meta['set_revisions'],
        'set_dates': results_meta['set_dates'],
        'threads': 'rasterfold: default (up to 9T); rasterfold-1t: 1T; jxl/jxl-e9: enc 1T, dec default; jxl-1t: enc 1T, dec 1T; webp/png: 1T',
        'limitations': (
            f'measured_sets={results_meta["measured_sets"]} ({results_meta["date"]}, rev {results_meta["codec_revision"][:12]}); '
            f'retained_sets={results_meta["retained_sets"]} ({results_meta["retained_date"]}, rev {results_meta["retained_revision"][:12]}, jxl-e9 not measured).'
        ),
    }

    # 1. corpus-size-ratio
    corpus_groups = {
        'photo': [r for r in corpus_rows if r['set'] == 'photo'],
        'tp': [r for r in corpus_rows if r['set'] == 'tp' or r['image'] in TP_OVERLAP_PHOTO],
        'tp-only': [r for r in corpus_rows if r['set'] == 'tp'],
        'synth': [r for r in corpus_rows if r['set'] == 'synth'],
        'all': list(corpus_rows),
    }
    corpus_bars: list[dict] = []
    corpus_table_rows: list[list[str]] = []
    for gname in ('photo', 'tp', 'tp-only', 'synth', 'all'):
        g_rows = corpus_groups[gname]
        if not g_rows:
            raise ValueError(f'{CORPUS_CSV}: empty corpus group {gname}')
        e7_sum = sum(r['jxl_e7'] for r in g_rows)
        totals = {c: sum(r[c] for r in g_rows) for c in CORPUS_CODECS}
        ratios = {c: totals[c] / e7_sum for c in CORPUS_CODECS}
        rf_geomean = _geomean([r['rasterfold'] / r['jxl_e7'] for r in g_rows])
        for c in CORPUS_CODECS:
            corpus_bars.append(
                {
                    'group': gname,
                    'series': c,
                    'value': f'{ratios[c]:.4f}',
                    'num': ratios[c],
                    'raw_total': str(totals[c]),
                    'denom_total': str(e7_sum),
                }
            )
        corpus_table_rows.append(
            [
                gname,
                str(len(g_rows)),
                str(totals['rasterfold']),
                str(totals['jxl_e7']),
                str(totals['jxl_e9']),
                str(totals['webp']),
                f'{ratios["rasterfold"]:.4f}',
                f'{rf_geomean:.4f}',
                f'{ratios["jxl_e9"]:.4f}',
                f'{ratios["webp"]:.4f}',
            ]
        )

    # Helper to index results_rows by set -> image -> codec
    by_set_img: dict[str, dict[str, dict[str, dict]]] = {s: {} for s in BENCHMARK_SETS}
    for r in results_rows:
        by_set_img[r['set']].setdefault(r['image'], {})[r['codec']] = r
    for s in BENCHMARK_SETS:
        if not by_set_img[s]:
            raise ValueError(f'{RESULTS_CSV}: empty benchmark set {s}')
        for img, cmap in by_set_img[s].items():
            if 'jxl' not in cmap:
                raise ValueError(f'{RESULTS_CSV}: missing baseline jxl row for {s}/{img}')

    # 2. benchmark-size-ratio, 3. encode-time, 4. decode-time, 5. peak-memory, 7. size-vs-decode
    bench_size_bars: list[dict] = []
    bench_size_table: list[list[str]] = []
    enc_time_bars: list[dict] = []
    enc_time_table: list[list[str]] = []
    dec_time_bars: list[dict] = []
    dec_time_table: list[list[str]] = []
    peak_mem_bars: list[dict] = []
    peak_mem_table: list[list[str]] = []
    svd_points: list[dict] = []
    svd_table: list[list[str]] = []

    for s in BENCHMARK_SETS:
        imgs = sorted(by_set_img[s].keys())
        n_imgs = len(imgs)
        for c in BENCHMARK_CODECS:
            present = [by_set_img[s][img][c] for img in imgs if c in by_set_img[s][img]]
            if present and len(present) != n_imgs:
                raise ValueError(f'{RESULTS_CSV}: partial codec coverage for {s}/{c} ({len(present)}/{n_imgs})')
            if not present:
                for target_bars in (bench_size_bars, enc_time_bars, dec_time_bars):
                    target_bars.append({'group': s, 'series': c, 'value': 'N/A', 'num': None})
                peak_mem_bars.append({'group': f'{s}-encode', 'series': c, 'value': 'N/A', 'num': None, 'mib': 'N/A'})
                peak_mem_bars.append({'group': f'{s}-decode', 'series': c, 'value': 'N/A', 'num': None, 'mib': 'N/A'})
                svd_points.append({'group': s, 'series': c, 'x': 'N/A', 'y': 'N/A', 'num_x': None, 'num_y': None})
                na_note = f'N/A (not measured in retained {set_date_map[s]} set)'
                bench_size_table.append([s, c, str(n_imgs), na_note, na_note])
                enc_time_table.append([s, c, CODEC_THREAD_LABELS[c], na_note, na_note])
                dec_time_table.append([s, c, CODEC_THREAD_LABELS[c], na_note, na_note])
                peak_mem_table.append([s, c, na_note, na_note, na_note, na_note])
                svd_table.append([s, c, CODEC_THREAD_LABELS[c], na_note, na_note])
                continue

            total_bytes = sum(by_set_img[s][img][c]['bytes'] for img in imgs)
            size_geo = _geomean([by_set_img[s][img][c]['bytes'] / by_set_img[s][img]['jxl']['bytes'] for img in imgs])
            total_enc = _fmt_us_sum([by_set_img[s][img][c]['encode_us'] for img in imgs])
            enc_geo = _geomean([by_set_img[s][img][c]['encode_us'] / by_set_img[s][img]['jxl']['encode_us'] for img in imgs])
            total_dec = _fmt_us_sum([by_set_img[s][img][c]['decode_us'] for img in imgs])
            dec_geo = _geomean([by_set_img[s][img][c]['decode_us'] / by_set_img[s][img]['jxl']['decode_us'] for img in imgs])
            max_enc_pk = max(by_set_img[s][img][c]['encode_peak'] for img in imgs)
            max_dec_pk = max(by_set_img[s][img][c]['decode_peak'] for img in imgs)

            bench_size_bars.append({'group': s, 'series': c, 'value': f'{size_geo:.4f}', 'num': size_geo, 'total_bytes': str(total_bytes)})
            bench_size_table.append([s, c, str(n_imgs), str(total_bytes), f'{size_geo:.4f}'])

            enc_time_bars.append({'group': s, 'series': c, 'value': f'{enc_geo:.4f}', 'num': enc_geo, 'total_us': total_enc})
            enc_time_table.append([s, c, CODEC_THREAD_LABELS[c], total_enc, f'{enc_geo:.4f}'])

            dec_time_bars.append({'group': s, 'series': c, 'value': f'{dec_geo:.4f}', 'num': dec_geo, 'total_us': total_dec})
            dec_time_table.append([s, c, CODEC_THREAD_LABELS[c], total_dec, f'{dec_geo:.4f}'])

            enc_mib = f'{max_enc_pk / (1024 * 1024):.2f}'
            dec_mib = f'{max_dec_pk / (1024 * 1024):.2f}'
            peak_mem_bars.append({'group': f'{s}-encode', 'series': c, 'value': str(max_enc_pk), 'num': float(max_enc_pk), 'mib': enc_mib})
            peak_mem_bars.append({'group': f'{s}-decode', 'series': c, 'value': str(max_dec_pk), 'num': float(max_dec_pk), 'mib': dec_mib})
            peak_mem_table.append([s, c, str(max_enc_pk), enc_mib, str(max_dec_pk), dec_mib])

            svd_points.append({'group': s, 'series': c, 'x': f'{dec_geo:.4f}', 'y': f'{size_geo:.4f}', 'num_x': dec_geo, 'num_y': size_geo})
            svd_table.append([s, c, CODEC_THREAD_LABELS[c], f'{dec_geo:.4f}', f'{size_geo:.4f}'])

    # 6. scatter-e9
    scatter_e9_points: list[dict] = []
    scatter_e9_table: list[list[str]] = []
    for r in corpus_rows:
        ratio = r['rasterfold'] / r['jxl_e9']
        scatter_e9_points.append(
            {
                'set': r['set'],
                'image': r['image'],
                'x': str(r['raw']),
                'y': f'{ratio:.4f}',
                'num_x': float(r['raw']),
                'num_y': ratio,
                'rf': str(r['rasterfold']),
                'e9': str(r['jxl_e9']),
            }
        )
        scatter_e9_table.append(
            [
                r['set'],
                r['image'],
                f'{r["width"]}x{r["height"]}',
                str(r['raw']),
                str(r['rasterfold']),
                str(r['jxl_e7']),
                str(r['jxl_e9']),
                str(r['webp']),
                f'{ratio:.4f}',
            ]
        )

    return {
        'corpus-size-ratio': {
            'name': 'corpus-size-ratio',
            'kind': 'bar',
            'title': 'Research Corpus: Total Compressed Size Ratio vs JPEG XL e7 (Lower Is Smaller)',
            'y_label': 'Total compressed bytes / JPEG XL e7 bytes',
            'y_max': 1.25,
            'groups': ('photo', 'tp', 'tp-only', 'synth', 'all'),
            'series': CORPUS_CODECS,
            'bars': corpus_bars,
            'provenance': corpus_prov,
            'table_headers': [
                'Set',
                'Images',
                'RasterFold (bytes)',
                'JPEG XL e7 (bytes)',
                'JPEG XL e9 (bytes)',
                'Lossless WebP (bytes)',
                'RF / e7 (total)',
                'RF / e7 (geomean)',
                'e9 / e7 (total)',
                'WebP / e7 (total)',
            ],
            'table_rows': corpus_table_rows,
        },
        'benchmark-size-ratio': {
            'name': 'benchmark-size-ratio',
            'kind': 'bar',
            'title': 'Benchmark Suite: Geometric-Mean Size Ratio vs JPEG XL e7 (Lower Is Smaller)',
            'y_label': 'Geomean per-image bytes / JPEG XL e7 bytes',
            'y_max': 2.50,
            'groups': BENCHMARK_SETS,
            'series': BENCHMARK_CODECS,
            'bars': bench_size_bars,
            'provenance': results_prov,
            'table_headers': [
                'Set',
                'Codec',
                'Images',
                'Total Bytes',
                'Geomean Size Ratio vs JXL e7',
            ],
            'table_rows': bench_size_table,
        },
        'encode-time': {
            'name': 'encode-time',
            'kind': 'bar',
            'title': 'Benchmark Suite: Geometric-Mean Encode Time Ratio vs JPEG XL e7 (Lower Is Faster)',
            'y_label': 'Geomean encode wall time / JPEG XL e7 (capped at 12x)',
            'y_max': 12.0,
            'groups': BENCHMARK_SETS,
            'series': BENCHMARK_CODECS,
            'bars': enc_time_bars,
            'provenance': results_prov,
            'table_headers': [
                'Set',
                'Codec',
                'Threads',
                'Total Encode Time (us)',
                'Geomean Encode Time Ratio vs JXL e7',
            ],
            'table_rows': enc_time_table,
        },
        'decode-time': {
            'name': 'decode-time',
            'kind': 'bar',
            'title': 'Benchmark Suite: Geometric-Mean Decode Time Ratio vs JPEG XL e7 (Lower Is Faster)',
            'y_label': 'Geomean decode wall time / JPEG XL e7',
            'y_max': 2.20,
            'groups': BENCHMARK_SETS,
            'series': BENCHMARK_CODECS,
            'bars': dec_time_bars,
            'provenance': results_prov,
            'table_headers': [
                'Set',
                'Codec',
                'Threads',
                'Total Decode Time (us)',
                'Geomean Decode Time Ratio vs JXL e7',
            ],
            'table_rows': dec_time_table,
        },
        'peak-memory': {
            'name': 'peak-memory',
            'kind': 'bar',
            'title': 'Benchmark Suite: Maximum Process Peak Working Set per Set (Lower Is Leaner)',
            'y_label': 'Peak working set (bytes, max scale 120 MiB)',
            'y_max': 125829120.0,
            'groups': (
                'measure-encode',
                'measure-decode',
                'photos-encode',
                'photos-decode',
                'synthetic-encode',
                'synthetic-decode',
            ),
            'series': BENCHMARK_CODECS,
            'bars': peak_mem_bars,
            'provenance': results_prov,
            'table_headers': [
                'Set',
                'Codec',
                'Max Encode Peak (bytes)',
                'Max Encode Peak (MiB)',
                'Max Decode Peak (bytes)',
                'Max Decode Peak (MiB)',
            ],
            'table_rows': peak_mem_table,
        },
        'scatter-e9': {
            'name': 'scatter-e9',
            'kind': 'scatter-corpus',
            'title': 'Research Corpus: Per-Image Size Ratio (RasterFold / JPEG XL e9) vs Raw RGB Bytes',
            'x_label': 'Raw RGB image size (bytes)',
            'y_label': 'RasterFold / JPEG XL e9 size ratio (capped at 3.0x; syn_checker=27.1857, syn_gradient=7.3393, syn_pixel=11.3019)',
            'x_min': 0.0,
            'x_max': 5000000.0,
            'y_min': 0.70,
            'y_max': 3.00,
            'points': scatter_e9_points,
            'provenance': corpus_prov,
            'table_headers': [
                'Set',
                'Image',
                'Dimensions',
                'Raw Bytes',
                'RasterFold (bytes)',
                'JPEG XL e7 (bytes)',
                'JPEG XL e9 (bytes)',
                'Lossless WebP (bytes)',
                'RF / e9 Ratio',
            ],
            'table_rows': scatter_e9_table,
        },
        'size-vs-decode': {
            'name': 'size-vs-decode',
            'kind': 'scatter-sets',
            'title': 'Benchmark Suite: Geometric-Mean Size Ratio vs Decode Time Ratio (Relative to JPEG XL e7)',
            'x_label': 'Geomean decode wall-time ratio vs JPEG XL e7 (lower is faster)',
            'y_label': 'Geomean compressed size ratio vs JPEG XL e7 (lower is smaller)',
            'x_min': 0.0,
            'x_max': 2.20,
            'y_min': 0.20,
            'y_max': 2.50,
            'groups': BENCHMARK_SETS,
            'series': BENCHMARK_CODECS,
            'points': svd_points,
            'provenance': results_prov,
            'table_headers': [
                'Set',
                'Codec',
                'Threads',
                'Geomean Decode Time Ratio vs JXL e7',
                'Geomean Size Ratio vs JXL e7',
            ],
            'table_rows': svd_table,
        },
    }


def _svg_prov_attrs(prov: dict[str, str]) -> str:
    attrs = [
        f'data-source="{xml_escape(prov["source"], quote=True)}"',
        f'data-codec-revision="{xml_escape(prov["codec_revision"], quote=True)}"',
        f'data-kat-fold-sha256="{xml_escape(prov["kat_fold_sha256"], quote=True)}"',
        f'data-date="{xml_escape(prov["date"], quote=True)}"',
        f'data-threads="{xml_escape(prov["threads"], quote=True)}"',
        f'data-limitations="{xml_escape(prov["limitations"], quote=True)}"',
    ]
    for extra in ('measured_sets', 'retained_sets', 'retained_revision', 'retained_date', 'set_revisions', 'set_dates'):
        if extra in prov:
            attrs.append(f'data-{extra.replace("_", "-")}="{xml_escape(prov[extra], quote=True)}"')
    return ' '.join(attrs)


def _format_tick(val: float) -> str:
    if val >= 10000.0:
        return f'{int(round(val))}'
    return f'{val:.2f}'


def render_bar_svg(spec: dict) -> str:
    name = spec['name']
    prov = spec['provenance']
    width, height = 940, 460
    plot_left, plot_top, plot_width, plot_height = 80.0, 74.0, 830.0, 270.0
    plot_bottom = plot_top + plot_height
    y_max = float(spec['y_max'])
    groups = spec['groups']
    series = spec['series']
    bar_map = {(b['group'], b['series']): b for b in spec['bars']}

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" '
        f'aria-labelledby="title-{name} desc-{name}" data-chart="{name}" data-kind="bar-chart" '
        f'data-plot-left="{plot_left:.2f}" data-plot-top="{plot_top:.2f}" '
        f'data-plot-width="{plot_width:.2f}" data-plot-height="{plot_height:.2f}" '
        f'data-plot-bottom="{plot_bottom:.2f}" data-y-max="{y_max:.4f}" {_svg_prov_attrs(prov)}>',
        f'  <title id="title-{name}">{xml_escape(spec["title"])}</title>',
        f'  <desc id="desc-{name}">Source: {xml_escape(prov["source"])} | revision: {xml_escape(prov["codec_revision"])} | date: {xml_escape(prov["date"])} | threads: {xml_escape(prov["threads"])} | limitations: {xml_escape(prov["limitations"])}</desc>',
        f'  <rect x="0" y="0" width="{width}" height="{height}" fill="#ffffff" stroke="#e2e8f0" stroke-width="1"/>',
        f'  <text x="16" y="26" font-family="system-ui, sans-serif" font-size="14" font-weight="700" fill="#0f172a">{xml_escape(spec["title"])}</text>',
        f'  <text x="16" y="44" font-family="system-ui, sans-serif" font-size="11" fill="#475569">{xml_escape(spec["y_label"])}</text>',
    ]

    # Legend
    lx = 16
    for s in series:
        color = CODEC_COLORS.get(s, '#334155')
        lines.append(f'  <rect x="{lx}" y="52" width="10" height="10" fill="{color}"/>')
        lines.append(
            f'  <text data-kind="legend-item" data-series="{xml_escape(s, quote=True)}" x="{lx + 14}" y="61" '
            f'font-family="system-ui, sans-serif" font-size="10" fill="#1e293b">{xml_escape(s)}</text>'
        )
        lx += 118

    # Plot frame, Y-axis scale ticks & baseline
    for step in range(5):
        frac = step / 4.0
        yv = y_max * frac
        yy = round(plot_bottom - plot_height * frac, 2)
        tlabel = _format_tick(yv)
        lines.append(
            f'  <line x1="{plot_left - 4:.2f}" y1="{yy:.2f}" x2="{plot_left + plot_width:.2f}" y2="{yy:.2f}" '
            f'stroke="#e2e8f0" stroke-width="1"/>'
        )
        lines.append(
            f'  <text data-kind="y-tick" data-value="{xml_escape(tlabel, quote=True)}" x="{plot_left - 6:.2f}" y="{yy + 3:.2f}" '
            f'text-anchor="end" font-family="monospace" font-size="8.5" fill="#475569">{xml_escape(tlabel)}</text>'
        )
    lines.append(f'  <line x1="{plot_left:.2f}" y1="{plot_top:.2f}" x2="{plot_left:.2f}" y2="{plot_bottom:.2f}" stroke="#94a3b8" stroke-width="1"/>')
    lines.append(f'  <line x1="{plot_left:.2f}" y1="{plot_bottom:.2f}" x2="{plot_left + plot_width:.2f}" y2="{plot_bottom:.2f}" stroke="#94a3b8" stroke-width="1"/>')

    group_span = plot_width / len(groups)
    inner_span = group_span * 0.84
    bar_w = round(inner_span / len(series), 2)

    for g_idx, g in enumerate(groups):
        gx_start = plot_left + g_idx * group_span + (group_span - inner_span) / 2.0
        gx_center = plot_left + (g_idx + 0.5) * group_span
        lines.append(
            f'  <text x="{gx_center:.2f}" y="{plot_bottom + 18:.2f}" text-anchor="middle" '
            f'font-family="system-ui, sans-serif" font-size="11" font-weight="600" fill="#1e293b">{xml_escape(g)}</text>'
        )
        for s_idx, s in enumerate(series):
            b = bar_map[(g, s)]
            bx = round(gx_start + s_idx * bar_w, 2)
            val_str = b['value']
            if b['num'] is None or val_str == 'N/A':
                na_x = round(bx + bar_w / 2.0, 2)
                na_y = round(plot_bottom - 6.0, 2)
                lines.append(
                    f'  <g data-kind="na" data-group="{xml_escape(g, quote=True)}" data-series="{xml_escape(s, quote=True)}" '
                    f'data-value="N/A" data-x="{na_x:.2f}" data-y="{na_y:.2f}">'
                    f'<text x="{na_x:.2f}" y="{na_y:.2f}" text-anchor="middle" font-family="monospace" font-size="8" fill="#64748b">N/A</text>'
                    f'</g>'
                )
            else:
                num = float(b['num'])
                clamped = min(max(num, 0.0), y_max)
                bh = round(plot_height * (clamped / y_max), 2)
                by = round(plot_bottom - bh, 2)
                color = CODEC_COLORS.get(s, '#2563eb')
                lines.append(
                    f'  <rect data-kind="bar" data-group="{xml_escape(g, quote=True)}" data-series="{xml_escape(s, quote=True)}" '
                    f'data-value="{xml_escape(val_str, quote=True)}" x="{bx:.2f}" y="{by:.2f}" width="{max(bar_w - 2.0, 2.0):.2f}" height="{bh:.2f}" fill="{color}">'
                    f'<title>{xml_escape(f"{g} / {s}: {val_str}")}</title></rect>'
                )
                lines.append(
                    f'  <text data-kind="bar-label" data-group="{xml_escape(g, quote=True)}" data-series="{xml_escape(s, quote=True)}" '
                    f'x="{bx + max(bar_w - 2.0, 2.0) / 2:.2f}" y="{max(by - 3.0, plot_top - 4.0):.2f}" text-anchor="middle" '
                    f'font-family="monospace" font-size="7" fill="#0f172a">{xml_escape(val_str)}</text>'
                )

    lines.append(
        f'  <text data-kind="provenance" x="16" y="{height - 46}" font-family="monospace" font-size="9.5" fill="#334155">'
        f'Source: {xml_escape(prov["source"])} | codec_revision={xml_escape(prov["codec_revision"])} | date={xml_escape(prov["date"])}</text>'
    )
    lines.append(
        f'  <text data-kind="threads" x="16" y="{height - 30}" font-family="monospace" font-size="9.5" fill="#334155">'
        f'Threads: {xml_escape(prov["threads"])}</text>'
    )
    lines.append(
        f'  <text data-kind="limitations" x="16" y="{height - 14}" font-family="monospace" font-size="9.5" fill="#475569">'
        f'Limitations: {xml_escape(prov["limitations"])}</text>'
    )
    lines.append('</svg>\n')
    return '\n'.join(lines)


def render_scatter_svg(spec: dict) -> str:
    name = spec['name']
    prov = spec['provenance']
    width, height = 940, 460
    plot_left, plot_top, plot_width, plot_height = 80.0, 74.0, 830.0, 270.0
    plot_bottom = plot_top + plot_height
    x_min, x_max = float(spec['x_min']), float(spec['x_max'])
    y_min, y_max = float(spec['y_min']), float(spec['y_max'])

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" '
        f'aria-labelledby="title-{name} desc-{name}" data-chart="{name}" data-kind="{spec["kind"]}" '
        f'data-plot-left="{plot_left:.2f}" data-plot-top="{plot_top:.2f}" '
        f'data-plot-width="{plot_width:.2f}" data-plot-height="{plot_height:.2f}" '
        f'data-plot-bottom="{plot_bottom:.2f}" data-x-min="{x_min:.4f}" data-x-max="{x_max:.4f}" '
        f'data-y-min="{y_min:.4f}" data-y-max="{y_max:.4f}" {_svg_prov_attrs(prov)}>',
        f'  <title id="title-{name}">{xml_escape(spec["title"])}</title>',
        f'  <desc id="desc-{name}">Source: {xml_escape(prov["source"])} | revision: {xml_escape(prov["codec_revision"])} | date: {xml_escape(prov["date"])} | threads: {xml_escape(prov["threads"])} | limitations: {xml_escape(prov["limitations"])}</desc>',
        f'  <rect x="0" y="0" width="{width}" height="{height}" fill="#ffffff" stroke="#e2e8f0" stroke-width="1"/>',
        f'  <text x="16" y="22" font-family="system-ui, sans-serif" font-size="14" font-weight="700" fill="#0f172a">{xml_escape(spec["title"])}</text>',
        f'  <text x="16" y="38" font-family="system-ui, sans-serif" font-size="11" fill="#475569">X: {xml_escape(spec["x_label"])} | Y: {xml_escape(spec["y_label"])}</text>',
    ]

    # Legend for series/set identification
    lx = 16
    if spec['kind'] == 'scatter-corpus':
        for s_name in ('photo', 'tp', 'synth'):
            color = SET_COLORS[s_name]
            lines.append(f'  <circle cx="{lx + 5}" cy="49" r="4.0" fill="{color}"/>')
            lines.append(
                f'  <text data-kind="legend-item" data-series="{s_name}" x="{lx + 13}" y="53" '
                f'font-family="system-ui, sans-serif" font-size="10" fill="#1e293b">{s_name}</text>'
            )
            lx += 86
    else:
        for s_name in spec['series']:
            color = CODEC_COLORS.get(s_name, '#334155')
            lines.append(f'  <circle cx="{lx + 5}" cy="49" r="4.0" fill="{color}" stroke="#0f172a" stroke-width="1"/>')
            lines.append(
                f'  <text data-kind="legend-item" data-series="{xml_escape(s_name, quote=True)}" x="{lx + 13}" y="53" '
                f'font-family="system-ui, sans-serif" font-size="9.5" fill="#1e293b">{xml_escape(s_name)}</text>'
            )
            lx += 92
        lines.append(
            f'  <text data-kind="legend-item" data-series="sets" x="16" y="65" '
            f'font-family="monospace" font-size="9" fill="#334155">Sets: M=measure (solid) | P=photos (dashed) | S=synthetic (dotted)</text>'
        )

    # Numeric X-axis and Y-axis ticks & grid lines
    for step in range(5):
        frac = step / 4.0
        xv = x_min + (x_max - x_min) * frac
        xx = round(plot_left + plot_width * frac, 2)
        xlabel = _format_tick(xv)
        lines.append(
            f'  <line x1="{xx:.2f}" y1="{plot_top:.2f}" x2="{xx:.2f}" y2="{plot_bottom + 4:.2f}" stroke="#e2e8f0" stroke-width="1"/>'
        )
        lines.append(
            f'  <text data-kind="x-tick" data-value="{xml_escape(xlabel, quote=True)}" x="{xx:.2f}" y="{plot_bottom + 15:.2f}" '
            f'text-anchor="middle" font-family="monospace" font-size="8.5" fill="#475569">{xml_escape(xlabel)}</text>'
        )
        yv = y_min + (y_max - y_min) * frac
        yy = round(plot_bottom - plot_height * frac, 2)
        ylabel = _format_tick(yv)
        lines.append(
            f'  <line x1="{plot_left - 4:.2f}" y1="{yy:.2f}" x2="{plot_left + plot_width:.2f}" y2="{yy:.2f}" stroke="#e2e8f0" stroke-width="1"/>'
        )
        lines.append(
            f'  <text data-kind="y-tick" data-value="{xml_escape(ylabel, quote=True)}" x="{plot_left - 6:.2f}" y="{yy + 3:.2f}" '
            f'text-anchor="end" font-family="monospace" font-size="8.5" fill="#475569">{xml_escape(ylabel)}</text>'
        )

    lines.append(f'  <line x1="{plot_left:.2f}" y1="{plot_top:.2f}" x2="{plot_left:.2f}" y2="{plot_bottom:.2f}" stroke="#94a3b8" stroke-width="1"/>')
    lines.append(f'  <line x1="{plot_left:.2f}" y1="{plot_bottom:.2f}" x2="{plot_left + plot_width:.2f}" y2="{plot_bottom:.2f}" stroke="#94a3b8" stroke-width="1"/>')

    # Reference line at y = 1.0
    if y_min <= 1.0 <= y_max:
        ref_y = round(plot_bottom - plot_height * ((1.0 - y_min) / (y_max - y_min)), 2)
        lines.append(
            f'  <line x1="{plot_left:.2f}" y1="{ref_y:.2f}" x2="{plot_left + plot_width:.2f}" y2="{ref_y:.2f}" '
            f'stroke="#94a3b8" stroke-dasharray="4 4" stroke-width="1"/>'
        )

    set_prefixes = {'measure': 'M', 'photos': 'P', 'synthetic': 'S'}
    set_strokes = {
        'measure': 'stroke="#0f172a" stroke-width="1.5"',
        'photos': 'stroke="#0f172a" stroke-width="1.5" stroke-dasharray="3 2"',
        'synthetic': 'stroke="#0f172a" stroke-width="1.5" stroke-dasharray="1 2"',
    }
    set_y_offsets = {'measure': -7.0, 'photos': 1.0, 'synthetic': 9.0}

    na_idx = 0
    for pt in spec['points']:
        if pt.get('num_x') is None or pt.get('num_y') is None:
            g = pt['group']
            s = pt['series']
            na_x = round(plot_left + 12.0, 2)
            na_y = round(plot_top + 16.0 + na_idx * 14.0, 2)
            na_idx += 1
            lines.append(
                f'  <g data-kind="na" data-group="{xml_escape(g, quote=True)}" data-series="{xml_escape(s, quote=True)}" '
                f'data-value="N/A" data-x="{na_x:.2f}" data-y="{na_y:.2f}">'
                f'<text x="{na_x:.2f}" y="{na_y:.2f}" font-family="monospace" font-size="8.5" fill="#64748b">{xml_escape(g)}/{xml_escape(s)}: N/A</text>'
                f'</g>'
            )
            continue
        nx = min(max(float(pt['num_x']), x_min), x_max)
        ny = min(max(float(pt['num_y']), y_min), y_max)
        cx = round(plot_left + plot_width * ((nx - x_min) / (x_max - x_min)), 2)
        cy = round(plot_bottom - plot_height * ((ny - y_min) / (y_max - y_min)), 2)
        if spec['kind'] == 'scatter-corpus':
            color = SET_COLORS.get(pt['set'], '#2563eb')
            tip = f'{pt["set"]}/{pt["image"]}: raw={pt["x"]}, rf/e9={pt["y"]}'
            lines.append(
                f'  <circle data-kind="point" data-set="{xml_escape(pt["set"], quote=True)}" '
                f'data-image="{xml_escape(pt["image"], quote=True)}" data-x="{xml_escape(pt["x"], quote=True)}" '
                f'data-y="{xml_escape(pt["y"], quote=True)}" data-rf="{xml_escape(pt["rf"], quote=True)}" '
                f'data-e9="{xml_escape(pt["e9"], quote=True)}" cx="{cx:.2f}" cy="{cy:.2f}" r="4.0" fill="{color}">'
                f'<title>{xml_escape(tip)}</title></circle>'
            )
        else:
            color = CODEC_COLORS.get(pt['series'], '#2563eb')
            g = pt['group']
            s = pt['series']
            prefix = set_prefixes.get(g, g[:1].upper())
            stroke_attr = set_strokes.get(g, 'stroke="#0f172a" stroke-width="1"')
            y_off = set_y_offsets.get(g, 0.0)
            tip = f'{g}/{s}: decode={pt["x"]}, size={pt["y"]}'
            label_txt = f'{prefix}:{s}'
            lines.append(
                f'  <circle data-kind="point" data-group="{xml_escape(g, quote=True)}" '
                f'data-series="{xml_escape(s, quote=True)}" data-x="{xml_escape(pt["x"], quote=True)}" '
                f'data-y="{xml_escape(pt["y"], quote=True)}" cx="{cx:.2f}" cy="{cy:.2f}" r="5.0" fill="{color}" {stroke_attr}>'
                f'<title>{xml_escape(tip)}</title></circle>'
            )
            lines.append(
                f'  <text data-kind="point-label" data-group="{xml_escape(g, quote=True)}" data-series="{xml_escape(s, quote=True)}" '
                f'x="{cx + 7.0:.2f}" y="{cy + y_off:.2f}" font-family="monospace" font-size="7.5" fill="#1e293b">{xml_escape(label_txt)}</text>'
            )

    lines.append(
        f'  <text data-kind="provenance" x="16" y="{height - 46}" font-family="monospace" font-size="9.5" fill="#334155">'
        f'Source: {xml_escape(prov["source"])} | codec_revision={xml_escape(prov["codec_revision"])} | date={xml_escape(prov["date"])}</text>'
    )
    lines.append(
        f'  <text data-kind="threads" x="16" y="{height - 30}" font-family="monospace" font-size="9.5" fill="#334155">'
        f'Threads: {xml_escape(prov["threads"])}</text>'
    )
    lines.append(
        f'  <text data-kind="limitations" x="16" y="{height - 14}" font-family="monospace" font-size="9.5" fill="#475569">'
        f'Limitations: {xml_escape(prov["limitations"])}</text>'
    )
    lines.append('</svg>\n')
    return '\n'.join(lines)


def render_markdown_table(spec: dict) -> str:
    name = spec['name']
    prov = spec['provenance']
    headers = spec['table_headers']
    rows = spec['table_rows']
    out = [
        f'<!-- GENERATED by scripts/build-docs.py for {name} from {prov["source"]} '
        f'(codec_revision={prov["codec_revision"]}, date={prov["date"]}); do not edit by hand. -->',
        '',
        f'*Source:* `{prov["source"]}` (`codec_revision={prov["codec_revision"]}`, `date={prov["date"]}`) | '
        f'*Threads:* {prov["threads"]} | *Limitations:* {prov["limitations"]}',
        '',
        '| ' + ' | '.join(headers) + ' |',
        '| ' + ' | '.join('---' for _ in headers) + ' |',
    ]
    for r in rows:
        out.append('| ' + ' | '.join(r) + ' |')
    out.append('')
    return '\n'.join(out)


def render_generated_charts_and_tables(root: Path) -> dict[str, str]:
    datasets = compute_chart_datasets(root)
    outputs: dict[str, str] = {}
    for name in CHART_NAMES:
        spec = datasets[name]
        chart_rel = f'docs/_includes/generated/charts/{name}.svg'
        table_rel = f'docs/_includes/generated/tables/{name}.md'
        if spec['kind'] == 'bar':
            outputs[chart_rel] = render_bar_svg(spec)
        else:
            outputs[chart_rel] = render_scatter_svg(spec)
        outputs[table_rel] = render_markdown_table(spec)
    return outputs


def _parse_pipe_table(md_text: str) -> tuple[list[str], list[list[str]]]:
    table_lines = [ln.strip() for ln in md_text.splitlines() if ln.strip().startswith('|') and ln.strip().endswith('|')]
    if len(table_lines) < 3:
        raise ValueError('Markdown table has fewer than 3 pipe lines')
    headers = [c.strip() for c in table_lines[0].strip('|').split('|')]
    rows = [[c.strip() for c in ln.strip('|').split('|')] for ln in table_lines[2:]]
    return headers, rows


def _independent_csv_reference(root: Path) -> dict[str, dict]:
    """Independently parse CSV files via csv.reader and compute reference expectations without compute_chart_datasets."""
    def _raw_csv_read(rel: str) -> tuple[dict[str, str], list[dict[str, str]]]:
        p = safe_repo_path(root, rel)
        lines = p.read_text(encoding='utf-8').splitlines()
        meta: dict[str, str] = {}
        data_lines: list[str] = []
        for ln in lines:
            if not data_lines and ln.lstrip().startswith('#'):
                for tok in ln.lstrip()[1:].strip().split():
                    if '=' in tok:
                        k, v = tok.split('=', 1)
                        meta[k.strip()] = v.strip()
            elif ln.strip():
                data_lines.append(ln)
        for rk, pat in (
            ('codec_revision', r'^[0-9a-f]{40}$'),
            ('kat_fold_sha256', r'^[0-9a-f]{64}$'),
            ('date', r'^\d{4}-\d{2}-\d{2}$'),
        ):
            val = meta.get(rk, '')
            if not re.fullmatch(pat, val):
                raise ValueError(f'{rel}: missing or invalid header {rk}={val!r}')
        if rel == RESULTS_CSV:
            for rk in ('measured_sets', 'retained_sets', 'retained_revision', 'retained_date', 'set_revisions', 'set_dates'):
                if not meta.get(rk):
                    raise ValueError(f'{rel}: missing required header {rk}')
            if meta['retained_sets'] != 'none':
                for tok in meta['retained_revision'].split(','):
                    rv = tok.split(':', 1)[1] if ':' in tok else tok
                    if not re.fullmatch(r'^[0-9a-f]{40}$', rv):
                        raise ValueError(f'{rel}: invalid retained_revision={meta["retained_revision"]!r}')
                for tok in meta['retained_date'].split(','):
                    dv = tok.split(':', 1)[1] if ':' in tok else tok
                    if not re.fullmatch(r'^\d{4}-\d{2}-\d{2}$', dv):
                        raise ValueError(f'{rel}: invalid retained_date={meta["retained_date"]!r}')
            rev_p = dict(item.split(':', 1) for item in meta['set_revisions'].split(',') if ':' in item)
            date_p = dict(item.split(':', 1) for item in meta['set_dates'].split(',') if ':' in item)
            for s in BENCHMARK_SETS:
                if not re.fullmatch(r'^[0-9a-f]{40}$', rev_p.get(s, '')):
                    raise ValueError(f'{rel}: invalid or missing set_revisions entry for {s}')
                if not re.fullmatch(r'^\d{4}-\d{2}-\d{2}$', date_p.get(s, '')):
                    raise ValueError(f'{rel}: invalid or missing set_dates entry for {s}')
        reader = list(csv.reader(data_lines))
        if len(reader) < 2:
            raise ValueError(f'{rel}: missing header or rows')
        hdr = [h.strip() for h in reader[0]]
        return meta, [{hdr[i]: cell.strip() for i, cell in enumerate(row)} for row in reader[1:]]

    def _ref_gm(vals: list[float]) -> float:
        return math.prod(vals) ** (1.0 / len(vals))

    from decimal import Decimal

    c_meta, c_rows = _raw_csv_read(CORPUS_CSV)
    r_meta, r_rows = _raw_csv_read(RESULTS_CSV)
    seen_c_imgs: set[str] = set()
    for r in c_rows:
        img = r.get('image', '')
        s_name = r.get('set', '')
        if not img or s_name not in ('photo', 'tp', 'synth') or img in seen_c_imgs:
            raise ValueError(f'{CORPUS_CSV}: invalid or duplicate row {r!r}')
        seen_c_imgs.add(img)
        if min(int(r['width']), int(r['height']), int(r['rasterfold']), int(r['jxl_e7']), int(r['jxl_e9']), int(r['webp'])) <= 0:
            raise ValueError(f'{CORPUS_CSV}: non-positive value in row {r!r}')
    set_dates = dict(item.split(':', 1) for item in r_meta['set_dates'].split(',') if ':' in item)

    c_prov = {
        'source': CORPUS_CSV,
        'codec_revision': c_meta['codec_revision'],
        'kat_fold_sha256': c_meta['kat_fold_sha256'],
        'date': c_meta['date'],
        'threads': 'rasterfold: default (up to 9T); jxl_e7, jxl_e9, webp: -threads 1',
        'limitations': 'Size-only research corpus (55 images: photo 39, tp 10 overlapping / tp-only 5 exclusive, synth 11); PNG, wall time, and memory are measured in benchmarks/results.csv.',
    }
    r_prov = {
        'source': RESULTS_CSV,
        'codec_revision': r_meta['codec_revision'],
        'kat_fold_sha256': r_meta['kat_fold_sha256'],
        'date': r_meta['date'],
        'measured_sets': r_meta['measured_sets'],
        'retained_sets': r_meta['retained_sets'],
        'retained_revision': r_meta['retained_revision'],
        'retained_date': r_meta['retained_date'],
        'set_revisions': r_meta['set_revisions'],
        'set_dates': r_meta['set_dates'],
        'threads': 'rasterfold: default (up to 9T); rasterfold-1t: 1T; jxl/jxl-e9: enc 1T, dec default; jxl-1t: enc 1T, dec 1T; webp/png: 1T',
        'limitations': (
            f'measured_sets={r_meta["measured_sets"]} ({r_meta["date"]}, rev {r_meta["codec_revision"][:12]}); '
            f'retained_sets={r_meta["retained_sets"]} ({r_meta["retained_date"]}, rev {r_meta["retained_revision"][:12]}, jxl-e9 not measured).'
        ),
    }

    c_groups = {
        'photo': [r for r in c_rows if r['set'] == 'photo'],
        'tp': [r for r in c_rows if r['set'] == 'tp' or r['image'] in TP_OVERLAP_PHOTO],
        'tp-only': [r for r in c_rows if r['set'] == 'tp'],
        'synth': [r for r in c_rows if r['set'] == 'synth'],
        'all': list(c_rows),
    }
    c_bars, c_tbl = [], []
    for g in ('photo', 'tp', 'tp-only', 'synth', 'all'):
        gr = c_groups[g]
        if not gr:
            raise ValueError(f'{CORPUS_CSV}: empty corpus group {g}')
        e7_tot = sum(int(r['jxl_e7']) for r in gr)
        tots = {c: sum(int(r[c]) for r in gr) for c in CORPUS_CODECS}
        rf_gm = _ref_gm([int(r['rasterfold']) / int(r['jxl_e7']) for r in gr])
        for c in CORPUS_CODECS:
            rat = tots[c] / e7_tot
            c_bars.append({'group': g, 'series': c, 'value': f'{rat:.4f}', 'num': rat})
        c_tbl.append([
            g, str(len(gr)), str(tots['rasterfold']), str(tots['jxl_e7']), str(tots['jxl_e9']), str(tots['webp']),
            f'{tots["rasterfold"] / e7_tot:.4f}', f'{rf_gm:.4f}', f'{tots["jxl_e9"] / e7_tot:.4f}', f'{tots["webp"] / e7_tot:.4f}',
        ])

    r_idx: dict[str, dict[str, dict[str, dict[str, str]]]] = {s: {} for s in BENCHMARK_SETS}
    seen_r_keys: set[tuple[str, str, str]] = set()
    for r in r_rows:
        s_name = r.get('set', '')
        img = r.get('image', '')
        codec = r.get('codec', '')
        r_key = (s_name, img, codec)
        if s_name not in BENCHMARK_SETS or codec not in BENCHMARK_CODECS or not img or r_key in seen_r_keys:
            raise ValueError(f'{RESULTS_CSV}: invalid or duplicate row {r!r}')
        seen_r_keys.add(r_key)
        if r.get('pixels_match') != 'True':
            raise ValueError(f'{RESULTS_CSV}: pixels_match is not True for {r_key}')
        if min(int(r['width']), int(r['height']), int(r['raw']), int(r['bytes']), float(r['encode_us']), float(r['decode_us']), int(r['encode_peak']), int(r['decode_peak'])) <= 0:
            raise ValueError(f'{RESULTS_CSV}: non-positive measurement in {r_key}')
        r_idx[s_name].setdefault(img, {})[codec] = r

    bs_bars, bs_tbl = [], []
    et_bars, et_tbl = [], []
    dt_bars, dt_tbl = [], []
    pm_bars, pm_tbl = [], []
    svd_pts, svd_tbl = [], []
    for s in BENCHMARK_SETS:
        imgs = sorted(r_idx[s].keys())
        if not imgs:
            raise ValueError(f'{RESULTS_CSV}: empty benchmark set {s}')
        for img in imgs:
            if 'jxl' not in r_idx[s][img]:
                raise ValueError(f'{RESULTS_CSV}: missing baseline jxl row for {s}/{img}')
        for c in BENCHMARK_CODECS:
            present = [i for i in imgs if c in r_idx[s][i]]
            if present and len(present) != len(imgs):
                raise ValueError(f'{RESULTS_CSV}: partial codec coverage for {s}/{c}')
            if not present:
                for tb in (bs_bars, et_bars, dt_bars):
                    tb.append({'group': s, 'series': c, 'value': 'N/A', 'num': None})
                pm_bars.append({'group': f'{s}-encode', 'series': c, 'value': 'N/A', 'num': None})
                pm_bars.append({'group': f'{s}-decode', 'series': c, 'value': 'N/A', 'num': None})
                svd_pts.append({'group': s, 'series': c, 'x': 'N/A', 'y': 'N/A', 'num_x': None, 'num_y': None})
                note = f'N/A (not measured in retained {set_dates[s]} set)'
                bs_tbl.append([s, c, str(len(imgs)), note, note])
                et_tbl.append([s, c, CODEC_THREAD_LABELS[c], note, note])
                dt_tbl.append([s, c, CODEC_THREAD_LABELS[c], note, note])
                pm_tbl.append([s, c, note, note, note, note])
                svd_tbl.append([s, c, CODEC_THREAD_LABELS[c], note, note])
                continue
            tb_val = sum(int(r_idx[s][i][c]['bytes']) for i in imgs)
            sg_val = _ref_gm([int(r_idx[s][i][c]['bytes']) / int(r_idx[s][i]['jxl']['bytes']) for i in imgs])
            te_dec = sum((Decimal(r_idx[s][i][c]['encode_us']) for i in imgs), Decimal(0))
            te_str = str(int(te_dec)) if te_dec == int(te_dec) else format(te_dec.normalize(), 'f')
            eg_val = _ref_gm([float(r_idx[s][i][c]['encode_us']) / float(r_idx[s][i]['jxl']['encode_us']) for i in imgs])
            td_dec = sum((Decimal(r_idx[s][i][c]['decode_us']) for i in imgs), Decimal(0))
            td_str = str(int(td_dec)) if td_dec == int(td_dec) else format(td_dec.normalize(), 'f')
            dg_val = _ref_gm([float(r_idx[s][i][c]['decode_us']) / float(r_idx[s][i]['jxl']['decode_us']) for i in imgs])
            ep_val = max(int(r_idx[s][i][c]['encode_peak']) for i in imgs)
            dp_val = max(int(r_idx[s][i][c]['decode_peak']) for i in imgs)
            bs_bars.append({'group': s, 'series': c, 'value': f'{sg_val:.4f}', 'num': sg_val})
            bs_tbl.append([s, c, str(len(imgs)), str(tb_val), f'{sg_val:.4f}'])
            et_bars.append({'group': s, 'series': c, 'value': f'{eg_val:.4f}', 'num': eg_val})
            et_tbl.append([s, c, CODEC_THREAD_LABELS[c], te_str, f'{eg_val:.4f}'])
            dt_bars.append({'group': s, 'series': c, 'value': f'{dg_val:.4f}', 'num': dg_val})
            dt_tbl.append([s, c, CODEC_THREAD_LABELS[c], td_str, f'{dg_val:.4f}'])
            emib, dmib = f'{ep_val / 1048576:.2f}', f'{dp_val / 1048576:.2f}'
            pm_bars.append({'group': f'{s}-encode', 'series': c, 'value': str(ep_val), 'num': float(ep_val)})
            pm_bars.append({'group': f'{s}-decode', 'series': c, 'value': str(dp_val), 'num': float(dp_val)})
            pm_tbl.append([s, c, str(ep_val), emib, str(dp_val), dmib])
            svd_pts.append({'group': s, 'series': c, 'x': f'{dg_val:.4f}', 'y': f'{sg_val:.4f}', 'num_x': dg_val, 'num_y': sg_val})
            svd_tbl.append([s, c, CODEC_THREAD_LABELS[c], f'{dg_val:.4f}', f'{sg_val:.4f}'])

    sc_pts, sc_tbl = [], []
    for r in c_rows:
        raw_sz = int(r['width']) * int(r['height']) * 3
        rat = int(r['rasterfold']) / int(r['jxl_e9'])
        sc_pts.append({'set': r['set'], 'image': r['image'], 'x': str(raw_sz), 'y': f'{rat:.4f}', 'num_x': float(raw_sz), 'num_y': rat})
        sc_tbl.append([r['set'], r['image'], f'{r["width"]}x{r["height"]}', str(raw_sz), r['rasterfold'], r['jxl_e7'], r['jxl_e9'], r['webp'], f'{rat:.4f}'])

    return {
        'corpus-size-ratio': {'kind': 'bar', 'bars': c_bars, 'provenance': c_prov, 'table_rows': c_tbl,
                              'table_headers': ['Set', 'Images', 'RasterFold (bytes)', 'JPEG XL e7 (bytes)', 'JPEG XL e9 (bytes)', 'Lossless WebP (bytes)', 'RF / e7 (total)', 'RF / e7 (geomean)', 'e9 / e7 (total)', 'WebP / e7 (total)']},
        'benchmark-size-ratio': {'kind': 'bar', 'bars': bs_bars, 'provenance': r_prov, 'table_rows': bs_tbl,
                                 'table_headers': ['Set', 'Codec', 'Images', 'Total Bytes', 'Geomean Size Ratio vs JXL e7']},
        'encode-time': {'kind': 'bar', 'bars': et_bars, 'provenance': r_prov, 'table_rows': et_tbl,
                        'table_headers': ['Set', 'Codec', 'Threads', 'Total Encode Time (us)', 'Geomean Encode Time Ratio vs JXL e7']},
        'decode-time': {'kind': 'bar', 'bars': dt_bars, 'provenance': r_prov, 'table_rows': dt_tbl,
                        'table_headers': ['Set', 'Codec', 'Threads', 'Total Decode Time (us)', 'Geomean Decode Time Ratio vs JXL e7']},
        'peak-memory': {'kind': 'bar', 'bars': pm_bars, 'provenance': r_prov, 'table_rows': pm_tbl,
                        'table_headers': ['Set', 'Codec', 'Max Encode Peak (bytes)', 'Max Encode Peak (MiB)', 'Max Decode Peak (bytes)', 'Max Decode Peak (MiB)']},
        'scatter-e9': {'kind': 'scatter-corpus', 'points': sc_pts, 'provenance': c_prov, 'table_rows': sc_tbl,
                       'table_headers': ['Set', 'Image', 'Dimensions', 'Raw Bytes', 'RasterFold (bytes)', 'JPEG XL e7 (bytes)', 'JPEG XL e9 (bytes)', 'Lossless WebP (bytes)', 'RF / e9 Ratio']},
        'size-vs-decode': {'kind': 'scatter-sets', 'points': svd_pts, 'provenance': r_prov, 'table_rows': svd_tbl,
                           'table_headers': ['Set', 'Codec', 'Threads', 'Geomean Decode Time Ratio vs JXL e7', 'Geomean Size Ratio vs JXL e7']},
    }


def self_test_charts(root: Path) -> list[str]:
    errors: list[str] = []
    try:
        datasets = _independent_csv_reference(root)
    except (OSError, ValueError, KeyError) as exc:
        return [f'dataset calculation failed: {exc}']

    benchmarks_path = safe_repo_path(root, 'docs/benchmarks.md')
    benchmarks_text = benchmarks_path.read_text(encoding='utf-8') if benchmarks_path.is_file() else ''

    for name in CHART_NAMES:
        spec = datasets[name]
        prov = spec['provenance']
        chart_rel = f'docs/_includes/generated/charts/{name}.svg'
        table_rel = f'docs/_includes/generated/tables/{name}.md'
        chart_inc = f'{{% include generated/charts/{name}.svg %}}'
        table_inc = f'{{% include generated/tables/{name}.md %}}'
        if chart_inc not in benchmarks_text:
            errors.append(f'docs/benchmarks.md: missing include {chart_inc}')
        if table_inc not in benchmarks_text:
            errors.append(f'docs/benchmarks.md: missing include {table_inc}')

        chart_path = safe_repo_path(root, chart_rel)
        if not chart_path.is_file():
            errors.append(f'{chart_rel}: missing SVG chart file')
        else:
            try:
                svg_root = ET.fromstring(chart_path.read_text(encoding='utf-8'))
            except ET.ParseError as exc:
                errors.append(f'{chart_rel}: invalid SVG XML: {exc}')
                svg_root = None

            if svg_root is not None:
                for attr, expected_val in (
                    ('data-chart', name),
                    ('data-source', prov['source']),
                    ('data-codec-revision', prov['codec_revision']),
                    ('data-kat-fold-sha256', prov['kat_fold_sha256']),
                    ('data-date', prov['date']),
                    ('data-threads', prov['threads']),
                    ('data-limitations', prov['limitations']),
                ):
                    if svg_root.attrib.get(attr) != expected_val:
                        errors.append(f'{chart_rel}: {attr}={svg_root.attrib.get(attr)!r} != {expected_val!r}')
                for extra in ('measured_sets', 'retained_sets', 'retained_revision', 'retained_date', 'set_revisions', 'set_dates'):
                    if extra in prov:
                        attr = f'data-{extra.replace("_", "-")}'
                        if svg_root.attrib.get(attr) != prov[extra]:
                            errors.append(f'{chart_rel}: {attr}={svg_root.attrib.get(attr)!r} != {prov[extra]!r}')

                all_text = ' '.join(''.join(el.itertext()) for el in svg_root.iter())
                for token in (prov['source'], prov['codec_revision'], prov['date'], prov['threads'], prov['limitations']):
                    if token not in all_text:
                        errors.append(f'{chart_rel}: visible SVG text missing provenance token {token!r}')

                legend_items = [el for el in svg_root.iter() if el.attrib.get('data-kind') == 'legend-item']
                y_ticks = [el for el in svg_root.iter() if el.attrib.get('data-kind') == 'y-tick']
                if not legend_items:
                    errors.append(f'{chart_rel}: missing legend items')
                for leg_el in legend_items:
                    lx_val = float(leg_el.attrib.get('x', '0'))
                    est_right = lx_val + len((leg_el.text or '').strip()) * 6.0
                    if est_right > 928.0:
                        errors.append(f'{chart_rel}: legend item {leg_el.attrib.get("data-series")!r} overflows viewport (est_right={est_right:.1f})')
                if len(y_ticks) < 2:
                    errors.append(f'{chart_rel}: missing Y-axis numeric ticks')

                na_coords: set[tuple[str, str]] = set()
                for el in svg_root.iter():
                    if el.attrib.get('data-kind') == 'na':
                        coord = (el.attrib.get('data-x', ''), el.attrib.get('data-y', ''))
                        if coord in na_coords:
                            errors.append(f'{chart_rel}: overlapping N/A label coordinates {coord}')
                        na_coords.add(coord)

                plot_left = float(svg_root.attrib.get('data-plot-left', '0'))
                plot_width = float(svg_root.attrib.get('data-plot-width', '0'))
                plot_height = float(svg_root.attrib.get('data-plot-height', '0'))
                plot_bottom = float(svg_root.attrib.get('data-plot-bottom', '0'))

                if spec['kind'] == 'bar':
                    y_max = float(svg_root.attrib.get('data-y-max', '0'))
                    expected_bars = {(b['group'], b['series']): b for b in spec['bars']}
                    seen_keys: set[tuple[str, str]] = set()
                    labels: dict[tuple[str, str], str] = {}
                    for el in svg_root.iter():
                        kind = el.attrib.get('data-kind')
                        if kind == 'bar-label':
                            labels[(el.attrib.get('data-group', ''), el.attrib.get('data-series', ''))] = (el.text or '').strip()
                        elif kind in ('bar', 'na'):
                            key = (el.attrib.get('data-group', ''), el.attrib.get('data-series', ''))
                            if key in seen_keys:
                                errors.append(f'{chart_rel}: duplicate bar/na element for {key}')
                            seen_keys.add(key)
                            if key not in expected_bars:
                                errors.append(f'{chart_rel}: unexpected bar/na key {key}')
                                continue
                            exp = expected_bars[key]
                            got_val = el.attrib.get('data-value', '')
                            if got_val != exp['value']:
                                errors.append(f'{chart_rel}: {key} data-value={got_val!r} != {exp["value"]!r}')
                            if exp['num'] is None:
                                if kind != 'na':
                                    errors.append(f'{chart_rel}: {key} expected data-kind="na", got {kind!r}')
                            else:
                                if kind != 'bar':
                                    errors.append(f'{chart_rel}: {key} expected data-kind="bar", got {kind!r}')
                                else:
                                    clamped = min(max(float(exp['num']), 0.0), y_max)
                                    exp_h = round(plot_height * (clamped / y_max), 2)
                                    exp_y = round(plot_bottom - exp_h, 2)
                                    got_h = float(el.attrib.get('height', '-1'))
                                    got_y = float(el.attrib.get('y', '-1'))
                                    if abs(got_h - exp_h) > 0.05 or abs(got_y - exp_y) > 0.05:
                                        errors.append(
                                            f'{chart_rel}: {key} bar geometry height={got_h:.2f}/y={got_y:.2f} != expected {exp_h:.2f}/{exp_y:.2f}'
                                        )
                    for key, exp in expected_bars.items():
                        if key not in seen_keys:
                            errors.append(f'{chart_rel}: missing series element for {key}')
                        elif exp['num'] is not None and labels.get(key) != exp['value']:
                            errors.append(f'{chart_rel}: {key} bar-label={labels.get(key)!r} != {exp["value"]!r}')
                else:
                    x_ticks = [el for el in svg_root.iter() if el.attrib.get('data-kind') == 'x-tick']
                    if len(x_ticks) < 2:
                        errors.append(f'{chart_rel}: missing X-axis numeric ticks')
                    x_min = float(svg_root.attrib.get('data-x-min', '0'))
                    x_max = float(svg_root.attrib.get('data-x-max', '1'))
                    y_min = float(svg_root.attrib.get('data-y-min', '0'))
                    y_max = float(svg_root.attrib.get('data-y-max', '1'))
                    if spec['kind'] == 'scatter-corpus':
                        expected_pts = {(p['set'], p['image']): p for p in spec['points']}
                        key_attrs = ('data-set', 'data-image')
                    else:
                        expected_pts = {(p['group'], p['series']): p for p in spec['points']}
                        key_attrs = ('data-group', 'data-series')
                    seen_pts: set[tuple[str, str]] = set()
                    point_labels: set[tuple[str, str]] = set()
                    for el in svg_root.iter():
                        kind = el.attrib.get('data-kind')
                        if kind == 'point-label':
                            point_labels.add((el.attrib.get('data-group', ''), el.attrib.get('data-series', '')))
                        elif kind in ('point', 'na'):
                            key = (el.attrib.get(key_attrs[0], ''), el.attrib.get(key_attrs[1], ''))
                            if key in seen_pts:
                                errors.append(f'{chart_rel}: duplicate point/na element for {key}')
                            seen_pts.add(key)
                            if key not in expected_pts:
                                errors.append(f'{chart_rel}: unexpected point/na key {key}')
                                continue
                            exp = expected_pts[key]
                            if exp['num_x'] is None or exp['num_y'] is None:
                                if kind != 'na':
                                    errors.append(f'{chart_rel}: {key} expected data-kind="na", got {kind!r}')
                            else:
                                if kind != 'point':
                                    errors.append(f'{chart_rel}: {key} expected data-kind="point", got {kind!r}')
                                    continue
                                if not any(child.tag.endswith('title') and (child.text or '').strip() for child in el):
                                    errors.append(f'{chart_rel}: {key} point missing <title> tooltip')
                                if el.attrib.get('data-x') != exp['x'] or el.attrib.get('data-y') != exp['y']:
                                    errors.append(
                                        f'{chart_rel}: {key} point data-x/data-y=({el.attrib.get("data-x")!r}, {el.attrib.get("data-y")!r}) != ({exp["x"]!r}, {exp["y"]!r})'
                                    )
                                nx = min(max(float(exp['num_x']), x_min), x_max)
                                ny = min(max(float(exp['num_y']), y_min), y_max)
                                exp_cx = round(plot_left + plot_width * ((nx - x_min) / (x_max - x_min)), 2)
                                exp_cy = round(plot_bottom - plot_height * ((ny - y_min) / (y_max - y_min)), 2)
                                got_cx = float(el.attrib.get('cx', '-1'))
                                got_cy = float(el.attrib.get('cy', '-1'))
                                if abs(got_cx - exp_cx) > 0.05 or abs(got_cy - exp_cy) > 0.05:
                                    errors.append(
                                        f'{chart_rel}: {key} point geometry cx={got_cx:.2f}/cy={got_cy:.2f} != expected {exp_cx:.2f}/{exp_cy:.2f}'
                                    )
                    for key, exp in expected_pts.items():
                        if key not in seen_pts:
                            errors.append(f'{chart_rel}: missing point/na element for {key}')
                        elif spec['kind'] == 'scatter-sets' and exp['num_x'] is not None and key not in point_labels:
                            errors.append(f'{chart_rel}: missing point-label for {key}')

        table_path = safe_repo_path(root, table_rel)
        if not table_path.is_file():
            errors.append(f'{table_rel}: missing Markdown table file')
        else:
            md_text = table_path.read_text(encoding='utf-8')
            for token in (prov['source'], prov['codec_revision'], prov['date'], prov['threads'], prov['limitations']):
                if token not in md_text:
                    errors.append(f'{table_rel}: missing provenance/limitation token {token!r}')
            try:
                got_headers, got_rows = _parse_pipe_table(md_text)
                if got_headers != spec['table_headers']:
                    errors.append(f'{table_rel}: table headers {got_headers!r} != {spec["table_headers"]!r}')
                if got_rows != spec['table_rows']:
                    errors.append(f'{table_rel}: table rows do not match CSV calculation')
            except ValueError as exc:
                errors.append(f'{table_rel}: {exc}')

    return errors


def render_generated_pages(root: Path, site_content: dict | None = None) -> dict[str, str]:
    if site_content is None:
        site_content = load_site_content(root)
    canonical_sources = {
        safe_repo_path(root, entry['source']): entry['source']
        for group in ('config', 'pages', 'assets')
        for entry in site_content.get(group, [])
        if isinstance(entry, dict) and isinstance(entry.get('source'), str)
    }
    seen_outputs: set[Path] = set()
    outputs: dict[str, str] = {}
    renderers = {
        'installation': render_installation_page,
        'usage': render_usage_page,
        'c-api': render_c_api_page,
        'format': render_format_page,
        'research-summary': render_research_summary_page,
        'contributing': render_contributing_page,
        'security': render_security_page,
    }
    generated_renderers = {
        'api': render_api_page,
        'cli': render_cli_page,
    }
    mirrored_map = {
        **DEFAULT_MIRRORED_MAP,
        **{
            p['source']: p['output']
            for p in site_content['pages']
            for p_out in [p.get('output')]
            if p.get('kind') == 'mirrored' and p_out
        },
    }
    for page in site_content['pages']:
        kind = page.get('kind')
        if kind not in ('mirrored', 'generated'):
            continue
        page_id = page.get('id')
        output_rel = page.get('output')
        renderer_map = renderers if kind == 'mirrored' else generated_renderers
        if page_id not in renderer_map or not output_rel:
            raise ValueError(f'unsupported {kind} page entry: {page!r}')
        out_path = safe_repo_path(root, output_rel)
        if out_path in canonical_sources:
            raise ValueError(
                f'{SITE_CONTENT}: mirrored page {page_id!r} output {output_rel!r} cannot overwrite source {canonical_sources[out_path]!r}'
            )
        if out_path in seen_outputs:
            raise ValueError(f'{SITE_CONTENT}: duplicate mirrored output path {output_rel!r}')
        seen_outputs.add(out_path)
        if kind == 'mirrored':
            outputs[output_rel] = renderer_map[page_id](root, page, mirrored_map)
        else:
            outputs[output_rel] = renderer_map[page_id](root, page)
    has_chart_config = any('generated/charts/' in entry.get('source', '') for entry in site_content.get('config', []))
    if has_chart_config or ((root / CORPUS_CSV).is_file() and (root / RESULTS_CSV).is_file()):
        outputs.update(render_generated_charts_and_tables(root))
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


LIQUID_DELIMITERS = ('{{', '{%')


def liquid_delimiter_errors(rel: str, text: str) -> list[str]:
    """Generated output is copied from plain Markdown, so Jekyll must not see Liquid in it."""
    errors: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        for token in LIQUID_DELIMITERS:
            if token in line:
                errors.append(f'{rel}:{lineno}: Liquid delimiter {token!r} would break the Jekyll build; reword the source')
    return errors


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
        errors.extend(liquid_delimiter_errors(rel, expected))
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
    has_chart_config = any('generated/charts/' in entry.get('source', '') for entry in site_content.get('config', []))
    if has_chart_config or ((root / CORPUS_CSV).is_file() and (root / RESULTS_CSV).is_file()):
        errors.extend(self_test_charts(root))
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

    return map_markdown_urls(text, rewrite_url)


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
        doc_to_staged_html[page['source']] = html_rel
    for entry in site_content['config']:
        staged_rel = entry['staged']
        if not staged_rel.startswith('_') and staged_rel.endswith('.md'):
            doc_to_staged_html[entry['source']] = re.sub(r'\.md$', '.html', staged_rel)

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
            if not staged_rel.startswith('_') and staged_rel.endswith('.md'):
                raw_text = src.read_text(encoding='utf-8')
                staged_text = rebase_staged_links(
                    raw_text,
                    entry['source'],
                    root,
                    site_content,
                    doc_to_staged_html,
                    asset_to_staged_url,
                )
                out_path.write_text(staged_text, encoding='utf-8', newline='\n')
            else:
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


LIVE_SAMPLED_PAGES: tuple[str, ...] = (
    'index.html',
    'format.html',
    'c-api.html',
    'api.html',
    'benchmarks.html',
    '404.html',
)
LIVE_SAMPLE_PAGES: tuple[str, ...] = LIVE_SAMPLED_PAGES


def _default_http_fetcher(url: str) -> tuple[int, bytes]:
    import http.client
    import urllib.error
    import urllib.request

    sep = '&' if '?' in url else '?'
    bust_url = f'{url}{sep}_rf_verify={int(__import__("time").time() * 1000)}'
    req = urllib.request.Request(
        bust_url,
        headers={
            'Cache-Control': 'no-cache, no-store, max-age=0',
            'Pragma': 'no-cache',
            'User-Agent': 'RasterFold-Pages-Verifier/1.0',
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return int(resp.status), resp.read()
    except urllib.error.HTTPError as exc:
        try:
            body = exc.read() if hasattr(exc, 'read') else b''
        except (OSError, http.client.HTTPException) as read_exc:
            raise ValueError(f'HTTP error body read failed for {url}: {read_exc}') from None
        return int(exc.code), body
    except (OSError, urllib.error.URLError, http.client.HTTPException) as exc:
        raise ValueError(f'HTTP request failed for {url}: {exc}') from None


def verify_live_publication(
    site_url: str,
    expected_receipt_path: Path,
    expected_commit: str,
    *,
    pages: tuple[str, ...] = LIVE_SAMPLED_PAGES,
    retries: int = 12,
    delay_seconds: float = 5.0,
    retry_delay: float | None = None,
    fetcher=None,
    sleep_fn=None,
) -> dict:
    import time

    if retry_delay is not None:
        delay_seconds = retry_delay
    if not isinstance(expected_commit, str) or re.fullmatch(r'[0-9a-f]{40}', expected_commit) is None:
        raise ValueError(f'expected_commit must be a 40-character lowercase hex SHA, got {expected_commit!r}')
    if retries < 1:
        raise ValueError(f'retries must be >= 1, got {retries}')
    if delay_seconds < 0:
        raise ValueError(f'delay_seconds must be >= 0, got {delay_seconds}')
    if not isinstance(site_url, str) or not site_url.strip():
        raise ValueError('site_url must be a non-empty URL')
    norm_url = site_url.strip()
    if not norm_url.endswith('/'):
        norm_url += '/'
    if not (norm_url.startswith('https://') or norm_url.startswith('http://127.0.0.1:') or norm_url.startswith('http://localhost:')):
        raise ValueError(f'site_url must use https:// (or local loopback http:// for tests), got {site_url!r}')

    receipt_file = Path(expected_receipt_path)
    if not receipt_file.is_file():
        raise ValueError(f'expected receipt file not found: {expected_receipt_path}')
    try:
        expected_receipt = json.loads(receipt_file.read_text(encoding='utf-8'))
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        raise ValueError(f'invalid expected receipt JSON in {expected_receipt_path}: {exc}') from None
    if not isinstance(expected_receipt, dict) or expected_receipt.get('schema') != 1:
        raise ValueError(f'expected receipt in {expected_receipt_path} must have schema=1')
    if expected_receipt.get('commit') != expected_commit:
        raise ValueError(
            f'expected receipt commit {expected_receipt.get("commit")!r} does not match expected_commit {expected_commit!r}'
        )
    expected_sources = expected_receipt.get('sources')
    expected_outputs = expected_receipt.get('outputs')
    if not isinstance(expected_sources, dict) or not expected_sources:
        raise ValueError(f'expected receipt in {expected_receipt_path} has empty or invalid sources')
    if not isinstance(expected_outputs, dict) or not expected_outputs:
        raise ValueError(f'expected receipt in {expected_receipt_path} has empty or invalid outputs')
    for page_rel in pages:
        if page_rel not in expected_outputs or not isinstance(expected_outputs[page_rel], str):
            raise ValueError(f'expected receipt missing output digest for sampled page {page_rel}')

    do_fetch = fetcher if fetcher is not None else _default_http_fetcher
    do_sleep = sleep_fn if sleep_fn is not None else time.sleep
    last_error = 'unknown verification error'

    for attempt in range(1, retries + 1):
        try:
            receipt_url = f'{norm_url}drift-build.json'
            status, body = do_fetch(receipt_url)
            if status != 200:
                raise ValueError(f'{receipt_url} returned HTTP {status}')
            try:
                live_receipt = json.loads(body.decode('utf-8'))
            except (UnicodeDecodeError, ValueError) as exc:
                raise ValueError(f'{receipt_url} returned invalid JSON: {exc}') from None
            if not isinstance(live_receipt, dict) or live_receipt.get('schema') != 1:
                raise ValueError(f'{receipt_url} has invalid schema: {live_receipt!r}')
            if live_receipt.get('commit') != expected_commit:
                raise ValueError(
                    f'live receipt commit {live_receipt.get("commit")!r} != expected {expected_commit!r}'
                )
            for key in ('sources', 'generated', 'outputs'):
                if live_receipt.get(key) != expected_receipt.get(key):
                    raise ValueError(f'live drift-build.json[{key!r}] differs from expected build receipt')
            if live_receipt != expected_receipt:
                raise ValueError(
                    'live drift-build.json differs from expected build receipt (stale same-commit or tampered artifact)'
                )

            verified_hashes: dict[str, str] = {}
            # Check bare site_url root as well as each sampled page
            root_status, root_body = do_fetch(norm_url)
            if root_status != 200:
                raise ValueError(f'{norm_url} returned HTTP {root_status}')
            root_hash = hashlib.sha256(root_body).hexdigest()
            if root_hash != expected_outputs['index.html']:
                raise ValueError(
                    f'{norm_url} SHA-256 {root_hash} != expected index.html {expected_outputs["index.html"]}'
                )

            for page_rel in pages:
                page_url = f'{norm_url}{page_rel}'
                p_status, p_body = do_fetch(page_url)
                allowed_statuses = (200, 404) if page_rel == '404.html' else (200,)
                if p_status not in allowed_statuses:
                    raise ValueError(f'{page_url} returned HTTP {p_status}')
                actual_hash = hashlib.sha256(p_body).hexdigest()
                expected_hash = expected_outputs[page_rel]
                if actual_hash != expected_hash:
                    raise ValueError(
                        f'live page {page_rel} sha256 {actual_hash} != expected receipt digest {expected_hash}'
                    )
                verified_hashes[page_rel] = actual_hash

            return {
                'site_url': norm_url,
                'commit': expected_commit,
                'attempt': attempt,
                'outputs_count': len(expected_outputs),
                'verified_pages': list(pages),
                'page_hashes': verified_hashes,
            }
        except ValueError as exc:
            last_error = str(exc)
            if attempt < retries:
                do_sleep(delay_seconds)

    raise ValueError(f'live site verification failed after {retries} attempt(s): {last_error}')


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--write', action='store_true', help='Write generated pages, charts, and tables (default when no mode flag is given)')
    parser.add_argument('--check', action='store_true', help='Verify generated pages, charts, tables, and site allowlist without modifying files')
    parser.add_argument('--self-test', action='store_true', help='Independently parse generated SVG charts and Markdown tables and assert numerical agreement with CSVs')
    parser.add_argument('--stage', metavar='DIR', help='Stage the allowlisted Jekyll source tree into DIR (must be inside build/)')
    parser.add_argument('--check-offline', metavar='DIR', type=Path, help='Verify the offline documentation of an extracted release package (its top directory) without any network')
    parser.add_argument('--verify-live', metavar='URL', help='Verify live published site URL and drift-build.json against a build job receipt')
    parser.add_argument('--expected-receipt', type=Path, help='Path to expected drift-build.json from the build job (required with --verify-live)')
    parser.add_argument('--expected-commit', help='Expected 40-char commit SHA for --verify-live')
    parser.add_argument('--retries', type=int, default=12, help='Maximum attempts for --verify-live (default: 12)')
    parser.add_argument('--retry-delay', type=float, default=5.0, help='Seconds between --verify-live attempts (default: 5.0)')
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        has_verify_live = args.verify_live is not None
        has_stage = args.stage is not None
        if args.check_offline is not None:
            if args.check or args.self_test or has_verify_live or args.write or has_stage:
                raise ValueError('--check-offline cannot be combined with another mode')
            report = check_offline_tree(args.check_offline.resolve())
            problems = report['unresolved'] + report['leftovers'] + [
                f'{page}: not reachable from {OFFLINE_ENTRY}' for page in report['unreachable']
            ]
            for problem in problems:
                print(f'build-docs: offline: {problem}', file=sys.stderr)
            print(
                f'build-docs: offline: {len(report["pages"])} page(s), {report["links"]} relative link(s), '
                f'{len(report["unresolved"])} unresolved, {len(report["unreachable"])} unreachable from {OFFLINE_ENTRY}'
            )
            return 1 if problems or not report['pages'] else 0
        if (args.check or args.self_test or has_verify_live) and (args.write or has_stage):
            raise ValueError('--check, --self-test, and --verify-live cannot be combined with --write or --stage')
        if has_verify_live:
            if not args.verify_live.strip():
                raise ValueError('--verify-live URL must be non-empty')
            if args.expected_receipt is None or not str(args.expected_receipt).strip() or not (args.expected_commit or '').strip():
                raise ValueError('--verify-live requires both --expected-receipt and --expected-commit')
            res = verify_live_publication(
                args.verify_live,
                args.expected_receipt,
                args.expected_commit,
                retries=args.retries,
                delay_seconds=args.retry_delay,
            )
            print(
                f'build-docs: verify-live: verified {len(res["verified_pages"])} page(s) and drift-build.json at {res["site_url"]} for commit {res["commit"]} (attempt {res["attempt"]})'
            )
            return 0
        if args.self_test and not args.check:
            errors = self_test_charts(root)
            for err in errors:
                print(f'build-docs: self-test: {err}', file=sys.stderr)
            if errors:
                return 1
            print(f'build-docs: self-test: {len(CHART_NAMES)} chart(s) and {len(CHART_NAMES)} table(s) match CSV data')
            return 0
        if args.check:
            errors = check_generated_pages(root)
            for err in errors:
                print(f'build-docs: {err}', file=sys.stderr)
            if errors:
                return 1
            print('build-docs: current')
            return 0
        if has_stage:
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
