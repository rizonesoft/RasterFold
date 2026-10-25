#!/usr/bin/env python3
"""Check document contracts; repair only deterministic, explicitly owned output.

No network, third-party packages, history rewrites, or automatic review receipts.
The manifest defines current documents, historical/planned evidence and generators.
"""
from __future__ import annotations

import argparse
import csv
from decimal import Decimal, InvalidOperation
import fnmatch
import hashlib
from html import unescape
from html.parser import HTMLParser
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import tomllib
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = 'docs/drift.json'
SUFFIXES = {'.md', '.markdown', '.rst', '.adoc', '.txt', '.html', '.htm', '.json', '.jsonl', '.yaml', '.yml', '.toml', '.ini', '.cfg', '.csv', '.svg', '.xml'}
NAMES = {'LICENSE', 'NOTICE', 'COPYING', '.gitattributes', '.gitignore'}
ROLES = {'current', 'plan', 'historical', 'template', 'configuration', 'data', 'generated'}
UNREAD = object()
BLOCK = re.compile(r'(?P<start><!-- drift:begin (?P<id>[a-z0-9-]+) -->\r?\n)(?P<body>.*?)(?P<end><!-- drift:end (?P=id) -->)', re.S)
FACT = re.compile(r'<!-- drift:fact ([a-z0-9_.-]+) = (.*?) -->')
COMMIT = re.compile(r'<!-- drift:commit ([0-9a-f]{7,40})(?: (\S+))? -->')
LINK = re.compile(r'!?\[[^\]\n]*\]\((?:<([^>\n]+)>|([^\s)]+))(?:\s+["\'][^\n]*?["\'])?\)')
REFERENCE = re.compile(r'^\s*\[(?!\^)[^\]]+\]:\s*<?([^\s>]+)>?', re.M)
BYTE_UNITS = {
    '': 1, 'b': 1, 'byte': 1, 'bytes': 1,
    'kib': 1024, 'mib': 1024 ** 2, 'gib': 1024 ** 3, 'tib': 1024 ** 4,
    'kb': 1000, 'mb': 1000 ** 2, 'gb': 1000 ** 3, 'tb': 1000 ** 4,
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def blank(text):
    return re.sub(r'[^\r\n]', ' ', text)


def prose(text):
    """Mask fenced examples while preserving original byte-decoded offsets."""
    lines, fence = [], None
    for line in text.splitlines(keepends=True):
        match = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
        if match and fence is None:
            fence = match[1]
            lines.append(blank(line))
        elif fence and re.match(r'^\s{0,3}' + re.escape(fence[0]) + '{' + str(len(fence)) + r',}\s*$', line):
            fence = None
            lines.append(blank(line))
        else:
            lines.append(blank(line) if fence else line)
    return ''.join(lines)


def code_spans(text):
    # A literal inline example must not suppress a real contract beside it.
    return re.sub(r'(?<!`)(`+)(?!`)((?:(?!\r?\n[ \t]*\r?\n).)*?)(?<!`)\1(?!`)', lambda m: blank(m[0]), text, flags=re.S)


class HTML(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links, self.anchors = [], set()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        for name in ('id', 'name'):
            if attrs.get(name):
                self.anchors.add(attrs[name])
        for name in ('href', 'src', 'poster'):
            if attrs.get(name):
                self.links.append(attrs[name])


def anchors(text, markdown=True):
    html = HTML()
    html.feed(text)
    found, seen = html.anchors, {}
    if markdown:
        for line in prose(text).splitlines():
            match = re.match(r'^#{1,6}\s+(.+?)\s*#*$', line)
            if not match:
                continue
            title = re.sub(r'<[^>]*>', '', unescape(match[1])).lower()
            title = re.sub(r'[^\w\-\s]', '', title).replace(' ', '-')
            n = seen.get(title, 0)
            found.add(title + (f'-{n}' if n else ''))
            seen[title] = n + 1
    return found



def _csv_stream(source):
    lines = source.splitlines(keepends=True)
    idx = 0
    while idx < len(lines) and lines[idx].lstrip().startswith('#'):
        idx += 1
    return io.StringIO(''.join(lines[idx:]))


def derive_facts(manifest, read):
    facts = {}
    for key, rule in manifest['facts'].items():
        source = read(rule['source'])
        if rule['kind'] == 'regex':
            values = re.findall(rule['pattern'], source, re.M)
            if len(values) != 1:
                raise ValueError(f'{key}: expected exactly one source match, got {len(values)}')
            value = int(values[0]) if rule.get('type') == 'int' else values[0]
            for extra in rule.get('also_match', ()):
                extra_source = read(extra['source'])
                extra_values = re.findall(extra['pattern'], extra_source, re.M)
                if len(extra_values) != 1:
                    raise ValueError(f'{key}: expected one match in {extra["source"]}, got {len(extra_values)}')
                extra_value = int(extra_values[0]) if rule.get('type') == 'int' else extra_values[0]
                if extra_value != value:
                    raise ValueError(f'{key}: {rule["source"]} has {value!r}, {extra["source"]} has {extra_value!r}')
        elif rule['kind'] == 'json':
            value = json.loads(source)
            for part in rule['key'].split('.'):
                value = value[part]
        elif rule['kind'] == 'csv':
            rows = [r for r in csv.DictReader(_csv_stream(source)) if all(r.get(k) == v for k, v in rule['where'].items())]
            if len(rows) != 1:
                raise ValueError(f'{key}: expected one CSV row, got {len(rows)}')
            value = rows[0][rule['column']]
            if rule.get('type') == 'int': value = int(value)
        else:
            raise ValueError(f'{key}: unknown provider kind')
        facts[key] = value
    return facts


class Checker:
    def __init__(self, root, git_dir=None):
        self.root = Path(root).resolve()
        # Address the selected ordinary checkout explicitly, including a Windows
        # view of a WSL UNC path. Do not alter global safe.directory settings.
        # Git's GIT_INDEX_FILE still selects a partial-commit temporary index.
        metadata = self.root / '.git'
        self.git_dir = git_dir or (metadata if metadata.is_dir() else None)
        self.errors, self.repairs, self.inventory = [], {}, []
        self.facts = {}
        self.manifest = {}
        self.files = set()

    def git(self, *args):
        prefix = ['git', '--no-replace-objects']
        if self.git_dir:
            prefix += ['--git-dir', str(self.git_dir), '--work-tree', str(self.root)]
        return subprocess.check_output([*prefix, *args], cwd=self.root, stderr=subprocess.PIPE, timeout=30)

    def path(self, rel):
        if not isinstance(rel, str) or not rel or '\\' in rel:
            raise ValueError(f'invalid repository path: {rel!r}')
        path = self.root / rel
        if Path(rel).is_absolute() or '..' in Path(rel).parts or not path.resolve().is_relative_to(self.root):
            raise ValueError(f'path escapes repository: {rel}')
        parent = self.root
        for part in Path(rel).parts:
            parent /= part
            if parent.is_symlink():
                raise ValueError(f'symlink is not an owned document: {rel}')
        return path

    def read(self, rel):
        encoding = self.manifest.get('encodings', {}).get(rel, 'utf-8')
        return self.path(rel).read_text(encoding=encoding)

    def error(self, kind, path, detail):
        self.errors.append({'kind': kind, 'path': path, 'detail': detail})

    def generated(self, rel, content, before=UNREAD):
        path = self.path(rel)
        data = content.encode(self.manifest.get('encodings', {}).get(rel, 'utf-8'))
        if before is UNREAD:
            before = path.read_bytes() if path.exists() else None
        if before != data:
            self.error('generated', rel, 'deterministic output differs; run --fix')
            self.repairs[rel] = (before, data)

    def load(self):
        self.manifest_bytes = self.path(MANIFEST).read_bytes()
        self.manifest = json.loads(self.manifest_bytes.decode('utf-8'))
        if self.manifest.get('schema') != 1:
            raise ValueError('unsupported drift manifest schema')
        self.files = {p for p in self.git('ls-files', '--cached', '--others', '--exclude-standard', '-z').decode().split('\0') if p}
        for rel in sorted(self.files):
            p = Path(rel)
            if p.suffix.lower() not in SUFFIXES and p.name not in NAMES:
                continue
            matching = [r for r in self.manifest['coverage'] if fnmatch.fnmatchcase(rel, r['pattern'])]
            if not matching:
                self.error('coverage', rel, 'supported document has no coverage role')
                continue
            role = matching[0]['role']
            if role not in ROLES or not matching[0].get('reason'):
                raise ValueError(f'invalid coverage rule for {rel}')
            self.inventory.append({'path': rel, 'role': role})

    def derive(self):
        self.facts = derive_facts(self.manifest, self.read)
        self.generated('docs/facts.json', json.dumps(self.facts, indent=2, sort_keys=True) + '\n')

    def revision_facts(self, revision):
        # revision ':' addresses the index; '<oid>:' addresses that exact tree.
        def read(rel):
            # Git-object reads must not depend on an unrelated working copy.
            if not isinstance(rel, str) or not rel or '\\' in rel or Path(rel).is_absolute() or '..' in Path(rel).parts:
                raise ValueError(f'invalid committed path: {rel!r}')
            encoding = manifest.get('encodings', {}).get(rel, 'utf-8')
            return self.git('show', revision + rel).decode(encoding).replace('\r\n', '\n')
        manifest = json.loads(self.git('show', revision + MANIFEST))
        if manifest.get('schema') != 1:
            raise ValueError('unsupported historical drift manifest schema')
        facts = derive_facts(manifest, read)
        if facts != json.loads(read('docs/facts.json')):
            raise ValueError(f'{revision} facts differ from the source in that tree')
        return facts

    def documents(self):
        spec = importlib.util.spec_from_file_location('drift_claims', self.root / 'scripts/todo-claims.py')
        claims = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(claims)
        claims.ROOT = self.root
        blocks = self.manifest['blocks']
        seen_blocks = set()
        for entry in self.inventory:
            rel, role = entry['path'], entry['role']
            path = self.path(rel)
            if not path.is_file():
                self.error('missing', rel, 'indexed document is missing; restore it or stage its deletion')
                continue
            original = path.read_bytes()
            text = original.decode(self.manifest.get('encodings', {}).get(rel, 'utf-8'))
            suffix = Path(rel).suffix.lower()
            try:
                if suffix == '.json':
                    json.loads(text)
                elif suffix == '.jsonl':
                    for line in text.splitlines():
                        if line.strip(): json.loads(line)
                elif suffix == '.toml':
                    tomllib.loads(text)
                elif suffix in ('.svg', '.xml'):
                    ET.fromstring(text)
            except (ValueError, ET.ParseError) as exc:
                self.error('syntax', rel, str(exc))
            scan = code_spans(prose(text)) if suffix in ('.md', '.markdown') else text
            if role == 'template':
                continue
            for line_no, line in enumerate(scan.splitlines(), 1):
                for match in claims.CLAIM_RE.finditer(line):
                    try:
                        claims.evaluate(match[1])
                    except (claims.Stale, claims.Malformed) as exc:
                        self.error('claim', rel, f'line {line_no}: {exc}')
                if len(claims.CLAIM_OPEN_RE.findall(line)) != len(claims.CLAIM_RE.findall(line)):
                    self.error('claim', rel, f'line {line_no}: unterminated claim')
                if line.count('<!-- drift:fact ') != len(list(FACT.finditer(line))):
                    self.error('fact', rel, f'line {line_no}: malformed fact binding')
                if line.count('<!-- drift:commit ') != len(list(COMMIT.finditer(line))):
                    self.error('commit', rel, f'line {line_no}: malformed commit reference')
                for match in FACT.finditer(line):
                    value = self.facts.get(match[1])
                    if match[1] not in self.facts or str(value) != match[2]:
                        self.error('fact', rel, f'line {line_no}: {match[1]} must equal {value!r}; review the surrounding sentence')
                for match in COMMIT.finditer(line):
                    try:
                        ref = match[1] + (':' + match[2] if match[2] else '^{commit}')
                        self.git('cat-file', '-e', ref)
                    except subprocess.SubprocessError:
                        self.error('commit', rel, f'line {line_no}: unresolved commit or committed path {ref}')
            replacements = []
            for match in BLOCK.finditer(scan):
                name = match['id']
                if name not in blocks or blocks[name]['path'] != rel or name in seen_blocks:
                    self.error('block', rel, f'unregistered, misplaced or duplicate block {name}')
                    continue
                seen_blocks.add(name)
                newline = '\r\n' if match['start'].endswith('\r\n') else '\n'
                expected = (blocks[name]['template'].format_map(self.facts).rstrip() + '\n').replace('\n', newline)
                if text[match.start('body'):match.end('body')] != expected:
                    replacements.append((match.start('body'), match.end('body'), expected))
            if any(scan.count(marker) != len(list(BLOCK.finditer(scan))) for marker in ('<!-- drift:begin ', '<!-- drift:end ')):
                self.error('block', rel, 'unclosed or malformed generated block')
            if replacements:
                updated = text
                for start, end, expected in reversed(replacements):
                    updated = updated[:start] + expected + updated[end:]
                self.generated(rel, updated, before=original)
            if suffix in ('.md', '.markdown', '.html', '.htm'):
                self.links(rel, scan, self.root)
            if role == 'current':
                for rule in self.manifest.get('assertions', []):
                    if not fnmatch.fnmatchcase(rel, rule['pattern']):
                        continue
                    expected = str(self.facts[rule['fact']])
                    for value in re.findall(rule['regex'], scan, re.I):
                        if rule.get('units') == 'bytes':
                            num_text, unit_text = value if isinstance(value, tuple) else (value, '')
                            unit_key = unit_text.strip().lower()
                            stated = f'{num_text} {unit_text}'.strip()
                            if unit_key not in BYTE_UNITS:
                                self.error('prose-fact', rel, f'{rule["fact"]}: stated {stated} (unsupported unit), source says {expected}; review required')
                                continue
                            clean = num_text.replace(',', '') if re.fullmatch(r'\d+(?:,\d{3})+(?:\.\d+)?', num_text) else (num_text.replace('_', '') if re.fullmatch(r'\d+(?:_\d{3})+(?:\.\d+)?', num_text) else (num_text if re.fullmatch(r'\d+(?:\.\d+)?', num_text) else None))
                            try:
                                scaled = Decimal(clean) * BYTE_UNITS[unit_key] if clean is not None else None
                            except InvalidOperation:
                                scaled = None
                            if scaled is None or scaled != int(scaled) or str(int(scaled)) != expected:
                                self.error('prose-fact', rel, f'{rule["fact"]}: stated {stated}, source says {expected}; review required')
                        else:
                            clean = value.replace(',', '') if re.fullmatch(r'\d+(?:,\d{3})+', value) else (value.replace('_', '') if re.fullmatch(r'\d+(?:_\d{3})+', value) else value)
                            if clean != expected:
                                self.error('prose-fact', rel, f'{rule["fact"]}: stated {value}, source says {expected}; review required')
        for name, block in blocks.items():
            if name not in seen_blocks:
                self.error('block', block['path'], f'required block {name} is missing; restore its markers')
        self.review_policies()
        for doc, review in self.manifest.get('reviews', {}).items():
            if doc not in self.files:
                self.error('review', doc, 'review contract names a missing document')
            for source, expected in review['sources'].items():
                path = self.path(source)
                if not path.is_file() or digest(path.read_bytes()) != expected:
                    self.error('stale', doc, f'{source} changed since review {review["evidence"]}; review prose and explicitly renew the receipt')

    def review_policies(self, reviews=None):
        reviews = self.manifest.get('reviews', {}) if reviews is None else reviews
        receipt_sources = sorted({p for r in reviews.values() for p in r['sources']})
        attributes = {}
        if receipt_sources:
            raw = self.git('check-attr', '-z', 'eol', 'text', '--', *receipt_sources).decode().split('\0')
            for i in range(0, len(raw) - 1, 3):
                attributes.setdefault(raw[i], {})[raw[i + 1]] = raw[i + 2]
        for doc, review in reviews.items():
            if doc not in review['sources']:
                self.error('review', doc, 'receipt must bind the reviewed document itself')
            for source in review['sources']:
                attr = attributes[source]
                if attr.get('eol') != 'lf' and attr.get('text') != 'unset':
                    self.error('eol', doc, f'{source}: receipt source needs eol=lf or -text so checkout conversion cannot invalidate its byte hash')
                path = self.path(source)
                if attr.get('eol') == 'lf' and path.is_file() and b'\r\n' in path.read_bytes():
                    self.error('eol', doc, f'{source}: working bytes contain CRLF despite eol=lf; preserve edits and normalize/refresh this file before renewing a review')

    def links(self, rel, text, base, site_prefix=None):
        html = HTML()
        html.feed(text)
        links = html.links
        if Path(rel).suffix in ('.md', '.markdown'):
            links += [m[1] or m[2] for m in LINK.finditer(text)] + REFERENCE.findall(text)
        for link in links:
            parts = urlsplit(unescape(link))
            if parts.scheme or parts.netloc or not link or link.startswith('{{'):
                continue
            target = unquote(parts.path)
            if site_prefix is not None and target.startswith('/'):
                prefix = site_prefix.rstrip('/') + '/'
                if not target.startswith(prefix):
                    self.error('site-link', rel, f'URL outside Pages base path: {link}')
                    continue
                target = target[len(prefix):]
                path = base / target
            elif target.startswith('/'):
                path = base / target.lstrip('/')
            else:
                path = base / Path(rel).parent / target if target else base / rel
            path = path.resolve()
            if not path.is_relative_to(base.resolve()):
                self.error('link', rel, f'link escapes tree: {link}')
                continue
            if path.is_dir():
                if site_prefix is None:
                    continue
                path /= 'index.html'
            if not path.is_file():
                key = rel + ' -> ' + link
                if key not in self.manifest.get('historical_links', {}):
                    self.error('link', rel, f'missing target: {link}')
                continue
            if parts.fragment and path.suffix in ('.md', '.markdown', '.html', '.htm'):
                if unquote(parts.fragment) not in anchors(path.read_text(encoding='utf-8'), path.suffix in ('.md', '.markdown')):
                    self.error('anchor', rel, f'missing heading or id: {link}')

    def site(self, directory, prefix, require_receipt=True):
        base = self.path(directory)
        if not base.is_dir() or not (base / 'index.html').is_file():
            self.error('site', directory, 'built site or index.html is missing')
            return
        receipt = base / 'drift-build.json'
        if require_receipt and not receipt.is_file():
            self.error('site', directory, 'missing source receipt drift-build.json')
        elif require_receipt:
            expected = self.site_receipt(directory)
            if json.loads(receipt.read_text(encoding='utf-8')) != expected:
                self.error('site', directory, 'site source receipt differs from current inputs; rebuild before publishing')
        forbidden = set(self.manifest.get('forbidden_site_paths', []))
        for path in base.rglob('*'):
            if any(part.lower() in forbidden for part in path.relative_to(base).parts):
                self.error('site-private', directory, f'private path in artifact: {path.relative_to(base)}')
        for path in base.rglob('*.html'):
            if not path.resolve().is_relative_to(base.resolve()):
                self.error('site', directory, 'symlink escapes built site')
                continue
            self.links(path.relative_to(base).as_posix(), path.read_text(encoding='utf-8'), base, prefix)

    def site_receipt(self, directory):
        paths = sorted(p for p in self.files if any(fnmatch.fnmatchcase(p, pat) for pat in self.manifest['site_sources']))
        base = self.path(directory)
        output = {}
        for p in sorted(base.rglob('*')):
            if p.is_symlink() or not p.resolve().is_relative_to(base):
                raise ValueError(f'unsafe built-site path: {p}')
            if p.is_file() and p.name != 'drift-build.json':
                output[p.relative_to(base).as_posix()] = digest(p.read_bytes())
        return {'schema': 1, 'sources': {p: digest(self.path(p).read_bytes()) for p in paths}, 'outputs': output}

    def generators(self, fix=False):
        env = dict(os.environ)
        if self.git_dir:
            env.update(GIT_DIR=str(self.git_dir), GIT_WORK_TREE=str(self.root))
        for gen in self.manifest['generators']:
            exists = self.path(gen['script']).is_file()
            if not exists:
                if gen.get('planned_owner'):
                    continue
                self.error('generator', gen['script'], 'required generator missing')
                continue
            args = gen['fix'] if fix else gen['check']
            result = subprocess.run([sys.executable, gen['script'], *args], cwd=self.root, env=env, capture_output=True, text=True, timeout=120)
            if result.returncode:
                self.error('generator', gen['script'], (result.stdout + result.stderr)[-1500:])

    def fix(self):
        for rel, (before, _) in self.repairs.items():
            path = self.path(rel)
            if (path.read_bytes() if path.exists() else None) != before:
                raise ValueError(f'{rel} changed during repair; refusing to overwrite')
        for rel, (before, after) in self.repairs.items():
            path = self.path(rel)
            current = path.read_bytes() if path.exists() else None
            if current != before:
                raise ValueError(f'{rel} changed during repair; refusing to overwrite')
            path.parent.mkdir(parents=True, exist_ok=True)
            fd, temp = tempfile.mkstemp(prefix='.drift-', dir=path.parent)
            try:
                with os.fdopen(fd, 'wb') as out:
                    out.write(after)
                    out.flush()
                    os.fsync(out.fileno())
                os.chmod(temp, path.stat().st_mode if path.exists() else 0o644)
                os.replace(temp, path)
            finally:
                if os.path.exists(temp): os.unlink(temp)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--git-dir', type=Path)
    parser.add_argument('--fix', action='store_true')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--coverage', action='store_true')
    parser.add_argument('--site-dir')
    parser.add_argument('--site-prefix', default='/RasterFold/')
    parser.add_argument('--write-site-receipt', action='store_true')
    parser.add_argument('--review', metavar='DOCUMENT')
    parser.add_argument('--evidence', help='Human/source review evidence required to renew one receipt')
    parser.add_argument('--commit-range', metavar='BASE..HEAD')
    parser.add_argument('--commit-message', type=Path, help='Check Drift-Fact declarations against staged facts, not unstaged files')
    args = parser.parse_args(argv)
    try:
        check = Checker(args.root, args.git_dir)
        if args.commit_message:
            if args.fix or args.review or args.commit_range or args.site_dir:
                raise ValueError('--commit-message cannot be combined with document/site operations')
            message = args.commit_message.read_text(encoding='utf-8')
            if not re.search(r'^Drift-Fact:', message, re.M):
                return 0
            facts = check.revision_facts(':')
            declarations(message, facts, check, str(args.commit_message))
            for error in check.errors: print(f'commit-fact: {error["detail"]}')
            return bool(check.errors)
        if args.write_site_receipt and (not args.site_dir or args.fix or args.review):
            raise ValueError('--write-site-receipt requires --site-dir and cannot repair or renew reviews')
        check.load()
        if args.review:
            if not args.evidence or not args.evidence.strip() or args.fix:
                raise ValueError('--review needs --evidence and cannot be combined with --fix')
            review = check.manifest['reviews'][args.review]
            check.review_policies({args.review: review})
            if check.errors:
                raise ValueError('review receipt not renewed: ' + '; '.join(e['detail'] for e in check.errors))
            review['sources'] = {p: digest(check.path(p).read_bytes()) for p in review['sources']}
            review['evidence'] = args.evidence
            check.generated(MANIFEST, json.dumps(check.manifest, indent=2) + '\n', before=check.manifest_bytes)
            check.fix()
            check.errors.clear()
            check.repairs.clear()
        check.derive()
        check.documents()
        if args.fix:
            check.fix()
            check.generators(fix=True)
            if any(e['kind'] == 'generator' for e in check.errors):
                raise ValueError('generator repair failed; inspect output and rerun check')
            return main([*(['--root', str(args.root)]), *(['--git-dir', str(args.git_dir)] if args.git_dir else []), *(['--site-dir', args.site_dir, '--site-prefix', args.site_prefix] if args.site_dir else []), *(['--json'] if args.json else []), *(['--coverage'] if args.coverage else []), *(['--commit-range', args.commit_range] if args.commit_range else [])])
        check.generators()
        if args.commit_range:
            if not re.fullmatch(r'[0-9a-f]{7,40}\.\.[0-9a-f]{7,40}', args.commit_range):
                raise ValueError('--commit-range requires two explicit commit ids')
            for oid in check.git('rev-list', args.commit_range).decode().splitlines():
                message = check.git('show', '-s', '--format=%B', oid).decode()
                if re.search(r'^Drift-Fact:', message, re.M):
                    # A historical declaration belongs to its own tree, not today's source.
                    facts = check.revision_facts(oid + ':')
                    declarations(message, facts, check, oid)
        if args.write_site_receipt:
            if not args.site_dir or check.errors:
                raise ValueError('site receipt requires --site-dir and clean source checks')
            check.site(args.site_dir, args.site_prefix, require_receipt=False)
            if not check.errors:
                check.generated(args.site_dir + '/drift-build.json', json.dumps(check.site_receipt(args.site_dir), indent=2) + '\n')
                check.fix()
                check.errors = [e for e in check.errors if e['path'] != args.site_dir + '/drift-build.json']
                check.repairs.clear()
        if args.site_dir:
            check.site(args.site_dir, args.site_prefix)
        if args.json:
            print(json.dumps({'errors': check.errors, 'repairable': sorted(check.repairs), 'coverage': check.inventory}, indent=2))
        else:
            for error in check.errors:
                print(f'{error["kind"]}: {error["path"]}: {error["detail"]}')
            if args.coverage:
                for entry in check.inventory: print(f'{entry["role"]}: {entry["path"]}')
            print(f'drift: {len(check.inventory)} supported documents; {len(check.errors)} error(s); {len(check.repairs)} deterministic repair(s)')
        return bool(check.errors)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        print(f'drift: {exc}', file=sys.stderr)
        return 2


def declarations(message, facts, check, where):
    for line in message.splitlines():
        if not line.startswith('Drift-Fact:'):
            continue
        match = re.fullmatch(r'Drift-Fact: ([a-z0-9_.-]+)=(.*)', line)
        if not match or match[1] not in facts or str(facts[match[1]]) != match[2]:
            check.error('commit-fact', where, f'{line}: differs from that commit\'s facts; correct the new message or add a corrective commit, never rewrite history')


if __name__ == '__main__':
    raise SystemExit(main())
