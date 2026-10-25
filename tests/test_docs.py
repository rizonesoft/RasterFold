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
    'docs/Gemfile',
    'docs/index.md',
    'docs/getting-started.md',
    'docs/installation.md',
    'docs/usage.md',
    'docs/c-api.md',
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

    for page in pages:
        staged_rel = page['staged']
        out_rel = re.sub(r'\.md$', '.html', staged_rel)
        md_text = (staged_dir / staged_rel).read_text(encoding='utf-8')
        body = md_text.split('---\n', 2)[2] if md_text.startswith('---\n') and md_text.count('---\n') >= 2 else md_text

        # Expand generated SVG and table includes sobenchmarks.html carries its charts and tables.
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
        doc_html = (
            '<!DOCTYPE html>\n<html lang="en"><head><meta charset="utf-8">'
            f'<title>{html.escape(page["title"])}</title></head><body>\n'
            f'{nav_html}\n<main id="main-content">\n'
            + '\n'.join(rendered_lines)
            + f'\n</main>\n{footer_html}\n</body></html>\n'
        )
        out_path = artifact_dir / out_rel
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(doc_html, encoding='utf-8', newline='\n')


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
            if exp_kind == 'mirrored':
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

        # Mirrored page overwriting its canonical source in place is rejected.
        bad_mirror = json.loads(json.dumps(orig_sc))
        for p in bad_mirror['pages']:
            if p['id'] == 'installation':
                p['output'] = p['source']
        self.write('docs/site-content.json', json.dumps(bad_mirror, indent=2) + '\n')
        with self.assertRaisesRegex(ValueError, "mirrored page 'installation' output cannot overwrite source"):
            build_docs.load_site_content(self.root)

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


if __name__ == '__main__':
    unittest.main()
