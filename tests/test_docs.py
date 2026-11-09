#!/usr/bin/env python3
"""Page-coverage, mirroring, link-rebasing, and publication-boundary tests for D08 T01."""
from __future__ import annotations

import contextlib
import html
import importlib.util
import io
import json
from pathlib import Path
import re
import shutil
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]

build_docs_spec = importlib.util.spec_from_file_location('build_docs', REPO / 'scripts/build-docs.py')
build_docs = importlib.util.module_from_spec(build_docs_spec)
build_docs_spec.loader.exec_module(build_docs)

drift_spec = importlib.util.spec_from_file_location('drift', REPO / 'scripts/drift-check.py')
drift = importlib.util.module_from_spec(drift_spec)
drift_spec.loader.exec_module(drift)

FIXTURE_FILES = (
    'docs/site-content.json',
    'docs/drift.json',
    'docs/facts.json',
    'docs/_config.yml',
    'docs/_includes/components/footer.html',
    'docs/_includes/head_custom.html',
    'docs/_includes/header_custom.html',
    'docs/404.md',
    'docs/site-validation.md',
    'docs/assets/css/site-states.css',
    'docs/assets/js/site-states.js',
    'docs/Gemfile',
    'docs/index.md',
    'docs/getting-started.md',
    'docs/installation.md',
    'docs/usage.md',
    'docs/c-api.md',
    'docs/api.md',
    'docs/format.md',
    'docs/examples.md',
    'docs/benchmarks.md',
    'docs/research-summary.md',
    'docs/limitations.md',
    'docs/troubleshooting.md',
    'docs/contributing.md',
    'docs/security.md',
    'docs/licenses.md',
    'docs/building.md',
    'docs/library.md',
    'docs/research.md',
    'docs/measurements.md',
    'docs/vision.md',
    'docs/document-drift.md',
    'README.md',
    'CONTRIBUTING.md',
    'SECURITY.md',
    'CODE_OF_CONDUCT.md',
    'LICENSE',
    'requirements-dev.txt',
    'toolchain.json',
    'spec/FOLD.md',
    'include/rasterfold.h',
    'src/rf.h',
    'src/main.c',
    'src/api.c',
    'src/bmp.c',
    'src/fold.c',
    'src/sys.c',
    'src/sys_posix.c',
    'src/rasterfold.def',
    'examples/encode.c',
    'examples/decode.c',
    'examples/ppm.h',
    'research/README.md',
    'testdata/photos/README.md',
    'testdata/photos/manifest.json',
    'testdata/photos/NOTICE-imagecompression-info.txt',
    'assets/brand/icon.svg',
    'assets/brand/cover.svg',
    'benchmarks/corpus.csv',
    'benchmarks/results.csv',
)


def _render_staged_markdown_to_html(staged_dir: Path, artifact_dir: Path, site_content: dict) -> None:
    """Render staged Markdown and assets into a testable HTML artifact tree matching Jekyll's output paths."""
    if artifact_dir.exists():
        shutil.rmtree(artifact_dir)
    artifact_dir.mkdir(parents=True, exist_ok=True)
    base_path = site_content['base_path'].rstrip('/')
    repo_slug = site_content['repository']
    pages = sorted(site_content['pages'], key=lambda p: p['nav_order'])

    nav_items = []
    for p in pages:
        slug = re.sub(r'\.md$', '.html', p['staged'])
        href = f'{base_path}/' if slug == 'index.html' else f'{base_path}/{slug}'
        nav_items.append(f'<li><a href="{html.escape(href, quote=True)}">{html.escape(p["title"])}</a></li>')
    nav_html = '<nav id="site-nav" aria-label="Main"><ul>' + ''.join(nav_items) + '</ul></nav>'

    for asset in site_content['assets']:
        src = staged_dir / asset['staged']
        dst = artifact_dir / asset['staged']
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)

    render_entries = list(pages)
    for cfg in site_content.get('config', []):
        staged_cfg = cfg.get('staged', '')
        if isinstance(staged_cfg, str) and not staged_cfg.startswith('_') and staged_cfg.endswith('.md'):
            render_entries.append({'staged': staged_cfg, 'source': cfg['source'], 'title': 'Page not found'})

    search_data = {}
    search_idx = 0
    for page in render_entries:
        staged_rel = page['staged']
        out_rel = re.sub(r'\.md$', '.html', staged_rel)
        md_text = (staged_dir / staged_rel).read_text(encoding='utf-8')
        front_matter = md_text.split('---\n', 2)[1] if md_text.startswith('---\n') and md_text.count('---\n') >= 2 else ''
        body = md_text.split('---\n', 2)[2] if md_text.startswith('---\n') and md_text.count('---\n') >= 2 else md_text

        if 'search_exclude: true' not in front_matter:
            rel_url = '/' if out_rel == 'index.html' else f'/{out_rel}'
            search_data[str(search_idx)] = {
                'doc': page['title'],
                'title': page['title'],
                'content': body[:200],
                'url': f'{base_path}{rel_url}',
                'relUrl': rel_url,
            }
            search_idx += 1

        # Expand generated SVG and table includes so benchmarks.html carries its charts and tables.
        def _expand_include(match: re.Match[str]) -> str:
            inc_rel = match.group(1).strip()
            inc_path = staged_dir / '_includes' / inc_rel
            if not inc_path.is_file():
                return ''
            inc_text = inc_path.read_text(encoding='utf-8')
            if inc_rel.endswith('.md'):
                return '<table><tbody><tr><td>' + html.escape(inc_text) + '</td></tr></tbody></table>'
            return inc_text

        body = re.sub(r'\{%\s*include\s+([^\s%]+)\s*%\}', _expand_include, body)

        # Convert Markdown links [label](url) to HTML anchors outside fenced code blocks so drift-check.py validates all links.
        rendered_lines = []
        fence = None
        for line in body.splitlines():
            fmatch = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
            if fmatch and fence is None:
                fence = fmatch.group(1)
                rendered_lines.append('<pre><code>')
                continue
            if fence and re.match(r'^\s{0,3}' + re.escape(fence[0]) + '{' + str(len(fence)) + r',}\s*$', line):
                fence = None
                rendered_lines.append('</code></pre>')
                continue
            if fence is not None:
                rendered_lines.append(html.escape(line))
                continue
            hmatch = re.match(r'^(#{1,6})\s+(.*)$', line)
            if hmatch:
                level = len(hmatch.group(1))
                heading_text = hmatch.group(2).strip()
                slug_id = next(iter(drift.anchors(f'# {heading_text}\n', True)), 'section')
                rendered_lines.append(f'<h{level} id="{html.escape(slug_id, quote=True)}">{html.escape(heading_text)}</h{level}>')
                continue
            line_html = build_docs.MD_LINK_RE.sub(
                lambda m: f'<a href="{html.escape(m.group("url").strip("<>"), quote=True)}">link</a>',
                line,
            )
            rendered_lines.append(f'<p>{line_html}</p>')

        edit_href = f'https://github.com/{repo_slug}/tree/main/{page["source"]}'
        footer_html = f'<footer><a href="{html.escape(edit_href, quote=True)}" id="edit-this-page">Edit this page on GitHub</a></footer>'
        header_controls_html = (
            '<div class="rf-header-controls" role="region" aria-label="Display preferences and version status">'
            '<div id="rf-theme-controls" role="group" aria-label="Color theme">'
            '<button type="button" class="rf-theme-btn" data-rf-theme-choice="light" aria-pressed="true">Light</button>'
            '<button type="button" class="rf-theme-btn" data-rf-theme-choice="dark" aria-pressed="false">Dark</button>'
            '<button type="button" class="rf-theme-btn" data-rf-theme-choice="hc-light" aria-pressed="false">HC Light</button>'
            '<button type="button" class="rf-theme-btn" data-rf-theme-choice="hc-dark" aria-pressed="false">HC Dark</button>'
            '</div>'
            '<div id="rf-density-controls" role="group" aria-label="Layout density">'
            '<button type="button" class="rf-density-btn" data-rf-density-choice="comfortable" aria-pressed="true">Comfortable</button>'
            '<button type="button" class="rf-density-btn" data-rf-density-choice="compact" aria-pressed="false">Compact</button>'
            '</div>'
            '<div id="rf-a11y-status" role="status" aria-live="polite"></div>'
            '</div>'
            f'<div id="rf-search-offline" role="alert" hidden><button type="button" id="rf-search-retry">Retry</button>'
            f'<a href="{base_path}/">Overview</a></div>'
        )
        doc_html = (
            '<!DOCTYPE html>\n<html lang="en"><head><meta charset="utf-8">'
            f'<title>{html.escape(page["title"])}</title>'
            f'<link rel="stylesheet" href="{base_path}/assets/css/site-states.css" id="rf-site-states-css">'
            f'<script src="{base_path}/assets/js/site-states.js" defer id="rf-site-states-js"></script>'
            '</head><body>\n'
            '<a class="skip-to-main" href="#main-content">Skip to main content</a>\n'
            f'{nav_html}\n{header_controls_html}\n<main id="main-content">\n'
            + '\n'.join(rendered_lines)
            + f'\n</main>\n{footer_html}\n</body></html>\n'
        )
        out_path = artifact_dir / out_rel
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(doc_html, encoding='utf-8', newline='\n')

    search_json_path = artifact_dir / 'assets/js/search-data.json'
    search_json_path.parent.mkdir(parents=True, exist_ok=True)
    search_json_path.write_text(json.dumps(search_data, indent=2) + '\n', encoding='utf-8', newline='\n')


class DocsMapTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix='rf-test-docs-')
        self.root = Path(self.tmp.name)
        for rel in FIXTURE_FILES:
            src = REPO / rel
            dst = self.root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
        for name in build_docs.CHART_NAMES:
            for rel in (
                f'docs/_includes/generated/charts/{name}.svg',
                f'docs/_includes/generated/tables/{name}.md',
            ):
                src = REPO / rel
                dst = self.root / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write(self, rel: str, content: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding='utf-8', newline='\n')

    def test_api_reference_content_and_parser_refusals(self) -> None:
        api = build_docs.parse_api_header(REPO, 'include/rasterfold.h', 'src/rasterfold.def')
        page = build_docs.render_api_page(REPO, {'source': 'include/rasterfold.h', 'title': 'API Reference', 'nav_order': 15})
        for fn in ('rf_version', 'rf_encode_image', 'rf_decode_image', 'rf_inspect', 'rf_free'):
            self.assertIn(f'### `{fn}`', page)
        for ty in api['types']:
            self.assertIn(f'### `{ty["name"]}`', page)
            for field in ty['fields']:
                self.assertIn(f'- `{field};`', page)
        for const in api['constants']:
            self.assertIn(f'### `{const["name"]}`', page)
        for ownership in ('release data using rf_free', 'caller releases it with rf_free', 'never free it', 'Cannot fail'):
            self.assertIn(ownership, page)
        self.assertIn('safe for concurrent calls', page)

        header = (self.root / 'include/rasterfold.h').read_text(encoding='utf-8')
        definitions = (self.root / 'src/rasterfold.def').read_text(encoding='utf-8')

        def refuses(mutate, pattern, defs=None):
            self.write('include/rasterfold.h', mutate(header))
            if defs is not None:
                self.write('src/rasterfold.def', defs)
            with self.assertRaisesRegex(ValueError, pattern):
                build_docs.parse_api_header(self.root, 'include/rasterfold.h', 'src/rasterfold.def')
            self.write('include/rasterfold.h', header)
            self.write('src/rasterfold.def', definitions)

        # Undocumented function: drop the rf_free comment entirely.
        free_doc = '/* Release the base pointer of a live allocation returned by this library\n * (rf_buffer.data or decoded rf_pixels.data), exactly once. NULL is\n * allowed. Never pass a caller allocation, an interior pointer, or a\n * pointer obtained from another loaded copy of the library. */\n'
        self.assertIn(free_doc, header)
        refuses(lambda t: t.replace(free_doc, ''), 'undocumented function')
        # A constant hidden inside an unsupported conditional is refused.
        refuses(lambda t: t.replace('#define RF_LAYOUT_RGB8 1u', '#if 1\n#define RF_HIDDEN 42\n#endif\n#define RF_LAYOUT_RGB8 1u'), 'unsupported conditional')
        # A declaration sharing the comment's closing line is not discarded.
        refuses(lambda t: t.replace('/* Returns the packed API version', '/* Public enum. */ typedef enum { RF_X } rf_x;\n/* Returns the packed API version'), 'unsupported public declaration')
        # The macro group's comment cannot document the following type.
        options_doc_start = header.index('/* Initialize size to sizeof(rf_options)')
        options_end = header.index('typedef struct {', options_doc_start)
        refuses(lambda t: t.replace(header[options_doc_start:options_end], ''), 'undocumented type')
        # A comment inside a multi-line signature is refused, not rendered.
        refuses(lambda t: t.replace('RF_API int rf_encode_image(const rf_pixels *pixels, const rf_options *options,\n', 'RF_API int rf_encode_image(const rf_pixels *pixels, const rf_options *options,\n                           // output buffer\n'), 'unsupported comment inside a declaration')
        # A constant under any other conditional is refused, as is a
        # directive inside a struct body.
        refuses(lambda t: t.replace('#define RF_LAYOUT_RGB8 1u', '#ifndef RF_DISABLE_EXTRA\n#define RF_EXTRA 1\n#endif\n#define RF_LAYOUT_RGB8 1u'), 'unsupported conditional')
        refuses(lambda t: t.replace('    uint32_t size, threads;', '#if 0\n    uint32_t hidden;\n#endif\n    uint32_t size, threads;'), 'unsupported directive inside a declaration')
        # Two typedefs on one line cannot hide the second one either.
        refuses(lambda t: t.replace('typedef struct { uint8_t *data; size_t size; } rf_buffer;', 'typedef struct { uint8_t *data; size_t size; } rf_buffer; typedef struct { int hidden; } rf_hidden;'), 'unsupported multiple declarations')
        # Two declarations on one line cannot hide the second one.
        refuses(lambda t: t.replace('RF_API uint32_t rf_version(void);', 'RF_API uint32_t rf_version(void); RF_API int rf_new(void);'), 'unsupported multiple declarations')
        # An ordinal-free export entry is refused, not silently skipped.
        refuses(lambda t: t, 'unsupported export entry', defs=definitions + 'rf_new\n')

    def test_required_page_inventory_and_source_mapping(self) -> None:
        site_content = build_docs.load_site_content(REPO)
        pages_by_id = {p['id']: p for p in site_content['pages']}
        self.assertEqual(set(build_docs.REQUIRED_PAGE_IDS), set(pages_by_id.keys()))

        expected_sources = {
            'overview': ('docs/index.md', None, 'authored'),
            'getting-started': ('docs/getting-started.md', None, 'authored'),
            'installation': ('docs/building.md', 'docs/installation.md', 'mirrored'),
            'usage': ('README.md', 'docs/usage.md', 'mirrored'),
            'c-api': ('docs/library.md', 'docs/c-api.md', 'mirrored'),
            'format': ('spec/FOLD.md', 'docs/format.md', 'mirrored'),
            'examples': ('docs/examples.md', None, 'authored'),
            'benchmarks': ('docs/benchmarks.md', None, 'authored'),
            'research-summary': ('docs/research.md', 'docs/research-summary.md', 'mirrored'),
            'limitations': ('docs/limitations.md', None, 'authored'),
            'troubleshooting': ('docs/troubleshooting.md', None, 'authored'),
            'contributing': ('CONTRIBUTING.md', 'docs/contributing.md', 'mirrored'),
            'security': ('SECURITY.md', 'docs/security.md', 'mirrored'),
            'licenses': ('docs/licenses.md', None, 'authored'),
            'api': ('include/rasterfold.h', 'docs/api.md', 'generated'),
        }
        drift_manifest = json.loads((REPO / 'docs/drift.json').read_text(encoding='utf-8'))
        build_docs_gen = next(g for g in drift_manifest['generators'] if g['script'] == 'scripts/build-docs.py')
        gen_outputs = set(build_docs_gen['outputs'])
        reviews = drift_manifest['reviews']

        for pid, (exp_src, exp_out, exp_kind) in expected_sources.items():
            page = pages_by_id[pid]
            self.assertEqual(exp_src, page['source'], pid)
            self.assertEqual(exp_kind, page['kind'], pid)
            self.assertEqual(exp_out, page.get('output'), pid)
            if exp_kind in ('mirrored', 'generated'):
                self.assertNotEqual(page['source'], page['output'], pid)
                self.assertIn(exp_out, gen_outputs, f'{pid} output must be registered in docs/drift.json generators')
            else:
                self.assertIn(exp_src, reviews, f'{pid} authored page must have a review receipt in docs/drift.json')

        # Verify content acceptance criteria across newly authored and updated pages.
        examples_text = (REPO / 'docs/examples.md').read_text(encoding='utf-8')
        for token in ('examples/encode.c', 'examples/decode.c', 'examples/ppm.h', 'rf_encode_image', 'rf_decode_image', 'rf_inspect'):
            self.assertIn(token, examples_text)

        troubleshooting_text = (REPO / 'docs/troubleshooting.md').read_text(encoding='utf-8')
        for token in (
            'RF_OK',
            'RF_ERR_USAGE',
            'RF_ERR_IO',
            'RF_ERR_UNSUPPORTED',
            'RF_ERR_FORMAT',
            'RF_ERR_LIMIT',
            'output file already exists',
            'output published but temporary cleanup failed',
            'output published but directory flush failed',
            'unsupported BMP: bit count',
            'invalid fold: header checksum',
            'invalid pixel stride or buffer size',
        ):
            self.assertIn(token, troubleshooting_text)

        limitations_text = (REPO / 'docs/limitations.md').read_text(encoding='utf-8')
        for token in ('32768', '33554432', '268435456', 'RF_LAYOUT_RGB8', 'Current:', 'Historical:', 'Planned (not yet shipped):'):
            self.assertIn(token, limitations_text)

        licenses_text = (REPO / 'docs/licenses.md').read_text(encoding='utf-8')
        for token in ('GPL-3.0', 'LICENSE', 'CODE_OF_CONDUCT.md', 'Kodak', 'NOTICE-imagecompression-info.txt', 'toolchain.json'):
            self.assertIn(token, licenses_text)

        index_text = (REPO / 'docs/index.md').read_text(encoding='utf-8')
        self.assertIn('Planned (not downloadable yet):', index_text)
        benchmarks_text = (REPO / 'docs/benchmarks.md').read_text(encoding='utf-8')
        self.assertIn('Current measured data:', benchmarks_text)
        self.assertIn('Historical retained data:', benchmarks_text)
        self.assertIn('Planned scope:', benchmarks_text)

    def test_shrinking_required_topic_inventory_fails_build_docs(self) -> None:
        orig_sc = json.loads((self.root / 'docs/site-content.json').read_text(encoding='utf-8'))
        for required_id in build_docs.REQUIRED_PAGE_IDS:
            shrunk = dict(orig_sc, pages=[p for p in orig_sc['pages'] if p['id'] != required_id])
            self.write('docs/site-content.json', json.dumps(shrunk, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, f'missing required page id\\(s\\): {re.escape(required_id)}'):
                build_docs.load_site_content(self.root)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(1, build_docs.main(['--root', str(self.root), '--check']))
            self.assertIn(f'missing required page id(s): {required_id}', err.getvalue())

        # Mirrored page overwriting its canonical source (exact or normalized path) or another canonical source is rejected.
        building_before = (self.root / 'docs/building.md').read_bytes()
        library_before = (self.root / 'docs/library.md').read_bytes()
        index_before = (self.root / 'docs/index.md').read_bytes()
        for bad_out in ('docs/building.md', './docs/building.md', 'docs/library.md', './docs/index.md'):
            bad_mirror = json.loads(json.dumps(orig_sc))
            for p in bad_mirror['pages']:
                if p['id'] == 'installation':
                    p['output'] = bad_out
            self.write('docs/site-content.json', json.dumps(bad_mirror, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, "mirrored page 'installation' output .* cannot overwrite source"):
                build_docs.load_site_content(self.root)
            with self.assertRaisesRegex(ValueError, "mirrored page 'installation' output .* cannot overwrite source"):
                build_docs.render_generated_pages(self.root, bad_mirror)
            with self.assertRaisesRegex(ValueError, "mirrored page 'installation' output .* cannot overwrite source"):
                build_docs.write_generated_pages(self.root)
        self.assertEqual(building_before, (self.root / 'docs/building.md').read_bytes())
        self.assertEqual(library_before, (self.root / 'docs/library.md').read_bytes())
        self.assertEqual(index_before, (self.root / 'docs/index.md').read_bytes())

        # Duplicate normalized mirrored output path is rejected.
        dup_out = json.loads(json.dumps(orig_sc))
        for p in dup_out['pages']:
            if p['id'] == 'c-api':
                p['output'] = './docs/installation.md'
        self.write('docs/site-content.json', json.dumps(dup_out, indent=2) + '\n')
        with self.assertRaisesRegex(ValueError, 'duplicate mirrored output path'):
            build_docs.load_site_content(self.root)
        with self.assertRaisesRegex(ValueError, 'duplicate mirrored output path'):
            build_docs.render_generated_pages(self.root, dup_out)

        self.write('docs/site-content.json', json.dumps(orig_sc, indent=2) + '\n')
        self.assertEqual([], build_docs.check_generated_pages(self.root))

    def test_mirrored_pages_detect_stale_sources_and_preserve_canonical_sources(self) -> None:
        canonical_sources = (
            ('docs/building.md', 'docs/installation.md'),
            ('README.md', 'docs/usage.md'),
            ('docs/library.md', 'docs/c-api.md'),
            ('spec/FOLD.md', 'docs/format.md'),
            ('docs/research.md', 'docs/research-summary.md'),
            ('CONTRIBUTING.md', 'docs/contributing.md'),
            ('SECURITY.md', 'docs/security.md'),
        )
        originals = {src: (self.root / src).read_bytes() for src, _ in canonical_sources}

        for src_rel, out_rel in canonical_sources:
            orig_text = originals[src_rel].decode('utf-8')
            if src_rel == 'README.md':
                mutated_text = orig_text.replace('## Embed the codec\n', '## Embed the codec\n\nMutated README section line.\n', 1)
            elif src_rel == 'docs/research.md':
                mutated_text = orig_text.replace('## 8. Prior art\n', '## 8. Prior art\n\nMutated research section line.\n', 1)
            else:
                mutated_text = orig_text + '\nAppended source line for stale check.\n'
            self.write(src_rel, mutated_text)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(1, build_docs.main(['--root', str(self.root), '--check']), src_rel)
            self.assertIn(f'{out_rel}: generated file differs from source', err.getvalue())

            # Regenerating updates the mirrored output without modifying the canonical source.
            written = build_docs.write_generated_pages(self.root)
            self.assertIn(out_rel, written)
            self.assertEqual(mutated_text.encode('utf-8'), (self.root / src_rel).read_bytes())
            self.assertEqual([], build_docs.check_generated_pages(self.root))

            # Restore original source and regenerate.
            (self.root / src_rel).write_bytes(originals[src_rel])
            build_docs.write_generated_pages(self.root)
            self.assertEqual(originals[src_rel], (self.root / src_rel).read_bytes())
            self.assertEqual([], build_docs.check_generated_pages(self.root))

    def test_liquid_delimiters_in_mirrored_source_fail_check(self) -> None:
        original = (self.root / 'docs/research.md').read_text(encoding='utf-8')
        for token in ('{{13, 12}, {16, 10}}', '{% raw %}'):
            self.write('docs/research.md', original.replace('## 8. Prior art\n', f'## 8. Prior art\n\nMaxima {token}.\n', 1))
            build_docs.write_generated_pages(self.root)
            errors = build_docs.check_generated_pages(self.root)
            self.assertTrue(any('docs/research-summary.md:' in e and 'Liquid delimiter' in e for e in errors), (token, errors))
        self.write('docs/research.md', original)
        build_docs.write_generated_pages(self.root)
        self.assertEqual([], build_docs.check_generated_pages(self.root))

    def test_staged_links_rebased_under_base_path_and_private_sources_refused(self) -> None:
        staged = build_docs.stage_site(self.root, 'build/site-source')
        for page in build_docs.load_site_content(self.root)['pages']:
            self.assertIn(page['staged'], staged)

        staged_index = (self.root / 'build/site-source/index.md').read_text(encoding='utf-8')
        for expected_link in (
            '/RasterFold/getting-started.html',
            '/RasterFold/installation.html',
            '/RasterFold/usage.html',
            '/RasterFold/c-api.html',
            '/RasterFold/format.html',
            '/RasterFold/examples.html',
            '/RasterFold/benchmarks.html',
            '/RasterFold/research-summary.html',
            '/RasterFold/limitations.html',
            '/RasterFold/troubleshooting.html',
            '/RasterFold/contributing.html',
            '/RasterFold/security.html',
            '/RasterFold/licenses.html',
        ):
            self.assertIn(expected_link, staged_index)

        staged_usage = (self.root / 'build/site-source/usage.md').read_text(encoding='utf-8')
        self.assertIn('/RasterFold/installation.html', staged_usage)
        self.assertIn('/RasterFold/c-api.html', staged_usage)

        staged_research = (self.root / 'build/site-source/research-summary.md').read_text(encoding='utf-8')
        self.assertIn('/RasterFold/format.html', staged_research)
        self.assertIn('/RasterFold/benchmarks.html', staged_research)

        staged_contrib = (self.root / 'build/site-source/contributing.md').read_text(encoding='utf-8')
        self.assertIn('/RasterFold/installation.html', staged_contrib)
        self.assertIn('/RasterFold/research-summary.html', staged_contrib)
        self.assertIn('/RasterFold/security.html', staged_contrib)
        self.assertIn('https://github.com/rizonesoft/RasterFold/blob/main/AGENTS.md', staged_contrib)
        self.assertIn('https://github.com/rizonesoft/RasterFold/blob/main/todo/README.md', staged_contrib)

        staged_examples = (self.root / 'build/site-source/examples.md').read_text(encoding='utf-8')
        self.assertIn('https://github.com/rizonesoft/RasterFold/blob/main/examples/encode.c', staged_examples)
        self.assertIn('https://github.com/rizonesoft/RasterFold/blob/main/examples/decode.c', staged_examples)
        self.assertIn('https://github.com/rizonesoft/RasterFold/blob/main/examples/ppm.h', staged_examples)
        self.assertIn('/RasterFold/c-api.html', staged_examples)
        self.assertIn('/RasterFold/troubleshooting.html', staged_examples)

        # Exporting a private source in site-content.json or linking to one in a page fails staging.
        self.write('todo/private-plan.md', '# Private plan\n')
        self.write('AGENTS.md', '# Private agent instructions\n')
        orig_sc = json.loads((self.root / 'docs/site-content.json').read_text(encoding='utf-8'))
        for forbidden_rel in ('todo/private-plan.md', 'AGENTS.md', 'docs/reviews/secret.md', '.intelligence/notes.md'):
            bad_sc = json.loads(json.dumps(orig_sc))
            bad_sc['pages'].append(
                {
                    'id': 'leaked',
                    'source': forbidden_rel,
                    'staged': 'leaked.md',
                    'title': 'Leaked',
                    'nav_order': 99,
                    'kind': 'authored',
                }
            )
            self.write('docs/site-content.json', json.dumps(bad_sc, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, 'forbidden source path'):
                build_docs.stage_site(self.root, 'build/site-source')
        self.write('docs/site-content.json', json.dumps(orig_sc, indent=2) + '\n')

    def test_built_site_artifact_missing_page_and_leaked_private_file_failures(self) -> None:
        (REPO / 'build').mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='test-docs-site-', dir=REPO / 'build') as tmp_build:
            tmp_build_path = Path(tmp_build)
            rel_stage = (tmp_build_path / 'site-source').relative_to(REPO).as_posix()
            rel_artifact = (tmp_build_path / 'site-artifact').relative_to(REPO).as_posix()
            artifact_dir = REPO / rel_artifact

            build_docs.stage_site(REPO, rel_stage)
            site_content = build_docs.load_site_content(REPO)
            _render_staged_markdown_to_html(
                REPO / rel_stage,
                artifact_dir,
                site_content,
            )

            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(
                    0,
                    drift.main(['--root', str(REPO), '--site-dir', rel_artifact, '--write-site-receipt']),
                    out.getvalue(),
                )
                self.assertEqual(
                    0,
                    drift.main(['--root', str(REPO), '--site-dir', rel_artifact]),
                    out.getvalue(),
                )

            # 1. Missing required built page fails drift-check.py --site-dir.
            examples_html_path = artifact_dir / 'examples.html'
            saved_examples_html = examples_html_path.read_bytes()
            examples_html_path.unlink()
            buf_missing = io.StringIO()
            with contextlib.redirect_stdout(buf_missing):
                self.assertEqual(1, drift.main(['--root', str(REPO), '--site-dir', rel_artifact]))
            self.assertIn(
                f'site: {rel_artifact}: missing allowlisted built output: examples.html',
                buf_missing.getvalue(),
            )
            examples_html_path.write_bytes(saved_examples_html)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, drift.main(['--root', str(REPO), '--site-dir', rel_artifact]))

            # 2. Leaked private file in built artifact fails drift-check.py --site-dir.
            leaked_todo = artifact_dir / 'todo/implementation-plan.html'
            leaked_todo.parent.mkdir(parents=True, exist_ok=True)
            leaked_todo.write_text('<html><body>leaked plan</body></html>\n', encoding='utf-8')
            leaked_agents = artifact_dir / 'AGENTS.md'
            leaked_agents.write_text('# leaked agents\n', encoding='utf-8')
            buf_leaked = io.StringIO()
            with contextlib.redirect_stdout(buf_leaked):
                self.assertEqual(1, drift.main(['--root', str(REPO), '--site-dir', rel_artifact]))
            self.assertIn(f'site-private: {rel_artifact}: private path in artifact:', buf_leaked.getvalue())
            shutil.rmtree(artifact_dir / 'todo')
            leaked_agents.unlink()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, drift.main(['--root', str(REPO), '--site-dir', rel_artifact]))

    def test_accessible_navigation_site_states_base_path_and_anchors(self) -> None:
        site_content = build_docs.load_site_content(self.root)
        config_sources = {e['source']: e['staged'] for e in site_content['config']}
        asset_sources = {e['source']: e['staged'] for e in site_content['assets']}

        self.assertEqual('_includes/head_custom.html', config_sources.get('docs/_includes/head_custom.html'))
        self.assertEqual('_includes/header_custom.html', config_sources.get('docs/_includes/header_custom.html'))
        self.assertEqual('404.md', config_sources.get('docs/404.md'))
        self.assertEqual('assets/css/site-states.css', asset_sources.get('docs/assets/css/site-states.css'))
        self.assertEqual('assets/js/site-states.js', asset_sources.get('docs/assets/js/site-states.js'))

        css_text = (self.root / 'docs/assets/css/site-states.css').read_text(encoding='utf-8')
        for theme in ('light', 'dark', 'hc-light', 'hc-dark'):
            self.assertIn(f'html[data-rf-theme="{theme}"]', css_text)
        for density in ('comfortable', 'compact'):
            self.assertIn(f'html[data-rf-density="{density}"]', css_text)
        self.assertIn(':focus-visible', css_text)
        self.assertIn('.rf-page-outline', css_text)
        self.assertIn('.rf-copy-button', css_text)
        self.assertIn('.search-no-result', css_text)
        self.assertIn('.rf-search-offline', css_text)

        staged = build_docs.stage_site(self.root, 'build/site-source')
        for expected in (
            '404.md',
            '_includes/head_custom.html',
            '_includes/header_custom.html',
            'assets/css/site-states.css',
            'assets/js/site-states.js',
        ):
            self.assertIn(expected, staged)

        staged_404 = (self.root / 'build/site-source/404.md').read_text(encoding='utf-8')
        self.assertIn('permalink: /404.html', staged_404)
        self.assertIn('nav_exclude: true', staged_404)
        self.assertIn('search_exclude: true', staged_404)
        self.assertIn('(/RasterFold/index.html)', staged_404)
        self.assertIn('(/RasterFold/troubleshooting.html)', staged_404)

        artifact_dir = self.root / 'build/site-artifact'
        _render_staged_markdown_to_html(self.root / 'build/site-source', artifact_dir, site_content)
        self.assertTrue((artifact_dir / '404.html').is_file())
        self.assertTrue((artifact_dir / 'assets/css/site-states.css').is_file())
        self.assertTrue((artifact_dir / 'assets/js/site-states.js').is_file())

        search_data = json.loads((artifact_dir / 'assets/js/search-data.json').read_text(encoding='utf-8'))
        indexed_urls = {entry['url'].split('#')[0] for entry in search_data.values()}
        expected_urls = {
            '/RasterFold/' if page['staged'] == 'index.md' else f"/RasterFold/{page['staged'][:-3]}.html"
            for page in site_content['pages']
        }
        self.assertEqual(expected_urls, indexed_urls)
        self.assertIn('/RasterFold/api.html', indexed_urls)
        self.assertNotIn('/RasterFold/404.html', indexed_urls)
        for url in indexed_urls:
            self.assertTrue(url.startswith('/RasterFold/'), url)

        # Base-path, asset, 404.html, and fragment checks via drift-check.py --site-dir
        (REPO / 'build').mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='test-docs-states-', dir=REPO / 'build') as tmp_build:
            tmp_build_path = Path(tmp_build)
            rel_stage = (tmp_build_path / 'site-source').relative_to(REPO).as_posix()
            rel_artifact = (tmp_build_path / 'site-artifact').relative_to(REPO).as_posix()
            repo_artifact = REPO / rel_artifact

            build_docs.stage_site(REPO, rel_stage)
            _render_staged_markdown_to_html(REPO / rel_stage, repo_artifact, site_content)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, drift.main(['--root', str(REPO), '--site-dir', rel_artifact, '--write-site-receipt']))
                self.assertEqual(0, drift.main(['--root', str(REPO), '--site-dir', rel_artifact]))

            # Missing 404.html or missing site-states.css fails expected_outputs
            not_found_path = repo_artifact / '404.html'
            saved_404 = not_found_path.read_bytes()
            not_found_path.unlink()
            buf_404 = io.StringIO()
            with contextlib.redirect_stdout(buf_404):
                self.assertEqual(1, drift.main(['--root', str(REPO), '--site-dir', rel_artifact]))
            self.assertIn(f'site: {rel_artifact}: missing allowlisted built output: 404.html', buf_404.getvalue())
            not_found_path.write_bytes(saved_404)

            # Root-relative asset link outside /RasterFold/ or broken fragment fails drift-check.py --site-dir
            index_path = repo_artifact / 'index.html'
            saved_index = index_path.read_text(encoding='utf-8')
            index_path.write_text(
                saved_index.replace(
                    '/RasterFold/assets/css/site-states.css',
                    '/assets/css/site-states.css',
                ).replace(
                    '</main>',
                    '<a href="/RasterFold/format.html#nonexistent-section-anchor">Bad anchor</a></main>',
                ),
                encoding='utf-8',
                newline='\n',
            )
            c_site = drift.Checker(REPO)
            c_site.manifest = json.loads((REPO / 'docs/drift.json').read_text(encoding='utf-8'))
            c_site.files = set(c_site.git('ls-files', '-z').decode('utf-8').rstrip('\0').split('\0'))
            (repo_artifact / 'drift-build.json').write_text(
                json.dumps(c_site.site_receipt(rel_artifact)),
                encoding='utf-8',
                newline='\n',
            )
            buf_bad = io.StringIO()
            with contextlib.redirect_stdout(buf_bad):
                self.assertEqual(1, drift.main(['--root', str(REPO), '--site-dir', rel_artifact]))
            self.assertIn('site-link: index.html: URL outside Pages base path: /assets/css/site-states.css', buf_bad.getvalue())
            self.assertIn('anchor: index.html: missing heading or id: /RasterFold/format.html#nonexistent-section-anchor', buf_bad.getvalue())

    def test_verify_live_publication_and_pages_workflow_contract(self) -> None:
        pages_yml_path = REPO / '.github/workflows/pages.yml'
        self.assertTrue(pages_yml_path.is_file(), 'missing .github/workflows/pages.yml')
        pages_yml = pages_yml_path.read_text(encoding='utf-8')
        self.assertIn('branches: [main]', pages_yml)
        self.assertIn('workflow_dispatch:', pages_yml)
        self.assertIn('source_ref:', pages_yml)
        self.assertIn('contents: read', pages_yml)
        self.assertIn('pages: write', pages_yml)
        self.assertIn('id-token: write', pages_yml)
        self.assertIn('group: pages', pages_yml)
        self.assertIn('cancel-in-progress: false', pages_yml)
        self.assertIn("grep -Eq '^[0-9a-f]{40}$'", pages_yml)
        self.assertIn('git merge-base --is-ancestor "$INPUT_SOURCE_REF" origin/main', pages_yml)
        self.assertIn('git checkout --detach "$TARGET_SHA"', pages_yml)
        self.assertIn('python3 scripts/build-docs.py --self-test', pages_yml)
        self.assertIn('python3 scripts/build-docs.py --check', pages_yml)
        self.assertIn('python3 scripts/build-docs.py --stage build/site-source', pages_yml)
        self.assertIn('python3 scripts/drift-check.py --site-dir build/site-artifact --write-site-receipt', pages_yml)
        self.assertIn('python3 scripts/drift-check.py --site-dir build/site-artifact', pages_yml)
        self.assertIn('name: drift-build-receipt', pages_yml)
        self.assertIn('--verify-live', pages_yml)
        self.assertIn('--expected-receipt build/expected-receipt/drift-build.json', pages_yml)
        self.assertIn('--expected-commit "$TARGET_SHA"', pages_yml)

        # Every action uses: line in pages.yml must be pinned to a 40-hex SHA with a version comment.
        uses_lines = [line.strip() for line in pages_yml.splitlines() if re.match(r'^\s*-?\s*uses:\s*', line)]
        self.assertGreaterEqual(len(uses_lines), 8)
        for line in uses_lines:
            self.assertRegex(
                line,
                r'^(- )?uses: [A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40} # v\d+\.\d+\.\d+$',
                f'unpinned or uncommented action in pages.yml: {line}',
            )

        # Build a staged and rendered fixture site with a valid drift-build.json receipt.
        (REPO / 'build').mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='test-docs-live-', dir=REPO / 'build') as tmp_build:
            tmp_build_path = Path(tmp_build)
            rel_stage = (tmp_build_path / 'site-source').relative_to(REPO).as_posix()
            rel_artifact = (tmp_build_path / 'site-artifact').relative_to(REPO).as_posix()
            artifact_dir = REPO / rel_artifact

            build_docs.stage_site(REPO, rel_stage)
            site_content = build_docs.load_site_content(REPO)
            _render_staged_markdown_to_html(REPO / rel_stage, artifact_dir, site_content)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, drift.main(['--root', str(REPO), '--site-dir', rel_artifact, '--write-site-receipt']))

            receipt_path = artifact_dir / 'drift-build.json'
            expected_receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
            expected_commit = expected_receipt['commit']
            self.assertRegex(expected_commit, r'^[0-9a-f]{40}$')

            base_url = 'https://rizonesoft.github.io/RasterFold'
            served_files: dict[str, bytes] = {
                f'{base_url}/': (artifact_dir / 'index.html').read_bytes(),
                f'{base_url}/drift-build.json': receipt_path.read_bytes(),
            }
            for rel_page in build_docs.LIVE_SAMPLE_PAGES:
                served_files[f'{base_url}/{rel_page}'] = (artifact_dir / rel_page).read_bytes()

            def _fetcher_from(mapping: dict[str, bytes]):
                def _fetch(url: str) -> tuple[int, bytes]:
                    if url not in mapping:
                        raise ValueError(f'HTTP 404 for {url}')
                    return 200, mapping[url]
                return _fetch

            # 1. Exact match passes verify_live_publication and CLI --verify-live.
            summary = build_docs.verify_live_publication(
                base_url,
                receipt_path,
                expected_commit,
                retries=1,
                retry_delay=0.0,
                fetcher=_fetcher_from(served_files),
            )
            self.assertEqual(expected_commit, summary['commit'])
            self.assertEqual(len(expected_receipt['outputs']), summary['outputs_count'])

            # 2. Stale same-commit deployment (same commit SHA, different outputs map) is rejected.
            stale_same_commit = json.loads(json.dumps(expected_receipt))
            stale_same_commit['outputs']['index.html'] = '0' * 64
            stale_map = dict(served_files)
            stale_map[f'{base_url}/drift-build.json'] = (json.dumps(stale_same_commit) + '\n').encode('utf-8')
            with self.assertRaisesRegex(ValueError, r"outputs.*differs from expected build receipt"):
                build_docs.verify_live_publication(
                    base_url,
                    receipt_path,
                    expected_commit,
                    retries=1,
                    retry_delay=0.0,
                    fetcher=_fetcher_from(stale_map),
                )

            # 3. Wrong commit SHA in live drift-build.json is rejected.
            wrong_commit_receipt = json.loads(json.dumps(expected_receipt))
            wrong_commit_receipt['commit'] = 'a' * 40
            wrong_commit_map = dict(served_files)
            wrong_commit_map[f'{base_url}/drift-build.json'] = (json.dumps(wrong_commit_receipt) + '\n').encode('utf-8')
            with self.assertRaisesRegex(ValueError, 'live receipt commit'):
                build_docs.verify_live_publication(
                    base_url,
                    receipt_path,
                    expected_commit,
                    retries=1,
                    retry_delay=0.0,
                    fetcher=_fetcher_from(wrong_commit_map),
                )

            # 4. Missing or malformed live drift-build.json is rejected.
            missing_receipt_map = dict(served_files)
            del missing_receipt_map[f'{base_url}/drift-build.json']
            with self.assertRaisesRegex(ValueError, 'HTTP 404'):
                build_docs.verify_live_publication(
                    base_url,
                    receipt_path,
                    expected_commit,
                    retries=1,
                    retry_delay=0.0,
                    fetcher=_fetcher_from(missing_receipt_map),
                )

            malformed_map = dict(served_files)
            malformed_map[f'{base_url}/drift-build.json'] = b'{not-valid-json'
            with self.assertRaisesRegex(ValueError, 'invalid JSON'):
                build_docs.verify_live_publication(
                    base_url,
                    receipt_path,
                    expected_commit,
                    retries=1,
                    retry_delay=0.0,
                    fetcher=_fetcher_from(malformed_map),
                )

            # 5. Sampled live page SHA-256 mismatch against drift-build.json["outputs"] is rejected.
            tampered_page_map = dict(served_files)
            tampered_page_map[f'{base_url}/benchmarks.html'] = served_files[f'{base_url}/benchmarks.html'] + b'<!-- tampered -->'
            with self.assertRaisesRegex(ValueError, 'live page benchmarks.html sha256'):
                build_docs.verify_live_publication(
                    base_url,
                    receipt_path,
                    expected_commit,
                    retries=1,
                    retry_delay=0.0,
                    fetcher=_fetcher_from(tampered_page_map),
                )

            # 6. Transient failure recovers within retry budget.
            attempts = {'count': 0}

            def _flaky_fetcher(url: str) -> tuple[int, bytes]:
                if url.endswith('/drift-build.json') and attempts['count'] < 2:
                    attempts['count'] += 1
                    raise ValueError('transient CDN propagation delay')
                return _fetcher_from(served_files)(url)

            recovered = build_docs.verify_live_publication(
                base_url,
                receipt_path,
                expected_commit,
                retries=3,
                retry_delay=0.0,
                fetcher=_flaky_fetcher,
            )
            self.assertEqual(2, attempts['count'])
            self.assertEqual(expected_commit, recovered['commit'])

            # 7. Retry exhaustion raises the last error.
            def _always_failing_fetcher(url: str) -> tuple[int, bytes]:
                raise ValueError(f'persistent CDN 503 for {url}')

            with self.assertRaisesRegex(ValueError, 'persistent CDN 503'):
                build_docs.verify_live_publication(
                    base_url,
                    receipt_path,
                    expected_commit,
                    retries=2,
                    retry_delay=0.0,
                    fetcher=_always_failing_fetcher,
                )

            # 8. Empty --verify-live "" or --stage "" in CLI main() rejects without calling write_generated_pages.
            write_calls = {'count': 0}
            orig_writer = build_docs.write_generated_pages

            def _forbidden_writer(*_a, **_kw):
                write_calls['count'] += 1
                return []

            build_docs.write_generated_pages = _forbidden_writer
            try:
                err_empty_verify = io.StringIO()
                with contextlib.redirect_stderr(err_empty_verify):
                    self.assertEqual(
                        1,
                        build_docs.main(
                            [
                                '--root',
                                str(self.root),
                                '--verify-live',
                                '',
                                '--expected-receipt',
                                str(receipt_path),
                                '--expected-commit',
                                expected_commit,
                            ]
                        ),
                    )
                self.assertEqual(0, write_calls['count'])
                self.assertIn('--verify-live URL must be non-empty', err_empty_verify.getvalue())

                err_empty_stage = io.StringIO()
                with contextlib.redirect_stderr(err_empty_stage):
                    self.assertEqual(1, build_docs.main(['--root', str(self.root), '--stage', '']))
                self.assertEqual(0, write_calls['count'])
                self.assertIn('invalid staging destination', err_empty_stage.getvalue())
            finally:
                build_docs.write_generated_pages = orig_writer

            # 9. Truncated HTTP response (http.client.IncompleteRead) inside _default_http_fetcher converts to ValueError and recovers via retry.
            import http.client
            import urllib.request

            urlopen_calls = {'count': 0}
            orig_urlopen = urllib.request.urlopen

            class _FakeResponse:
                def __init__(self, status: int, body: bytes, truncate: bool = False) -> None:
                    self.status = status
                    self._body = body
                    self._truncate = truncate

                def __enter__(self):
                    return self

                def __exit__(self, exc_type, exc, tb):
                    return False

                def read(self) -> bytes:
                    if self._truncate:
                        raise http.client.IncompleteRead(self._body[:8], len(self._body) - 8)
                    return self._body

            def _mock_urlopen(req, timeout=20):
                raw_url = req.full_url.split('?_rf_verify=', 1)[0]
                if raw_url not in served_files:
                    raise ValueError(f'unexpected URL {raw_url}')
                urlopen_calls['count'] += 1
                if urlopen_calls['count'] == 1:
                    return _FakeResponse(200, served_files[raw_url], truncate=True)
                return _FakeResponse(200, served_files[raw_url], truncate=False)

            urllib.request.urlopen = _mock_urlopen
            try:
                recovered_http = build_docs.verify_live_publication(
                    base_url,
                    receipt_path,
                    expected_commit,
                    retries=2,
                    retry_delay=0.0,
                )
                self.assertEqual(2, recovered_http['attempt'])
                self.assertEqual(expected_commit, recovered_http['commit'])
            finally:
                urllib.request.urlopen = orig_urlopen


if __name__ == '__main__':
    unittest.main()
