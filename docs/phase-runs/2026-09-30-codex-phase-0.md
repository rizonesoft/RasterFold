# Codex phase 0: Locked baseline

Phase: 0

Scope: plan

Starting commit: `8b30db5cb2901fce31693102df832d0d3143a083`

Started: 2026-09-30T20:10:16.369900+00:00

## Graph audit

### `python3 scripts/todo-graph.py validate`

Exit: 0

```text

32 todos, 222 sections -- 0 fatal, 0 warning(s)
```

### `python3 scripts/todo-graph.py plan --check`

Exit: 0

```text
implementation plan is current -- 0/222 sections complete (0%)
```

### `python3 scripts/todo-graph.py query ready`

Exit: 0

```text
00-workspace/TODO-01-dev-automation.md §1  Commit the port and wire the commit hook
00-workspace/TODO-02-measurement-references.md §1  e9 and WebP references in the research tools
00-workspace/TODO-02-measurement-references.md §2  JPEG XL e9 in the benchmark
00-workspace/TODO-03-held-out-validation.md §1  The held-out set
00-workspace/TODO-04-wsl-and-runners.md §1 🔒  Local toolchains and WSL gates
01-compression/TODO-01-palette-method.md §1 🔒  One range coder shared between methods
01-compression/TODO-02-model-experiments.md §1  A sixth and seventh context model
01-compression/TODO-02-model-experiments.md §2  Two adaptation speeds per context
01-compression/TODO-02-model-experiments.md §3  A second mixer weight set
01-compression/TODO-02-model-experiments.md §4  Learning cost on small images
01-compression/TODO-02-model-experiments.md §5  An exact-repeat context
01-compression/TODO-02-model-experiments.md §6  The ceiling of encoder-chosen parameters
01-compression/TODO-04-exact-repeats.md §1 🔒  Where the repeats are
01-compression/TODO-05-every-photo-beats-e9.md §1 🔒  Why the losing photos lose
01-compression/TODO-06-beat-webp-generated.md §1 🔒  Where WebP still wins
02-speed/TODO-01-compiler-options.md §1  Profile-guided optimization
02-speed/TODO-02-parallel-and-simd.md §1  The size cost of horizontal strips
02-speed/TODO-02-parallel-and-simd.md §2  The speed of an int16 SIMD mixer
03-memory/TODO-01-buffer-lifetimes.md §1  Decode streams straight from the input file
03-memory/TODO-01-buffer-lifetimes.md §3  Encode straight into the output buffer
04-robustness/TODO-01-decoder-hardening.md §1  Stop decoding a stream that has run out
06-library/TODO-01-dll-and-api.md §1 🔒  The public header
08-docs/TODO-02-drift-gates.md §1  One facts file and claims everywhere
09-release/TODO-01-release-v1.md §1 🔒  One version source and the changelog
99-manual/TODO-01-operator-setup.md §3  Approve format version 3
99-manual/TODO-01-operator-setup.md §4  The missed-milestone policy

runnable elsewhere (context: none):
99-manual/TODO-01-operator-setup.md §1  Remove the retired run-guard folders  requires operator (missing: operator)
99-manual/TODO-01-operator-setup.md §2  Log in the GitHub CLI  requires operator (missing: operator)
99-manual/TODO-01-operator-setup.md §5  Enable GitHub Pages  requires operator (missing: operator)
99-manual/TODO-01-operator-setup.md §6  Android release signing identity  requires operator (missing: operator)
99-manual/TODO-01-operator-setup.md §7  HEVC distribution decision  requires operator (missing: operator)

26 runnable now, 5 runnable elsewhere
```

### `python3 scripts/todo-graph.py query blocked`

Exit: 0

```text
00-workspace/TODO-01-dev-automation.md §2  First push: CI green and read back
    waiting on: dev-automation §1, operator-setup §2
00-workspace/TODO-01-dev-automation.md §3  The static analysis gate
    waiting on: dev-automation §1
00-workspace/TODO-01-dev-automation.md §4  Prove the review panel end to end
    waiting on: dev-automation §1
00-workspace/TODO-01-dev-automation.md §5  Raise the claims coverage floor
    waiting on: dev-automation §1
00-workspace/TODO-02-measurement-references.md §3  The committed corpus results file
    waiting on: measurement-references §1
00-workspace/TODO-03-held-out-validation.md §2  Validate the milestones on it
    waiting on: held-out-validation §1
00-workspace/TODO-04-wsl-and-runners.md §2  Portable platform and SIMD boundary
    waiting on: wsl-and-runners §1
00-workspace/TODO-04-wsl-and-runners.md §3  Isolated Codex lifecycle and review runner
    waiting on: wsl-and-runners §1
00-workspace/TODO-04-wsl-and-runners.md §4  Family-local intelligence
    waiting on: wsl-and-runners §3
01-compression/TODO-01-palette-method.md §2  Method 2: palette encoder, decoder, oracle, spec
    waiting on: palette-method §1
01-compression/TODO-01-palette-method.md §3  Measure method 2 against JPEG XL and WebP
    waiting on: palette-method §2
01-compression/TODO-02-model-experiments.md §7  The accepted variants measured together
    waiting on: model-experiments §1, model-experiments §2, model-experiments §3, model-experiments §4, model-experiments §5, model-experiments §6
01-compression/TODO-03-format-version-3.md §1  Version 3: the accepted model changes
    waiting on: model-experiments §7, operator-setup §3
01-compression/TODO-03-format-version-3.md §2  Version 3: horizontal strips
    waiting on: format-version-3 §1, parallel-and-simd §1
01-compression/TODO-04-exact-repeats.md §2  A match model, measured
    waiting on: exact-repeats §1
01-compression/TODO-04-exact-repeats.md §3  The match model in format version 3
    waiting on: exact-repeats §2, format-version-3 §1
01-compression/TODO-05-every-photo-beats-e9.md §2  Encoder-chosen parameters in version 3
    waiting on: every-photo-beats-e9 §1, model-experiments §6, format-version-3 §1
01-compression/TODO-06-beat-webp-generated.md §2  A colour-cache model in version 3
    waiting on: beat-webp-generated §1, format-version-3 §1
01-compression/TODO-06-beat-webp-generated.md §3  Copy runs for palette images
    waiting on: beat-webp-generated §1, palette-method §2
02-speed/TODO-01-compiler-options.md §2  Link-time optimization
    waiting on: compiler-options §1
02-speed/TODO-01-compiler-options.md §3  An AVX2 build, measured before dispatching
    waiting on: compiler-options §2
03-memory/TODO-01-buffer-lifetimes.md §2  Decode straight into the BMP
    waiting on: buffer-lifetimes §1
04-robustness/TODO-01-decoder-hardening.md §2  A long fuzz campaign over every method
    waiting on: decoder-hardening §1, palette-method §2
05-milestones/TODO-01-beat-jxl-and-webp.md §1  Milestone 1 gate: Locked baseline
    waiting on: dev-automation §1, dev-automation §2, dev-automation §3, dev-automation §4, dev-automation §5, measurement-references §1, measurement-references §2, measurement-references §3, html-docs §1, html-docs §2, html-docs §3, drift-gates §1, drift-gates §2, drift-gates §3, drift-gates §4, drift-gates §5, drift-gates §6, drift-gates §7, wsl-and-runners §1, wsl-and-runners §2, wsl-and-runners §3, wsl-and-runners §4, repository-presentation §1, operator-setup §3, operator-setup §4
05-milestones/TODO-01-beat-jxl-and-webp.md §2  Milestone 2 gate: Same bytes, leaner and harder
    waiting on: buffer-lifetimes §1, buffer-lifetimes §2, buffer-lifetimes §3, decoder-hardening §1, compiler-options §1, compiler-options §2, milestones-beat-jxl-and-webp §1
05-milestones/TODO-01-beat-jxl-and-webp.md §3  Milestone 3 gate: Few-colour images solved
    waiting on: palette-method §1, palette-method §2, palette-method §3, decoder-hardening §2, milestones-beat-jxl-and-webp §2
05-milestones/TODO-01-beat-jxl-and-webp.md §4  Milestone 4 gate: Format version 3: beat JPEG XL e9 on photographs
    waiting on: model-experiments §1, model-experiments §2, model-experiments §3, model-experiments §4, model-experiments §5, model-experiments §6, model-experiments §7, format-version-3 §1, milestones-beat-jxl-and-webp §3
05-milestones/TODO-01-beat-jxl-and-webp.md §5  Milestone 5 gate: Exact repeats
    waiting on: exact-repeats §1, exact-repeats §2, exact-repeats §3, milestones-beat-jxl-and-webp §4
05-milestones/TODO-01-beat-jxl-and-webp.md §6  Milestone 6 gate: Faster than WebP to decode
    waiting on: parallel-and-simd §1, format-version-3 §2, milestones-beat-jxl-and-webp §5
05-milestones/TODO-01-beat-jxl-and-webp.md §7  Milestone 7 gate: Every photograph beats JPEG XL e9
    waiting on: every-photo-beats-e9 §1, every-photo-beats-e9 §2, milestones-beat-jxl-and-webp §6
05-milestones/TODO-01-beat-jxl-and-webp.md §8  Milestone 8 gate: Beat WebP on generated images
    waiting on: beat-webp-generated §1, beat-webp-generated §2, beat-webp-generated §3, milestones-beat-jxl-and-webp §7
05-milestones/TODO-01-beat-jxl-and-webp.md §9  Milestone 9 gate: Holds on images never tuned on
    waiting on: held-out-validation §1, held-out-validation §2, milestones-beat-jxl-and-webp §8
05-milestones/TODO-01-beat-jxl-and-webp.md §10  Milestone 10 gate: RasterFold 1.0
    waiting on: dll-and-api §1, dll-and-api §2, dll-and-api §3, dll-and-api §4, dll-and-api §5, command-line §1, command-line §2, command-line §3, command-line §4, generated-references §1, generated-references §2, release-v1 §1, release-v1 §2, release-v1 §3, release-v1 §4, milestones-beat-jxl-and-webp §9
05-milestones/TODO-02-beat-itself.md §1  Plan milestone 11: SIMD decisions
    waiting on: milestones-beat-jxl-and-webp §10
05-milestones/TODO-02-beat-itself.md §2  Milestone 11 gate: SIMD decisions
    waiting on: milestones-beat-itself §1, compiler-options §3, parallel-and-simd §2
05-milestones/TODO-02-beat-itself.md §3  Plan milestone 12: Two-layer mixing
    waiting on: milestones-beat-itself §2
05-milestones/TODO-02-beat-itself.md §4  Milestone 12 gate: Two-layer mixing
    waiting on: milestones-beat-itself §3
05-milestones/TODO-02-beat-itself.md §5  Plan milestone 13: Stronger prediction
    waiting on: milestones-beat-itself §4
05-milestones/TODO-02-beat-itself.md §6  Milestone 13 gate: Stronger prediction
    waiting on: milestones-beat-itself §5
05-milestones/TODO-02-beat-itself.md §7  Plan milestone 14: Adaptive colour transform
    waiting on: milestones-beat-itself §6
05-milestones/TODO-02-beat-itself.md §8  Milestone 14 gate: Adaptive colour transform
    waiting on: milestones-beat-itself §7
05-milestones/TODO-02-beat-itself.md §9  Plan milestone 15: Effort levels
    waiting on: milestones-beat-itself §8
05-milestones/TODO-02-beat-itself.md §10  Milestone 15 gate: Effort levels
    waiting on: milestones-beat-itself §9
05-milestones/TODO-02-beat-itself.md §11  Plan milestone 16: Max-effort encoder search
    waiting on: milestones-beat-itself §10
05-milestones/TODO-02-beat-itself.md §12  Milestone 16 gate: Max-effort encoder search
    waiting on: milestones-beat-itself §11
05-milestones/TODO-02-beat-itself.md §13  Plan milestone 17: Trained starting state
    waiting on: milestones-beat-itself §12
05-milestones/TODO-02-beat-itself.md §14  Milestone 17 gate: Trained starting state
    waiting on: milestones-beat-itself §13
05-milestones/TODO-02-beat-itself.md §15  Plan milestone 18: Streaming
    waiting on: milestones-beat-itself §14
05-milestones/TODO-02-beat-itself.md §16  Milestone 18 gate: Streaming
    waiting on: milestones-beat-itself §15
05-milestones/TODO-02-beat-itself.md §17  Plan milestone 19: Many-core scaling
    waiting on: milestones-beat-itself §16
05-milestones/TODO-02-beat-itself.md §18  Milestone 19 gate: Many-core scaling
    waiting on: milestones-beat-itself §17
05-milestones/TODO-02-beat-itself.md §19  Plan milestone 20: RasterFold 2.0
    waiting on: milestones-beat-itself §18
05-milestones/TODO-02-beat-itself.md §20  Milestone 20 gate: RasterFold 2.0
    waiting on: milestones-beat-itself §19
05-milestones/TODO-03-reach.md §1  Plan milestone 21: Run mode for flat regions
    waiting on: milestones-beat-itself §20
05-milestones/TODO-03-reach.md §2  Milestone 21 gate: Run mode for flat regions
    waiting on: milestones-reach §1
05-milestones/TODO-03-reach.md §3  Plan milestone 22: Verified hashed context tables
    waiting on: milestones-reach §2
05-milestones/TODO-03-reach.md §4  Milestone 22 gate: Verified hashed context tables
    waiting on: milestones-reach §3
05-milestones/TODO-03-reach.md §5  Plan milestone 23: Higher-precision coder
    waiting on: milestones-reach §4
05-milestones/TODO-03-reach.md §6  Milestone 23 gate: Higher-precision coder
    waiting on: milestones-reach §5
05-milestones/TODO-03-reach.md §7  Plan milestone 24: Learned predictor at the max level
    waiting on: milestones-reach §6
05-milestones/TODO-03-reach.md §8  Milestone 24 gate: Learned predictor at the max level
    waiting on: milestones-reach §7
05-milestones/TODO-03-reach.md §9  Plan milestone 25: Texture-gated models
    waiting on: milestones-reach §8
05-milestones/TODO-03-reach.md §10  Milestone 25 gate: Texture-gated models
    waiting on: milestones-reach §9
05-milestones/TODO-03-reach.md §11  Plan milestone 26: Random-access decode
    waiting on: milestones-reach §10
05-milestones/TODO-03-reach.md §12  Milestone 26 gate: Random-access decode
    waiting on: milestones-reach §11
05-milestones/TODO-03-reach.md §13  Plan milestone 27: Portable and byte-exact
    waiting on: milestones-reach §12
05-milestones/TODO-03-reach.md §14  Milestone 27 gate: Portable and byte-exact
    waiting on: milestones-reach §13
05-milestones/TODO-03-reach.md §15  Plan milestone 28: Alpha, 16-bit, and metadata
    waiting on: milestones-reach §14
05-milestones/TODO-03-reach.md §16  Milestone 28 gate: Alpha, 16-bit, and metadata
    waiting on: milestones-reach §15
05-milestones/TODO-03-reach.md §17  Plan milestone 29: Coverage-proven decoder
    waiting on: milestones-reach §16
05-milestones/TODO-03-reach.md §18  Milestone 29 gate: Coverage-proven decoder
    waiting on: milestones-reach §17
05-milestones/TODO-03-reach.md §19  Plan milestone 30: RasterFold 3.0
    waiting on: milestones-reach §18
05-milestones/TODO-03-reach.md §20  Milestone 30 gate: RasterFold 3.0
    waiting on: milestones-reach §19
05-milestones/TODO-04-beyond-still-files.md §1  Plan milestone 31: Language bindings and a streaming API
    waiting on: milestones-reach §20
05-milestones/TODO-04-beyond-still-files.md §2  Milestone 31 gate: Language bindings and a streaming API
    waiting on: milestones-beyond-still-files §1
05-milestones/TODO-04-beyond-still-files.md §3  Plan milestone 32: More formats
    waiting on: milestones-beyond-still-files §2
05-milestones/TODO-04-beyond-still-files.md §4  Milestone 32 gate: More formats
    waiting on: milestones-beyond-still-files §3
05-milestones/TODO-04-beyond-still-files.md §5  Plan milestone 33: Lossless JPEG recompression
    waiting on: milestones-beyond-still-files §4
05-milestones/TODO-04-beyond-still-files.md §6  Milestone 33 gate: Lossless JPEG recompression
    waiting on: milestones-beyond-still-files §5
05-milestones/TODO-04-beyond-still-files.md §7  Plan milestone 34: Image sequences
    waiting on: milestones-beyond-still-files §6
05-milestones/TODO-04-beyond-still-files.md §8  Milestone 34 gate: Image sequences
    waiting on: milestones-beyond-still-files §7
05-milestones/TODO-04-beyond-still-files.md §9  Plan milestone 35: Gigapixel images
    waiting on: milestones-beyond-still-files §8
05-milestones/TODO-04-beyond-still-files.md §10  Milestone 35 gate: Gigapixel images
    waiting on: milestones-beyond-still-files §9
05-milestones/TODO-04-beyond-still-files.md §11  Plan milestone 36: Browser decoding
    waiting on: milestones-beyond-still-files §10
05-milestones/TODO-04-beyond-still-files.md §12  Milestone 36 gate: Browser decoding
    waiting on: milestones-beyond-still-files §11
05-milestones/TODO-04-beyond-still-files.md §13  Plan milestone 37: Host plugins
    waiting on: milestones-beyond-still-files §12
05-milestones/TODO-04-beyond-still-files.md §14  Milestone 37 gate: Host plugins
    waiting on: milestones-beyond-still-files §13
05-milestones/TODO-04-beyond-still-files.md §15  Plan milestone 38: Second implementation
    waiting on: milestones-beyond-still-files §14
05-milestones/TODO-04-beyond-still-files.md §16  Milestone 38 gate: Second implementation
    waiting on: milestones-beyond-still-files §15
05-milestones/TODO-04-beyond-still-files.md §17  Plan milestone 39: Automatic effort
    waiting on: milestones-beyond-still-files §16
05-milestones/TODO-04-beyond-still-files.md §18  Milestone 39 gate: Automatic effort
    waiting on: milestones-beyond-still-files §17
05-milestones/TODO-04-beyond-still-files.md §19  Plan milestone 40: RasterFold 4.0
    waiting on: milestones-beyond-still-files §18
05-milestones/TODO-04-beyond-still-files.md §20  Milestone 40 gate: RasterFold 4.0
    waiting on: milestones-beyond-still-files §19
05-milestones/TODO-05-efficiency-and-trust.md §1  Plan milestone 41: GPU encode search
    waiting on: milestones-beyond-still-files §20
05-milestones/TODO-05-efficiency-and-trust.md §2  Milestone 41 gate: GPU encode search
    waiting on: milestones-efficiency-and-trust §1
05-milestones/TODO-05-efficiency-and-trust.md §3  Plan milestone 42: Energy per megapixel
    waiting on: milestones-efficiency-and-trust §2
05-milestones/TODO-05-efficiency-and-trust.md §4  Milestone 42 gate: Energy per megapixel
    waiting on: milestones-efficiency-and-trust §3
05-milestones/TODO-05-efficiency-and-trust.md §5  Plan milestone 43: Phone-class ARM
    waiting on: milestones-efficiency-and-trust §4
05-milestones/TODO-05-efficiency-and-trust.md §6  Milestone 43 gate: Phone-class ARM
    waiting on: milestones-efficiency-and-trust §5
05-milestones/TODO-05-efficiency-and-trust.md §7  Plan milestone 44: Proven decoder bounds
    waiting on: milestones-efficiency-and-trust §6
05-milestones/TODO-05-efficiency-and-trust.md §8  Milestone 44 gate: Proven decoder bounds
    waiting on: milestones-efficiency-and-trust §7
05-milestones/TODO-05-efficiency-and-trust.md §9  Plan milestone 45: Reproducible, signed releases
    waiting on: milestones-efficiency-and-trust §8
05-milestones/TODO-05-efficiency-and-trust.md §10  Milestone 45 gate: Reproducible, signed releases
    waiting on: milestones-efficiency-and-trust §9
05-milestones/TODO-05-efficiency-and-trust.md §11  Plan milestone 46: Collection archives
    waiting on: milestones-efficiency-and-trust §10
05-milestones/TODO-05-efficiency-and-trust.md §12  Milestone 46 gate: Collection archives
    waiting on: milestones-efficiency-and-trust §11
05-milestones/TODO-05-efficiency-and-trust.md §13  Plan milestone 47: Tiny decoder profile
    waiting on: milestones-efficiency-and-trust §12
05-milestones/TODO-05-efficiency-and-trust.md §14  Milestone 47 gate: Tiny decoder profile
    waiting on: milestones-efficiency-and-trust §13
05-milestones/TODO-05-efficiency-and-trust.md §15  Plan milestone 48: Recognised format
    waiting on: milestones-efficiency-and-trust §14
05-milestones/TODO-05-efficiency-and-trust.md §16  Milestone 48 gate: Recognised format
    waiting on: milestones-efficiency-and-trust §15
05-milestones/TODO-05-efficiency-and-trust.md §17  Plan milestone 49: Third-party benchmarks
    waiting on: milestones-efficiency-and-trust §16
05-milestones/TODO-05-efficiency-and-trust.md §18  Milestone 49 gate: Third-party benchmarks
    waiting on: milestones-efficiency-and-trust §17
05-milestones/TODO-05-efficiency-and-trust.md §19  Plan milestone 50: RasterFold 5.0
    waiting on: milestones-efficiency-and-trust §18
05-milestones/TODO-05-efficiency-and-trust.md §20  Milestone 50 gate: RasterFold 5.0
    waiting on: milestones-efficiency-and-trust §19
05-milestones/TODO-06-new-data.md §1  Plan milestone 51: Learned model at the default level
    waiting on: milestones-efficiency-and-trust §20
05-milestones/TODO-06-new-data.md §2  Milestone 51 gate: Learned model at the default level
    waiting on: milestones-new-data §1
05-milestones/TODO-06-new-data.md §3  Plan milestone 52: Transmitted priors
    waiting on: milestones-new-data §2
05-milestones/TODO-06-new-data.md §4  Milestone 52 gate: Transmitted priors
    waiting on: milestones-new-data §3
05-milestones/TODO-06-new-data.md §5  Plan milestone 53: Camera raw
    waiting on: milestones-new-data §4
05-milestones/TODO-06-new-data.md §6  Milestone 53 gate: Camera raw
    waiting on: milestones-new-data §5
05-milestones/TODO-06-new-data.md §7  Plan milestone 54: Medical and scientific imagery
    waiting on: milestones-new-data §6
05-milestones/TODO-06-new-data.md §8  Milestone 54 gate: Medical and scientific imagery
    waiting on: milestones-new-data §7
05-milestones/TODO-06-new-data.md §9  Plan milestone 55: Error resilience
    waiting on: milestones-new-data §8
05-milestones/TODO-06-new-data.md §10  Milestone 55 gate: Error resilience
    waiting on: milestones-new-data §9
05-milestones/TODO-06-new-data.md §11  Plan milestone 56: Progressive decoding
    waiting on: milestones-new-data §10
05-milestones/TODO-06-new-data.md §12  Milestone 56 gate: Progressive decoding
    waiting on: milestones-new-data §11
05-milestones/TODO-06-new-data.md §13  Plan milestone 57: Lossless edit deltas
    waiting on: milestones-new-data §12
05-milestones/TODO-06-new-data.md §14  Milestone 57 gate: Lossless edit deltas
    waiting on: milestones-new-data §13
05-milestones/TODO-06-new-data.md §15  Plan milestone 58: Hardware decoder
    waiting on: milestones-new-data §14
05-milestones/TODO-06-new-data.md §16  Milestone 58 gate: Hardware decoder
    waiting on: milestones-new-data §15
05-milestones/TODO-06-new-data.md §17  Plan milestone 59: Twenty-year compatibility
    waiting on: milestones-new-data §16
05-milestones/TODO-06-new-data.md §18  Milestone 59 gate: Twenty-year compatibility
    waiting on: milestones-new-data §17
05-milestones/TODO-06-new-data.md §19  Plan milestone 60: RasterFold 6.0
    waiting on: milestones-new-data §18
05-milestones/TODO-06-new-data.md §20  Milestone 60 gate: RasterFold 6.0
    waiting on: milestones-new-data §19
06-library/TODO-01-dll-and-api.md §2  Shared/static libraries and Windows import libraries
    waiting on: dll-and-api §1
06-library/TODO-01-dll-and-api.md §3  The command line on the library
    waiting on: dll-and-api §2
06-library/TODO-01-dll-and-api.md §4  API tests and the ABI list
    waiting on: dll-and-api §2
06-library/TODO-01-dll-and-api.md §5  Example programs
    waiting on: dll-and-api §4
07-tool/TODO-01-command-line.md §1  Commands and options
    waiting on: dll-and-api §3
07-tool/TODO-01-command-line.md §2  PNG in and out
    waiting on: command-line §1
07-tool/TODO-01-command-line.md §3  PPM in and out
    waiting on: command-line §1
07-tool/TODO-01-command-line.md §4  Exit codes, messages, and overwrite
    waiting on: command-line §1
08-docs/TODO-01-html-docs.md §1  The Jekyll site and its generated pages
    waiting on: drift-gates §1
08-docs/TODO-01-html-docs.md §2  Comparison charts
    waiting on: html-docs §1, measurement-references §3
08-docs/TODO-01-html-docs.md §3  Publish with GitHub Pages
    waiting on: html-docs §2, html-docs §4, html-docs §5, drift-gates §5, drift-gates §7, operator-setup §5
08-docs/TODO-01-html-docs.md §4  Complete current documentation map
    waiting on: html-docs §1, drift-gates §7
08-docs/TODO-01-html-docs.md §5  Accessible navigation and site states
    waiting on: html-docs §4
08-docs/TODO-01-html-docs.md §6  Converter user guide and capability reference
    waiting on: html-docs §4, converter-interface §8, converter-delivery §1
08-docs/TODO-01-html-docs.md §7  Versioned downloads and complete references
    waiting on: release-v1 §1, release-v1 §2, generated-references §1, generated-references §2
08-docs/TODO-01-html-docs.md §8  Published whole-product documentation acceptance
    waiting on: html-docs §3, html-docs §6, html-docs §7, converter-delivery §1
08-docs/TODO-02-drift-gates.md §2  Figures bound to their data
    waiting on: drift-gates §1, measurement-references §3
08-docs/TODO-02-drift-gates.md §3  Generated files match their sources
    waiting on: drift-gates §1, html-docs §1
08-docs/TODO-02-drift-gates.md §4  Staleness: verified dates and data revisions
    waiting on: drift-gates §2
08-docs/TODO-02-drift-gates.md §5  Links, and the gates wired everywhere
    waiting on: drift-gates §3, drift-gates §4
08-docs/TODO-02-drift-gates.md §6  Deterministic repair and complete file coverage
    waiting on: drift-gates §1, drift-gates §3
08-docs/TODO-02-drift-gates.md §7  Commit and built-site provenance
    waiting on: drift-gates §5, drift-gates §6
08-docs/TODO-03-references.md §1  The command-line reference
    waiting on: command-line §1, html-docs §1
08-docs/TODO-03-references.md §2  The API reference
    waiting on: dll-and-api §1, html-docs §1
08-docs/TODO-04-repository-presentation.md §1  Repository presentation and developer entry points
    waiting on: wsl-and-runners §1
09-release/TODO-01-release-v1.md §2  The four-target release package
    waiting on: release-v1 §1, dll-and-api §2, converter-delivery §3
09-release/TODO-01-release-v1.md §3  The release workflow
    waiting on: release-v1 §2
09-release/TODO-01-release-v1.md §4  Release rasterfold-v1.0.0
    waiting on: release-v1 §3, milestones-beat-jxl-and-webp §9, converter-delivery §4
10-converter/TODO-01-engine.md §1  Pixel contract and capability registry
    waiting on: dll-and-api §1
10-converter/TODO-01-engine.md §2  Pinned bundled codec builds
    waiting on: converter-engine §1, wsl-and-runners §1
10-converter/TODO-01-engine.md §3  Bounded asynchronous jobs
    waiting on: converter-engine §1, converter-engine §2
10-converter/TODO-01-engine.md §4  Exactness verification and differential fixtures
    waiting on: converter-engine §1, converter-engine §3
10-converter/TODO-01-engine.md §5  Safe import and export
    waiting on: converter-engine §3, converter-engine §4
10-converter/TODO-01-engine.md §6  Comparable measurements and reports
    waiting on: converter-engine §3, converter-engine §4
10-converter/TODO-02-codecs.md §1  RasterFold, BMP, PNG and PNM
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5, command-line §2, command-line §3
10-converter/TODO-02-codecs.md §2  WebP
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §3  JPEG XL
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §4  AVIF
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §5  JPEG optimization and import
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §6  TIFF and GIF
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §7  HEIF and HEIC
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5, operator-setup §7
10-converter/TODO-02-codecs.md §8  QOI
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §9  JPEG 2000
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §10  OpenEXR
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §11  Camera RAW and DNG import
    waiting on: converter-engine §2, converter-engine §4, converter-engine §5
10-converter/TODO-02-codecs.md §12  Animation and multi-page preservation
    waiting on: converter-codecs §2, converter-codecs §3, converter-codecs §4, converter-codecs §6, converter-engine §4
10-converter/TODO-03-interface.md §1  C interface components and SVG icons
    waiting on: converter-engine §1
10-converter/TODO-03-interface.md §2  Windows native integration
    waiting on: converter-interface §1, converter-engine §3, converter-engine §5
10-converter/TODO-03-interface.md §3  Linux native integration
    waiting on: converter-interface §1, converter-engine §3, converter-engine §5
10-converter/TODO-03-interface.md §4  Android native integration
    waiting on: converter-interface §1, converter-engine §3, converter-engine §5
10-converter/TODO-03-interface.md §5  Single-image conversion and preview
    waiting on: converter-interface §2, converter-interface §3, converter-interface §4, converter-codecs §1, converter-engine §6
10-converter/TODO-03-interface.md §6  Synchronized comparisons
    waiting on: converter-interface §5, converter-engine §6, converter-codecs §2, converter-codecs §3, converter-codecs §4
10-converter/TODO-03-interface.md §7  Preferences and accessible error recovery
    waiting on: converter-interface §5, converter-interface §6
10-converter/TODO-03-interface.md §8  Guided RasterFold demonstration
    waiting on: converter-interface §6, converter-interface §7
10-converter/TODO-04-delivery.md §1  Full capability and device matrix
    waiting on: converter-codecs §1, converter-codecs §2, converter-codecs §3, converter-codecs §4, converter-codecs §5, converter-codecs §6, converter-codecs §7, converter-codecs §8, converter-codecs §9, converter-codecs §10, converter-codecs §11, converter-codecs §12, converter-interface §8
10-converter/TODO-04-delivery.md §2  Hostile input and resource limits
    waiting on: converter-delivery §1
10-converter/TODO-04-delivery.md §3  Self-contained native packages
    waiting on: converter-delivery §1, converter-delivery §2, release-v1 §1, operator-setup §6
10-converter/TODO-04-delivery.md §4  Whole-product release acceptance
    waiting on: converter-delivery §3, html-docs §8, release-v1 §3

191 section(s) blocked
```

### `python3 scripts/todo-graph.py query budget`

Exit: 0

```text
budget  222 sections (2 discovered); backlog 0 entries. Nothing is capped: not operator-directed sections, not discovered sections, not the backlog.

  phase  sections  discovered  title
      0        31           0  Milestone 1: Locked baseline
      1         7           0  Milestone 2: Same bytes, leaner and harder
      2         5           0  Milestone 3: Few-colour images solved
      3         9           0  Milestone 4: Format version 3: beat JPEG XL e9 on photographs
      4         4           0  Milestone 5: Exact repeats
      5         3           0  Milestone 6: Faster than WebP to decode
      6         3           0  Milestone 7: Every photograph beats JPEG XL e9
      7         4           0  Milestone 8: Beat WebP on generated images
      8         3           0  Milestone 9: Holds on images never tuned on
      9        51           2  Milestone 10: RasterFold 1.0
     10         4           0  Milestone 11: SIMD decisions
     11         2           0  Milestone 12: Two-layer mixing
     12         2           0  Milestone 13: Stronger prediction
     13         2           0  Milestone 14: Adaptive colour transform
     14         2           0  Milestone 15: Effort levels
     15         2           0  Milestone 16: Max-effort encoder search
     16         2           0  Milestone 17: Trained starting state
     17         2           0  Milestone 18: Streaming
     18         2           0  Milestone 19: Many-core scaling
     19         2           0  Milestone 20: RasterFold 2.0
     20         2           0  Milestone 21: Run mode for flat regions
     21         2           0  Milestone 22: Verified hashed context tables
     22         2           0  Milestone 23: Higher-precision coder
     23         2           0  Milestone 24: Learned predictor at the max level
     24         2           0  Milestone 25: Texture-gated models
     25         2           0  Milestone 26: Random-access decode
     26         2           0  Milestone 27: Portable and byte-exact
     27         2           0  Milestone 28: Alpha, 16-bit, and metadata
     28         2           0  Milestone 29: Coverage-proven decoder
     29         2           0  Milestone 30: RasterFold 3.0
     30         2           0  Milestone 31: Language bindings and a streaming API
     31         2           0  Milestone 32: More formats
     32         2           0  Milestone 33: Lossless JPEG recompression
     33         2           0  Milestone 34: Image sequences
     34         2           0  Milestone 35: Gigapixel images
     35         2           0  Milestone 36: Browser decoding
     36         2           0  Milestone 37: Host plugins
     37         2           0  Milestone 38: Second implementation
     38         2           0  Milestone 39: Automatic effort
     39         2           0  Milestone 40: RasterFold 4.0
     40         2           0  Milestone 41: GPU encode search
     41         2           0  Milestone 42: Energy per megapixel
     42         2           0  Milestone 43: Phone-class ARM
     43         2           0  Milestone 44: Proven decoder bounds
     44         2           0  Milestone 45: Reproducible, signed releases
     45         2           0  Milestone 46: Collection archives
     46         2           0  Milestone 47: Tiny decoder profile
     47         2           0  Milestone 48: Recognised format
     48         2           0  Milestone 49: Third-party benchmarks
     49         2           0  Milestone 50: RasterFold 5.0
     50         2           0  Milestone 51: Learned model at the default level
     51         2           0  Milestone 52: Transmitted priors
     52         2           0  Milestone 53: Camera raw
     53         2           0  Milestone 54: Medical and scientific imagery
     54         2           0  Milestone 55: Error resilience
     55         2           0  Milestone 56: Progressive decoding
     56         2           0  Milestone 57: Lossless edit deltas
     57         2           0  Milestone 58: Hardware decoder
     58         2           0  Milestone 59: Twenty-year compatibility
     59         2           0  Milestone 60: RasterFold 6.0

discovered sections per campaign run (provenance, no cap):
  run=61f3a8f4474f  2

history 1 entry; latest 2026-09-29 approved by operator: Initial budget at the port of the TODO system from Isotone: no caps (schema 3, both null), as in the system it was ported from.
```

## Critical events

- Initial checkout clean on `main`; both ownership records absent. Native trust check: `all required Codex hooks trusted and enabled`.
- Reviewed all four open Codex intelligence entries; no disposition during this campaign.
- Android cross-compilation remains separate from runtime proof.

## Sections log

### D00 T04 §1

- Controller admitted run `ab65fd2f08d9`, generation `97b4ffef7c374fa7ac36c697c7140b1c`, session `01a0f3ee-8793-7b33-aed2-7c219d1dd691`.
- Resolved all 31 phase rows; full dependency output retained in `build/codex-phase-0-resolve.log`. First row is open with no dependencies.
- Existing implementation is being verified against the full contract; no prior evidence is treated as a stamp.

- Found and repaired an in-scope packaging defect: required artifacts omitted from `build.json` could silently disappear from the ZIP. Added five tests covering all target layouts, each required artifact omission, absent/empty artifact maps, tampering and unsafe paths; registered them in the full sweep.
- Installer fixtures: `Ran 4 tests ... OK`; native Windows correctness: `33 passed, 0 failed`, exit 0, retained in `build/codex-phase-0-windows-correctness.log`. Linux correctness also reports `33 passed, 0 failed`.
- First gate sweep found stale `build/todo-operator.json`; regenerated through `plan --sync`. Source-review receipts for README, building and contributing were renewed after reading against changed packaging and gate code. The final sweep will include the new package tests.
- Built all four target ZIPs through hash verification; outputs and digests retained in `build/codex-phase-0-packages.log`. No Android runtime proof is claimed.
- Supervisor process observed alive with matching session/generation; heartbeat log has no errors.

- Operator narrowed execution: "complete the current section then stop". Finish only D00 T04 §1 and stop the controller; no later section may start.

- Final candidate sweep: `python3 scripts/check-all.py --all-targets`, exit 0, `check-all: 30/30 passed`. Full output: `build/codex-phase-0-final-gates.log`.
- Final evidence: Linux correctness 33 passed/0 failed; API 8 tests OK; artifacts/consumers 4 tests OK; seeded fuzz 300 mutations, 293 rejected/7 valid, no crash/hang/mismatch; graph self-test 772 cases/0 failed with optional missing sibling clamp probe explicitly skipped; Claude guard 112 cases/0 failed; Codex lifecycle 55 tests OK; package tests 5 OK; installer tests 4 OK.
- Windows driven correctness 33 passed/0 failed and package tests 5 OK, each exit 0. All target package layouts have no loose artifact copies; manifests cover 5 Linux, 8 Windows and 4 artifacts per Android ABI.
- Research pass is not applicable: this section changes build/package infrastructure and makes no compression or speed claim. No codec/format/oracle/KAT source changed.

## Stopped run=ab65fd2f08d9

D00 T04 §1 implementation candidate: `73f0143949aceb1dd8fa4a695069d1df0952fa45`. Final local gates: 30/30 passed. The independent review exited 124 after 900.112 seconds at 2026-09-30T20:32:49Z, with empty stdout and no verdict. Requested pin: `claude-opus-5-5`, high effort, 900-second timeout; actual model and cost unresolved. Prompt SHA-256: `12386f8e7826c9eefca54f2997600399bd164205b1095c48e80c2660edbfb5e9`. Raw evidence remains in `build/codex-d00-t04-s1-review/independent.*`; canonical findings: `docs/reviews/00-workspace/D00-T04-s1.md`.

No fallback, panel approval, stamp, row flip, push or remote CI claim. The section is not complete. Stop because the mandatory reviewer is unavailable, preserving the operator's narrowed scope: "complete the current section then stop". No later section was started; the phase and plan remain open.
