"""The 60-milestone roadmap: one table, two outputs.

  python research/milestones.py --check     fail if the generated files differ from this table
  python research/milestones.py --write     regenerate them

Generates `docs/research.md` section 9 (between its BEGIN/END markers) and the
milestone files `todo/05-milestones/TODO-01..06`. Milestones 1 to 10 are gates
over work sections that already exist elsewhere in the tree; milestones 11 to 60
are a planning section (it authors the milestone's work one milestone ahead,
from the previous milestone's measured results) plus a gate. Every gate carries
the no-regression rule and the missed-milestone policy (D99 T01 §4). Edit the
table here, run --write, and commit the table and the generated files together.
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
RESEARCH = REPO / "docs" / "research.md"
TODO = REPO / "todo" / "05-milestones"
BEGIN, END = "<!-- BEGIN milestones (research/milestones.py) -->", "<!-- END milestones -->"

SIZE = "`python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns)"
SPEED = "`python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`"
BENCH = "`python tests/benchmark.py -n 3`"
IDLE = "Idle machine (timing)"
FF = "ffmpeg with libjxl (benchmarks)"

# (number, title, target, measurement, needs, requires, work refs for 1-10, band title for the first of a band)
M = [
    (1, "Locked baseline",
     "`pwsh scripts/check-all.ps1` green with every gate `PASS`; the `plan-gates` and `build` workflows green on the last push; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python scripts/drift-check.py` exits 0. Baseline on 2026-09-29: photos 0.971 of JPEG XL e7 and 0.983 of e9, 28 of 39 photos below e9",
     f"`pwsh scripts/check-all.ps1`, `python scripts/review_prompt.py ci-wait` on the last push, {SIZE}",
     [FF], None,
     "D00 T01 §1, D00 T01 §2, D00 T01 §3, D00 T01 §4, D00 T01 §5, D00 T02 §1, D00 T02 §2, D00 T02 §3, D08 T01 §1, D08 T01 §2, D08 T01 §3, D08 T02 §1, D08 T02 §2, D08 T02 §3, D08 T02 §4, D08 T02 §5, D99 T01 §3, D99 T01 §4"),
    (2, "Same bytes, leaner and harder",
     "every file byte-identical to milestone 1 (`KAT_FOLD` unchanged, the oracle agrees); decode `alloc_peak` at most 9.2 MB on `measure/image.bmp` (10.57 MB at milestone 1); one-thread decode at least 5% faster; a 4096x4096 header with a 20-byte payload refused in under a second",
     f"`bin/windows-x64/bin/rasterfold.exe decode --time`, {SPEED}, `python tests/test_correctness.py`",
     [IDLE], None,
     "D03 T01 §1, D03 T01 §2, D03 T01 §3, D04 T01 §1, D02 T01 §1, D02 T01 §2"),
    (3, "Few-colour images solved",
     "method 2 shipped; `syn_checker` below 210 bytes and `syn_pixel` below 53 bytes (JPEG XL e9's sizes); photo sizes unchanged; 20,000 fuzz mutations over every method clean",
     SIZE, [FF], None,
     "D01 T01 §1, D01 T01 §2, D01 T01 §3, D04 T01 §2"),
    (4, "Format version 3: beat JPEG XL e9 on photographs",
     "format version 3 with the accepted model changes: photo total at most 0.975 of JPEG XL e9, test pack at most 0.980, at least 35 of 39 photos below e9, three-thread decode at most 0.6 of JPEG XL's",
     f"{SIZE}, {BENCH}", [FF, IDLE], None,
     "D01 T02 §1, D01 T02 §2, D01 T02 §3, D01 T02 §4, D01 T02 §5, D01 T02 §6, D01 T02 §7, D01 T03 §1"),
    (5, "Exact repeats",
     "a match model in version 3: `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` below JPEG XL e9 (616, 271, 2195, and 2038 bytes); photos no worse than milestone 4",
     SIZE, [FF], None,
     "D01 T04 §1, D01 T04 §2, D01 T04 §3"),
    (6, "Faster than WebP to decode",
     "horizontal strips in version 3: photo decode wall time below lossless WebP's on the 12-thread development machine; strip size cost at most 0.3%",
     f"{BENCH}, {SIZE}", [FF, IDLE], None,
     "D02 T02 §1, D01 T03 §2"),
    (7, "Every photograph beats JPEG XL e9",
     "39 of 39 photos and 10 of 10 test-pack photos below JPEG XL e9; encode at most 0.3 of JPEG XL e7's time",
     f"{SIZE}, {BENCH}", [FF, IDLE], None,
     "D01 T05 §1, D01 T05 §2"),
    (8, "Beat WebP on generated images",
     "the generated set's total and geometric mean below lossless WebP's, and every generated image within 1.1 times WebP's size",
     SIZE, [FF], None,
     "D01 T06 §1, D01 T06 §2, D01 T06 §3"),
    (9, "Holds on images never tuned on",
     "milestones 4, 7, and 8 hold on the held-out set (`python research/ab.py size heldout`) without any retuning",
     "`python research/ab.py size heldout` against JPEG XL e9 and WebP", [FF], None,
     "D00 T03 §1, D00 T03 §2"),
    (10, "RasterFold 1.0",
     "`rasterfold-v1.0.0` released: format version 3 frozen; `rasterfold.exe` and `rasterfold.dll` with the C API; BMP, PNG, and PPM in and out; the HTML docs published; 100,000 fuzz mutations clean; smaller than JPEG XL e9 and lossless WebP on every set, and faster and leaner than JPEG XL on encode, decode, and memory",
     f"{SIZE}, {BENCH}, `python tests/fuzz_decode.py`", [FF, IDLE], None,
     "D06 T01 §1, D06 T01 §2, D06 T01 §3, D06 T01 §4, D06 T01 §5, D07 T01 §1, D07 T01 §2, D07 T01 §3, D07 T01 §4, D08 T03 §1, D08 T03 §2, D09 T01 §1, D09 T01 §2, D09 T01 §3, D09 T01 §4"),
    (11, "SIMD decisions", "one-thread decode at least 10% faster than 1.0, bytes within 0.1%", SPEED, [IDLE], None, "D02 T01 §3, D02 T02 §2"),
    (12, "Two-layer mixing", "photo total at least 0.4% smaller than milestone 11", SIZE, [FF], None, None),
    (13, "Stronger prediction", "photo total at least 0.5% smaller than milestone 12", SIZE, [FF], None, None),
    (14, "Adaptive colour transform", "photo total at least 0.3% smaller than milestone 13, decode cost unchanged", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (15, "Effort levels", "a fast level decoding at least twice as fast as the default and still smaller than JPEG XL e7; the default unchanged", f"{SPEED}, {SIZE}", [FF, IDLE], None, None),
    (16, "Max-effort encoder search", "the max level at least 1% smaller than the default at the same decode cost", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (17, "Trained starting state", "images of Kodak size and below at least 0.5% smaller, confirmed on the held-out set", "`python research/ab.py size kodak` and `heldout`", [FF], None, None),
    (18, "Streaming", "peak memory on a 24-megapixel image at most one third of milestone 17's", "`bin/windows-x64/bin/rasterfold.exe decode --time` on a 24-megapixel image", [], None, None),
    (19, "Many-core scaling", "decode throughput on 16 or more threads at least twice milestone 18's, size within 0.2%", f"{SPEED}, {SIZE}", [IDLE], None, None),
    (20, "RasterFold 2.0", "`rasterfold-v2.0.0`: against 1.0, the default at least 1.5% smaller, the max level at least 2.5% smaller, one-thread decode at least 1.2 times faster, memory at most one third", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
    (21, "Run mode for flat regions", "generated images decode at least twice as fast and photos at least 5% faster, sizes no worse", SPEED, [IDLE], None, None),
    (22, "Verified hashed context tables", "decode at least 10% faster at equal memory, size within 0.1%", SPEED, [IDLE], None, None),
    (23, "Higher-precision coder", "generated-image geometric mean at least 10% smaller, photos unchanged", SIZE, [FF], None, None),
    (24, "Learned predictor at the max level", "the max level at least 1% smaller on photos, fast and default byte-identical, confirmed on the held-out set", SIZE, [FF], None, None),
    (25, "Texture-gated models", "the default level at least 0.4% smaller at equal decode time", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (26, "Random-access decode", "decoding a 1/16 region at least 8 times faster than a full decode, size within 0.1%", "`bin/windows-x64/bin/rasterfold.exe decode --region` timings", [IDLE], None, None),
    (27, "Portable and byte-exact", "Linux, macOS, and Windows on ARM64 builds with identical bytes on every platform", "the known-answer vectors on every CI platform", [], None, None),
    (28, "Alpha, 16-bit, and metadata", "alpha, 16-bit samples, and ICC and EXIF preserved, 8-bit RGB files byte-identical to milestone 27, the 16-bit set below JPEG XL e9", "`python research/ab.py size rgba16`", [FF], None, None),
    (29, "Coverage-proven decoder", "libFuzzer branch coverage of the decoder at least 95%, no crash in 10^9 executions, hostile-input decode time at most twice honest time", "the libFuzzer campaign report", [], None, None),
    (30, "RasterFold 3.0", "`rasterfold-v3.0.0`: against 2.0, generated images at least 10% smaller, photos at the max level at least 1% smaller, decode at least 1.3 times faster, with random access, portability, alpha, and 16-bit", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
    (31, "Language bindings and a streaming API", "Python, Rust, and C# bindings over the 1.x C API and a streaming callback API, each with byte-exact round trips and API overhead at most 1%", "the bindings' test suites", [], None, None),
    (32, "More formats", "TIFF, 16-bit PNG, and PAM in and out with exact pixels", "round trips against Pillow and libtiff", [], None, None),
    (33, "Lossless JPEG recompression", "JPEG files stored at least 2% smaller than JPEG XL's JPEG recompression, the original JPEG rebuilt bit for bit", "a JPEG corpus against `cjxl --lossless_jpeg=1`", [FF], None, None),
    (34, "Image sequences", "bursts and screen recordings smaller than lossless animated JPEG XL and animated WebP", "a sequence corpus", [FF], None, None),
    (35, "Gigapixel images", "the size limit raised from 32 megapixels to 4 gigapixels with peak memory proportional to width", "`decode --time` on a 1-gigapixel image", [], None, None),
    (36, "Browser decoding", "decoding in a browser at most twice native time, from a WebAssembly module under 200 KB", "a headless-browser timing harness", [], None, None),
    (37, "Host plugins", "open and save with exact pixels in Pillow, ffmpeg, libvips, and GIMP", "a round-trip test in each host", [], None, None),
    (38, "Second implementation", "an independent Rust decoder, written from the spec alone, byte-identical on the conformance suite", "the conformance suite", [], None, None),
    (39, "Automatic effort", "at least 90% of the max level's gain at no more than 1.5 times the default encode time", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (40, "RasterFold 4.0", "`rasterfold-v4.0.0`: bindings, more formats, JPEG recompression, sequences, gigapixel images, browser decoding, and plugins, with no regression on 3.0 files", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
    (41, "GPU encode search", "max-level encode at least 10 times faster on a GPU, bytes identical to the CPU search", SPEED, ["GPU (compute)", IDLE], None, None),
    (42, "Energy per megapixel", "decode energy per megapixel at least 20% lower than milestone 41's", "hardware energy counters over a decode run", [IDLE], None, None),
    (43, "Phone-class ARM", "decode per GHz on ARM64 within 1.2 times x86-64, and 4K frames at the fast level in real time", "timings on an ARM64 device", ["ARM64 device", IDLE], None, None),
    (44, "Proven decoder bounds", "bounded model checking proves no out-of-bounds access or overflow in the decoder for every input up to a stated size", "the CBMC run in CI", [], None, None),
    (45, "Reproducible, signed releases", "the same source builds a byte-identical executable on any machine; releases are signed with a software bill of materials", "two independent builds compared", [], None, None),
    (46, "Collection archives", "a library of similar photos at least 3% smaller than encoding each file alone", "a photo-collection corpus", [], None, None),
    (47, "Tiny decoder profile", "a profile decodable in at most 256 KB of working memory by a decoder of at most 20 KB of code", "the profile's decoder size and memory", [], None, None),
    (48, "Recognised format", "detected by libmagic and the major image libraries without our plugins; media type registered", "upstream releases that carry the entries", [], "outside parties accept the registration and the upstream entries", None),
    (49, "Third-party benchmarks", "public benchmark results by others match our measurements within their noise", "the published third-party results", [], "outside benchmark maintainers run and publish the results", None),
    (50, "RasterFold 5.0", "`rasterfold-v5.0.0`: lower energy, a verified decoder, reproducible signed builds, ARM parity, archives, and the tiny profile", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
    (51, "Learned model at the default level", "the default photo size at least 2% smaller at no more than 1.2 times decode time", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (52, "Transmitted priors", "images under 1 megapixel at least 1% smaller", SIZE, [FF], None, None),
    (53, "Camera raw", "raw sensor data smaller than DNG's lossless JPEG and than JPEG XL on the same raw files", "a raw corpus", [FF], None, None),
    (54, "Medical and scientific imagery", "16-bit grayscale and multi-channel images smaller than JPEG-LS and JPEG XL on public medical and satellite sets", "the medical and satellite corpora", [FF], None, None),
    (55, "Error resilience", "a file with a damaged byte still yields every undamaged tile", "a corruption-injection test", [], None, None),
    (56, "Progressive decoding", "a usable preview from the first 5% of the bytes, lossless at the end, for at most 1% size cost", SIZE, [FF], None, None),
    (57, "Lossless edit deltas", "an edited copy of an image stored in at most 10% of a full file", "an edit corpus", [], None, None),
    (58, "Hardware decoder", "an FPGA decoder of the fast or tiny profile at 100 megapixels per second or more, byte-exact with the software", "the FPGA testbench on the conformance suite", ["FPGA board"], None, None),
    (59, "Twenty-year compatibility", "every file written by every release since version 2 decodes in the newest release, proven by a permanent compatibility corpus", "the compatibility corpus in CI", [], None, None),
    (60, "RasterFold 6.0", "`rasterfold-v6.0.0`: the learned default model, raw and scientific data, resilience, progressive decoding, edit deltas, and the hardware reference", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
]
BANDS = [
    (1, "Milestones 1 to 10: Beat JPEG XL e9 and WebP, then Release 1.0", "beat-jxl-and-webp"),
    (11, "Milestones 11 to 20: RasterFold Beats RasterFold (2.0)", "beat-itself"),
    (21, "Milestones 21 to 30: Reach (3.0)", "reach"),
    (31, "Milestones 31 to 40: Beyond Single Still Files (4.0)", "beyond-still-files"),
    (41, "Milestones 41 to 50: Efficiency and Trust (5.0)", "efficiency-and-trust"),
    (51, "Milestones 51 to 60: New Data and Long-Term Robustness (6.0)", "new-data"),
]
RELEASES = {10: "1.0.0", 20: "2.0.0", 30: "3.0.0", 40: "4.0.0", 50: "5.0.0", 60: "6.0.0"}
RULE = ("It keeps everything milestone {prev} reached: no photo size more than 0.1% worse, no encode or decode time "
        "more than 3% worse, no higher peak memory, and every older file still decoding.")
POLICY = ("If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments "
          "from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.")


def band_of(n: int) -> int:
    return (n - 1) // 10


def section_numbers(n: int) -> tuple[int | None, int]:
    """(planning section, gate section) inside the band file."""
    i = (n - 1) % 10
    if n <= 10:
        return None, i + 1
    return 2 * i + 1, 2 * i + 2


def gate_ref(n: int) -> str:
    return f"D05 T{band_of(n) + 1:02d} §{section_numbers(n)[1]}"


def plan_ref(n: int) -> str:
    return f"D05 T{band_of(n) + 1:02d} §{section_numbers(n)[0]}"


def local(ref: str, t: int) -> str:
    return ref.split(" ", 2)[2] if ref.startswith(f"D05 T{t:02d} ") else ref


def render_band(b: int) -> str:
    first, title, slug = BANDS[b]
    t = b + 1
    rows, body = [], []
    for n, name, target, measure, needs, requires, work, *_ in M[first - 1:first + 9]:
        p, g = section_numbers(n)
        prev = gate_ref(n - 1) if n > 1 else None
        if p is not None:
            rows.append((p, f"Plan milestone {n}: {name}", local(prev, t) if prev else "--"))
            deps = local(plan_ref(n), t) + (", " + work if work else "")
        else:
            deps = work + (", " + local(prev, t) if prev else "")
        rows.append((g, f"Milestone {n} gate: {name}", deps))
        if p is not None:
            body.append(f"""## {p}. Plan Milestone {n}: {name}

Milestone {n}'s work is authored here, one milestone ahead, from what milestone {n - 1} actually measured: its target is *{target}*. The design notes are `docs/research.md` section 9 (milestone {n}) and the sections of `docs/research.md` it points at; the baseline is milestone {n - 1}'s gate stamp (`{gate_ref(n - 1)}`), never a figure from memory.

- [ ] Read milestone {n - 1}'s gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`{gate_ref(n)}`). Done when: `python scripts/todo-graph.py resolve '{gate_ref(n)}'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone {n} -- {name.lower()}"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve '{gate_ref(n)}'` exits 4 naming at least one new work section, proving the gate waits on the authored work.
""")
        extra = []
        # One Needs line per section (the parser keeps the last): the most
        # constraining host wins.
        rank = {"FPGA board": 0, "ARM64 device": 0, "GPU (compute)": 0, IDLE: 1, FF: 2}
        if needs:
            extra.append(f"**Needs:** {sorted(needs, key=lambda x: rank.get(x, 3))[0]}\n")
        if requires:
            extra.append(f"**Requires:** operator -- {requires}.\n")
        release = RELEASES.get(n)
        release_items = ""
        if release and n != 10:
            release_items = (f"- [ ] Push the tag `rasterfold-v{release}`. Done when: the release workflow publishes the package and its checksums.\n"
                             f"- [ ] Run the backlog review for `rasterfold-v{release}`: promote each backlog entry into a section or record the operator's deferral on it. Done when: every entry carries a `reviewed: rasterfold-v{release}` field or has been promoted.\n")
        source = "\n-> SOURCE: webp-second-target\n" if n == 8 else ""
        body.append(f"""## {g}. Milestone {n} Gate: {name}

{''.join(e + chr(10) for e in extra)}**Job:** A user can rely on milestone {n}: {target}.
{source}
Milestone {n} of the roadmap (`docs/research.md` section 9). {RULE.format(prev=n - 1) if n > 1 else "It is the baseline every later milestone is measured against."} {POLICY}

- [ ] Measure the target with {measure}. Done when: {target}, each figure quoted with its command.
- [ ] {"Measure the no-regression rule against milestone " + str(n - 1) + "'s gate stamp: " + SIZE + ", " + BENCH + ", `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone " + str(n - 1) + "'s figures." if n > 1 else "Record the baseline figures (" + SIZE + ", " + BENCH + ") as the reference for milestone 2. Done when: they are quoted in this section's stamp."}
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
{release_items}- [ ] Commit: `"milestone: reach milestone {n} -- {name.lower()}"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.
""")
    order = "\n".join(f"|  {i:>2}   |  §{s:<3} | {d} | {dep} |  [ ]   |" for i, (s, d, dep) in enumerate(sorted(rows), 1))
    return f"""---
schema_version: 1
id: milestones-{slug}
domain: 05-milestones
status: draft
title: "TODO-{t:02d} -- {title}"
depends_on: []
track: G{t}
---

# TODO-{t:02d} -- {title}

> **Goal:** Milestones {first} to {first + 9} of the roadmap in `docs/research.md` section 9 are reached in order, each proven by a gate that measures its target and the no-regression rule against the milestone before it.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** No milestone gate has run. This file is generated from `research/milestones.py`; edit the table there and run `python research/milestones.py --write`, never this file by hand.
<!-- claim: exists research/milestones.py -->

## Inputs

- [`research/milestones.py`](../../research/milestones.py) -- the milestone table this file is generated from
- [`docs/research.md`](../../docs/research.md) -- section 9, the roadmap; sections 2 and 3, the measurement protocol and what was already tried

## Outcome

- Every milestone gate in this file is stamped with its target and no-regression figures quoted.

**Adjacency:** all=not-applicable (milestone gates and planning sections; the work sections they depend on carry the product adjacency)

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
{order}

---

{chr(10).join(body)}
## Verification

- [ ] `python research/milestones.py --check` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
"""


def render_research() -> str:
    out = [BEGIN, "",
           "Each milestone keeps everything the one before it reached and adds one measurable step: no photo size more than",
           "0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still",
           "decoding. Milestones 1 to 10 are measured against JPEG XL and WebP; from milestone 11 RasterFold is measured",
           "against its own previous release. Targets past milestone 10 are estimates: each is re-derived from the previous",
           "milestone's measured results when its planning section runs. The gates live in `todo/05-milestones/`; this",
           "table is generated from `research/milestones.py`.", ""]
    for first, title, _slug in BANDS:
        out += [f"### {title}", "", "| # | milestone | target | gate |", "|---|---|---|---|"]
        for n, name, target, *_ in M[first - 1:first + 9]:
            out.append(f"| {n} | {name} | {target} | `{gate_ref(n)}` |")
        out.append("")
    out.append(END)
    return "\n".join(out)


def targets() -> dict[Path, str]:
    files = {}
    for b, (_first, _title, slug) in enumerate(BANDS):
        files[TODO / f"TODO-{b + 1:02d}-{slug}.md"] = render_band(b)
    text = RESEARCH.read_text(encoding="utf-8")
    if BEGIN in text:
        head, rest = text.split(BEGIN, 1)
        tail = rest.split(END, 1)[1]
        files[RESEARCH] = head + render_research() + tail
    else:
        files[RESEARCH] = text.rstrip("\n") + "\n\n---\n\n## 9. Roadmap: milestones 1 to 60\n\n" + render_research() + "\n"
    return files


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "--check"
    stale = []
    for path, want in targets().items():
        have = path.read_text(encoding="utf-8") if path.exists() else None
        if have != want:
            stale.append(path)
            if mode == "--write":
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(want.encode("utf-8"))
    if mode == "--write":
        print(f"milestones: wrote {len(stale)} file(s)")
        return 0
    for p in stale:
        print(f"milestones: {p.relative_to(REPO)} is stale; run python research/milestones.py --write")
    print(f"milestones: {'current' if not stale else str(len(stale)) + ' stale'}")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
