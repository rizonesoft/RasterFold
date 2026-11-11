#!/usr/bin/env python3
"""Check document contracts; repair only deterministic, explicitly owned output.

No network, third-party packages, history rewrites, or automatic review receipts.
The manifest defines current documents, historical/planned evidence and generators.
"""
from __future__ import annotations

import argparse
import ast
import codecs
import csv
from datetime import datetime, timedelta, timezone
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
import shutil
import string
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
COMMIT = re.compile(r'<!-- drift:commit ([0-9a-f]{7,40})(?: ((?:(?!-->)\S)+))? -->')
DRIFT_FACT_TRAILER = re.compile(r'^[ \t]*drift-fact(?:\s*:|[ \t]+[a-z0-9_.-]+[ \t]*=)', re.I)
LINK = re.compile(r'!?\[[^\]\n]*\]\((?:<([^>\n]+)>|([^\s)]+))(?:\s+["\'][^\n]*?["\'])?\)')
REFERENCE = re.compile(
    r'^(?: {0,3}>[ \t]?)* {0,3}\[(?!\^)([^\]\n]+)\]:[ \t]*(?:\r?\n(?: {0,3}>[ \t]?)*[ \t]*)?(?:<([^>\n]+)>|([^\s>]+))'
    r'(?:(?:[ \t]+|\r?\n(?: {0,3}>[ \t]?)*[ \t]*)(?:"(?:\\.|(?!\r?\n[ \t]*\r?\n)[^"])*"|\'(?:\\.|(?!\r?\n[ \t]*\r?\n)[^\'])*\'|\((?:\\.|(?!\r?\n[ \t]*\r?\n)[^)])*\))[ \t]*(?=\r?\n|\Z))?',
    re.M,
)
HTML_BLOCK_TAG_RE = re.compile(
    r'^ {0,3}<(?:!--|!\[CDATA\[|\?|'
    r'/?(?:address|article|aside|base|basefont|blockquote|body|caption|center|col|colgroup|'
    r'dd|details|dialog|dir|div|dl|dt|fieldset|figcaption|figure|footer|form|frame|frameset|'
    r'h[1-6]|head|header|hr|html|iframe|legend|li|link|main|menu|menuitem|nav|noframes|'
    r'ol|optgroup|option|p|param|pre|script|search|section|style|summary|table|tbody|td|'
    r'tfoot|th|thead|title|tr|track|ul)(?:[\s/>]|$))',
    re.I,
)
BYTE_UNITS = {
    '': 1, 'b': 1, 'byte': 1, 'bytes': 1,
    'kib': 1024, 'mib': 1024 ** 2, 'gib': 1024 ** 3, 'tib': 1024 ** 4,
    'kb': 1000, 'mb': 1000 ** 2, 'gb': 1000 ** 3, 'tb': 1000 ** 4,
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def blank(text):
    return re.sub(r'[^\r\n]', ' ', text)


def _leading_cols(s):
    cols = 0
    for ch in s:
        if ch == ' ':
            cols += 1
        elif ch == '\t':
            cols = (cols // 4 + 1) * 4
        else:
            break
    return cols


def prose(text):
    """Mask fenced and indented code blocks while preserving original byte-decoded offsets."""
    lines, fence = [], None
    fence_bq_depth = 0
    in_indented_code = False
    indented_bq_depth = 0
    prev_bq_depth = 0
    can_start_indented_code = True
    list_indent = 0
    for line in text.splitlines(keepends=True):
        bq_match = re.match(r'^(?: {0,3}>[ \t]?)*', line)
        bq_depth = bq_match[0].count('>')
        inner = line[bq_match.end():]
        if fence and bq_depth < fence_bq_depth:
            fence = None
            fence_bq_depth = 0
            can_start_indented_code = True
            list_indent = 0
        if in_indented_code and bq_depth != indented_bq_depth:
            in_indented_code = False
            indented_bq_depth = 0
            can_start_indented_code = True
            list_indent = 0
        if bq_depth > prev_bq_depth and not fence and not in_indented_code:
            can_start_indented_code = True
            list_indent = 0
        prev_bq_depth = bq_depth
        match = re.match(r'^ {0,3}(`{3,}|~{3,})', inner)
        if match and fence is None and not in_indented_code:
            fence = match[1]
            fence_bq_depth = bq_depth
            can_start_indented_code = True
            list_indent = 0
            lines.append(blank(line))
            continue
        if fence:
            if bq_depth == fence_bq_depth and re.match(r'^ {0,3}' + re.escape(fence[0]) + '{' + str(len(fence)) + r',}[ \t]*\r?$', inner):
                fence = None
                fence_bq_depth = 0
                can_start_indented_code = True
            lines.append(blank(line))
            continue
        if not inner.strip():
            if in_indented_code:
                lines.append(blank(line))
            else:
                can_start_indented_code = True
                lines.append(line)
            continue
        cols = _leading_cols(inner)
        if cols < list_indent:
            list_indent = 0
        code_threshold = list_indent + 4
        if in_indented_code:
            if cols >= code_threshold:
                lines.append(blank(line))
                continue
            in_indented_code = False
            indented_bq_depth = 0
        if can_start_indented_code and cols >= code_threshold:
            in_indented_code = True
            indented_bq_depth = bq_depth
            lines.append(blank(line))
            continue
        m_list = re.match(r'^( {0,3}(?:[-*+]|\d+[.)])[ \t]+)', inner)
        if m_list:
            list_indent = len(m_list[1].expandtabs(4))
            can_start_indented_code = False
        elif (
            re.match(
                r'^ {0,3}(?:#{1,6}(?:[ \t]|$)|(?:=+|-+)[ \t]*$|(?:\*[ \t]*){3,}$|(?:-[ \t]*){3,}$|(?:_[ \t]*){3,}$)',
                inner.rstrip('\r\n'),
            )
            or HTML_BLOCK_TAG_RE.match(inner)
        ):
            can_start_indented_code = True
        else:
            can_start_indented_code = False
        lines.append(line)
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


def _skip_inline_ws(text, pos):
    n = len(text)
    newlines = 0
    while pos < n:
        ch = text[pos]
        if ch in (' ', '\t', '\r'):
            pos += 1
        elif ch == '\n':
            newlines += 1
            if newlines > 1:
                return None
            pos += 1
            m_bq = re.match(r'(?: {0,3}>[ \t]?)+', text[pos:])
            if m_bq:
                pos += m_bq.end()
        else:
            break
    return pos


def _parse_inline_link_at(text, bracket_pos):
    n = len(text)
    if bracket_pos > 0 and text[bracket_pos - 1] == '\\' and (bracket_pos < 2 or text[bracket_pos - 2] != '\\'):
        return None
    depth = 0
    i = bracket_pos
    while i < n:
        ch = text[i]
        if ch == '\\':
            i += 2
            continue
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
            if depth == 0:
                break
        elif ch == '\n' and re.match(r'[ \t]*\r?\n', text[i + 1:]):
            return None
        i += 1
    else:
        return None
    close_bracket = i
    if close_bracket + 1 >= n or text[close_bracket + 1] != '(':
        return None
    label = text[bracket_pos + 1:close_bracket]
    j = _skip_inline_ws(text, close_bracket + 2)
    if j is None or j >= n:
        return None
    if text[j] == '<':
        k = j + 1
        while k < n and text[k] not in ('>', '\n', '\r', '<'):
            if text[k] == '\\':
                k += 2
                continue
            k += 1
        if k >= n or text[k] != '>':
            return None
        dest = text[j + 1:k]
        m = k + 1
    else:
        k = j
        pdepth = 0
        while k < n:
            ch = text[k]
            if ch == '\\':
                k += 2
                continue
            if ch in (' ', '\t', '\n', '\r'):
                break
            if ch == '(':
                pdepth += 1
            elif ch == ')':
                if pdepth == 0:
                    break
                pdepth -= 1
            k += 1
        dest = text[j:k]
        m = k
    m = _skip_inline_ws(text, m)
    if m is None or m >= n:
        return None
    if text[m] in ('"', "'", '('):
        q = ')' if text[m] == '(' else text[m]
        m += 1
        while m < n and text[m] not in (q, '\n', '\r'):
            if text[m] == '\\':
                m += 2
                continue
            m += 1
        if m >= n or text[m] != q:
            return None
        m += 1
        m = _skip_inline_ws(text, m)
        if m is None or m >= n:
            return None
    if text[m] != ')':
        return None
    return label, dest, m + 1


def _find_closing_bracket(text, bracket_pos):
    n = len(text)
    if bracket_pos > 0 and text[bracket_pos - 1] == '\\' and (bracket_pos < 2 or text[bracket_pos - 2] != '\\'):
        return None
    depth = 0
    i = bracket_pos
    while i < n:
        ch = text[i]
        if ch == '\\':
            i += 2
            continue
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
            if depth == 0:
                return i
        elif ch == '\n' and re.match(r'[ \t]*\r?\n', text[i + 1:]):
            return None
        i += 1
    return None


def _parse_reference_link_at(text, bracket_pos):
    close_bracket = _find_closing_bracket(text, bracket_pos)
    if close_bracket is None or close_bracket + 1 >= len(text) or text[close_bracket + 1] != '[':
        return None
    if bracket_pos == 0 or text[bracket_pos - 1] not in ('\n', '\r'):
        pass
    close_ref = _find_closing_bracket(text, close_bracket + 1)
    if close_ref is None:
        return None
    ref_raw = text[close_bracket + 2:close_ref]
    if '\n' in ref_raw or '\r' in ref_raw or ref_raw.startswith('^'):
        return None
    label = text[bracket_pos + 1:close_bracket]
    if label.startswith('^'):
        return None
    ref_name = (ref_raw if ref_raw.strip() else label).strip()
    if not ref_name:
        return None
    return label, ref_name, close_ref + 1


def _extract_markdown_links(text):
    out = []
    for idx, ch in enumerate(text):
        if ch == '[':
            res = _parse_inline_link_at(text, idx)
            if res is not None and res[1]:
                out.append(res[1])
    out.extend(m[2] or m[3] for m in REFERENCE.finditer(text))
    return out


def _extract_unresolved_references(text):
    defined = {re.sub(r'\s+', ' ', m[1].strip()).casefold() for m in REFERENCE.finditer(text)}
    scan_text = REFERENCE.sub(lambda m: blank(m[0]), text)
    unresolved = []
    i = 0
    n = len(scan_text)
    while i < n:
        if scan_text[i] == '\\':
            i += 2
            continue
        if scan_text[i] == '!' and i + 1 < n and scan_text[i + 1] == '[':
            inline_img = _parse_inline_link_at(scan_text, i + 1)
            if inline_img is not None:
                i = inline_img[2]
                continue
        if scan_text[i] == '[':
            inline = _parse_inline_link_at(scan_text, i)
            if inline is not None:
                i = inline[2]
                continue
            ref = _parse_reference_link_at(scan_text, i)
            if ref is not None:
                _, ref_name, end_pos = ref
                key = re.sub(r'\s+', ' ', ref_name).casefold()
                if key not in defined:
                    unresolved.append(ref_name)
                i = end_pos
                continue
        i += 1
    return unresolved


def _strip_markdown_links_to_text(raw):
    out = []
    i = 0
    n = len(raw)
    while i < n:
        if raw[i] == '!' and i + 1 < n and raw[i + 1] == '[':
            res = _parse_inline_link_at(raw, i + 1) or _parse_reference_link_at(raw, i + 1)
            if res is not None:
                label, _, end_pos = res
                out.append(_strip_markdown_links_to_text(label))
                i = end_pos
                continue
        if raw[i] == '[':
            res = _parse_inline_link_at(raw, i) or _parse_reference_link_at(raw, i)
            if res is not None:
                label, _, end_pos = res
                out.append(_strip_markdown_links_to_text(label))
                i = end_pos
                continue
        out.append(raw[i])
        i += 1
    return ''.join(out)


def _register_heading_anchor(raw_title, found, seen):
    cleaned = _strip_markdown_links_to_text(raw_title)
    title = re.sub(r'<[^>]*>', '', unescape(cleaned)).strip().lower()
    title = re.sub(r'[^\w\-\s]', '', title).replace(' ', '-')
    if not title:
        return
    n = seen.get(title, 0)
    found.add(title + (f'-{n}' if n else ''))
    seen[title] = n + 1


def anchors(text, markdown=True):
    html = HTML()
    html.feed(prose(text) if markdown else text)
    found, seen = html.anchors, {}
    if markdown:
        body = re.sub(r'\A---\r?\n.*?\r?\n(?:---|\.\.\.)[ \t]*(?:\r?\n|\Z)', '', text, count=1, flags=re.S)
        prose_body = REFERENCE.sub(lambda m: blank(m[0]), prose(body))
        para_lines = []
        for line in prose_body.splitlines():
            atx = re.match(r'^ {0,3}#{1,6}(?:[ \t]+(.+?))?[ \t]*#*[ \t]*$', line)
            if atx:
                para_lines.clear()
                if atx[1]:
                    _register_heading_anchor(atx[1], found, seen)
                continue
            setext = re.match(r'^ {0,3}(=+|-+)[ \t]*$', line)
            if setext and para_lines:
                _register_heading_anchor(' '.join(para_lines), found, seen)
                para_lines.clear()
                continue
            if (
                not line.strip()
                or re.match(
                    r'^(?: {4,}| {0,3}\t| {0,3}(?:>|[-*+][ \t]|\d+[.)][ \t]|\||(?:\*[ \t]*){3,}$|(?:_[ \t]*){3,}$|(?:-[ \t]*){3,}$))',
                    line,
                )
                or HTML_BLOCK_TAG_RE.match(line)
            ):
                para_lines.clear()
            else:
                para_lines.append(line.strip())
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
            if '=' not in token:
                return {}
            k, v = token.split('=', 1)
            k, v = k.strip(), v.strip()
            if not k or not v or k in meta:
                return {}
            meta[k] = v
    return meta


def _parse_meta_map(raw):
    if not (raw or '').strip():
        return {}
    out = {}
    for item in raw.split(','):
        item = item.strip()
        if not item or ':' not in item:
            return {}
        k, v = item.split(':', 1)
        k, v = k.strip(), v.strip()
        if not k or not v or k in out:
            return {}
        out[k] = v
    return out


def _extract_kat_fold_sha256(tc_text, tc_rel='tests/test_correctness.py'):
    try:
        tree = ast.parse(tc_text, filename=tc_rel)
    except SyntaxError as exc:
        raise ValueError(f'syntax error in {tc_rel}: {exc}') from exc
    matches = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name) and node.targets[0].id == 'KAT_FOLD':
            matches.append(node.value)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.target.id == 'KAT_FOLD' and node.value is not None:
            matches.append(node.value)
    if len(matches) != 1:
        raise ValueError(f'missing or duplicate KAT_FOLD assignment in {tc_rel}')
    call = matches[0]
    if (
        not isinstance(call, ast.Call)
        or not isinstance(call.func, ast.Attribute)
        or not isinstance(call.func.value, ast.Name)
        or call.func.value.id != 'bytes'
        or call.func.attr != 'fromhex'
        or len(call.args) != 1
        or call.keywords
        or not isinstance(call.args[0], ast.Constant)
        or not isinstance(call.args[0].value, str)
    ):
        raise ValueError(f'unsupported KAT_FOLD definition in {tc_rel}')
    try:
        raw = bytes.fromhex(call.args[0].value)
    except ValueError as exc:
        raise ValueError(f'invalid KAT_FOLD hex literal in {tc_rel}: {exc}') from exc
    if not raw:
        raise ValueError(f'empty KAT_FOLD in {tc_rel}')
    return digest(raw)


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


def _has_blank_line(text, start, end):
    return bool(re.search(r'\n[ \t]*\n', text[start:end]))


def _scan_link_tail(text, close_b, allow_newlines=False):
    n = len(text)
    ws = ' \t\n' if allow_newlines else ' \t'
    stop_nl = '' if allow_newlines else '\n'
    j = close_b + 1
    while j < n and text[j] in ws:
        j += 1
    if allow_newlines and _has_blank_line(text, close_b, j):
        return None
    if j < n and text[j] == '(':
        k = j + 1
        while k < n and text[k] in ws:
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
            while k < n and text[k] in ws:
                k += 1
            if k < n and text[k] in ('"', "'", '('):
                close_q = ')' if text[k] == '(' else text[k]
                k += 1
                while k < n and (allow_newlines or text[k] != '\n'):
                    if text[k] == '\\':
                        k += 2
                        continue
                    if text[k] == close_q:
                        k += 1
                        break
                    k += 1
            while k < n and text[k] in ws:
                k += 1
            if k < n and text[k] == ')':
                depth = 0
                k += 1
        if depth == 0 and not (allow_newlines and _has_blank_line(text, close_b, k)):
            return k
    elif j < n and text[j] == '[':
        k = j + 1
        depth = 1
        while k < n and (allow_newlines or text[k] != '\n') and depth > 0:
            ch = text[k]
            if ch == '\\':
                k += 2
                continue
            if ch == '[':
                depth += 1
            elif ch == ']':
                depth -= 1
            k += 1
        if depth == 0 and not (allow_newlines and _has_blank_line(text, close_b, k)):
            return k
    return None


def _is_escaped(text, idx):
    bs = 0
    p = idx - 1
    while p >= 0 and text[p] == '\\':
        bs += 1
        p -= 1
    return (bs % 2) == 1


def _strip_markdown_images(text, allow_newlines=False):
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text[i:i + 2] == '![' and not _is_escaped(text, i):
            k = i + 2
            depth = 1
            while k < n and (allow_newlines or text[k] != '\n') and depth > 0:
                ch = text[k]
                if ch == '\\':
                    k += 2
                    continue
                if ch == '[':
                    depth += 1
                elif ch == ']':
                    depth -= 1
                k += 1
            if depth == 0 and not (allow_newlines and _has_blank_line(text, i, k)):
                close_b = k - 1
                tail_end = _scan_link_tail(text, close_b, allow_newlines=allow_newlines)
                end_idx = tail_end if tail_end is not None else k
                out.append(re.sub(r'[^\n]', ' ', text[i:end_idx]))
                i = end_idx
                continue
            if not allow_newlines and depth > 0:
                out.append(' ')
                break
        out.append(text[i])
        i += 1
    return ''.join(out)


def _strip_markdown_links(text, allow_newlines=False):
    text = _strip_markdown_images(text, allow_newlines=allow_newlines)
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] == ']' and not _is_escaped(text, i):
            tail_end = _scan_link_tail(text, i, allow_newlines=allow_newlines)
            if tail_end is not None:
                out.append(']' + re.sub(r'[^\n]', ' ', text[i + 1:tail_end]))
                i = tail_end
                continue
        out.append(text[i])
        i += 1
    return ''.join(out)


def _mask_document_markdown(text):
    no_comments = re.sub(r'<!--.*?-->', lambda m: ' ' * len(m.group(0)), text, flags=re.S)
    stripped = _strip_markdown_links(no_comments, allow_newlines=True)
    stripped = re.sub(r'(?m)^\s*\[[^\]\n]+\]:.*$', lambda m: ' ' * len(m.group(0)), stripped)
    chars = list(text)
    for idx, (orig_ch, mask_ch) in enumerate(zip(text, stripped)):
        if orig_ch != mask_ch and orig_ch != '\n':
            chars[idx] = ' '
    return ''.join(chars)


NON_RENDERED_TAGS = {
    'audio', 'canvas', 'embed', 'head', 'iframe', 'math', 'noscript',
    'object', 'script', 'style', 'svg', 'template', 'title', 'video',
}
VOID_TAGS = {
    'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input',
    'link', 'meta', 'param', 'source', 'track', 'wbr',
}
CSS_NUM_PART = r'(?:\d+(?:\.\d+)?|\.\d+)(?:e[+-]?\d+)?'
VALID_DISPLAY_VALUES = {
    'none', 'inline', 'block', 'inline-block', 'flex', 'inline-flex',
    'grid', 'inline-grid', 'table', 'inline-table', 'table-row', 'table-cell',
    'table-column', 'table-row-group', 'table-header-group',
    'table-footer-group', 'table-column-group', 'table-caption', 'list-item',
    'contents', 'flow-root', 'inherit', 'initial', 'unset', 'revert', 'revert-layer',
}
VALID_VISIBILITY_VALUES = {
    'visible', 'hidden', 'collapse', 'inherit', 'initial', 'unset', 'revert', 'revert-layer',
}
VALID_CONTENT_VISIBILITY_VALUES = {
    'visible', 'auto', 'hidden', 'inherit', 'initial', 'unset', 'revert', 'revert-layer',
}
VALID_FONT_SIZE_KEYWORDS = {
    'xx-small', 'x-small', 'small', 'medium', 'large', 'x-large', 'xx-large',
    'xxx-large', 'smaller', 'larger', 'inherit', 'initial', 'unset', 'revert', 'revert-layer',
}
VALID_OPACITY_RE = re.compile(rf'[+-]?{CSS_NUM_PART}%?', re.I)
VALID_FONT_SIZE_LEN_RE = re.compile(
    rf'[+-]?(?:0+(?:\.0+)?|\.0+)(?:e[+-]?\d+)?|'
    rf'\+?{CSS_NUM_PART}(?:px|em|rem|ex|ch|vw|vh|vmin|vmax|cm|mm|in|pt|pc|q|%)',
    re.I,
)


def _is_valid_css_value(prop, val_lower):
    if prop == 'display':
        return val_lower in VALID_DISPLAY_VALUES
    if prop == 'visibility':
        return val_lower in VALID_VISIBILITY_VALUES
    if prop == 'content-visibility':
        return val_lower in VALID_CONTENT_VISIBILITY_VALUES
    if prop == 'opacity':
        return val_lower in {'inherit', 'initial', 'unset', 'revert', 'revert-layer'} or bool(VALID_OPACITY_RE.fullmatch(val_lower))
    if prop == 'font-size':
        return val_lower in VALID_FONT_SIZE_KEYWORDS or bool(VALID_FONT_SIZE_LEN_RE.fullmatch(val_lower))
    return False


def _decode_css_text(val):
    no_comments = re.sub(r'/\*.*?\*/', '', val, flags=re.S)
    return re.sub(
        r'\\([0-9a-fA-F]{1,6})\s?|\\(.)',
        lambda m: chr(int(m.group(1), 16)) if m.group(1) else m.group(2),
        no_comments,
    )


def _style_is_hidden(style_text):
    css = _decode_css_text(style_text)
    effective = {}
    for decl in css.split(';'):
        if ':' not in decl:
            continue
        prop_raw, val_raw = decl.split(':', 1)
        prop = prop_raw.strip().lower()
        if prop not in ('display', 'visibility', 'content-visibility', 'opacity', 'font-size'):
            continue
        val = val_raw.strip()
        imp_match = re.search(r'!\s*important\s*$', val, re.I)
        imp = 1 if imp_match else 0
        if imp_match:
            val = val[:imp_match.start()].strip()
        val_lower = val.lower()
        if not val_lower or not _is_valid_css_value(prop, val_lower):
            continue
        prev_imp, _ = effective.get(prop, (-1, ''))
        if imp >= prev_imp:
            effective[prop] = (imp, val_lower)
    if effective.get('display', (0, ''))[1] == 'none':
        return True
    if effective.get('visibility', (0, ''))[1] in ('hidden', 'collapse'):
        return True
    if effective.get('content-visibility', (0, ''))[1] == 'hidden':
        return True
    op_val = effective.get('opacity', (0, ''))[1]
    if op_val and VALID_OPACITY_RE.fullmatch(op_val) and float(op_val.rstrip('%')) <= 0.0:
        return True
    fs_val = effective.get('font-size', (0, ''))[1]
    if fs_val and VALID_FONT_SIZE_LEN_RE.fullmatch(fs_val):
        fs_num = re.sub(r'(?:px|em|rem|ex|ch|vw|vh|vmin|vmax|cm|mm|in|pt|pc|q|%)$', '', fs_val, flags=re.I)
        if float(fs_num) <= 0.0:
            return True
    return False


def _tag_is_hidden(tag_lower, attrs):
    if tag_lower in NON_RENDERED_TAGS:
        return True
    for k, v in attrs:
        name = (k or '').lower()
        val = unescape(v or '').strip()
        if name == 'hidden':
            return True
        if name == 'style' and _style_is_hidden(val):
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
        masked = re.sub(r'`+<!--.*?-->`+', lambda m: ' ' * len(m.group(0)), segment)
        masked = re.sub(
            r'(`+)([^`]*?)\1',
            lambda m: m.group(1) + m.group(2).replace('<', '\x01').replace('>', '\x02') + m.group(1),
            masked,
        )
        self.chunks.clear()
        self.feed(masked)
        return ''.join(self.chunks).replace('\x01', '<').replace('\x02', '>')


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
        self.generator_before = {}
        self._before_generator_publish_hook = None

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

    def validate_manifest(self):
        coverage = self.manifest.get('coverage')
        if not isinstance(coverage, list):
            raise ValueError('invalid coverage rules in drift manifest')
        for idx, rule in enumerate(coverage):
            if (
                not isinstance(rule, dict)
                or not isinstance(rule.get('pattern'), str)
                or not rule['pattern'].strip()
                or rule.get('role') not in ROLES
                or not isinstance(rule.get('reason'), str)
                or not rule['reason'].strip()
            ):
                raise ValueError(f'invalid coverage rule at index {idx}')
        facts = self.manifest.get('facts')
        if not isinstance(facts, dict):
            raise ValueError('invalid facts table in drift manifest')
        valid_kinds = {'regex', 'json', 'csv', 'csv-meta', 'csv-aggregate'}
        for key, rule in facts.items():
            if not isinstance(key, str) or not key or not isinstance(rule, dict):
                raise ValueError(f'invalid fact definition: {key!r}')
            kind = rule.get('kind')
            if kind not in valid_kinds:
                raise ValueError(f'{key}: unknown provider kind {kind!r}')
            self.path(rule.get('source', ''))
            if kind == 'regex':
                if not isinstance(rule.get('pattern'), str) or not rule['pattern']:
                    raise ValueError(f'{key}: regex provider requires non-empty pattern')
                re.compile(rule['pattern'])
            elif kind == 'json':
                if not isinstance(rule.get('key'), str) or not rule['key']:
                    raise ValueError(f'{key}: json provider requires non-empty key string')
            elif kind == 'csv':
                if not isinstance(rule.get('where'), dict) or not isinstance(rule.get('column'), str) or not rule['column']:
                    raise ValueError(f'{key}: csv provider requires where dict and column string')
            elif kind == 'csv-meta':
                if not isinstance(rule.get('key'), str) or not rule['key']:
                    raise ValueError(f'{key}: csv-meta provider requires key string')
            elif kind == 'csv-aggregate':
                if rule.get('op') not in {'count', 'sum', 'max', 'ratio', 'geomean'}:
                    raise ValueError(f'{key}: csv-aggregate provider requires valid op')
        blocks = self.manifest.get('blocks')
        if not isinstance(blocks, dict):
            raise ValueError('invalid blocks table in drift manifest')
        formatter = string.Formatter()
        for name, block in blocks.items():
            if not isinstance(name, str) or not name or not isinstance(block, dict):
                raise ValueError(f'invalid block definition: {name!r}')
            self.path(block.get('path', ''))
            template = block.get('template')
            if not isinstance(template, str):
                raise ValueError(f'block {name}: template must be a string')
            for _, field_name, _, _ in formatter.parse(template):
                if field_name is not None and field_name not in facts:
                    raise ValueError(f'block {name}: unknown fact placeholder {field_name!r}')
        assertions = self.manifest.get('assertions', [])
        if not isinstance(assertions, list):
            raise ValueError('invalid assertions list in drift manifest')
        for idx, rule in enumerate(assertions):
            if (
                not isinstance(rule, dict)
                or not isinstance(rule.get('pattern'), str)
                or not rule['pattern'].strip()
                or not isinstance(rule.get('regex'), str)
                or not rule['regex']
                or rule.get('fact') not in facts
            ):
                raise ValueError(f'invalid assertion rule at index {idx}')
            re.compile(rule['regex'])
        reviews = self.manifest.get('reviews', {})
        if not isinstance(reviews, dict):
            raise ValueError('invalid reviews table in drift manifest')
        for doc, review in reviews.items():
            self.path(doc)
            if not isinstance(review, dict) or not isinstance(review.get('evidence'), str):
                raise ValueError(f'invalid review receipt for {doc}')
            sources = review.get('sources')
            if not isinstance(sources, dict) or not sources:
                raise ValueError(f'invalid review sources for {doc}')
            for src, sha in sources.items():
                self.path(src)
                if not isinstance(sha, str) or not re.fullmatch(r'[0-9a-f]{64}', sha):
                    raise ValueError(f'invalid review source digest for {doc}: {src}')
        generators = self.manifest.get('generators')
        if not isinstance(generators, list):
            raise ValueError('invalid generators list in drift manifest')
        for idx, gen in enumerate(generators):
            if not isinstance(gen, dict):
                raise ValueError(f'invalid generator entry at index {idx}')
            self.path(gen.get('script', ''))
            if (
                not isinstance(gen.get('check'), list)
                or not all(isinstance(a, str) for a in gen['check'])
                or not isinstance(gen.get('fix'), list)
                or not all(isinstance(a, str) for a in gen['fix'])
            ):
                raise ValueError(f'invalid generator command arguments for {gen.get("script")!r}')
            outputs = gen.get('outputs', [])
            if not isinstance(outputs, list):
                raise ValueError(f'invalid generator outputs for {gen.get("script")!r}')
            for out in outputs:
                self.path(out)
            cache_outputs = gen.get('cache_outputs', [])
            if not isinstance(cache_outputs, list):
                raise ValueError(f'invalid generator cache_outputs for {gen.get("script")!r}')
            for cout in cache_outputs:
                self.path(cout)
            planned = gen.get('planned_outputs', {})
            if not isinstance(planned, dict):
                raise ValueError(f'invalid generator planned_outputs for {gen.get("script")!r}')
            for out, owner in planned.items():
                self.path(out)
                if not isinstance(owner, str) or not owner.strip():
                    raise ValueError(f'invalid planned_output owner for {out}')
        encodings = self.manifest.get('encodings', {})
        if not isinstance(encodings, dict):
            raise ValueError('invalid encodings table in drift manifest')
        for rel, enc in encodings.items():
            self.path(rel)
            if not isinstance(enc, str) or not enc.strip():
                raise ValueError(f'invalid encoding for {rel}')
            try:
                codecs.lookup(enc)
            except LookupError as exc:
                raise ValueError(f'unknown encoding {enc!r} for {rel}') from exc
        site_sources = self.manifest.get('site_sources', [])
        if not isinstance(site_sources, list) or not all(isinstance(p, str) and p.strip() for p in site_sources):
            raise ValueError('invalid site_sources list in drift manifest')
        forbidden_site_paths = self.manifest.get('forbidden_site_paths', [])
        if not isinstance(forbidden_site_paths, list) or not all(isinstance(p, str) and p.strip() for p in forbidden_site_paths):
            raise ValueError('invalid forbidden_site_paths list in drift manifest')

    def load(self):
        self.manifest_bytes = self.path(MANIFEST).read_bytes()
        self.manifest = json.loads(self.manifest_bytes.decode('utf-8'))
        if self.manifest.get('schema') != 1:
            raise ValueError('unsupported drift manifest schema')
        self.validate_manifest()
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
            encoding = self.manifest.get('encodings', {}).get(rel, 'utf-8')
            try:
                text = original.decode(encoding)
            except UnicodeDecodeError as exc:
                self.error('encoding', rel, f'cannot decode as {encoding}: {exc}')
                continue
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
                _mask_document_markdown(
                    re.sub(
                        r'(`+)(.+?)\1',
                        lambda m: m.group(1) + m.group(2).replace('<', '\x01').replace('>', '\x02') + m.group(1),
                        prose_text,
                        flags=re.S,
                    )
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
                    commit_id, commit_path = match[1], match[2]
                    if commit_path is not None and (
                        not commit_path
                        or commit_path.startswith('/')
                        or commit_path.endswith('/')
                        or '\\' in commit_path
                        or '//' in commit_path
                        or '..' in commit_path.split('/')
                        or '.' in commit_path.split('/')
                    ):
                        self.error('commit', rel, f'line {line_no}: invalid committed path {commit_path}')
                        continue
                    try:
                        self.git('cat-file', '-e', f'{commit_id}^{{commit}}')
                        if commit_path:
                            self.git('cat-file', '-e', f'{commit_id}^{{commit}}:{commit_path}')
                    except subprocess.SubprocessError:
                        ref = commit_id + (':' + commit_path if commit_path else '^{commit}')
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
                self.links(rel, text, self.root)
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
        self.data_freshness()

    def data_freshness(self):
        data_files = [rel for rel in ('benchmarks/corpus.csv', 'benchmarks/results.csv') if rel in self.files]
        if not data_files:
            return
        tc_rel = 'tests/test_correctness.py'
        tc_path = self.path(tc_rel)
        if not tc_path.is_file():
            self.error('stale', tc_rel, 'missing tests/test_correctness.py required to verify KAT_FOLD data freshness')
            return
        tc_text = tc_path.read_text(encoding='utf-8')
        try:
            expected_kat = _extract_kat_fold_sha256(tc_text, tc_rel)
        except ValueError:
            self.error('stale', tc_rel, 'missing or invalid KAT_FOLD hex literal in tests/test_correctness.py')
            return

        if 'benchmarks/corpus.csv' in self.files and self.path('benchmarks/corpus.csv').is_file():
            meta = _csv_meta(self.read('benchmarks/corpus.csv'))
            if (
                not re.fullmatch(r'[0-9a-f]{40}', meta.get('codec_revision', ''))
                or not re.fullmatch(r'[0-9a-f]{64}', meta.get('kat_fold_sha256', ''))
                or not re.fullmatch(r'\d{4}-\d{2}-\d{2}', meta.get('date', ''))
            ):
                self.error(
                    'stale',
                    'benchmarks/corpus.csv',
                    'missing or malformed provenance header (codec_revision, kat_fold_sha256, date required); regenerate benchmarks/corpus.csv with python3 research/ab.py size all --csv benchmarks/corpus.csv',
                )
            elif meta['kat_fold_sha256'] != expected_kat:
                self.error(
                    'stale',
                    'benchmarks/corpus.csv',
                    f'KAT_FOLD changed in tests/test_correctness.py since benchmarks/corpus.csv was produced at {meta["codec_revision"]} (recorded {meta["kat_fold_sha256"]}, current {expected_kat}); regenerate benchmarks/corpus.csv with python3 research/ab.py size all --csv benchmarks/corpus.csv',
                )

        if 'benchmarks/results.csv' in self.files and self.path('benchmarks/results.csv').is_file():
            meta = _csv_meta(self.read('benchmarks/results.csv'))
            set_revs = _parse_meta_map(meta.get('set_revisions', ''))
            set_dates = _parse_meta_map(meta.get('set_dates', ''))
            set_kats = _parse_meta_map(meta.get('set_kat_hashes', ''))
            req_sets = ('measure', 'photos', 'synthetic')
            if (
                not re.fullmatch(r'[0-9a-f]{40}', meta.get('codec_revision', ''))
                or not re.fullmatch(r'[0-9a-f]{64}', meta.get('kat_fold_sha256', ''))
                or not re.fullmatch(r'\d{4}-\d{2}-\d{2}', meta.get('date', ''))
                or set(set_revs) != set(req_sets)
                or set(set_dates) != set(req_sets)
                or set(set_kats) != set(req_sets)
                or any(not re.fullmatch(r'[0-9a-f]{40}', set_revs.get(s, '')) for s in req_sets)
                or any(not re.fullmatch(r'\d{4}-\d{2}-\d{2}', set_dates.get(s, '')) for s in req_sets)
                or any(not re.fullmatch(r'[0-9a-f]{64}', set_kats.get(s, '')) for s in req_sets)
            ):
                self.error(
                    'stale',
                    'benchmarks/results.csv',
                    'missing or malformed provenance header (codec_revision, kat_fold_sha256, date, set_revisions, set_dates, set_kat_hashes required); regenerate benchmarks/results.csv with python3 tests/benchmark.py',
                )
            else:
                stale_sets = [s for s in req_sets if set_kats[s] != expected_kat]
                if meta['kat_fold_sha256'] != expected_kat or stale_sets:
                    stale_desc = ', '.join(stale_sets) if stale_sets else 'header'
                    self.error(
                        'stale',
                        'benchmarks/results.csv',
                        f'KAT_FOLD changed in tests/test_correctness.py since benchmarks/results.csv was produced (stale sets: {stale_desc}; recorded {meta["kat_fold_sha256"]}, current {expected_kat}); regenerate benchmarks/results.csv with python3 tests/benchmark.py',
                    )

    def review_policies(self, reviews=None):
        reviews = self.manifest.get('reviews', {}) if reviews is None else reviews
        receipt_sources = sorted({p for r in reviews.values() for p in r['sources']})
        attributes = {}
        if receipt_sources:
            raw = self.git('check-attr', '-z', 'eol', 'text', '--', *receipt_sources).decode().split('\0')
            for i in range(0, len(raw) - 1, 3):
                attributes.setdefault(raw[i], {})[raw[i + 1]] = raw[i + 2]
        for doc, review in reviews.items():
            evidence = str(review.get('evidence', '')).strip()
            if not evidence or not re.search(r'\b\d{4}-\d{2}-\d{2}\b', evidence):
                self.error('review', doc, 'receipt must record non-empty dated review evidence (YYYY-MM-DD)')
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
        is_md = Path(rel).suffix in ('.md', '.markdown')
        md_scan = code_spans(prose(text)) if is_md else text
        html = HTML()
        html.feed(md_scan)
        links = html.links
        if is_md:
            links += _extract_markdown_links(md_scan)
            for ref_name in _extract_unresolved_references(md_scan):
                self.error('link', rel, f'unresolved reference link: [{ref_name}]')
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

    def _allowlisted_site_contract(self):
        sc_rel = 'docs/site-content.json'
        if sc_rel not in self.files and not (self.root / sc_rel).is_file():
            return None
        sc_path = self.path(sc_rel)
        if not sc_path.is_file():
            raise ValueError('missing docs/site-content.json')
        site_cfg = json.loads(sc_path.read_text(encoding='utf-8'))
        if not isinstance(site_cfg, dict) or site_cfg.get('schema') != 1:
            raise ValueError('unsupported docs/site-content.json schema')
        patterns = self.manifest.get('site_sources', [])
        required_sources = [sc_rel]
        expected_outputs = []
        for entry in site_cfg.get('config', []):
            if not isinstance(entry, dict) or not isinstance(entry.get('source'), str) or not entry['source']:
                raise ValueError('invalid config entry in docs/site-content.json')
            required_sources.append(entry['source'])
            staged_cfg = entry.get('staged')
            if isinstance(staged_cfg, str) and not staged_cfg.startswith('_') and staged_cfg.endswith('.md'):
                expected_outputs.append(staged_cfg[:-3] + '.html')
        for page in site_cfg.get('pages', []):
            if not isinstance(page, dict) or not isinstance(page.get('source'), str) or not page['source'] or not isinstance(page.get('staged'), str) or not page['staged']:
                raise ValueError('invalid page entry in docs/site-content.json')
            required_sources.append(page['source'])
            if page.get('output'):
                required_sources.append(page['output'])
            staged = page['staged']
            expected_outputs.append(staged[:-3] + '.html' if staged.endswith('.md') else staged)
        for asset in site_cfg.get('assets', []):
            if not isinstance(asset, dict) or not isinstance(asset.get('source'), str) or not asset['source'] or not isinstance(asset.get('staged'), str) or not asset['staged']:
                raise ValueError('invalid asset entry in docs/site-content.json')
            required_sources.append(asset['source'])
            expected_outputs.append(asset['staged'])
        for src_rel in required_sources:
            if not any(fnmatch.fnmatchcase(src_rel, pat) for pat in patterns):
                raise ValueError(f'allowlisted site source not covered by site_sources: {src_rel}')
            if not self.path(src_rel).is_file():
                raise ValueError(f'missing allowlisted site source: {src_rel}')
        return {
            'base_path': site_cfg.get('base_path'),
            'sources': sorted(set(required_sources)),
            'expected_outputs': sorted(set(expected_outputs)),
        }

    def site(self, directory, prefix, require_receipt=True):
        base = self.path(directory)
        if not base.is_dir() or not (base / 'index.html').is_file():
            self.error('site', directory, 'built site or index.html is missing')
            return
        has_unsafe = False
        for path in sorted(base.rglob('*')):
            if path.is_symlink() or not path.resolve().is_relative_to(base.resolve()):
                self.error('site', directory, f'symlink escapes built site: {path.relative_to(base).as_posix()}')
                has_unsafe = True
        if has_unsafe:
            return
        try:
            contract = self._allowlisted_site_contract()
        except (OSError, ValueError) as exc:
            self.error('site', directory, str(exc))
            contract = None
        if contract is not None:
            if contract['base_path'] != prefix:
                self.error('site', directory, f'site base_path {contract["base_path"]!r} does not match --site-prefix {prefix!r}')
            for out_rel in contract['expected_outputs']:
                out_path = (base / out_rel).resolve()
                if not out_path.is_relative_to(base.resolve()) or not out_path.is_file():
                    self.error('site', directory, f'missing allowlisted built output: {out_rel}')
        receipt = base / 'drift-build.json'
        if require_receipt and not receipt.is_file():
            self.error('site', directory, 'missing source receipt drift-build.json')
        elif require_receipt:
            try:
                actual_receipt = json.loads(receipt.read_text(encoding='utf-8'))
                expected = self.site_receipt(directory)
            except (UnicodeDecodeError, ValueError, OSError) as exc:
                self.error('site', directory, f'invalid source receipt drift-build.json: {exc}')
            else:
                if not isinstance(actual_receipt, dict):
                    self.error('site', directory, 'invalid source receipt drift-build.json: root must be a JSON object')
                elif actual_receipt != expected:
                    if actual_receipt.get('commit') != expected.get('commit'):
                        self.error(
                            'site',
                            directory,
                            f'site source receipt commit {actual_receipt.get("commit")!r} differs from HEAD {expected.get("commit")!r}; site source receipt differs from current inputs; rebuild before publishing',
                        )
                    else:
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

    def _git_head_sha(self):
        try:
            head = self.git('rev-parse', 'HEAD').decode('utf-8', errors='replace').strip()
        except (OSError, subprocess.SubprocessError):
            return None
        return head if re.fullmatch(r'[0-9a-f]{40}', head) else None

    def site_receipt(self, directory):
        paths = {p for p in self.files if any(fnmatch.fnmatchcase(p, pat) for pat in self.manifest['site_sources']) and self.path(p).is_file()}
        contract = self._allowlisted_site_contract()
        if contract is not None:
            paths.update(contract['sources'])
        base = self.path(directory)
        output = {}
        for p in sorted(base.rglob('*')):
            if p.is_symlink() or not p.resolve().is_relative_to(base.resolve()):
                raise ValueError(f'unsafe built-site path: {p}')
            rel_out = p.relative_to(base).as_posix()
            if p.is_file() and rel_out != 'drift-build.json':
                output[rel_out] = digest(p.read_bytes())
        if contract is not None:
            for out_rel in contract['expected_outputs']:
                if out_rel not in output:
                    raise ValueError(f'missing allowlisted built output in {directory}: {out_rel}')
        receipt = {'schema': 1, 'sources': {p: digest(self.path(p).read_bytes()) for p in sorted(paths)}, 'outputs': output}
        head_sha = self._git_head_sha()
        if head_sha is not None:
            receipt['commit'] = head_sha
        return receipt

    # State an agent harness rewrites in its own session, at times no generator controls: Claude Code's cron lock
    # changes whenever a session job is created, fires or is deleted (a campaign session always has its heartbeat
    # job), and both harnesses keep per-session state. .gitignore excludes exactly these paths. They are not part of
    # the tree a generator's --check is proved read-only against; everything else under .claude/ and .codex/ is.
    HARNESS_RUNTIME = ('.claude/scheduled_tasks.lock', '.claude/state', '.codex/state')
    HARNESS_RUNTIME_PARENTS = frozenset(r.rsplit('/', 1)[0] for r in HARNESS_RUNTIME)

    @classmethod
    def _harness_runtime(cls, rel):
        return any(rel == r or rel.startswith(r + '/') for r in cls.HARNESS_RUNTIME)

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
                    and not self._harness_runtime(f'{rel}/{c.name}')
                )
            )
            if rel in self.HARNESS_RUNTIME_PARENTS:
                # Creating or deleting a runtime entry moves this directory's own write time; its mode, owner and
                # every other child are still compared.
                meta = (st.st_mode, st.st_uid, st.st_gid, 0)
            return ('dir', *meta, children)
        return ('other', *meta, st.st_size)

    def _watched_paths(self):
        ignored_dirs = {'__pycache__', '.git', 'build', 'bin', '.venv', '.remember', '.runner'}
        paths = {
            '.',
            *self.files,
            *self.manifest.get('files', {}).keys(),
            *(gen['script'] for gen in self.manifest['generators']),
            *(out for g in self.manifest['generators'] for out in g.get('outputs', [])),
            *(out for g in self.manifest['generators'] for out in g.get('cache_outputs', [])),
            *(out for g in self.manifest['generators'] for out in g.get('planned_outputs', {})),
        }
        if self.root.is_dir():
            for dirpath, dirnames, filenames in os.walk(self.root, topdown=True, followlinks=False):
                rel_dir = Path(dirpath).relative_to(self.root)
                if rel_dir == Path('tools'):
                    dirnames[:] = [d for d in dirnames if d == 'githooks']
                else:
                    dirnames[:] = [d for d in dirnames if d not in ignored_dirs
                                   and not self._harness_runtime((rel_dir / d).as_posix())]
                if str(rel_dir) != '.':
                    paths.add(rel_dir.as_posix())
                for d in dirnames:
                    paths.add((rel_dir / d).as_posix() if str(rel_dir) != '.' else d)
                for name in filenames:
                    if rel_dir == Path('tools') and name not in ('bootstrap.py', 'bootstrap.ps1', 'provision.py', 'provision.ps1'):
                        continue
                    if self._harness_runtime((rel_dir / name).as_posix()):
                        continue
                    paths.add((rel_dir / name).as_posix() if str(rel_dir) != '.' else name)
        for rel in list(paths):
            if rel != '.':
                for parent in Path(rel).parents:
                    if str(parent) != '.':
                        paths.add(parent.as_posix())
        return sorted(paths)

    def _snapshot_paths(self, paths):
        return {rel: self._entry_state(rel) for rel in paths}

    def _copy_checkout_to_stage(self, stage_root):
        for rel in self._watched_paths():
            if rel == '.':
                continue
            src = self.path(rel)
            if not src.exists():
                continue
            dst = stage_root / rel
            if src.is_dir():
                dst.mkdir(parents=True, exist_ok=True)
            elif src.is_file():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst, follow_symlinks=False)
            else:
                raise ValueError(f'unsupported checkout entry for staging: {rel}')

    @staticmethod
    def _valid_generated_at(value):
        if not isinstance(value, str) or re.fullmatch(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$', value) is None:
            return False
        try:
            parsed = datetime.strptime(value, '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=timezone.utc)
        except ValueError:
            return False
        if parsed.strftime('%Y-%m-%dT%H:%M:%SZ') != value:
            return False
        now = datetime.now(timezone.utc)
        return (now - timedelta(days=365 * 20)) <= parsed <= (now + timedelta(days=1))

    @classmethod
    def _cache_output_semantically_equal(cls, before_bytes, after_bytes):
        if before_bytes == after_bytes:
            return True
        if before_bytes is None or after_bytes is None:
            return False
        try:
            before_text = before_bytes.decode('utf-8')
            after_text = after_bytes.decode('utf-8')
            before_obj = json.loads(before_text)
            after_obj = json.loads(after_text)
        except (UnicodeDecodeError, ValueError):
            return False
        if not isinstance(before_obj, dict) or not isinstance(after_obj, dict):
            return False
        if before_text != json.dumps(before_obj, indent=2, sort_keys=True) + '\n':
            return False
        if not (cls._valid_generated_at(before_obj.get('generated_at')) and cls._valid_generated_at(after_obj.get('generated_at'))):
            return False
        b_copy = {k: v for k, v in before_obj.items() if k != 'generated_at'}
        a_copy = {k: v for k, v in after_obj.items() if k != 'generated_at'}
        return json.dumps(b_copy, indent=2, sort_keys=True) == json.dumps(a_copy, indent=2, sort_keys=True)

    @staticmethod
    def _stage_change_allowed(rel, before_state, after_state, declared_outputs):
        if rel in declared_outputs:
            return True
        allowed_dir_children = {}
        for out in declared_outputs:
            parts = Path(out).parts
            if parts:
                allowed_dir_children.setdefault('.', set()).add(parts[0])
                for idx in range(1, len(parts)):
                    parent_rel = Path(*parts[:idx]).as_posix()
                    allowed_dir_children.setdefault(parent_rel, set()).add(parts[idx])
        if rel not in allowed_dir_children or after_state[0] != 'dir':
            return False
        if before_state[0] == 'dir':
            if before_state[1:4] != after_state[1:4]:
                return False
            before_children = set(before_state[5])
            after_children = set(after_state[5])
            return before_children <= after_children and (after_children - before_children) <= allowed_dir_children[rel]
        if before_state[0] == 'absent':
            return set(after_state[5]) <= allowed_dir_children[rel]
        return False

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
                out_path = self.path(out)
                if not fix:
                    self.generator_before[out] = out_path.read_bytes() if out_path.is_file() else None
                    if self._entry_state(out)[0] != 'file':
                        self.error('generator', gen['script'], f'missing generated output: {out}')
                elif out not in self.generator_before:
                    self.generator_before[out] = out_path.read_bytes() if out_path.is_file() else None
            for cout in gen.get('cache_outputs', []):
                cout_path = self.path(cout)
                if not fix or cout not in self.generator_before:
                    self.generator_before[cout] = cout_path.read_bytes() if cout_path.is_file() else None
            if not fix:
                for rel in self._watched_paths():
                    if rel != '.':
                        self.path(rel)
                before_snap = self._snapshot_paths(self._watched_paths())
                result = subprocess.run([sys.executable, gen['script'], *gen['check']], cwd=self.root, env=env, capture_output=True, text=True, timeout=120)
                if result.returncode:
                    self.error('generator', gen['script'], (result.stdout + result.stderr).strip()[-1500:])
                for out, owner in gen.get('planned_outputs', {}).items():
                    if self._entry_state(out)[0] != 'absent' and before_snap.get(out) == ('absent', None):
                        self.error('generator', gen['script'], f'planned output {out} ({owner}) exists before being promoted to outputs')
                after_snap = self._snapshot_paths(sorted(set(before_snap) | set(self._watched_paths())))
                for rel in sorted(after_snap):
                    if after_snap[rel] != before_snap.get(rel, ('absent', None)):
                        self.error('generator', gen['script'], f'--check modified {rel}; check mode must be read-only')
            else:
                cache_outputs = list(gen.get('cache_outputs', []))
                declared_outputs = set(gen.get('outputs', [])) | set(cache_outputs)
                with tempfile.TemporaryDirectory(prefix='drift-gen-') as tmp:
                    stage_root = Path(tmp)
                    self._copy_checkout_to_stage(stage_root)
                    stage_checker = Checker(stage_root, self.git_dir)
                    stage_checker.manifest = self.manifest
                    stage_checker.files = set(self.files)
                    stage_before = stage_checker._snapshot_paths(stage_checker._watched_paths())
                    stage_env = dict(env)
                    if self.git_dir:
                        stage_env.update(GIT_DIR=str(self.git_dir), GIT_WORK_TREE=str(stage_root))
                    result = subprocess.run(
                        [sys.executable, gen['script'], *gen['fix']],
                        cwd=stage_root,
                        env=stage_env,
                        capture_output=True,
                        text=True,
                        timeout=120,
                    )
                    if result.returncode:
                        self.error('generator', gen['script'], (result.stdout + result.stderr).strip()[-1500:])
                        continue
                    for out, owner in gen.get('planned_outputs', {}).items():
                        if stage_checker._entry_state(out)[0] != 'absent':
                            raise ValueError(f"{gen['script']}: planned output {out} ({owner}) written during --fix before being promoted to outputs")
                    stage_after = stage_checker._snapshot_paths(sorted(set(stage_before) | set(stage_checker._watched_paths())))
                    for rel in sorted(stage_after):
                        before_state = stage_before.get(rel, ('absent', None))
                        after_state = stage_after[rel]
                        if after_state != before_state and not self._stage_change_allowed(rel, before_state, after_state, declared_outputs):
                            raise ValueError(f"{gen['script']}: --fix modified undeclared path {rel}")
                    for out in gen.get('outputs', []):
                        if stage_checker._entry_state(out)[0] != 'file':
                            raise ValueError(f"{gen['script']}: missing generated output: {out}")
                    if self._before_generator_publish_hook is not None:
                        self._before_generator_publish_hook(gen['script'])
                    gen_repairs = {}
                    for out in gen.get('outputs', []):
                        staged_bytes = stage_checker.path(out).read_bytes()
                        expected_before = self.generator_before.get(out)
                        if staged_bytes != expected_before:
                            gen_repairs[out] = (expected_before, staged_bytes)
                    for cout in cache_outputs:
                        if (self.root / cout).exists() and stage_checker._entry_state(cout)[0] == 'file':
                            staged_bytes = stage_checker.path(cout).read_bytes()
                            expected_before = self.generator_before.get(cout)
                            if not self._cache_output_semantically_equal(expected_before, staged_bytes):
                                gen_repairs[cout] = (expected_before, staged_bytes)
                if gen_repairs:
                    saved_repairs = dict(self.repairs)
                    try:
                        self.repairs = gen_repairs
                        self.fix()
                    finally:
                        self.repairs = saved_repairs
                    for out, (_, staged_bytes) in gen_repairs.items():
                        self.generator_before[out] = staged_bytes

    def _allowed_repair_paths(self, extra=()):
        generated_paths = {
            'docs/facts.json',
            *(out for gen in self.manifest.get('generators', []) for out in gen.get('outputs', [])),
            *(out for gen in self.manifest.get('generators', []) for out in gen.get('cache_outputs', [])),
            *extra,
        }
        block_paths = {block['path'] for block in self.manifest.get('blocks', {}).values()}
        return generated_paths, block_paths - generated_paths

    def _strip_registered_blocks(self, rel, raw_bytes):
        encoding = self.manifest.get('encodings', {}).get(rel, 'utf-8')
        text = raw_bytes.decode(encoding)
        suffix = Path(rel).suffix.lower()
        scan = code_spans(prose(text)) if suffix in ('.md', '.markdown') else text
        blocks = self.manifest.get('blocks', {})
        spans = []
        ids = []
        for match in BLOCK.finditer(scan):
            name = match['id']
            if name in blocks and blocks[name]['path'] == rel:
                ids.append(name)
                spans.append((match.start('body'), match.end('body')))
        stripped = text
        for start, end in reversed(spans):
            stripped = stripped[:start] + stripped[end:]
        return ids, stripped

    def fix(self, allowed_extra=()):
        generated_paths, block_only_paths = self._allowed_repair_paths(allowed_extra)
        allowed_all = generated_paths | block_only_paths
        for rel, (before, after) in self.repairs.items():
            if rel not in allowed_all:
                raise ValueError(f'refusing to repair unregistered path: {rel}')
            if rel in block_only_paths:
                if before is None or after is None:
                    raise ValueError(f'{rel}: block repair requires existing document bytes')
                before_ids, before_outside = self._strip_registered_blocks(rel, before)
                after_ids, after_outside = self._strip_registered_blocks(rel, after)
                if not before_ids or before_ids != after_ids or before_outside != after_outside:
                    raise ValueError(f'{rel}: refusing to modify content outside registered generated blocks')
            path = self.path(rel)
            if (path.read_bytes() if path.exists() else None) != before:
                raise ValueError(f'{rel} changed during repair; refusing to overwrite')
        for rel, (before, after) in self.repairs.items():
            path = self.path(rel)
            current = path.read_bytes() if path.exists() else None
            if current != before:
                raise ValueError(f'{rel} changed during repair; refusing to overwrite')
            if current == after:
                continue
            mode = path.stat().st_mode if path.exists() else 0o644
            path.parent.mkdir(parents=True, exist_ok=True)
            fd, temp = tempfile.mkstemp(prefix='.drift-', dir=path.parent)
            try:
                with os.fdopen(fd, 'wb') as out:
                    out.write(after)
                    out.flush()
                    os.fsync(out.fileno())
                os.chmod(temp, mode)
                path = self.path(rel)
                if (path.read_bytes() if path.exists() else None) != before:
                    raise ValueError(f'{rel} changed during repair; refusing to overwrite')
                os.replace(temp, path)
            finally:
                if os.path.exists(temp): os.unlink(temp)
            if not path.is_file() or path.read_bytes() != after:
                raise ValueError(f'{rel}: read-back verification failed after repair')


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
            if not _has_drift_fact_declarations(message):
                return 0
            facts = check.revision_facts(':')
            declarations(message, facts, check, str(args.commit_message))
            for error in check.errors: print(f'commit-fact: {error["detail"]}')
            return bool(check.errors)
        if args.write_site_receipt and (not args.site_dir or args.fix or args.review):
            raise ValueError('--write-site-receipt requires --site-dir and cannot repair or renew reviews')
        check.load()
        if args.review:
            if not args.evidence or not re.search(r'\b\d{4}-\d{2}-\d{2}\b', args.evidence.strip()) or args.fix:
                raise ValueError('--review needs dated --evidence (YYYY-MM-DD) and cannot be combined with --fix')
            review = check.manifest['reviews'][args.review]
            check.review_policies({args.review: {**review, 'evidence': args.evidence.strip()}})
            if check.errors:
                raise ValueError('review receipt not renewed: ' + '; '.join(e['detail'] for e in check.errors))
            review['sources'] = {p: digest(check.path(p).read_bytes()) for p in review['sources']}
            review['evidence'] = args.evidence.strip()
            check.generated(MANIFEST, json.dumps(check.manifest, indent=2) + '\n', before=check.manifest_bytes)
            check.fix(allowed_extra=(MANIFEST,))
            check.errors.clear()
            check.repairs.clear()
        check.derive()
        check.documents()
        if args.fix:
            check.fix()
            check.repairs.clear()
            check.generators(fix=True)
            if any(e['kind'] == 'generator' for e in check.errors):
                raise ValueError('generator repair failed; inspect output and rerun check')
            return main([*(['--root', str(args.root)]), *(['--git-dir', str(args.git_dir)] if args.git_dir else []), *(['--site-dir', args.site_dir, '--site-prefix', args.site_prefix] if args.site_dir else []), *(['--json'] if args.json else []), *(['--coverage'] if args.coverage else []), *(['--commit-range', args.commit_range] if args.commit_range else [])])
        check.generators()
        if args.commit_range:
            if re.fullmatch(r'[0-9a-f]{7,40}\.\.[0-9a-f]{7,40}', args.commit_range):
                base_oid, head_oid = args.commit_range.split('..', 1)
                check.git('cat-file', '-e', f'{base_oid}^{{commit}}')
                check.git('cat-file', '-e', f'{head_oid}^{{commit}}')
                oids = check.git('rev-list', args.commit_range).decode().splitlines()
            elif re.fullmatch(r'[0-9a-f]{7,40}', args.commit_range):
                check.git('cat-file', '-e', f'{args.commit_range}^{{commit}}')
                oids = [check.git('rev-parse', f'{args.commit_range}^{{commit}}').decode().strip()]
            else:
                raise ValueError('--commit-range requires two explicit commit ids or a single commit id')
            for oid in oids:
                message = check.git('show', '-s', '--format=%B', oid).decode()
                if _has_drift_fact_declarations(message):
                    # A historical declaration belongs to its own tree, not today's source.
                    facts = check.revision_facts(oid + ':')
                    declarations(message, facts, check, oid)
        if args.write_site_receipt:
            if not args.site_dir or check.errors:
                raise ValueError('site receipt requires --site-dir and clean source checks')
            check.site(args.site_dir, args.site_prefix, require_receipt=False)
            if not check.errors:
                check.generated(args.site_dir + '/drift-build.json', json.dumps(check.site_receipt(args.site_dir), indent=2) + '\n')
                check.fix(allowed_extra=(args.site_dir + '/drift-build.json',))
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


def _body_lines(message):
    lines = message.splitlines()
    return lines[1:] if len(lines) > 1 else []


def _has_drift_fact_declarations(message):
    return any(DRIFT_FACT_TRAILER.match(line) for line in _body_lines(message))


def declarations(message, facts, check, where):
    for line in _body_lines(message):
        if not DRIFT_FACT_TRAILER.match(line):
            continue
        match = re.fullmatch(r'Drift-Fact: ([a-z0-9_.-]+)=(.+)', line)
        if not match or match[1] not in facts or str(facts[match[1]]) != match[2]:
            check.error('commit-fact', where, f'{line}: differs from that commit\'s facts; correct the new message or add a corrective commit, never rewrite history')


if __name__ == '__main__':
    raise SystemExit(main())
