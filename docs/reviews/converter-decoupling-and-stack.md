## Round 1: astra (`gpt-6-astra` via `codex`)

- Writer: claude
- Run: 2026-11-11T18:39:14Z
- Candidate: 43ee9c95969a7fc451aeff4233d827b8868ee6cc
- Effort: medium
- Lenses: adversarial, integration, source-defect, plan-delivery, consistency, record, quality, optimization
- Lines: 22
- Digest: 5d765423631f008b

Found four P2 defects in `e1a46b2..43ee9c9`.

1. **P2: Release documentation lost its prerequisite edge.** `todo/09-release/TODO-01-release-v1.md:47` removes converter acceptance without retaining its documentation prerequisites. `D08 T01 §7` and `D08 T03 §1`/`§2` now block only converter acceptance, although release §4 requires matching downloads and CLI/API references. The audit passes because those owners reach a later gate. **Smallest fix:** add `D08 T01 §7` directly to release §4’s dependencies; it already depends on both references.

2. **P2: Phase 9 still requests GUI downloads.** `todo/08-docs/TODO-01-html-docs.md:216` requires executable/GUI packages for each target. Converter packages now arrive in Phase 60 and only for Windows. The stated acceptance condition therefore exceeds the 1.0 scope. **Smallest fix:** require CLI/SDK assets for 1.0 and explicitly assign adding Windows converter downloads to Phase 60.

3. **P2: Later implementation instructions retain the discarded C UI architecture.** `todo/10-converter/TODO-03-interface.md:130` still directs implementation into `converter/ui/*.c`; the same assumption continues at lines 158, 186 and 213. These sections must now integrate with the C++23 `rui::` shell under `converter/app/`. Additionally, `todo/10-converter/TODO-01-engine.md:91` still requires application files to compile as C11, contradicting the new shell build. **Smallest fix:** specify the later UI modules as C++ files integrated with the shell, and restrict the C11 build requirement to engine/adapters. Update the remaining “C interface” outcome accordingly.

4. **P2: The capability registry still promises bundled HEIC writing.** `converter/codecs.json:674` and `converter/codecs.json:723` name bundled libheif; lines 680 and 729 advertise writing. These planned capabilities now contradict the recorded import-only Windows-system-provider decision. Marking them `planned` prevents execution but does not make the advertised plan accurate. **Smallest fix:** update both registry entries to the system provider with writing disabled, retain their unimplemented availability, and reconcile the refusal descriptions and registry expectations.

Read-only verification: the graph audit returned zero errors across 253 sections. Both new tests passed and failed under targeted in-memory mutations removing later-gate coverage or missing-gate validation. A disconnected ordinary owner was correctly rejected. No files changed.

adversarial: approve
integration: needs-attention
source-defect: approve
plan-delivery: needs-attention
consistency: needs-attention
record: approve
quality: approve
optimization: approve
HIGHEST: P2
OVERALL: needs-attention

## Disposition of round 1

All four findings were verified against the tree and fixed in the commit that records this round.

- P2, release documentation edge: `D09 T01 §4` depends on `D08 T01 §7` again, so the downloads and references page blocks the 1.0 release directly.
- P2, GUI downloads in Phase 9: the `docs/downloads.md` item in `D08 T01 §7` names command-line and library packages for Windows x64 and Linux x64, and assigns the Windows converter download to `D08 T01 §6` in Phase 60.
- P2, discarded C interface architecture: the later interface items in `D10 T03` name `converter/app/*.cpp` on the `resolute-ui` shell, the outcome line names the Windows interface, `D10 T01 §2` owns the CMake build with C11 limited to the engine and adapters, and `D10 T03 §1` depends on `D10 T01 §2` and `§3`.
- P2, registry promised bundled HEIC writing: `converter/codecs.json` and `converter/src/registry.c` name the Windows Imaging Component as the HEIF and HEIC provider with writing disabled, and `scripts/check-converter.py` treats both as import-only; `python3 scripts/check-converter.py --case contract` passes.

