# RasterFold -- Implementation Plan

The order to run every section in, from today to a codec that beats lossless JPEG XL on every image class it is measured on.

> **Progress:** **92 of 246 sections complete (37%).** 246 sections (22 discovered); backlog 4 entries. Derived from the Implementation Order tables by `python scripts/todo-graph.py plan --sync` -- never edited by hand.
>
> **Plan/graph parity.** Every numbered TODO section, open or shipped, appears in exactly one phase table row. `plan --check` enforces missing, unknown, duplicate, and status parity. Read live totals from the generated Progress line above and `python scripts/todo-graph.py query stats`.

Seeded 2026-09-29 from the roadmap then kept in `suggestions.md`, when the TODO system was ported from Isotone. That file became the research record [`../docs/research.md`](../docs/research.md): it keeps the measurement protocol, the "Already tried" table, and the designs (sections 4 to 7), each naming the section here that owns it.

**How to use this.** The front door is the `process-plan` skill. It audits, then runs `process-phase` on the first phase that has a ready row, then the next ready phase after that closeout or park. One row is `process-todo-section` then `review-todo-section`.

**Copy a row and paste it.** The skills resolve a reference from whatever shape it arrives in, so this is a complete instruction:

```
process todo section: | [ ] | `D03 T01 §1` | Decode streams straight from the input file | 3 |
```

> [!IMPORTANT]
> **The boxes are derived. Never tick one by hand.**
>
> They are a projection of each TODO's Implementation Order table, and those flip in exactly one place: `review-todo-section`, after a `Verified:` stamp exists.
>
> ```bash
> python scripts/todo-graph.py plan --sync     # rewrite the boxes, then re-align every table
> python scripts/todo-graph.py plan --check    # fail if they have gone stale
> ```

**Nothing is capped, and nothing is left behind.** A section a campaign discovers on its own carries an Origin line and passes the admission test in `README.md`; anything else goes to [`backlog.md`](./backlog.md), where it is kept.

**How phases are authored.** A phase is a `### Phase <N> -- <Title>` heading, one paragraph on why it runs where it does, and one table. Every section in the tree sits in exactly one phase row; a new TODO file places each of its sections here in the commit that authors it. A section's phase is never earlier than the phase of anything it depends on.

---

## The acceptance bar

The [product vision](../docs/vision.md) also requires the native converter and complete documentation. The capability matrix, platform evidence and source-drift gates are release requirements; codec research retains its measured stop conditions and negative-result path.

RasterFold is finished milestone by milestone: **the 60 milestones of the roadmap (`docs/research.md` section 9), each keeping everything the one before it reached**, with every file it ever wrote still decoding, every malformed file refused cleanly, every document true to the code and the data, and every claim reproducible from a command in this repository. The first ten beat JPEG XL e9 and lossless WebP and release 1.0; from milestone 11 RasterFold beats its own previous release.

<!-- no-align -->

| Aim | Owned by |
| --- | --- |
| Native C converter with no additional user installs on Windows/Linux x64 and Android ARM64/x64 | `D10 T01 §2` · `D10 T04 §3` |
| Every required format has truthful lossless capabilities and independent round trips | `D10 T01 §1` · `D10 T01 §4` · `D10 T04 §1` |
| Single-image preview, synchronized comparisons, modern accessible SVG interface and real RasterFold demo | `D10 T03 §5` · `D10 T03 §6` · `D10 T03 §7` · `D10 T03 §8` |
| Complete, private-content-safe GitHub Pages documentation matches delivered artifacts | `D08 T01 §4` · `D08 T01 §8` · `D10 T04 §4` |
| Source facts, documents, supported text, commit references and built-site provenance detect drift and repair only deterministic output | `D08 T02 §6` · `D08 T02 §7` |
| Every clone refuses a broken plan, and CI reads back green | `D00 T01 §1` (hook) · `D00 T01 §2` (CI read back) |
| The C sources stay free of defects static analysis can find | `D00 T01 §3` |
| The review panel works before the first stamp | `D00 T01 §4` |
| Measured claims stay true | `D00 T01 §5` · `D08 T02 §1` · `D08 T02 §2` |
| Every measurement reports JPEG XL e7, JPEG XL e9, and WebP | `D00 T02 §1` · `D00 T02 §2` · `D00 T02 §3` |
| Documentation is HTML with comparison charts, and never drifts or goes stale | `D08 T01 §1` · `D08 T01 §2` · `D08 T01 §3` · `D08 T02 §1` · `D08 T02 §2` · `D08 T02 §3` · `D08 T02 §4` · `D08 T02 §5` · `D08 T03 §1` · `D08 T03 §2` |
| Operator-only work never stalls a runner | `D99 T01 §1` · `D99 T01 §2` · `D99 T01 §3` · `D99 T01 §4` · `D99 T01 §5` |
| Every file ever written still decodes | `D01 T01 §2` · `D01 T03 §1` · `D09 T01 §4` |
| Malformed files are refused quickly and never crash the decoder | `D04 T01 §1` · `D04 T01 §2` |
| A library, a complete command line, and a reproducible release | `D06 T01 §1` · `D07 T01 §1` · `D09 T01 §2` |
| Milestone 1, Locked baseline: `pwsh scripts/check-all.ps1` green with every gate `PASS`; the `plan-gates` and `build` workflows green on the last push; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python scripts/drift-check.py` exits 0. Baseline on 2026-09-29: photos 0.971 of JPEG XL e7 and 0.983 of e9, 28 of 39 photos below e9 | `D05 T01 §1` |
| Milestone 2, Same bytes, leaner and harder: every file byte-identical to milestone 1 (`KAT_FOLD` unchanged, the oracle agrees); decode `alloc_peak` at most 9.2 MB on `measure/image.bmp` (10.57 MB at milestone 1); one-thread decode at least 5% faster; a 4096x4096 header with a 20-byte payload refused in under a second | `D05 T01 §2` |
| Milestone 3, Few-colour images solved: method 2 shipped; `syn_checker` below 210 bytes and `syn_pixel` below 53 bytes (JPEG XL e9's sizes); photo sizes unchanged; 20,000 fuzz mutations over every method clean | `D05 T01 §3` |
| Milestone 4, Format version 3: beat JPEG XL e9 on photographs: format version 3 with the accepted model changes (`D01 T02 §1`, `D01 T02 §7`, `D01 T03 §1`; operator decision 2026-10-29 under `D99 T01 §4` after `D01 T02 §2` to `§6` missed): photo total at most 0.982 of JPEG XL e9 (and at most 0.970 of JPEG XL e7), test pack at most 0.986 of JPEG XL e9 (and at most 0.973 of e7), at least 31 of 39 photos below e9 (and 39 of 39 at or below e7), three-thread decode at most 0.6 of JPEG XL's, 7-model decode `alloc_peak` at most 10.0 MB (+786 KB for the `k5`/`k6` counter tables over Milestone 3's 5-model 9.16 MB), and 7-model encode/decode time at most 1.27x Milestone 3's 5-model baseline | `D05 T01 §4` |
| Milestone 5, Exact repeats: a match model in version 3: `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` below JPEG XL e9 (616, 271, 2195, and 2038 bytes); photos no worse than milestone 4 | `D05 T01 §5` |
| Milestone 6, Faster than WebP to decode: horizontal strips in version 3: photo decode wall time below lossless WebP's on the 12-thread development machine; strip size cost at most 0.3% | `D05 T01 §6` |
| Milestone 7, Every photograph beats JPEG XL e9: 39 of 39 photos and 10 of 10 test-pack photos below JPEG XL e9; encode at most 0.3 of JPEG XL e7's time | `D05 T01 §7` |
| Milestone 8, Beat WebP on generated images: the generated set's total and geometric mean below lossless WebP's, and every generated image within 1.1 times WebP's size | `D05 T01 §8` |
| Milestone 9, Holds on images never tuned on: milestones 4, 7, and 8 hold on the held-out set (`python research/ab.py size heldout`) without any retuning | `D05 T01 §9` |
| Milestone 10, RasterFold 1.0: `rasterfold-v1.0.0` released: format version 3 frozen; Windows/Linux x64 and Android ARM64/x64 CLI, shared/static C libraries and the complete native converter; BMP, PNG, and PPM in the CLI; the full HTML docs published; 100,000 fuzz mutations clean; smaller than JPEG XL e9 and lossless WebP on every set, and faster and leaner than JPEG XL on encode, decode, and memory | `D05 T01 §10` |
| Milestone 11, SIMD decisions: one-thread decode at least 10% faster than 1.0, bytes within 0.1% | `D05 T02 §2` |
| Milestone 12, Two-layer mixing: photo total at least 0.4% smaller than milestone 11 | `D05 T02 §4` |
| Milestone 13, Stronger prediction: photo total at least 0.5% smaller than milestone 12 | `D05 T02 §6` |
| Milestone 14, Adaptive colour transform: photo total at least 0.3% smaller than milestone 13, decode cost unchanged | `D05 T02 §8` |
| Milestone 15, Effort levels: a fast level decoding at least twice as fast as the default and still smaller than JPEG XL e7; the default unchanged | `D05 T02 §10` |
| Milestone 16, Max-effort encoder search: the max level at least 1% smaller than the default at the same decode cost | `D05 T02 §12` |
| Milestone 17, Trained starting state: images of Kodak size and below at least 0.5% smaller, confirmed on the held-out set | `D05 T02 §14` |
| Milestone 18, Streaming: peak memory on a 24-megapixel image at most one third of milestone 17's | `D05 T02 §16` |
| Milestone 19, Many-core scaling: decode throughput on 16 or more threads at least twice milestone 18's, size within 0.2% | `D05 T02 §18` |
| Milestone 20, RasterFold 2.0: `rasterfold-v2.0.0`: against 1.0, the default at least 1.5% smaller, the max level at least 2.5% smaller, one-thread decode at least 1.2 times faster, memory at most one third | `D05 T02 §20` |
| Milestone 21, Run mode for flat regions: generated images decode at least twice as fast and photos at least 5% faster, sizes no worse | `D05 T03 §2` |
| Milestone 22, Verified hashed context tables: decode at least 10% faster at equal memory, size within 0.1% | `D05 T03 §4` |
| Milestone 23, Higher-precision coder: generated-image geometric mean at least 10% smaller, photos unchanged | `D05 T03 §6` |
| Milestone 24, Learned predictor at the max level: the max level at least 1% smaller on photos, fast and default byte-identical, confirmed on the held-out set | `D05 T03 §8` |
| Milestone 25, Texture-gated models: the default level at least 0.4% smaller at equal decode time | `D05 T03 §10` |
| Milestone 26, Random-access decode: decoding a 1/16 region at least 8 times faster than a full decode, size within 0.1% | `D05 T03 §12` |
| Milestone 27, Portable and byte-exact: Linux, macOS, and Windows on ARM64 builds with identical bytes on every platform | `D05 T03 §14` |
| Milestone 28, Alpha, 16-bit, and metadata: alpha, 16-bit samples, and ICC and EXIF preserved, 8-bit RGB files byte-identical to milestone 27, the 16-bit set below JPEG XL e9 | `D05 T03 §16` |
| Milestone 29, Coverage-proven decoder: libFuzzer branch coverage of the decoder at least 95%, no crash in 10^9 executions, hostile-input decode time at most twice honest time | `D05 T03 §18` |
| Milestone 30, RasterFold 3.0: `rasterfold-v3.0.0`: against 2.0, generated images at least 10% smaller, photos at the max level at least 1% smaller, decode at least 1.3 times faster, with random access, portability, alpha, and 16-bit | `D05 T03 §20` |
| Milestone 31, Language bindings and a streaming API: Python, Rust, and C# bindings over the 1.x C API and a streaming callback API, each with byte-exact round trips and API overhead at most 1% | `D05 T04 §2` |
| Milestone 32, More formats: TIFF, 16-bit PNG, and PAM in and out with exact pixels | `D05 T04 §4` |
| Milestone 33, Lossless JPEG recompression: JPEG files stored at least 2% smaller than JPEG XL's JPEG recompression, the original JPEG rebuilt bit for bit | `D05 T04 §6` |
| Milestone 34, Image sequences: bursts and screen recordings smaller than lossless animated JPEG XL and animated WebP | `D05 T04 §8` |
| Milestone 35, Gigapixel images: the size limit raised from 32 megapixels to 4 gigapixels with peak memory proportional to width | `D05 T04 §10` |
| Milestone 36, Browser decoding: decoding in a browser at most twice native time, from a WebAssembly module under 200 KB | `D05 T04 §12` |
| Milestone 37, Host plugins: open and save with exact pixels in Pillow, ffmpeg, libvips, and GIMP | `D05 T04 §14` |
| Milestone 38, Second implementation: an independent Rust decoder, written from the spec alone, byte-identical on the conformance suite | `D05 T04 §16` |
| Milestone 39, Automatic effort: at least 90% of the max level's gain at no more than 1.5 times the default encode time | `D05 T04 §18` |
| Milestone 40, RasterFold 4.0: `rasterfold-v4.0.0`: bindings, more formats, JPEG recompression, sequences, gigapixel images, browser decoding, and plugins, with no regression on 3.0 files | `D05 T04 §20` |
| Milestone 41, GPU encode search: max-level encode at least 10 times faster on a GPU, bytes identical to the CPU search | `D05 T05 §2` |
| Milestone 42, Energy per megapixel: decode energy per megapixel at least 20% lower than milestone 41's | `D05 T05 §4` |
| Milestone 43, Phone-class ARM: decode per GHz on ARM64 within 1.2 times x86-64, and 4K frames at the fast level in real time | `D05 T05 §6` |
| Milestone 44, Proven decoder bounds: bounded model checking proves no out-of-bounds access or overflow in the decoder for every input up to a stated size | `D05 T05 §8` |
| Milestone 45, Reproducible, signed releases: the same source builds a byte-identical executable on any machine; releases are signed with a software bill of materials | `D05 T05 §10` |
| Milestone 46, Collection archives: a library of similar photos at least 3% smaller than encoding each file alone | `D05 T05 §12` |
| Milestone 47, Tiny decoder profile: a profile decodable in at most 256 KB of working memory by a decoder of at most 20 KB of code | `D05 T05 §14` |
| Milestone 48, Recognised format: detected by libmagic and the major image libraries without our plugins; media type registered | `D05 T05 §16` |
| Milestone 49, Third-party benchmarks: public benchmark results by others match our measurements within their noise | `D05 T05 §18` |
| Milestone 50, RasterFold 5.0: `rasterfold-v5.0.0`: lower energy, a verified decoder, reproducible signed builds, ARM parity, archives, and the tiny profile | `D05 T05 §20` |
| Milestone 51, Learned model at the default level: the default photo size at least 2% smaller at no more than 1.2 times decode time | `D05 T06 §2` |
| Milestone 52, Transmitted priors: images under 1 megapixel at least 1% smaller | `D05 T06 §4` |
| Milestone 53, Camera raw: raw sensor data smaller than DNG's lossless JPEG and than JPEG XL on the same raw files | `D05 T06 §6` |
| Milestone 54, Medical and scientific imagery: 16-bit grayscale and multi-channel images smaller than JPEG-LS and JPEG XL on public medical and satellite sets | `D05 T06 §8` |
| Milestone 55, Error resilience: a file with a damaged byte still yields every undamaged tile | `D05 T06 §10` |
| Milestone 56, Progressive decoding: a usable preview from the first 5% of the bytes, lossless at the end, for at most 1% size cost | `D05 T06 §12` |
| Milestone 57, Lossless edit deltas: an edited copy of an image stored in at most 10% of a full file | `D05 T06 §14` |
| Milestone 58, Hardware decoder: an FPGA decoder of the fast or tiny profile at 100 megapixels per second or more, byte-exact with the software | `D05 T06 §16` |
| Milestone 59, Twenty-year compatibility: every file written by every release since version 2 decodes in the newest release, proven by a permanent compatibility corpus | `D05 T06 §18` |
| Milestone 60, RasterFold 6.0: `rasterfold-v6.0.0`: the learned default model, raw and scientific data, resilience, progressive decoding, edit deltas, and the hardware reference | `D05 T06 §20` |

---

### Phase 0 -- Milestone 1: Locked baseline

The ported plan, hooks, gates, measurement references, HTML docs, and drift gates come first: every later milestone is measured, reviewed, and documented through them. The operator rows sit here because this phase waits on them (the GitHub CLI login for the CI read-back, Pages for the docs); the two recorded decisions are stamped here so the format-version and missed-milestone rules hold from the start.

|  ✔  | Section      | Deliverable                                                   | Items |
| :-: | ------------ | ------------------------------------------------------------- | :---: |
| [x] | `D00 T01 §7` | The campaign guard on a legacy console encoding               |   3   |
| [x] | `D00 T01 §8` | A read-only independent reviewer beside a live run            |   4   |
| [x] | `D00 T01 §9` | Windows Job Object process-tree cleanup in review-dispatch.py |   3   |
| [x] | `D99 T01 §1` | Remove the retired run-guard folders                          |   3   |
| [x] | `D99 T01 §2` | Log in the GitHub CLI                                         |   2   |
| [x] | `D99 T01 §3` | Approve format version 3                                      |   2   |
| [x] | `D99 T01 §4` | The missed-milestone policy                                   |   2   |
| [x] | `D99 T01 §5` | Enable GitHub Pages                                           |   3   |
| [x] | `D00 T01 §1` | Commit the port and wire the commit hook                      |   6   |
| [x] | `D00 T01 §3` | The static analysis gate                                      |   4   |
| [x] | `D00 T01 §6` | Claude review rounds through the reviewer pin                 |   6   |
| [x] | `D00 T04 §1` | Local toolchains and WSL gates                                |   6   |
| [x] | `D08 T04 §1` | Repository presentation and developer entry points            |   4   |
| [x] | `D00 T04 §2` | Portable platform and SIMD boundary                           |   4   |
| [x] | `D00 T04 §3` | Isolated Codex lifecycle and review runner                    |   5   |
| [x] | `D00 T04 §4` | Family-local intelligence                                     |   4   |
| [x] | `D00 T01 §4` | Prove the review panel end to end                             |   4   |
| [x] | `D00 T01 §5` | Raise the claims coverage floor                               |   3   |
| [x] | `D00 T01 §2` | First push: CI green and read back                            |   3   |
| [x] | `D00 T02 §1` | e9 and WebP references in the research tools                  |   3   |
| [x] | `D00 T02 §2` | JPEG XL e9 in the benchmark                                   |   2   |
| [x] | `D00 T02 §3` | The committed corpus results file                             |   3   |
| [x] | `D08 T02 §1` | One facts file and claims everywhere                          |   5   |
| [x] | `D08 T01 §1` | The Jekyll site and its generated pages                       |   7   |
| [x] | `D08 T02 §3` | Generated files match their sources                           |   3   |
| [x] | `D08 T01 §2` | Comparison charts                                             |   5   |
| [x] | `D08 T02 §2` | Figures bound to their data                                   |   3   |
| [x] | `D08 T02 §4` | Staleness: verified dates and data revisions                  |   4   |
| [x] | `D08 T02 §5` | Links, and the gates wired everywhere                         |   3   |
| [x] | `D08 T02 §6` | Deterministic repair and complete file coverage               |   5   |
| [x] | `D08 T02 §7` | Commit and built-site provenance                              |   5   |
| [x] | `D08 T01 §4` | Complete current documentation map                            |   5   |
| [x] | `D08 T01 §5` | Accessible navigation and site states                         |   5   |
| [x] | `D08 T01 §3` | Publish with GitHub Pages                                     |   2   |
| [x] | `D05 T01 §1` | Milestone 1 gate: Locked baseline                             |   4   |

### Phase 1 -- Milestone 2: Same bytes, leaner and harder

Changes that keep every byte, so they need no format decision and every later timing runs on the leaner, harder, faster build.

|  ✔  | Section      | Deliverable                                                     | Items |
| :-: | ------------ | --------------------------------------------------------------- | :---: |
| [x] | `D03 T01 §1` | Decode streams straight from the input file                     |   3   |
| [x] | `D03 T01 §2` | Decode straight into the BMP                                    |   3   |
| [x] | `D03 T01 §3` | Encode straight into the output buffer                          |   3   |
| [x] | `D04 T01 §1` | Stop decoding a stream that has run out                         |   4   |
| [x] | `D04 T01 §3` | Oracle and spec agree on padding and counters                   |   5   |
| [x] | `D02 T01 §1` | Profile-guided optimization                                     |   5   |
| [x] | `D02 T01 §2` | Link-time optimization                                          |   3   |
| [x] | `D02 T01 §4` | Hot-loop vectorization and row-context specialization in `cm.c` |   4   |
| [x] | `D05 T01 §2` | Milestone 2 gate: Same bytes, leaner and harder                 |   4   |

### Phase 2 -- Milestone 3: Few-colour images solved

The palette method is a new method number, so it needs no version decision and removes the worst synthetic loss early; the long fuzz campaign follows once method 2 exists for it to reach.

|  ✔  | Section      | Deliverable                                                      | Items |
| :-: | ------------ | ---------------------------------------------------------------- | :---: |
| [x] | `D01 T01 §1` | One range coder shared between methods                           |   3   |
| [x] | `D01 T01 §2` | Method 2: palette encoder, decoder, oracle, spec                 |   7   |
| [x] | `D01 T01 §3` | Measure method 2 against JPEG XL and WebP                        |   4   |
| [x] | `D01 T01 §4` | Two-step neighbour pattern and two-colour deduction for method 2 |   4   |
| [x] | `D04 T01 §2` | A long fuzz campaign over every method                           |   3   |
| [x] | `D05 T01 §3` | Milestone 3 gate: Few-colour images solved                       |   5   |

### Phase 3 -- Milestone 4: Format version 3: beat JPEG XL e9 on photographs

The model experiments, measured together and ported as format version 3.

|  ✔  | Section       | Deliverable                                                        | Items |
| :-: | ------------- | ------------------------------------------------------------------ | :---: |
| [x] | `D01 T02 §1`  | A sixth and seventh context model                                  |   5   |
| [x] | `D01 T02 §2`  | Two adaptation speeds per context                                  |   4   |
| [x] | `D01 T02 §3`  | A second mixer weight set                                          |   4   |
| [x] | `D01 T02 §4`  | Learning cost on small images                                      |   4   |
| [x] | `D01 T02 §5`  | An exact-repeat context                                            |   4   |
| [x] | `D01 T02 §6`  | The ceiling of encoder-chosen parameters                           |   4   |
| [x] | `D01 T02 §7`  | The accepted variants measured together                            |   4   |
| [x] | `D00 T01 §10` | OpenCode campaign Stop hook and heartbeat                          |   5   |
| [x] | `D01 T03 §1`  | Version 3: the accepted model changes                              |   8   |
| [x] | `D05 T01 §4`  | Milestone 4 gate: Format version 3: beat JPEG XL e9 on photographs |   4   |

### Phase 4 -- Milestone 5: Exact repeats

Exact repeats extend version 3 before release.

|  ✔  | Section       | Deliverable                                             | Items |
| :-: | ------------- | ------------------------------------------------------- | :---: |
| [x] | `D01 T04 §1`  | Where the repeats are                                   |   3   |
| [x] | `D01 T04 §2`  | A match model, measured                                 |   4   |
| [x] | `D01 T04 §3`  | The match model in format version 3                     |   3   |
| [x] | `D01 T04 §4`  | Exact modular planes, measured                          |   6   |
| [x] | `D00 T01 §11` | Restarting a build after a late plan review             |   4   |
| [x] | `D01 T04 §5`  | Exact modular planes in format version 3                |   3   |
| [x] | `D01 T04 §6`  | Gated modular prediction, measured                      |   6   |
| [x] | `D01 T04 §7`  | Gated modular prediction in format version 3            |   3   |
| [x] | `D01 T04 §8`  | Learned gated modular prediction, measured              |   6   |
| [x] | `D01 T04 §9`  | Learned gated modular prediction in format version 3    |   3   |
| [x] | `D01 T04 §10` | Learned gated modular prediction v2 in format version 3 |   6   |
| [x] | `D05 T01 §5`  | Milestone 5 gate: Exact repeats                         |   7   |

### Phase 5 -- Milestone 6: Faster than WebP to decode

Strips extend version 3 and make decode scale past three threads.

|  ✔  | Section      | Deliverable                                   | Items |
| :-: | ------------ | --------------------------------------------- | :---: |
| [x] | `D02 T02 §1` | The size cost of horizontal strips            |   4   |
| [x] | `D02 T03 §1` | Initialize only the model state an image uses |   6   |
| [x] | `D01 T03 §2` | Version 3: horizontal strips                  |   5   |
| [x] | `D02 T02 §3` | Warm-started horizontal strips, measured      |   5   |
| [x] | `D01 T03 §3` | Version 3: warm-started horizontal strips     |   4   |
| [x] | `D05 T01 §6` | Milestone 6 gate: Faster than WebP to decode  |   5   |

### Phase 6 -- Milestone 7: Photographs beat JPEG XL e9

Most of the photographs still losing to JPEG XL e9, within the gate's tolerance.

|  ✔  | Section      | Deliverable                                                                      | Items |
| :-: | ------------ | -------------------------------------------------------------------------------- | :---: |
| [x] | `D01 T05 §1` | Why the losing photos lose                                                       |   4   |
| [x] | `D01 T05 §2` | Encoder-chosen parameters in version 3                                           |   5   |
| [x] | `D01 T05 §3` | Strip-adaptation priors, collision-reduced contexts, and noise-damped prediction |   3   |
| [x] | `D01 T05 §4` | Port Milestone 7 CM improvements into version 3                                  |   3   |
| [x] | `D05 T01 §7` | Milestone 7 gate: Photographs beat JPEG XL e9                                    |   4   |

### Phase 7 -- Milestone 8: Beat WebP on generated images

Generated and screen content against WebP.

|  ✔  | Section      | Deliverable                                     | Items |
| :-: | ------------ | ----------------------------------------------- | :---: |
| [x] | `D01 T06 §1` | Where WebP still wins                           |   4   |
| [x] | `D01 T06 §2` | A colour-cache model in version 3               |   3   |
| [x] | `D01 T06 §3` | Copy runs for palette images                    |   3   |
| [ ] | `D01 T06 §4` | Coding the palette itself for palette images    |   3   |
| [ ] | `D01 T06 §5` | Above-copy runs for palette images              |   3   |
| [ ] | `D01 T06 §6` | A neighbour-candidate colour cache in version 3 |   3   |
| [ ] | `D05 T01 §8` | Milestone 8 gate: Beat WebP on generated images |   5   |

### Phase 8 -- Milestone 9: Holds on images never tuned on

Everything so far confirmed on images nobody tuned on, before anything is frozen.

|  ✔  | Section      | Deliverable                                      | Items |
| :-: | ------------ | ------------------------------------------------ | :---: |
| [ ] | `D00 T03 §1` | The held-out set                                 |   4   |
| [ ] | `D00 T03 §2` | Validate the milestones on it                    |   3   |
| [ ] | `D05 T01 §9` | Milestone 9 gate: Holds on images never tuned on |   4   |

### Phase 9 -- Milestone 10: RasterFold 1.0

The 1.0 product: library, command line, references, and the release that freezes version 3.

|  ✔  | Section       | Deliverable                                      | Items |
| :-: | ------------- | ------------------------------------------------ | :---: |
| [ ] | `D99 T01 §6`  | Android release signing identity                 |   3   |
| [ ] | `D99 T01 §7`  | HEVC distribution decision                       |   3   |
| [x] | `D06 T01 §1`  | The public header                                |   3   |
| [x] | `D06 T01 §2`  | The DLL and its import libraries                 |   3   |
| [x] | `D06 T01 §3`  | The command line on the library                  |   2   |
| [x] | `D06 T01 §4`  | API tests and the ABI list                       |   4   |
| [x] | `D06 T01 §5`  | Example programs                                 |   2   |
| [ ] | `D07 T01 §1`  | Commands and options                             |   3   |
| [ ] | `D07 T01 §2`  | PNG in and out                                   |   3   |
| [ ] | `D07 T01 §3`  | PPM in and out                                   |   2   |
| [ ] | `D07 T01 §4`  | Exit codes, messages, and overwrite              |   3   |
| [ ] | `D08 T03 §1`  | The command-line reference                       |   3   |
| [ ] | `D08 T03 §2`  | The API reference                                |   3   |
| [x] | `D09 T01 §1`  | One version source and the changelog             |   5   |
| [ ] | `D10 T01 §1`  | Pixel contract and capability registry           |   8   |
| [ ] | `D10 T01 §2`  | Pinned bundled codec builds                      |   9   |
| [ ] | `D10 T01 §3`  | Bounded asynchronous jobs                        |   6   |
| [ ] | `D10 T01 §4`  | Exactness verification and differential fixtures |   7   |
| [ ] | `D10 T01 §5`  | Safe import and export                           |   6   |
| [ ] | `D10 T01 §6`  | Comparable measurements and reports              |   7   |
| [ ] | `D10 T02 §1`  | RasterFold, BMP, PNG and PNM                     |   6   |
| [ ] | `D10 T02 §2`  | WebP                                             |   6   |
| [ ] | `D10 T02 §3`  | JPEG XL                                          |   6   |
| [ ] | `D10 T02 §4`  | AVIF                                             |   6   |
| [ ] | `D10 T02 §5`  | JPEG optimization and import                     |   6   |
| [ ] | `D10 T02 §6`  | TIFF and GIF                                     |   6   |
| [ ] | `D10 T02 §7`  | HEIF and HEIC                                    |   6   |
| [ ] | `D10 T02 §8`  | QOI                                              |   6   |
| [ ] | `D10 T02 §9`  | JPEG 2000                                        |   6   |
| [ ] | `D10 T02 §10` | OpenEXR                                          |   6   |
| [ ] | `D10 T02 §11` | Camera RAW and DNG import                        |   6   |
| [ ] | `D10 T02 §12` | Animation and multi-page preservation            |   5   |
| [ ] | `D10 T03 §1`  | C interface components and SVG icons             |   8   |
| [ ] | `D10 T03 §2`  | Windows native integration                       |   6   |
| [ ] | `D10 T03 §3`  | Linux native integration                         |   6   |
| [ ] | `D10 T03 §4`  | Android native integration                       |   8   |
| [ ] | `D10 T03 §5`  | Single-image conversion and preview              |   7   |
| [ ] | `D10 T03 §6`  | Synchronized comparisons                         |   6   |
| [ ] | `D10 T03 §7`  | Preferences and accessible error recovery        |   6   |
| [ ] | `D10 T03 §8`  | Guided RasterFold demonstration                  |   5   |
| [ ] | `D10 T04 §1`  | Full capability and device matrix                |   5   |
| [ ] | `D10 T04 §2`  | Hostile input and resource limits                |   5   |
| [ ] | `D10 T04 §3`  | Self-contained native packages                   |   6   |
| [ ] | `D09 T01 §2`  | The release package                              |   2   |
| [ ] | `D09 T01 §3`  | The release workflow                             |   2   |
| [ ] | `D08 T01 §6`  | Converter user guide and capability reference    |   6   |
| [ ] | `D08 T01 §7`  | Versioned downloads and complete references      |   5   |
| [ ] | `D08 T01 §8`  | Published whole-product documentation acceptance |   4   |
| [ ] | `D10 T04 §4`  | Whole-product release acceptance                 |   5   |
| [ ] | `D09 T01 §4`  | Release rasterfold-v1.0.0                        |   6   |
| [ ] | `D05 T01 §10` | Milestone 10 gate: RasterFold 1.0                |   4   |

### Phase 10 -- Milestone 11: SIMD decisions

Milestone 11's work is authored one milestone ahead by its planning section, from milestone 10's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                | Items |
| :-: | ------------ | ------------------------------------------ | :---: |
| [ ] | `D05 T02 §1` | Plan milestone 11: SIMD decisions          |   5   |
| [ ] | `D02 T01 §3` | An AVX2 build, measured before dispatching |   3   |
| [ ] | `D02 T02 §2` | The speed of an int16 SIMD mixer           |   4   |
| [ ] | `D05 T02 §2` | Milestone 11 gate: SIMD decisions          |   4   |

### Phase 11 -- Milestone 12: Two-layer mixing

Milestone 12's work is authored one milestone ahead by its planning section, from milestone 11's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                         | Items |
| :-: | ------------ | ----------------------------------- | :---: |
| [ ] | `D05 T02 §3` | Plan milestone 12: Two-layer mixing |   4   |
| [ ] | `D05 T02 §4` | Milestone 12 gate: Two-layer mixing |   4   |

### Phase 12 -- Milestone 13: Stronger prediction

Milestone 13's work is authored one milestone ahead by its planning section, from milestone 12's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                            | Items |
| :-: | ------------ | -------------------------------------- | :---: |
| [ ] | `D05 T02 §5` | Plan milestone 13: Stronger prediction |   4   |
| [ ] | `D05 T02 §6` | Milestone 13 gate: Stronger prediction |   4   |

### Phase 13 -- Milestone 14: Adaptive colour transform

Milestone 14's work is authored one milestone ahead by its planning section, from milestone 13's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                  | Items |
| :-: | ------------ | -------------------------------------------- | :---: |
| [ ] | `D05 T02 §7` | Plan milestone 14: Adaptive colour transform |   4   |
| [ ] | `D05 T02 §8` | Milestone 14 gate: Adaptive colour transform |   4   |

### Phase 14 -- Milestone 15: Effort levels

Milestone 15's work is authored one milestone ahead by its planning section, from milestone 14's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                      | Items |
| :-: | ------------- | -------------------------------- | :---: |
| [ ] | `D05 T02 §9`  | Plan milestone 15: Effort levels |   4   |
| [ ] | `D05 T02 §10` | Milestone 15 gate: Effort levels |   4   |

### Phase 15 -- Milestone 16: Max-effort encoder search

Milestone 16's work is authored one milestone ahead by its planning section, from milestone 15's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                                  | Items |
| :-: | ------------- | -------------------------------------------- | :---: |
| [ ] | `D05 T02 §11` | Plan milestone 16: Max-effort encoder search |   4   |
| [ ] | `D05 T02 §12` | Milestone 16 gate: Max-effort encoder search |   4   |

### Phase 16 -- Milestone 17: Trained starting state

Milestone 17's work is authored one milestone ahead by its planning section, from milestone 16's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                               | Items |
| :-: | ------------- | ----------------------------------------- | :---: |
| [ ] | `D05 T02 §13` | Plan milestone 17: Trained starting state |   4   |
| [ ] | `D05 T02 §14` | Milestone 17 gate: Trained starting state |   4   |

### Phase 17 -- Milestone 18: Streaming

Milestone 18's work is authored one milestone ahead by its planning section, from milestone 17's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                  | Items |
| :-: | ------------- | ---------------------------- | :---: |
| [ ] | `D05 T02 §15` | Plan milestone 18: Streaming |   5   |
| [ ] | `D05 T02 §16` | Milestone 18 gate: Streaming |   4   |

### Phase 18 -- Milestone 19: Many-core scaling

Milestone 19's work is authored one milestone ahead by its planning section, from milestone 18's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                          | Items |
| :-: | ------------- | ------------------------------------ | :---: |
| [ ] | `D05 T02 §17` | Plan milestone 19: Many-core scaling |   4   |
| [ ] | `D05 T02 §18` | Milestone 19 gate: Many-core scaling |   4   |

### Phase 19 -- Milestone 20: RasterFold 2.0

Milestone 20's work is authored one milestone ahead by its planning section, from milestone 19's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                       | Items |
| :-: | ------------- | --------------------------------- | :---: |
| [ ] | `D05 T02 §19` | Plan milestone 20: RasterFold 2.0 |   4   |
| [ ] | `D05 T02 §20` | Milestone 20 gate: RasterFold 2.0 |   6   |

### Phase 20 -- Milestone 21: Run mode for flat regions

Milestone 21's work is authored one milestone ahead by its planning section, from milestone 20's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                  | Items |
| :-: | ------------ | -------------------------------------------- | :---: |
| [ ] | `D05 T03 §1` | Plan milestone 21: Run mode for flat regions |   4   |
| [ ] | `D05 T03 §2` | Milestone 21 gate: Run mode for flat regions |   4   |

### Phase 21 -- Milestone 22: Verified hashed context tables

Milestone 22's work is authored one milestone ahead by its planning section, from milestone 21's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                       | Items |
| :-: | ------------ | ------------------------------------------------- | :---: |
| [ ] | `D05 T03 §3` | Plan milestone 22: Verified hashed context tables |   4   |
| [ ] | `D05 T03 §4` | Milestone 22 gate: Verified hashed context tables |   4   |

### Phase 22 -- Milestone 23: Higher-precision coder

Milestone 23's work is authored one milestone ahead by its planning section, from milestone 22's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                               | Items |
| :-: | ------------ | ----------------------------------------- | :---: |
| [ ] | `D05 T03 §5` | Plan milestone 23: Higher-precision coder |   4   |
| [ ] | `D05 T03 §6` | Milestone 23 gate: Higher-precision coder |   4   |

### Phase 23 -- Milestone 24: Learned predictor at the max level

Milestone 24's work is authored one milestone ahead by its planning section, from milestone 23's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                           | Items |
| :-: | ------------ | ----------------------------------------------------- | :---: |
| [ ] | `D05 T03 §7` | Plan milestone 24: Learned predictor at the max level |   4   |
| [ ] | `D05 T03 §8` | Milestone 24 gate: Learned predictor at the max level |   4   |

### Phase 24 -- Milestone 25: Texture-gated models

Milestone 25's work is authored one milestone ahead by its planning section, from milestone 24's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                             | Items |
| :-: | ------------- | --------------------------------------- | :---: |
| [ ] | `D05 T03 §9`  | Plan milestone 25: Texture-gated models |   4   |
| [ ] | `D05 T03 §10` | Milestone 25 gate: Texture-gated models |   4   |

### Phase 25 -- Milestone 26: Random-access decode

Milestone 26's work is authored one milestone ahead by its planning section, from milestone 25's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                             | Items |
| :-: | ------------- | --------------------------------------- | :---: |
| [ ] | `D05 T03 §11` | Plan milestone 26: Random-access decode |   4   |
| [ ] | `D05 T03 §12` | Milestone 26 gate: Random-access decode |   4   |

### Phase 26 -- Milestone 27: Portable and byte-exact

Milestone 27's work is authored one milestone ahead by its planning section, from milestone 26's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                                | Items |
| :-: | ------------- | ------------------------------------------ | :---: |
| [ ] | `D05 T03 §13` | Plan milestone 27: Portable and byte-exact |   4   |
| [ ] | `D05 T03 §14` | Milestone 27 gate: Portable and byte-exact |   4   |

### Phase 27 -- Milestone 28: Alpha, 16-bit, and metadata

Milestone 28's work is authored one milestone ahead by its planning section, from milestone 27's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                                    | Items |
| :-: | ------------- | ---------------------------------------------- | :---: |
| [ ] | `D05 T03 §15` | Plan milestone 28: Alpha, 16-bit, and metadata |   5   |
| [ ] | `D05 T03 §16` | Milestone 28 gate: Alpha, 16-bit, and metadata |   4   |

### Phase 28 -- Milestone 29: Coverage-proven decoder

Milestone 29's work is authored one milestone ahead by its planning section, from milestone 28's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                                | Items |
| :-: | ------------- | ------------------------------------------ | :---: |
| [ ] | `D05 T03 §17` | Plan milestone 29: Coverage-proven decoder |   4   |
| [ ] | `D05 T03 §18` | Milestone 29 gate: Coverage-proven decoder |   4   |

### Phase 29 -- Milestone 30: RasterFold 3.0

Milestone 30's work is authored one milestone ahead by its planning section, from milestone 29's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                       | Items |
| :-: | ------------- | --------------------------------- | :---: |
| [ ] | `D05 T03 §19` | Plan milestone 30: RasterFold 3.0 |   4   |
| [ ] | `D05 T03 §20` | Milestone 30 gate: RasterFold 3.0 |   6   |

### Phase 30 -- Milestone 31: Language bindings and a streaming API

Milestone 31's work is authored one milestone ahead by its planning section, from milestone 30's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                              | Items |
| :-: | ------------ | -------------------------------------------------------- | :---: |
| [ ] | `D05 T04 §1` | Plan milestone 31: Language bindings and a streaming API |   4   |
| [ ] | `D05 T04 §2` | Milestone 31 gate: Language bindings and a streaming API |   4   |

### Phase 31 -- Milestone 32: More formats

Milestone 32's work is authored one milestone ahead by its planning section, from milestone 31's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                     | Items |
| :-: | ------------ | ------------------------------- | :---: |
| [ ] | `D05 T04 §3` | Plan milestone 32: More formats |   4   |
| [ ] | `D05 T04 §4` | Milestone 32 gate: More formats |   4   |

### Phase 32 -- Milestone 33: Lossless JPEG recompression

Milestone 33's work is authored one milestone ahead by its planning section, from milestone 32's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                    | Items |
| :-: | ------------ | ---------------------------------------------- | :---: |
| [ ] | `D05 T04 §5` | Plan milestone 33: Lossless JPEG recompression |   4   |
| [ ] | `D05 T04 §6` | Milestone 33 gate: Lossless JPEG recompression |   4   |

### Phase 33 -- Milestone 34: Image sequences

Milestone 34's work is authored one milestone ahead by its planning section, from milestone 33's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                        | Items |
| :-: | ------------ | ---------------------------------- | :---: |
| [ ] | `D05 T04 §7` | Plan milestone 34: Image sequences |   4   |
| [ ] | `D05 T04 §8` | Milestone 34 gate: Image sequences |   4   |

### Phase 34 -- Milestone 35: Gigapixel images

Milestone 35's work is authored one milestone ahead by its planning section, from milestone 34's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                         | Items |
| :-: | ------------- | ----------------------------------- | :---: |
| [ ] | `D05 T04 §9`  | Plan milestone 35: Gigapixel images |   4   |
| [ ] | `D05 T04 §10` | Milestone 35 gate: Gigapixel images |   4   |

### Phase 35 -- Milestone 36: Browser decoding

Milestone 36's work is authored one milestone ahead by its planning section, from milestone 35's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                         | Items |
| :-: | ------------- | ----------------------------------- | :---: |
| [ ] | `D05 T04 §11` | Plan milestone 36: Browser decoding |   4   |
| [ ] | `D05 T04 §12` | Milestone 36 gate: Browser decoding |   4   |

### Phase 36 -- Milestone 37: Host plugins

Milestone 37's work is authored one milestone ahead by its planning section, from milestone 36's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                     | Items |
| :-: | ------------- | ------------------------------- | :---: |
| [ ] | `D05 T04 §13` | Plan milestone 37: Host plugins |   4   |
| [ ] | `D05 T04 §14` | Milestone 37 gate: Host plugins |   4   |

### Phase 37 -- Milestone 38: Second implementation

Milestone 38's work is authored one milestone ahead by its planning section, from milestone 37's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                              | Items |
| :-: | ------------- | ---------------------------------------- | :---: |
| [ ] | `D05 T04 §15` | Plan milestone 38: Second implementation |   4   |
| [ ] | `D05 T04 §16` | Milestone 38 gate: Second implementation |   4   |

### Phase 38 -- Milestone 39: Automatic effort

Milestone 39's work is authored one milestone ahead by its planning section, from milestone 38's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                         | Items |
| :-: | ------------- | ----------------------------------- | :---: |
| [ ] | `D05 T04 §17` | Plan milestone 39: Automatic effort |   4   |
| [ ] | `D05 T04 §18` | Milestone 39 gate: Automatic effort |   4   |

### Phase 39 -- Milestone 40: RasterFold 4.0

Milestone 40's work is authored one milestone ahead by its planning section, from milestone 39's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                       | Items |
| :-: | ------------- | --------------------------------- | :---: |
| [ ] | `D05 T04 §19` | Plan milestone 40: RasterFold 4.0 |   4   |
| [ ] | `D05 T04 §20` | Milestone 40 gate: RasterFold 4.0 |   6   |

### Phase 40 -- Milestone 41: GPU encode search

Milestone 41's work is authored one milestone ahead by its planning section, from milestone 40's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                          | Items |
| :-: | ------------ | ------------------------------------ | :---: |
| [ ] | `D05 T05 §1` | Plan milestone 41: GPU encode search |   4   |
| [ ] | `D05 T05 §2` | Milestone 41 gate: GPU encode search |   4   |

### Phase 41 -- Milestone 42: Energy per megapixel

Milestone 42's work is authored one milestone ahead by its planning section, from milestone 41's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                             | Items |
| :-: | ------------ | --------------------------------------- | :---: |
| [ ] | `D05 T05 §3` | Plan milestone 42: Energy per megapixel |   4   |
| [ ] | `D05 T05 §4` | Milestone 42 gate: Energy per megapixel |   4   |

### Phase 42 -- Milestone 43: Phone-class ARM

Milestone 43's work is authored one milestone ahead by its planning section, from milestone 42's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                        | Items |
| :-: | ------------ | ---------------------------------- | :---: |
| [ ] | `D05 T05 §5` | Plan milestone 43: Phone-class ARM |   4   |
| [ ] | `D05 T05 §6` | Milestone 43 gate: Phone-class ARM |   4   |

### Phase 43 -- Milestone 44: Proven decoder bounds

Milestone 44's work is authored one milestone ahead by its planning section, from milestone 43's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                              | Items |
| :-: | ------------ | ---------------------------------------- | :---: |
| [ ] | `D05 T05 §7` | Plan milestone 44: Proven decoder bounds |   4   |
| [ ] | `D05 T05 §8` | Milestone 44 gate: Proven decoder bounds |   4   |

### Phase 44 -- Milestone 45: Reproducible, signed releases

Milestone 45's work is authored one milestone ahead by its planning section, from milestone 44's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                                      | Items |
| :-: | ------------- | ------------------------------------------------ | :---: |
| [ ] | `D05 T05 §9`  | Plan milestone 45: Reproducible, signed releases |   4   |
| [ ] | `D05 T05 §10` | Milestone 45 gate: Reproducible, signed releases |   4   |

### Phase 45 -- Milestone 46: Collection archives

Milestone 46's work is authored one milestone ahead by its planning section, from milestone 45's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                            | Items |
| :-: | ------------- | -------------------------------------- | :---: |
| [ ] | `D05 T05 §11` | Plan milestone 46: Collection archives |   4   |
| [ ] | `D05 T05 §12` | Milestone 46 gate: Collection archives |   4   |

### Phase 46 -- Milestone 47: Tiny decoder profile

Milestone 47's work is authored one milestone ahead by its planning section, from milestone 46's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                             | Items |
| :-: | ------------- | --------------------------------------- | :---: |
| [ ] | `D05 T05 §13` | Plan milestone 47: Tiny decoder profile |   4   |
| [ ] | `D05 T05 §14` | Milestone 47 gate: Tiny decoder profile |   4   |

### Phase 47 -- Milestone 48: Recognised format

Milestone 48's work is authored one milestone ahead by its planning section, from milestone 47's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                          | Items |
| :-: | ------------- | ------------------------------------ | :---: |
| [ ] | `D05 T05 §15` | Plan milestone 48: Recognised format |   4   |
| [ ] | `D05 T05 §16` | Milestone 48 gate: Recognised format |   4   |

### Phase 48 -- Milestone 49: Third-party benchmarks

Milestone 49's work is authored one milestone ahead by its planning section, from milestone 48's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                               | Items |
| :-: | ------------- | ----------------------------------------- | :---: |
| [ ] | `D05 T05 §17` | Plan milestone 49: Third-party benchmarks |   4   |
| [ ] | `D05 T05 §18` | Milestone 49 gate: Third-party benchmarks |   4   |

### Phase 49 -- Milestone 50: RasterFold 5.0

Milestone 50's work is authored one milestone ahead by its planning section, from milestone 49's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                       | Items |
| :-: | ------------- | --------------------------------- | :---: |
| [ ] | `D05 T05 §19` | Plan milestone 50: RasterFold 5.0 |   4   |
| [ ] | `D05 T05 §20` | Milestone 50 gate: RasterFold 5.0 |   6   |

### Phase 50 -- Milestone 51: Learned model at the default level

Milestone 51's work is authored one milestone ahead by its planning section, from milestone 50's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                           | Items |
| :-: | ------------ | ----------------------------------------------------- | :---: |
| [ ] | `D05 T06 §1` | Plan milestone 51: Learned model at the default level |   4   |
| [ ] | `D05 T06 §2` | Milestone 51 gate: Learned model at the default level |   4   |

### Phase 51 -- Milestone 52: Transmitted priors

Milestone 52's work is authored one milestone ahead by its planning section, from milestone 51's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                           | Items |
| :-: | ------------ | ------------------------------------- | :---: |
| [ ] | `D05 T06 §3` | Plan milestone 52: Transmitted priors |   4   |
| [ ] | `D05 T06 §4` | Milestone 52 gate: Transmitted priors |   4   |

### Phase 52 -- Milestone 53: Camera raw

Milestone 53's work is authored one milestone ahead by its planning section, from milestone 52's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                   | Items |
| :-: | ------------ | ----------------------------- | :---: |
| [ ] | `D05 T06 §5` | Plan milestone 53: Camera raw |   4   |
| [ ] | `D05 T06 §6` | Milestone 53 gate: Camera raw |   4   |

### Phase 53 -- Milestone 54: Medical and scientific imagery

Milestone 54's work is authored one milestone ahead by its planning section, from milestone 53's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section      | Deliverable                                       | Items |
| :-: | ------------ | ------------------------------------------------- | :---: |
| [ ] | `D05 T06 §7` | Plan milestone 54: Medical and scientific imagery |   4   |
| [ ] | `D05 T06 §8` | Milestone 54 gate: Medical and scientific imagery |   4   |

### Phase 54 -- Milestone 55: Error resilience

Milestone 55's work is authored one milestone ahead by its planning section, from milestone 54's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                         | Items |
| :-: | ------------- | ----------------------------------- | :---: |
| [ ] | `D05 T06 §9`  | Plan milestone 55: Error resilience |   4   |
| [ ] | `D05 T06 §10` | Milestone 55 gate: Error resilience |   4   |

### Phase 55 -- Milestone 56: Progressive decoding

Milestone 56's work is authored one milestone ahead by its planning section, from milestone 55's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                             | Items |
| :-: | ------------- | --------------------------------------- | :---: |
| [ ] | `D05 T06 §11` | Plan milestone 56: Progressive decoding |   4   |
| [ ] | `D05 T06 §12` | Milestone 56 gate: Progressive decoding |   4   |

### Phase 56 -- Milestone 57: Lossless edit deltas

Milestone 57's work is authored one milestone ahead by its planning section, from milestone 56's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                             | Items |
| :-: | ------------- | --------------------------------------- | :---: |
| [ ] | `D05 T06 §13` | Plan milestone 57: Lossless edit deltas |   4   |
| [ ] | `D05 T06 §14` | Milestone 57 gate: Lossless edit deltas |   4   |

### Phase 57 -- Milestone 58: Hardware decoder

Milestone 58's work is authored one milestone ahead by its planning section, from milestone 57's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                         | Items |
| :-: | ------------- | ----------------------------------- | :---: |
| [ ] | `D05 T06 §15` | Plan milestone 58: Hardware decoder |   4   |
| [ ] | `D05 T06 §16` | Milestone 58 gate: Hardware decoder |   4   |

### Phase 58 -- Milestone 59: Twenty-year compatibility

Milestone 59's work is authored one milestone ahead by its planning section, from milestone 58's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                                  | Items |
| :-: | ------------- | -------------------------------------------- | :---: |
| [ ] | `D05 T06 §17` | Plan milestone 59: Twenty-year compatibility |   4   |
| [ ] | `D05 T06 §18` | Milestone 59 gate: Twenty-year compatibility |   4   |

### Phase 59 -- Milestone 60: RasterFold 6.0

Milestone 60's work is authored one milestone ahead by its planning section, from milestone 59's measured results; the gate measures the target and the no-regression rule.

|  ✔  | Section       | Deliverable                       | Items |
| :-: | ------------- | --------------------------------- | :---: |
| [ ] | `D05 T06 §19` | Plan milestone 60: RasterFold 6.0 |   4   |
| [ ] | `D05 T06 §20` | Milestone 60 gate: RasterFold 6.0 |   6   |

**Groomed 2026-09-30:** Added the native converter and complete documentation requirements, repaired prerequisite order within existing phases, and checked release reachability with `python3 scripts/plan-audit.py`. No existing section changed phase and no completion box was ticked.
