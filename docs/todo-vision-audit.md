# TODO and product vision audit

Date: 2026-09-30. Base: `bc57d9e079798fd4585abfe41d078c7d04f4ebb8`. Groom run: `61f3a8f4474f`. This is attended maintenance; neither runner was started. No TODO completion row or operator decision was changed.

## Scope and decisions

The operator requested complete GitHub Pages documentation, a native lossless converter demonstrating RasterFold and leading codecs, single-image preview and comparisons, a modern C interface with original SVG icons, and document drift detection and repair. Follow-up answers selected native Windows/Linux/Android and allowed audited bundled codec source with no additional user installs. [The product vision](vision.md) and [converter design](design/converter.md) retain those decisions. Vendored C++ behind C adapters and minimal Android SDK interop are explicit implementation defaults, not a claim that every third-party codec is written in C.

The [documentation design](design/documentation.md) preserves the existing Jekyll/Just the Docs decision and requires an explicit public-content allowlist. This checkout is private; account eligibility, visibility and the recorded operator Pages row remain deployment prerequisites.

## Changes and ownership

| Area | Owners | Result |
| --- | --- | --- |
| Converter engine | D10 T01 sections 1 to 6 | Pixel contract, pins/notices, bounded jobs, representation-specific exactness, safe files and fair reports |
| Codec adapters | D10 T02 sections 1 to 12 | Explicit format/depth/alpha/frame/metadata matrix, RAW import semantics and verification with explicit decoder provenance |
| Native interface | D10 T03 sections 1 to 8 | Windows, Wayland/X11 and Android, C SVG rendering, single-image preview, synchronized comparison, accessibility and real demo |
| Native delivery | D10 T04 sections 1 to 4 | Four-target runtime evidence, hostile inputs, self-contained packages and whole-product acceptance |
| Full documentation | D08 T01 sections 4 to 8 | Five new sections for current docs, navigation, converter guide, references/downloads and published acceptance |
| Drift | D08 T02 sections 6 and 7 | Complete supported-file inventory, deterministic repair, commit facts and site provenance |
| Versioned release | D09 T01 sections 1 to 4 | One product version, independent ABI/format versions, existing packager extended across all targets |

There are 39 added sections: 30 converter, five documentation, two drift and two release-only operator prerequisites. The two operator prerequisites carry the groom run Origin; the other additions are user-directed. The tree grows from 28 files/183 sections to 32 files/222 sections. New sections have small steps with named implementation paths, concrete completion conditions, test ownership and safety requirements. Future checkpoints explicitly require their test drivers to be implemented first. Existing section addresses and phase membership are preserved.

## Findings corrected

- The release plan named only Windows CLI/DLL artifacts. It now requires Windows/Linux x64 and Android ARM64/x64 CLI/SDK and separate converter packages, with runtime evidence distinguished from cross-compilation.
- The initial documentation work lacked complete product manuals, current-versus-planned labeling, accessible search/navigation states and private-content isolation. Those now have explicit owners and design contracts.
- Old workspace claims described committed work as in flight and the GitHub CLI as absent. Claims and surrounding prose were corrected against the checkout. Historical measurement commands retain their dated evidence and are not passed off as current runtime proof.
- Six prerequisite edges appeared after their consumers in phase/row order. Rows were reordered only within their original phases. `scripts/plan-audit.py` now makes such ordering errors fail.
- The adjacency inspector failed to resolve cross-file XREFs carrying a ` -- reason` suffix. The parser now separates the address from the explanation, with a regression that proves a real cross-file delivery owner is recognized. Incorrect document anchors were also corrected.
- Source-claim coverage was 27/27 blocks with a floor of one. It is now 31/31 with a floor of 31. All Current state blocks counted by the canonical checker are covered.
- Three backlog items were promoted without losing their source keys: grayscale to D05 T02 section 1, streaming to D05 T02 section 15, and alpha/16-bit to D05 T03 section 15. Their generated planning rows must carry the obligation into implementation or an explicit measured result. The backlog is now empty; existing operator removal records and budget decisions remain intact.
- Documentation and converter domains now participate in the existing design gate. Native display and Android runtime requirements are explicit; installed SDKs and environment variables cannot stand in for driven proof.

## Verification and limits

`python3 scripts/todo-graph.py validate` reports 32 TODOs, 222 sections, zero fatal findings, zero warnings and zero adjacency advisories. `python3 scripts/plan-audit.py` reports ten product requirement groups, with every listed owner on the dependency path to D09 T01 section 4. These are structural and source checks, not proof that future implementations are correct.

The drift implementation has regression fixtures for staged-tree isolation, wrong facts, stale source reviews, deterministic repair, historical preservation, bad references, native encoding preservation, CRLF block repair, inline example isolation, explicit review receipts, symlink escapes and Pages artifact/source mismatch. [The drift guide](document-drift.md) states the exact repair and semantic-proof boundaries. Generated output is implemented now; the complete site and converter remain planned work.

The research plan still has 60 milestones. Compression/speed targets need fresh corpus measurements before implementation relies on them, and a missed target follows the existing negative-result policy. Static plan completeness cannot guarantee a research win, operating-system availability, vendor licensing suitability or a working deployment. Full native runtime, accessibility, signed packaging, external review and live Pages readback remain acceptance obligations of their owning sections.

## Independent review corrections

The first independent Claude plan review identified undeclared site-receipt dependencies, Android SAF work ahead of its platform bridge, missing staging ownership, overbroad independent-decoder claims, a generated/manual page conflict and incomplete format/provider ownership. The plan now stages an explicit allowlist, assigns SAF to Android integration, distinguishes runtime fresh-decode verification from independent differential fixtures, gives exactness definitions their own authored page and owns PBM/PGM/PAM plus the PNG/APNG provider. Release signing custody and a concrete HEVC distribution decision are visible operator rows in Phase 9. Required native contexts, generated-output registration, build-feature reporting, final release Pages readback and multi-target ABI/static-analysis scope are explicit.

The first code review and separate Windows execution exposed host line-ending and locale assumptions. The checker now preserves original encoding and line endings inside authored files, while repository-generated files use the existing LF pins. Plan reads use explicit UTF-8. Both hosts test staged export with the repository line-ending policy. Inline examples cannot hide adjacent real contracts, and an unrelated working-copy symlink cannot affect facts derived from Git objects. Registered review receipts also bind the reviewed document bytes.

Local full verification completed with 29/29 gates passing: four-target builds, 33 Linux correctness cases, eight API tests, native consumer/ABI checks, 300 seeded fuzz mutations, graph self-tests, source claims, runner fixtures and provisioning. Changed documentation tooling subsequently passed 42 drift fixtures and 15 plan-audit/projection tests on Linux and Windows. These are local results; remote CI and independent source-review verdicts are recorded separately.

The actual staged export exposed one fresh-checkout defect after those gates: `plan --check` required two ignored JSON caches. It now checks canonical Markdown unconditionally and caches only when present, without writing either. Regression cases cover absent caches, stale present caches and stale Markdown with no caches. The real pre-commit export now passes TODO validation, generated milestones, release coverage and document drift with no pre-existing build directory.

## Native context derivation

The 2026-09-30 source audit classified the following checkpoints; this is a requirement derived from their commands and observations, not a claim that an unbuilt application ran. `tests/test_plan_audit.py` verifies that these explicit capabilities are never inferred from installed tools or environment variables.

| Sections | Required context | Checkpoint evidence needed |
| --- | --- | --- |
| D10 T01 section 2; D10 T02 sections 1 to 12 | Windows/Linux runtime plus both Android runtimes | Native bundled-provider loader or adapter checks on all four targets |
| D10 T01 section 5 | Windows/Linux runtime | Native file publication and refusal checks |
| D10 T03 sections 2, 3 and 4 | The corresponding Windows display, Linux display or both Android runtimes | Platform input, lifecycle and accessibility observations |
| D10 T03 sections 5 to 8; D10 T04 sections 1 to 4; D08 T01 section 6 | Both desktop displays and both Android runtimes | Complete interactions, packages or guide-following evidence across advertised targets |
| D08 T01 sections 5 and 8 | Linux display | Browser, navigation and accessibility observations |

Runtime contexts permit command-line/library execution without a desktop session; display contexts additionally require interactive UI evidence. Both families may declare a context only after recording a successful probe, and must still execute each section's complete checkpoint. Operator decisions are never inferred or declared by an agent.
