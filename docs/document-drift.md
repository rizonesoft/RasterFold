# Document drift checks and repair

Run `python3 scripts/drift-check.py` before committing source or documentation changes. The command returns 0 when its contracts hold, 1 for reported drift, and 2 when it cannot complete a check. It uses Python 3.11+ and the standard library. It makes no network requests.

Use `--root` only with a checkout whose Git configuration you trust. For an ordinary checkout, the tool addresses its `.git` directory explicitly, allowing native Windows to read the owner's WSL checkout without global `safe.directory` changes. This bypasses Git's ownership discovery check for that selected repository. Git-directory files, including submodules, retain normal discovery and its ownership checks.

## What is checked

`docs/drift.json` defines the checked facts, document roles, generated blocks, source review receipts and generators. `docs/facts.json` is derived from exact source matches and uniquely selected data rows. Facts describe the checked source, not the last binary built on disk.

Every tracked or newly added supported information file is inventoried. Supported extensions are Markdown, RST, AsciiDoc, text, HTML, JSON/JSONL, YAML, TOML, INI/CFG, CSV, SVG and XML, plus LICENSE, NOTICE, COPYING, `.gitignore` and `.gitattributes`. Ignored outputs and downloaded third-party trees are excluded. Explicit encoding rules preserve original license bytes.

Applicable checks include source claims, fact bindings, named generated blocks, local Markdown/HTML links and ATX heading anchors, and explicit commit/path references. Built HTML IDs are checked directly; Setext source headings remain an owned extension in D08 T02 section 5. JSON, JSONL, TOML and SVG/XML also receive syntax checks. YAML, CSV, INI, RST and AsciiDoc receive inventory and declared-contract checks; this is not a general syntax or prose parser for those formats. Fenced examples and registered authoring templates are distinguished from live contracts.

Current, planned and historical information have separate roles. Dated benchmarks and review findings remain evidence of their recorded conditions. A registered source review receipt binds the reviewed document and its dependencies; it fails whenever those bytes change, including on the same day. Initial receipts are author-attested source reviews, not independent section stamps. Receipt sources require `eol=lf` or `-text`, and every receipt must include its own document. Older Windows checkouts can retain CRLF after an attribute update: preserve local edits and refresh or normalize only the affected file before review. Renewal refuses such bytes rather than recording a host-specific hash. Arbitrary prose accuracy, external URL availability, benchmark reproducibility and screen appearance still require their own review or driven test. Inventory coverage alone is not semantic proof.

```bash
python3 scripts/drift-check.py --coverage
python3 scripts/drift-check.py --json
python3 scripts/plan-audit.py
```

The plan audit uses the canonical TODO parser. It checks prerequisite order within and between phases and requires every owner in `docs/vision-map.json` to block the release gate through the dependency graph. The graph validator separately enforces section/row parity, reciprocal references, cycles and review stamps.

## Repair generated output

```bash
python3 scripts/drift-check.py --fix
python3 scripts/drift-check.py
```

Repair writes only registered fact output, named generated blocks and the outputs of registered repository generators. It compares bytes before replacing a file, uses atomic replacement, preserves file modes and refuses symlink or parent traversal. Run it with the repository's single writer rule; it is not a multi-file transaction. Inspect the resulting Git diff and stage the intended files. A second repair on clean inputs makes no change.

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

An optional commit trailer such as `Drift-Fact: format_version=2` must match facts derived from that commit's own manifest and source. `commit-msg` reads the index. CI checks declarations across the pushed or proposed commit range; ordinary commit subjects are not interpreted as machine facts. A false historical declaration calls for a corrective commit, never a history rewrite.

The pre-commit hook exports the index and runs TODO, vision and drift checks on those exact bytes. An unstaged fix cannot conceal a bad staged document. Plan checks always validate canonical Markdown from the selected working tree or staged export and also validate existing ignored caches; missing caches in a fresh clone or export are allowed and are never created by a check. Local `scripts/check-all.py` and CI run the same checks and regression fixtures. CI runs for every main-branch push and pull request, including documentation-only changes.

## Check a built Pages artifact

The full Pages site remains planned. Its publisher must build only the product-content allowlist described in [the documentation design](design/documentation.md), then run:

```bash
python3 scripts/drift-check.py --site-dir _site --site-prefix /RasterFold/ --write-site-receipt
python3 scripts/drift-check.py --site-dir _site --site-prefix /RasterFold/
```

Receipt creation requires clean source checks and a valid artifact. The checker hashes declared inputs and every output, checks local HTML links and fragments under the project base path, and refuses reserved private paths. Changed source or output bytes invalidate the receipt. This proves input/artifact correspondence after a trusted build; hashing cannot prove that an arbitrary HTML page tells the truth. The future Pages workflow must publish that checked artifact and compare the deployed receipt on readback. Local fixture success is not deployment evidence.
