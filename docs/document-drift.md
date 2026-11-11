# Document drift checks and repair

Run `python3 scripts/drift-check.py` before committing source or documentation changes. The command returns 0 when its contracts hold, 1 for reported drift, and 2 when it cannot complete a check. It uses Python 3.11+ and the standard library. It makes no network requests.

Use `--root` only with a checkout whose Git configuration you trust. For an ordinary checkout, the tool addresses its `.git` directory explicitly, allowing native Windows to read the owner's WSL checkout without global `safe.directory` changes. This bypasses Git's ownership discovery check for that selected repository. Git-directory files, including submodules, retain normal discovery and its ownership checks.

## What is checked

`docs/drift.json` defines the checked facts, document roles, generated blocks, source review receipts and generators, and is validated on load against unknown roles, missing reasons, unknown fact providers or placeholders, invalid encodings and escaping paths. `docs/facts.json` is derived from exact source matches and uniquely selected data rows. Facts describe the checked source, not the last binary built on disk.

Every tracked or newly added supported information file is inventoried. Supported extensions are Markdown, RST, AsciiDoc, text, HTML, JSON/JSONL, YAML, TOML, INI/CFG, CSV, SVG and XML, plus LICENSE, NOTICE, COPYING, `.gitignore` and `.gitattributes`. Ignored outputs and downloaded third-party trees are excluded. Explicit encoding rules preserve original license bytes.

Applicable checks include source claims, fact bindings, named generated blocks, local Markdown/HTML links (including nested bracket labels, balanced-parenthesis destinations, angle-bracket destinations and reference link definitions), ATX (including up to three leading spaces and linked headings) and Setext (`===` and `---`) heading anchors, and explicit commit/path references. Built HTML IDs and `/RasterFold/` base-path links are checked directly in the built Jekyll site artifact (`build/site-artifact/`). JSON, JSONL, TOML and SVG/XML also receive syntax checks. YAML, CSV, INI, RST and AsciiDoc receive inventory and declared-contract checks; this is not a general syntax or prose parser for those formats. Fenced examples and registered authoring templates are distinguished from live contracts.

Current, planned and historical information have separate roles. Dated benchmarks and review findings remain evidence of their recorded conditions. A registered source review receipt binds the reviewed document and its dependencies; it fails whenever those bytes change, including on the same day. Initial receipts are author-attested source reviews, not independent section stamps. Receipt sources require `eol=lf` or `-text`, and every receipt must include its own document. Older Windows checkouts can retain CRLF after an attribute update: preserve local edits and refresh or normalize only the affected file before review. Renewal refuses such bytes rather than recording a host-specific hash. Arbitrary prose accuracy, external URL availability, benchmark reproducibility and screen appearance still require their own review or driven test. Inventory coverage alone is not semantic proof.

```bash
python3 scripts/drift-check.py --coverage
python3 scripts/drift-check.py --json
python3 scripts/plan-audit.py
```

The plan audit uses the canonical TODO parser. It checks prerequisite order within and between phases and requires every owner in `docs/vision-map.json` to block the release gate, or one of the map's `later_gates` (the native converter's acceptance, which follows the release), through the dependency graph. The graph validator separately enforces section/row parity, reciprocal references, cycles and review stamps.

## Repair generated output

```bash
python3 scripts/drift-check.py --fix
python3 scripts/drift-check.py
```

Repair writes only registered fact output, named generated blocks and the outputs of registered repository generators. Generator repairs run in an isolated staging copy of the checkout, refuse undeclared or planned-output writes, and publish changed outputs through the same safe writer as direct repairs. Every repair compares bytes before replacing a file, uses atomic replacement with `os.fsync`, preserves file modes, verifies the written bytes on read-back and refuses symlink or parent traversal. Run it with the repository's single writer rule; it is not a multi-file transaction. Inspect the resulting Git diff and stage the intended files. A second repair on clean inputs makes no change. In check mode each generator is proved read-only by comparing the checkout before and after it runs. That comparison leaves out the state an agent harness rewrites in its own session, which `.gitignore` already excludes (`.claude/scheduled_tasks.lock`, `.claude/state/`, `.codex/state/`): Claude Code rewrites its cron lock whenever a session job is created, fires or is deleted, so a campaign's heartbeat would otherwise fail the proof with no generator at fault. Every other path under `.claude/` and `.codex/` is still compared.

Authored prose, source review receipts, historical measurements and Git history are never automatically rewritten. Unresolved manual errors still produce a failing exit after safe repairs. Correct the source or the supported assertion deliberately. After actually reading a document against its changed sources, renew only that receipt with concrete evidence:

```bash
python3 scripts/drift-check.py --review docs/library.md --evidence 'Author-attested: 2026-10-25 reviewed ownership and return codes against include/rasterfold.h and src/api.c; named test evidence in the review record'
```

The command records your review assertion; it cannot perform or certify that human review for you.

## Declare facts and commit references

Place a real annotation next to the prose it supports. These fenced examples are documentation, not active assertions:

```html
<!-- drift:fact format_version = 2 -->
<!-- claim: exists src/cm.c -->
<!-- drift:commit <full-commit-id> include/rasterfold.h -->
```

An optional commit trailer such as `Drift-Fact: format_version=2` must match facts derived from that commit's own manifest and source. Malformed `Drift-Fact` trailer lines are rejected. `commit-msg` reads the index (`--git-dir`) and refuses if `scripts/drift-check.py` was removed from the index while present in `HEAD`. CI checks declarations across the pushed or proposed commit range (`BASE..HEAD`, falling back to `HEAD^..HEAD` or `HEAD` when `BASE` is absent or all-zeros); ordinary commit subjects are not interpreted as machine facts. A false historical declaration calls for a corrective commit, never a history rewrite.

The pre-commit hook exports the index, refuses if any tracked gate script in `HEAD` was removed from the index, and runs TODO, vision and drift checks on those exact bytes. An unstaged fix cannot conceal a bad staged document. Plan checks always validate canonical Markdown from the selected working tree or staged export and also validate existing ignored caches; missing caches in a fresh clone or export are allowed and are never created by a check. Local `scripts/check-all.py` and CI run the same checks and regression fixtures against the pinned candidate commit. CI runs for every main-branch push and pull request, including documentation-only changes.

## Check a built Pages artifact

The Pages build stages only the product-content allowlist declared in `docs/site-content.json` and described in [the documentation design](design/documentation.md), builds `build/site-artifact/`, and runs:

```bash
python3 scripts/drift-check.py --site-dir build/site-artifact --site-prefix /RasterFold/ --write-site-receipt
python3 scripts/drift-check.py --site-dir build/site-artifact --site-prefix /RasterFold/
```

Receipt creation (`build/site-artifact/drift-build.json`) requires clean source checks and a valid artifact. The checker verifies that `docs/site-content.json` and every allowlisted input (`config`, `pages`, `assets`) are covered by `site_sources`, records the checked-out Git `commit` SHA (`HEAD`), hashes every declared input and every built output, verifies that every allowlisted page and asset was built, checks local HTML links and fragments under the project base path, and refuses symlinks or reserved private paths. Changed commit identity, source, allowlist mapping or output bytes invalidate the receipt. This proves input/artifact correspondence after a trusted build; hashing cannot prove that an arbitrary HTML page tells the truth. The Pages deployment workflow (`.github/workflows/pages.yml`, `D08 T01 §3`) publishes that checked artifact and runs `python3 scripts/build-docs.py --verify-live` to compare the deployed receipt and sampled pages against the build job's trusted `drift-build.json` artifact. Local fixture success is not deployment evidence.
