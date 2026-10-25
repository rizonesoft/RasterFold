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
import math
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



TP_OVERLAP_PHOTO = (
    'kodim01',
    'kodim03',
    'kodim05',
    'kodim13',
    'kodim23',
)
CORPUS_KODAK = tuple(f'kodim{i:02d}' for i in range(1, 25))
CORPUS_IC = (
    'ic_artificial', 'ic_big_building', 'ic_big_tree', 'ic_bridge', 'ic_cathedral',
    'ic_deer', 'ic_fireworks', 'ic_flower_foveon', 'ic_hdr', 'ic_leaves_iso_1600',
    'ic_leaves_iso_200', 'ic_nightshot_iso_100', 'ic_nightshot_iso_1600', 'ic_spider_web',
)
CORPUS_TP_ONLY = (
    'tp_deer_crop', 'tp_fireworks_crop', 'tp_flower_foveon_crop',
    'tp_nightshot_iso_1600_crop', 'tp_spider_web_crop',
)
CORPUS_SYNTH = (
    'syn_solid', 'syn_checker', 'syn_stripes', 'syn_gradient', 'syn_noise',
    'syn_pixel', 'syn_ui', 'syn_text', 'syn_diagram', 'syn_odd', 'syn_wide',
)
CORPUS_GROUP_MEMBERS = {
    'photo': {('photo', img) for img in ('measure', *CORPUS_KODAK, *CORPUS_IC)},
    'tp': {('photo', img) for img in TP_OVERLAP_PHOTO} | {('tp', img) for img in CORPUS_TP_ONLY},
    'tp-only': {('tp', img) for img in CORPUS_TP_ONLY},
    'synth': {('synth', img) for img in CORPUS_SYNTH},
}
CORPUS_GROUP_MEMBERS['all'] = (
    CORPUS_GROUP_MEMBERS['photo'] | CORPUS_GROUP_MEMBERS['tp-only'] | CORPUS_GROUP_MEMBERS['synth']
)
BENCHMARK_SET_IMAGES = {
    'measure': {'image'},
    'photos': {
        'deer_crop', 'fireworks_crop', 'flower_foveon_crop',
        'kodim01', 'kodim03', 'kodim05', 'kodim13', 'kodim23',
        'nightshot_iso_1600_crop', 'spider_web_crop',
    },
    'synthetic': {
        'solid', 'checker', 'stripes', 'gradient', 'noise',
        'pixel', 'ui', 'text', 'diagram', 'odd', 'wide',
    },
}


def _csv_stream(source):
    lines = source.splitlines(keepends=True)
    idx = 0
    while idx < len(lines) and lines[idx].lstrip().startswith('#'):
        idx += 1
    return io.StringIO(''.join(lines[idx:]))


def _csv_meta(source):
    meta = {}
    for line in source.splitlines():
        stripped = line.strip()
        if not stripped.startswith('#'):
            break
        for token in stripped[1:].split():
            if '=' in token:
                k, v = token.split('=', 1)
                if k and v:
                    meta[k] = v
    return meta


def _validate_csv_table(all_rows, key):
    if not all_rows:
        raise ValueError(f'{key}: CSV file has 0 data rows')
    sample = all_rows[0]
    if 'codec' in sample:
        seen = set()
        for r in all_rows:
            row_id = (r.get('set'), r.get('image'), r.get('codec'))
            if not all(row_id) or row_id in seen:
                raise ValueError(f'{key}: invalid or duplicate benchmark CSV row {row_id!r}')
            if 'pixels_match' in r and r.get('pixels_match') != 'True':
                raise ValueError(f'{key}: benchmark CSV row {row_id!r} has pixels_match={r.get("pixels_match")!r}')
            seen.add(row_id)
    elif 'set' in sample and 'image' in sample:
        seen_pairs, seen_images = set(), set()
        for r in all_rows:
            pair = (r.get('set'), r.get('image'))
            img = r.get('image')
            if not all(pair) or pair in seen_pairs or img in seen_images:
                raise ValueError(f'{key}: invalid or duplicate corpus CSV row {pair!r}')
            seen_pairs.add(pair)
            seen_images.add(img)


def _select_csv_rows(source, rule, key):
    all_rows = list(csv.DictReader(_csv_stream(source)))
    _validate_csv_table(all_rows, key)
    if 'group' in rule:
        group = rule['group']
        if group == 'photo':
            rows = [r for r in all_rows if r.get('set') == 'photo']
        elif group == 'tp':
            rows = [r for r in all_rows if r.get('set') == 'tp' or r.get('image') in TP_OVERLAP_PHOTO]
        elif group == 'tp-only':
            rows = [r for r in all_rows if r.get('set') == 'tp']
        elif group == 'synth':
            rows = [r for r in all_rows if r.get('set') == 'synth']
        elif group == 'all':
            rows = list(all_rows)
        else:
            raise ValueError(f'{key}: unknown corpus group {group!r}')
        exp_members = CORPUS_GROUP_MEMBERS[group]
        got_members = [(r.get('set'), r.get('image')) for r in rows]
        if len(got_members) != len(exp_members) or set(got_members) != exp_members:
            raise ValueError(
                f'{key}: corpus group {group!r} expected {len(exp_members)} authoritative members, got {len(got_members)} (diff={set(got_members) ^ exp_members})'
            )
        if 'where' in rule:
            rows = [r for r in rows if all(r.get(k) == v for k, v in rule['where'].items())]
    elif 'where' in rule and isinstance(rule['where'], dict) and rule['where']:
        rows = [r for r in all_rows if all(r.get(k) == v for k, v in rule['where'].items())]
        bset = rule['where'].get('set')
        if bset in BENCHMARK_SET_IMAGES and 'image' not in rule['where'] and 'codec' in rule['where']:
            exp_imgs = BENCHMARK_SET_IMAGES[bset]
            got_imgs = [r.get('image') for r in rows]
            if len(got_imgs) != len(exp_imgs) or set(got_imgs) != exp_imgs:
                raise ValueError(
                    f'{key}: benchmark set {bset!r} expected {len(exp_imgs)} authoritative images, got {len(got_imgs)}'
                )
    else:
        raise ValueError(f'{key}: csv-aggregate requires non-empty group or where selector')
    if not rows:
        raise ValueError(f'{key}: CSV selector matched 0 rows')
    join_key = rule.get('join_key', 'image')
    if join_key in rows[0]:
        keys = [r.get(join_key) for r in rows]
        if len(set(keys)) != len(keys):
            raise ValueError(f'{key}: duplicate {join_key!r} keys in selected rows')
    return rows


def _strip_markdown_links(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] == ']':
            j = i + 1
            while j < n and text[j] in ' \t':
                j += 1
            if j < n and text[j] == '(':
                k = j + 1
                while k < n and text[k] in ' \t':
                    k += 1
                depth = 1
                if k < n and text[k] == '<':
                    k += 1
                    while k < n and text[k] not in '>\n':
                        k += 2 if text[k] == '\\' else 1
                    if k < n and text[k] == '>':
                        k += 1
                else:
                    while k < n and text[k] not in ' \t\n':
                        ch = text[k]
                        if ch == '\\':
                            k += 2
                            continue
                        if ch == '(':
                            depth += 1
                        elif ch == ')':
                            depth -= 1
                            if depth == 0:
                                k += 1
                                break
                        k += 1
                if depth > 0:
                    while k < n and text[k] in ' \t':
                        k += 1
                    if k < n and text[k] in ('"', "'", '('):
                        close_q = ')' if text[k] == '(' else text[k]
                        k += 1
                        while k < n and text[k] != '\n':
                            if text[k] == '\\':
                                k += 2
                                continue
                            if text[k] == close_q:
                                k += 1
                                break
                            k += 1
                    while k < n and text[k] in ' \t':
                        k += 1
                    if k < n and text[k] == ')':
                        depth = 0
                        k += 1
                if depth == 0:
                    out.append(']')
                    i = k
                    continue
            elif j < n and text[j] == '[':
                k = j + 1
                depth = 1
                while k < n and text[k] != '\n' and depth > 0:
                    ch = text[k]
                    if ch == '\\':
                        k += 2
                        continue
                    if ch == '[':
                        depth += 1
                    elif ch == ']':
                        depth -= 1
                    k += 1
                if depth == 0:
                    out.append(']')
                    i = k
                    continue
        out.append(text[i])
        i += 1
    return ''.join(out)


NON_RENDERED_TAGS = {
    'audio', 'canvas', 'embed', 'head', 'iframe', 'math', 'noscript',
    'object', 'script', 'style', 'svg', 'template', 'title', 'video',
}
VOID_TAGS = {
    'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input',
    'link', 'meta', 'param', 'source', 'track', 'wbr',
}
HIDDEN_STYLE_RE = re.compile(
    r'(?:display\s*:\s*none|visibility\s*:\s*(?:hidden|collapse)|(?:opacity|font-size)\s*:\s*[+-]?(?:0+(?:\.0*)?|\.0+)(?:[a-z%]*)?(?![.0-9]))',
    re.I,
)


def _decode_css_text(val):
    no_comments = re.sub(r'/\*.*?\*/', '', val, flags=re.S)
    return re.sub(
        r'\\([0-9a-fA-F]{1,6})\s?|\\(.)',
        lambda m: chr(int(m.group(1), 16)) if m.group(1) else m.group(2),
        no_comments,
    )


def _tag_is_hidden(tag_lower, attrs):
    if tag_lower in NON_RENDERED_TAGS:
        return True
    for k, v in attrs:
        name = (k or '').lower()
        val = unescape(v or '').strip()
        if name == 'hidden':
            return True
        if name == 'aria-hidden' and val.lower() == 'true':
            return True
        if name == 'style' and HIDDEN_STYLE_RE.search(_decode_css_text(val)):
            return True
    return False


class _VisibleHTMLState(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.chunks = []

    def is_hidden(self):
        return bool(self.rawdata) or any(h for _, h in self.stack)

    def handle_starttag(self, tag, attrs):
        t = tag.lower()
        if ':' in t or t in VOID_TAGS:
            return
        hidden = any(h for _, h in self.stack) or _tag_is_hidden(t, attrs)
        self.stack.append((t, hidden))

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)

    def handle_endtag(self, tag):
        t = tag.lower()
        for idx in range(len(self.stack) - 1, -1, -1):
            if self.stack[idx][0] == t:
                del self.stack[idx:]
                break

    def handle_data(self, data):
        if not any(h for _, h in self.stack):
            self.chunks.append(data)

    def consume(self, segment):
        masked = re.sub(r'`+<!--.*?-->`+', ' ', segment)
        masked = re.sub(
            r'(`+)([^`]*?)\1',
            lambda m: m.group(1) + m.group(2).replace('<', '\x00LT\x00').replace('>', '\x00GT\x00') + m.group(1),
            masked,
        )
        self.chunks.clear()
        self.feed(masked)
        return ''.join(self.chunks).replace('\x00LT\x00', '<').replace('\x00GT\x00', '>')


def _check_visible_fact(segment, expected_str, html_state=None):
    state = _VisibleHTMLState() if html_state is None else html_state
    cleaned = state.consume(segment)
    if state.is_hidden():
        return False
    cleaned = re.sub(r'(?m)^\s*\[[^\]\n]+\]:.*$', ' ', cleaned)
    cleaned = _strip_markdown_links(cleaned)
    if re.fullmatch(r'-?\d+(?:\.\d+)?', expected_str):
        m = re.search(
            r'(?<![A-Za-z0-9_.\-/#])'
            r'(-?\d+(?:,\d{3})*(?:\.\d+)?|-?\d+(?:_\d{3})*(?:\.\d+)?|-?\d+(?:\.\d+)?)'
            r'(?![A-Za-z0-9_.\-])'
            r'[*_`\'")\]]*(?:[- ]+[A-Za-z`]+[*_`\'")\]]*)*\.?\s*$',
            cleaned,
        )
        if not m:
            return False
        return m.group(1).replace(',', '').replace('_', '') == expected_str
    return bool(expected_str) and bool(
        re.search(r'(?<![A-Za-z0-9_.-])' + re.escape(expected_str) + r'(?![A-Za-z0-9_.-])[*_`\'")\]]*\s*$', cleaned)
    )


def _parse_csv_scalar(raw_val, val_type, key):
    if val_type == 'int':
        if not re.fullmatch(r'-?\d+', str(raw_val)):
            raise ValueError(f'{key}: expected integer CSV value, got {raw_val!r}')
        return int(raw_val)
    if val_type == 'number':
        try:
            d = Decimal(str(raw_val))
        except InvalidOperation as exc:
            raise ValueError(f'{key}: expected numeric CSV value, got {raw_val!r}') from exc
        if not d.is_finite():
            raise ValueError(f'{key}: non-finite numeric CSV value {raw_val!r}')
        return int(d) if d == int(d) else float(d.normalize())
    return raw_val


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
            if not isinstance(rule.get('where'), dict) or not rule['where']:
                raise ValueError(f'{key}: csv provider requires non-empty where selector')
            rows = [r for r in csv.DictReader(_csv_stream(source)) if all(r.get(k) == v for k, v in rule['where'].items())]
            if len(rows) != 1:
                raise ValueError(f'{key}: expected one CSV row, got {len(rows)}')
            col = rule.get('column', '')
            if col not in rows[0] or rows[0][col] in (None, ''):
                raise ValueError(f'{key}: missing or empty CSV column {col!r}')
            value = _parse_csv_scalar(rows[0][col], rule.get('type'), key)
        elif rule['kind'] == 'csv-meta':
            meta = _csv_meta(source)
            meta_key = rule.get('key', '')
            if meta_key == 'set_status':
                target_set = rule.get('set', '')
                measured = {s for s in meta.get('measured_sets', '').split(',') if s}
                retained = {s for s in meta.get('retained_sets', '').split(',') if s}
                if target_set in measured and target_set not in retained:
                    value = 'measured'
                elif target_set in retained and target_set not in measured:
                    value = 'retained'
                else:
                    raise ValueError(f'{key}: ambiguous or missing set_status for {target_set!r} in {rule["source"]}')
            else:
                if not meta_key or meta_key not in meta:
                    raise ValueError(f'{key}: missing CSV metadata key {meta_key!r} in {rule["source"]}')
                raw_meta = meta[meta_key]
                subkey = rule.get('subkey')
                if subkey is not None:
                    pairs = {}
                    for item in raw_meta.split(','):
                        if ':' not in item:
                            raise ValueError(f'{key}: malformed key:value entry {item!r} in {meta_key!r}')
                        sk, sv = item.split(':', 1)
                        if not sk or not sv or sk in pairs:
                            raise ValueError(f'{key}: duplicate or empty subkey {sk!r} in {meta_key!r}')
                        pairs[sk] = sv
                    if subkey not in pairs:
                        raise ValueError(f'{key}: missing subkey {subkey!r} in {meta_key!r}')
                    raw_meta = pairs[subkey]
                value = _parse_csv_scalar(raw_meta, rule.get('type'), key)
        elif rule['kind'] == 'csv-aggregate':
            rows = _select_csv_rows(source, rule, key)
            op = rule.get('op')
            precision = int(rule.get('precision', 4))
            if op == 'count':
                value = len(rows)
            elif op == 'sum':
                col = rule['column']
                vals = [Decimal(r[col]) for r in rows]
                total = sum(vals, Decimal(0))
                value = _parse_csv_scalar(str(total), rule.get('type', 'int'), key)
            elif op == 'max':
                col = rule['column']
                vals = [Decimal(r[col]) for r in rows]
                mx = max(vals)
                value = _parse_csv_scalar(str(mx), rule.get('type', 'int'), key)
            elif op == 'ratio':
                col = rule['column']
                num = sum((Decimal(r[col]) for r in rows), Decimal(0))
                if 'denominator_column' in rule:
                    den_col = rule['denominator_column']
                    den = sum((Decimal(r[den_col]) for r in rows), Decimal(0))
                elif 'denominator_where' in rule:
                    den_rows = _select_csv_rows(source, {'where': rule['denominator_where']}, f'{key}.denominator')
                    join_key = rule.get('join_key', 'image')
                    num_by_key = {r[join_key]: r for r in rows}
                    den_by_key = {r[join_key]: r for r in den_rows}
                    if len(num_by_key) != len(rows) or len(den_by_key) != len(den_rows) or set(num_by_key) != set(den_by_key):
                        raise ValueError(f'{key}: ratio denominator rows do not match numerator keys on {join_key!r}')
                    den_col = rule.get('denominator_where_column', col)
                    den = sum((Decimal(r[den_col]) for r in den_rows), Decimal(0))
                else:
                    raise ValueError(f'{key}: ratio requires denominator_column or denominator_where')
                if den <= 0:
                    raise ValueError(f'{key}: non-positive ratio denominator {den}')
                value = f'{float(num / den):.{precision}f}'
            elif op == 'geomean':
                col = rule['column']
                if 'denominator_column' in rule:
                    den_col = rule['denominator_column']
                    ratios = [float(Decimal(r[col]) / Decimal(r[den_col])) for r in rows]
                elif 'denominator_where' in rule:
                    den_rows = _select_csv_rows(source, {'where': rule['denominator_where']}, f'{key}.denominator')
                    join_key = rule.get('join_key', 'image')
                    num_by_key = {r[join_key]: r for r in rows}
                    den_by_key = {r[join_key]: r for r in den_rows}
                    if len(num_by_key) != len(rows) or len(den_by_key) != len(den_rows) or set(num_by_key) != set(den_by_key):
                        raise ValueError(f'{key}: geomean denominator rows do not match numerator keys on {join_key!r}')
                    den_col = rule.get('denominator_where_column', col)
                    ratios = [float(Decimal(r[col]) / Decimal(den_by_key[r[join_key]][den_col])) for r in rows]
                else:
                    raise ValueError(f'{key}: geomean requires denominator_column or denominator_where')
                if any(v <= 0 for v in ratios):
                    raise ValueError(f'{key}: non-positive ratio in geomean')
                gm = math.exp(sum(math.log(v) for v in ratios) / len(ratios))
                value = f'{gm:.{precision}f}'
            else:
                raise ValueError(f'{key}: unknown csv-aggregate op {op!r}')
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
            prose_text = prose(text) if suffix in ('.md', '.markdown') else text
            masked_prose = (
                re.sub(
                    r'(`+)(.+?)\1',
                    lambda m: m.group(1) + m.group(2).replace('<', '\x00LT\x00').replace('>', '\x00GT\x00') + m.group(1),
                    prose_text,
                    flags=re.S,
                )
                if suffix in ('.md', '.markdown')
                else prose_text
            )
            prose_lines = masked_prose.splitlines()
            scan = code_spans(prose_text) if suffix in ('.md', '.markdown') else text
            if role == 'template' or rel == MANIFEST:
                continue
            html_state = _VisibleHTMLState()
            for line_no, line in enumerate(scan.splitlines(), 1):
                raw_line = prose_lines[line_no - 1] if line_no - 1 < len(prose_lines) else line
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
                prev_fact_end = 0
                for match in FACT.finditer(line):
                    value = self.facts.get(match[1])
                    segment = raw_line[prev_fact_end:match.start()]
                    prev_fact_end = match.end()
                    if match[1] not in self.facts or str(value) != match[2]:
                        html_state.consume(segment)
                        self.error('fact', rel, f'line {line_no}: {match[1]} must equal {value!r}; review the surrounding sentence')
                    elif not _check_visible_fact(segment, str(value), html_state):
                        self.error('fact', rel, f'line {line_no}: visible figure before {match[1]} does not match {value!r}; review the surrounding sentence')
                    html_state.consume(raw_line[match.start():match.end()])
                html_state.consume(raw_line[prev_fact_end:] + '\n')
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

    def _entry_state(self, rel):
        try:
            p = self.root if rel == '.' else self.path(rel)
        except ValueError as exc:
            return ('invalid', str(exc))
        try:
            st = p.lstat()
        except FileNotFoundError:
            return ('absent', None)
        meta = (st.st_mode, st.st_uid, st.st_gid, st.st_mtime_ns)
        if p.is_file():
            return ('file', *meta, st.st_size, digest(p.read_bytes()))
        if p.is_dir():
            children = tuple(
                sorted(
                    c.name
                    for c in p.iterdir()
                    if c.name not in ('__pycache__', '.git', 'build', 'bin', '.venv')
                )
            )
            return ('dir', *meta, children)
        return ('other', *meta, st.st_size)

    def _watched_paths(self):
        paths = {
            '.',
            *self.manifest.get('files', {}).keys(),
            *(gen['script'] for gen in self.manifest['generators']),
            *(out for g in self.manifest['generators'] for out in g.get('outputs', [])),
            *(out for g in self.manifest['generators'] for out in g.get('planned_outputs', {})),
        }
        for top in ('docs', 'todo', 'research', 'spec', 'scripts'):
            base = self.root / top
            if base.exists() or base.is_symlink():
                paths.add(top)
                if base.is_dir() and not base.is_symlink():
                    for p in base.rglob('*'):
                        if '__pycache__' not in p.parts:
                            paths.add(p.relative_to(self.root).as_posix())
        for rel in list(paths):
            if rel != '.':
                for parent in Path(rel).parents:
                    if str(parent) != '.':
                        paths.add(parent.as_posix())
        return sorted(paths)

    def _snapshot_paths(self, paths):
        return {rel: self._entry_state(rel) for rel in paths}

    def generators(self, fix=False):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
        if self.git_dir:
            env.update(GIT_DIR=str(self.git_dir), GIT_WORK_TREE=str(self.root))
        for gen in self.manifest['generators']:
            if self._entry_state(gen['script'])[0] != 'file':
                if gen.get('planned_owner'):
                    continue
                self.error('generator', gen['script'], 'required generator missing')
                continue
            for out, owner in gen.get('planned_outputs', {}).items():
                if self._entry_state(out)[0] != 'absent':
                    self.error('generator', gen['script'], f'planned output {out} ({owner}) exists before being promoted to outputs')
            for out in gen.get('outputs', []):
                if not fix and self._entry_state(out)[0] != 'file':
                    self.error('generator', gen['script'], f'missing generated output: {out}')
            before_snap = self._snapshot_paths(self._watched_paths()) if not fix else None
            args = gen['fix'] if fix else gen['check']
            result = subprocess.run([sys.executable, gen['script'], *args], cwd=self.root, env=env, capture_output=True, text=True, timeout=120)
            if result.returncode:
                self.error('generator', gen['script'], (result.stdout + result.stderr).strip()[-1500:])
            for out, owner in gen.get('planned_outputs', {}).items():
                if self._entry_state(out)[0] != 'absent' and (before_snap is None or before_snap.get(out) == ('absent', None)):
                    self.error('generator', gen['script'], f'planned output {out} ({owner}) exists before being promoted to outputs')
            if not fix:
                after_snap = self._snapshot_paths(sorted(set(before_snap) | set(self._watched_paths())))
                for rel in sorted(after_snap):
                    if after_snap[rel] != before_snap.get(rel, ('absent', None)):
                        self.error('generator', gen['script'], f'--check modified {rel}; check mode must be read-only')
            else:
                for out in gen.get('outputs', []):
                    if self._entry_state(out)[0] != 'file':
                        self.error('generator', gen['script'], f'missing generated output: {out}')

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
